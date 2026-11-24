#!/usr/bin/env bash
# Local end-to-end proof without the real host, no root: the shipped broker config (mosquitto-hb.conf + acl, paths
# moved into a temp dir) on a local Mosquitto 1.6 binary (Ubuntu 20.04's apt package: the 1.6.9 the home server
# runs, beside any broker already on 1883), hb-server from this checkout in a uv venv, the smoke users made with
# the admin CLI while the server runs, then smoke.py against 127.0.0.1. Runs the server tests in the same venv
# first (the real-broker ACL test included). Stops both processes and removes the temp dir on exit.
#   bash deploy/e2e-local.sh        (Windows: wsl -d Ubuntu-20.04 -e bash deploy/e2e-local.sh)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
export PATH="$PATH:$HOME/.local/bin:/usr/sbin" PYTHONDONTWRITEBYTECODE=1
. "$here/install.sh"
preflight 127.0.0.1 # the installer's port rule: 8883 8443 8444 1884 must be free
echo "== $(mosquitto -h 2>&1 | head -n 1), $(uv --version)"

t=$(mktemp -d)
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait || true
  rm -rf "$t"
}
trap cleanup EXIT

sh "$here/mkca.sh" 127.0.0.1 "$t/pki" >"$t/mkca.log" 2>&1 || { cat "$t/mkca.log"; exit 1; }
mkdir -p "$t/var/fw" "$t/mq" "$t/bin"
: >"$t/var/passwd"
sed -e "s#/var/lib/hb-mosquitto/#$t/mq/#; s#/var/lib/hb/#$t/var/#; s#/etc/mosquitto-hb/#$here/#; s#/etc/hb/#$t/pki/#" \
  -e 's/^listener 8883$/listener 8883 127.0.0.1/' "$here/mosquitto-hb.conf" >"$t/mosquitto-hb.conf"
mosquitto -c "$t/mosquitto-hb.conf" >"$t/mosquitto.log" 2>&1 &
pids+=($!)
echo $! >"$t/mosquitto.pid"
# auth.reload_broker() asks systemd for hb-mosquitto's MainPID: answer with this broker's pid
printf '#!/bin/sh\ncat %s/mosquitto.pid\n' "$t" >"$t/bin/systemctl"
chmod +x "$t/bin/systemctl"

echo "== venv"
py=$t/venv/bin/python
uv venv -q --python '>=3.11' "$t/venv"
uv pip install -q --python "$py" $("$py" -c "import tomllib; p = tomllib.load(open('$root/server/pyproject.toml', 'rb')); print(' '.join(p['project']['dependencies'] + p['dependency-groups']['dev']))")

echo "== server tests"
(cd "$root/server" && "$py" -m pytest -q -rs -p no:cacheprovider --basetemp "$t/pytest")

export PATH="$t/bin:$PATH" PYTHONPATH="$root/server" HB_HOSTS=127.0.0.1 HB_DB=$t/var/hb.db HB_PASSWD=$t/var/passwd \
  HB_ROUTER_PASS=$t/var/router.pass HB_FW_DIR=$t/var/fw HB_CFG_KEY=$t/pki/cfg-sign.key HB_TLS_CERT=$t/pki/leaf.crt \
  HB_TLS_KEY=$t/pki/leaf.key
echo "== hb-server"
"$py" -m hb_server serve >"$t/server.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do [ "$(ports_up | grep -cxE '8883|8443|8444|1884')" = 4 ] && break; sleep 0.2; done
ports_up | grep -xE '8883|8443|8444|1884' | tr '\n' ' '
echo "listening"

echo "== smoke users (admin CLI, server running)"
pa=$("$py" -m hb_server adduser smoke_a "Smoke A" --circle smoke)
pb=$("$py" -m hb_server adduser smoke_b "Smoke B" --circle smoke)
"$py" -m hb_server register HB-000001 --assign smoke_b

echo "== smoke.py 127.0.0.1"
rc=0
HB_SMOKE_A_PASS=$pa HB_SMOKE_B_PASS=$pb "$py" "$here/smoke.py" 127.0.0.1 --ca "$t/pki/ca.crt" || rc=$?
if [ "$rc" != 0 ]; then
  echo "--- mosquitto.log"; tail -n 40 "$t/mosquitto.log"
  echo "--- server.log"; tail -n 40 "$t/server.log"
fi
exit "$rc"
