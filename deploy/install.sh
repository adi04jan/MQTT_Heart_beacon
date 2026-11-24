#!/usr/bin/env bash
# Heart Beacon v2 installer for the home server: Ubuntu 20.04 whose shared Mosquitto 1.6.9 (1883, water tank,
# Frigate, logger) is never touched: nothing under /etc/mosquitto/, no mosquitto.service action.
#   sudo bash deploy/install.sh <public-ip>            dry run: prints every action, changes nothing
#   sudo bash deploy/install.sh --apply <public-ip>    performs them
# Idempotent: a re-run refreshes code, configs and units (and restarts the two hb units); it keeps the DB, the
# config-signing key, the CA and the router password, and re-issues the TLS leaf only when it does not name this
# IP or expires within 30 days. Refuses without the IP, if 8883/8443/8444/1884 are held by anything but its own
# units, or if any action would touch the shared broker (checked for the whole plan before anything runs).
# HB_PKI (default /root/hb-pki) is where mkca.sh keeps the CA; put an existing ca.key + ca.crt there first.
# Sourced by uninstall.sh, e2e-local.sh and the tests for its helpers (main runs only when executed).
set -euo pipefail

UV_VERSION=0.13.7
PY=3.12
PORTS="8883 8443 8444 1884"
OPT=/opt/hb ETC=/etc/hb MQ=/etc/mosquitto-hb VAR=/var/lib/hb MQVAR=/var/lib/hb-mosquitto UNITS=/etc/systemd/system
PKI=${HB_PKI:-/root/hb-pki}
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UVENV="UV_PYTHON_INSTALL_DIR=$OPT/python UV_CACHE_DIR=$OPT/.cache"
APPLY=0

die() { echo "REFUSED: $*" >&2; exit 1; }

# True if a command would touch the shared broker: /etc/mosquitto itself, or its unit by name.
shared() {
  local s=" $* " a='/etc/mosquitto([^-[:alnum:]_.]|$)' b='(^|[^-[:alnum:]_])mosquitto\.service' \
    c='systemctl[[:space:]]([^;&|]*[[:space:]])?mosquitto([[:space:]]|$)'
  [[ $s =~ $a || $s =~ $b || $s =~ $c ]]
}

# Every change goes through here: always printed, executed only with --apply.
run() {
  if shared "$1"; then die "touches the shared broker: $1"; fi
  echo "+ $1"
  if [ "$APPLY" = 1 ]; then bash -c "set -euo pipefail; $1"; fi
}

listening() { ss -ltnH; }
active() { systemctl is-active --quiet "$1" 2>/dev/null; }
ports_up() { listening | awk '{print $4}' | sed 's/.*://' | sort -un; }

valid_ip() {
  local re='^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$' i
  [[ ${1:-} =~ $re ]] || return 1
  for i in 1 2 3 4; do (( BASH_REMATCH[i] <= 255 )) || return 1; done
}

need() {
  local t missing=""
  for t in ss systemctl openssl curl tar sed awk useradd install /usr/sbin/mosquitto; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  [ -z "$missing" ] || die "missing tools:$missing"
}

# <ip>: refuses on a missing or bad IP, or one of our ports held by something other than our own unit.
preflight() {
  valid_ip "${1:-}" || die "usage: install.sh [--apply] <public-ipv4>   (got '${1:-}')"
  local p busy=""
  for p in $(ports_up); do
    case " $PORTS " in *" $p "*) ;; *) continue ;; esac
    case $p in 8883 | 1884) active hb-mosquitto && continue ;; *) active hb-server && continue ;; esac
    busy="$busy $p"
  done
  [ -z "$busy" ] || die "port(s)$busy already in use; hb needs $PORTS (1883 stays the shared broker's)"
}

# <uid> <path>...: refuses a symlink at any of the paths, or an existing one not owned by <uid>. Root writes into
# these; a compromised hb account must not steer those writes (or a chown) with a planted link.
check_paths() {
  local want=$1 p
  shift
  for p in "$@"; do [ ! -L "$p" ] || die "$p is a symlink: root will not write through it"; done
  for p in "$@"; do [ ! -e "$p" ] || [ "$(stat -c %u -- "$p")" = "$want" ] || die "$p is not owned by uid $want"; done
}

# Everything root writes or chowns: root-owned trees ($ETC is root:hb 0750, its files root:hb 0640, so hb can read
# but not plant), and the only hb-owned paths root touches ($VAR, $MQVAR, router.pass).
safe_paths() {
  check_paths 0 $OPT $MQ $ETC "$PKI" $MQ/mosquitto-hb.conf $MQ/acl $UNITS/hb-mosquitto.service \
    $UNITS/hb-server.service $ETC/{ca.crt,leaf.crt,leaf.key,cfg-sign.key,cfg-sign.pub,hb-server.env}
  check_paths "$(id -u hb 2>/dev/null || echo none)" $VAR $MQVAR $VAR/router.pass
}

# Installed leaf chains to the installed CA, carries IP SAN $1 and is good for 30 more days.
leaf_ok() {
  [ -f $ETC/leaf.key ] && openssl verify -CAfile $ETC/ca.crt $ETC/leaf.crt >/dev/null 2>&1 &&
    openssl x509 -in $ETC/leaf.crt -noout -checkend 2592000 >/dev/null 2>&1 &&
    openssl x509 -in $ETC/leaf.crt -noout -ext subjectAltName 2>/dev/null | grep -Eq "IP Address:${1//./\\.}(,|$)"
}

plan() {
  local ip=$1
  # service user and directories
  if ! id hb >/dev/null 2>&1; then
    run "useradd --system --user-group --home-dir $VAR --no-create-home --shell /usr/sbin/nologin hb"
  fi
  run "install -d -m 755 $OPT $MQ && install -d -o root -g hb -m 750 $ETC && install -d -o hb -g hb -m 700 $VAR $MQVAR"
  # code: server/ and deploy/ from this checkout
  if [ "$SRC" != "$OPT" ]; then
    run "tar -C $SRC --exclude=.venv --exclude=__pycache__ --exclude=.pytest_cache -cf - server deploy | tar -C $OPT --no-same-owner -xf -"
  fi
  # uv and a Python $PY venv, all under $OPT (the system Python is 3.8)
  if [ ! -x $OPT/bin/uv ]; then
    run "curl -LsSf https://astral.sh/uv/$UV_VERSION/install.sh | env UV_UNMANAGED_INSTALL=$OPT/bin sh"
  fi
  if [ ! -x $OPT/server/.venv/bin/python ]; then
    run "$UVENV $OPT/bin/uv venv --python $PY $OPT/server/.venv"
  fi
  run "$UVENV $OPT/bin/uv pip install --python $OPT/server/.venv/bin/python -r $OPT/server/pyproject.toml"
  # TLS: CA + leaf with the IP SAN (mkca.sh), and the config-signing key (made once, never replaced: its public
  # half is pinned in the firmware and the app, like the CA)
  if ! leaf_ok "$ip" || [ ! -f $ETC/cfg-sign.key ]; then
    if [ -f $ETC/ca.crt ] && [ ! -f "$PKI/ca.key" ]; then
      die "$ETC/ca.crt exists but $PKI/ca.key does not: copy ca.key + ca.crt from the dev machine into $PKI (a new CA would break the one pinned in the firmware and app)"
    fi
    run "install -d -m 700 $PKI"
    if [ -f $ETC/cfg-sign.key ] && [ ! -f "$PKI/cfg-sign.key" ]; then
      run "install -m 600 $ETC/cfg-sign.key $PKI/" # mkca.sh keeps an existing key
    fi
    run "sh $OPT/deploy/mkca.sh $ip $PKI"
    run "install -o root -g hb -m 640 $PKI/ca.crt $PKI/leaf.crt $PKI/leaf.key $PKI/cfg-sign.pub $ETC/"
    if [ ! -f $ETC/cfg-sign.key ]; then
      run "install -o root -g hb -m 640 $PKI/cfg-sign.key $ETC/"
    fi
  fi
  # router's broker password (the server hashes it into the password file). hb owns it, so it is made in a
  # root-only temp dir and installed fresh: never written through whatever sits at the target.
  if [ ! -e $VAR/router.pass ]; then
    run "t=\$(mktemp -d) && openssl rand -hex 32 > \$t/p && rm -f $VAR/router.pass && install -o hb -g hb -m 600 \$t/p $VAR/router.pass && rm -r \$t"
  fi
  # broker config, server settings, units
  run "install -m 644 $OPT/deploy/mosquitto-hb.conf $OPT/deploy/acl $MQ/"
  run "sed 's/^HB_HOSTS=.*/HB_HOSTS=$ip/' $OPT/deploy/hb-server.env > $ETC/hb-server.env && chown -h root:hb $ETC/hb-server.env && chmod 640 $ETC/hb-server.env"
  run "install -m 644 $OPT/deploy/hb-mosquitto.service $OPT/deploy/hb-server.service $UNITS/"
  run "systemctl daemon-reload && systemctl enable hb-mosquitto hb-server && systemctl restart hb-mosquitto hb-server"
}

main() {
  local a ip="" mode
  for a in "$@"; do
    case $a in --apply) APPLY=1 ;; -*) die "unknown option $a" ;; *) ip=$a ;; esac
  done
  need
  preflight "$ip"
  safe_paths
  if [ "$APPLY" = 1 ] && [ "$(id -u)" != 0 ]; then die "--apply needs root: sudo bash $0 --apply $ip"; fi
  mode=$APPLY
  APPLY=0
  plan "$ip" >/dev/null # the whole plan through the guard before anything runs
  APPLY=$mode
  [ "$APPLY" = 1 ] || echo "# dry run: nothing is changed; add --apply to perform these"
  plan "$ip"
  if [ "$APPLY" = 1 ]; then
    for _ in $(seq 30); do [ "$(ports_up | grep -cxE '8883|8443|8444|1884')" = 4 ] && break; sleep 1; done
    systemctl --no-pager --lines=5 status hb-mosquitto hb-server ||
      die "a unit is not running: journalctl -u hb-mosquitto -u hb-server -n 50"
    [ "$(ports_up | grep -cxE '8883|8443|8444|1884')" = 4 ] || die "not all of $PORTS are listening (ss -ltn)"
    echo "# installed; listening on $PORTS. Next: RUNBOOK.md step 4 (copy ca.key off this server)."
  fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then main "$@"; fi
