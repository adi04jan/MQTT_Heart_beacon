"""deploy/install.sh preflight and shared-broker guard, sourced into bash with `ss`, systemd, `id` and the tool check
stubbed. Nothing is installed: no test passes --apply as root, and `run` only prints without --apply."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which("bash")
pytestmark = pytest.mark.skipif(BASH is None, reason="no bash")
IP = "203.0.113.10"


def sh(script: str, listening: str = "", active: str = "", pre: str = "") -> tuple[int, str]:
    """Source install.sh, with `ss -ltnH` printing `listening` and the units in `active` running, then run `script`.
    Fed on stdin as bytes: no shell quoting, no CRLF from Windows."""
    text = f"""{pre}
source deploy/install.sh
listening() {{ cat <<'SS'
{listening}
SS
}}
active() {{ case " {active} " in *" $1 "*) return 0 ;; esac; return 1; }}
need() {{ :; }}
id() {{ if [ "${{1:-}}" = -u ]; then echo 1000; else return 1; fi; }}
{script}
"""
    r = subprocess.run([BASH, "-s"], input=text.encode(), cwd=ROOT, capture_output=True, timeout=120)
    return r.returncode, (r.stdout + r.stderr).decode(errors="replace")


def ss(*ports: str) -> str:
    return "\n".join(f"LISTEN 0 100 {p} 0.0.0.0:*" for p in ports)


@pytest.mark.parametrize("ip", ["", "203.0.113", "300.1.2.3", "01.2.3.4", "example.com", "203.0.113.10/24"])
def test_refuses_without_a_valid_ipv4(ip):
    rc, out = sh(f"main {ip}")
    assert rc == 1 and "REFUSED: usage" in out, out


def test_refuses_our_ports_held_by_someone_else():
    rc, out = sh(f"preflight {IP}", ss("0.0.0.0:1883", "[::]:8443", "127.0.0.1:1884"))
    assert rc == 1 and "port(s) 1884 8443 already in use" in out, out
    rc, out = sh(f"preflight {IP}", ss("*:8883", "127.0.0.1:8444"))
    assert rc == 1 and "8444 8883" in out, out


def test_the_shared_broker_on_1883_and_other_ports_are_fine():
    rc, out = sh(f"preflight {IP} && echo OK", ss("0.0.0.0:1883", "[::]:1883", "0.0.0.0:22", "127.0.0.53%lo:53"))
    assert rc == 0 and "OK" in out, out


def test_a_rerun_may_find_our_own_units_on_their_ports():
    ours = ss("0.0.0.0:8883", "[::]:8883", "127.0.0.1:1884", "0.0.0.0:8443", "127.0.0.1:8444")
    assert sh(f"preflight {IP}", ours, active="hb-mosquitto hb-server")[0] == 0
    rc, out = sh(f"preflight {IP}", ours, active="hb-mosquitto")  # the API ports are someone else's
    assert rc == 1 and "port(s) 8443 8444 already" in out, out


SHARED = ["rm -rf /etc/mosquitto/conf.d/x", "cp a /etc/mosquitto", "echo x > '/etc/mosquitto'",
          "systemctl restart mosquitto", "systemctl reload mosquitto.service", "systemctl --now disable mosquitto",
          "rm /etc/systemd/system/mosquitto.service.d/override.conf", "systemctl daemon-reload && systemctl stop mosquitto"]
OURS = ["install -m 644 a /etc/mosquitto-hb/", "systemctl restart hb-mosquitto hb-server",
        "rm -f /etc/systemd/system/hb-mosquitto.service", "sh /opt/hb/deploy/mkca.sh 1.2.3.4 /root/hb-pki"]


def test_guard_spots_every_touch_of_the_shared_broker_and_none_of_ours():
    rc, out = sh("\n".join(f"if shared \"{c}\"; then echo T; else echo F; fi" for c in SHARED + OURS))
    assert rc == 0 and out.split() == ["T"] * len(SHARED) + ["F"] * len(OURS), out


def test_dry_run_prints_the_whole_plan_and_runs_nothing():
    rc, out = sh(f"main {IP}")
    assert rc == 0, out
    acts = [ln[2:] for ln in out.splitlines() if ln.startswith("+ ")]
    assert "# dry run" in out and len(acts) >= 12
    for want in ("useradd --system", "/install.sh | env UV_UNMANAGED_INSTALL=/opt/hb/bin sh", "uv venv --python 3.12",
                 f"mkca.sh {IP} /root/hb-pki", "install -o hb -g hb -m 600 $t/p /var/lib/hb/router.pass",
                 "install -d -o root -g hb -m 750 /etc/hb", "install -o root -g hb -m 640 /root/hb-pki/ca.crt",
                 f"s/^HB_HOSTS=.*/HB_HOSTS={IP}/", "systemctl enable hb-mosquitto hb-server"):
        assert any(want in a for a in acts), want
    assert not any(re.search(r"/etc/mosquitto(?![-\w.])|(?<![-\w])mosquitto\.service", a) for a in acts)


def test_a_plan_touching_the_shared_broker_is_refused_before_anything_runs():
    rc, out = sh(f"run() {{ if shared \"$1\"; then die \"touches the shared broker: $1\"; fi; echo \"RAN $1\"; }}\n"
                 f"main {IP}", pre="HB_PKI=/etc/mosquitto/pki")  # the CA dir pointed into the shared broker
    assert rc == 1 and "REFUSED: touches the shared broker" in out and "RAN" not in out, out


PREFIX = 'r=$(mktemp -d)\ntrap \'rm -rf "$r"\' EXIT\n'  # a temp root prefix, removed however the script exits


def test_a_symlink_planted_where_root_writes_is_refused():
    rc, out = sh(PREFIX + "VAR=$r/var/lib/hb\nmkdir -p $VAR\nln -s /etc/passwd $VAR/router.pass 2>/dev/null || true\n"
                 "[ -L $VAR/router.pass ] || { echo NOSYMLINK; exit 0; }\n"
                 f"run() {{ echo \"RAN $1\"; }}\nmain {IP}")
    if "NOSYMLINK" in out:
        pytest.skip("this bash cannot make symlinks (Git Bash without native links); runs in WSL / Linux")
    assert rc == 1 and "router.pass is a symlink" in out and "RAN" not in out, out


def test_a_root_tree_not_owned_by_root_is_refused():
    rc, out = sh(PREFIX + f"ETC=$r/etc/hb\nmkdir -p $ETC\nrun() {{ echo \"RAN $1\"; }}\nmain {IP}")
    assert rc == 1 and "/etc/hb is not owned by uid 0" in out and "RAN" not in out, out


def test_apply_needs_root():
    rc, out = sh(f"run() {{ echo \"RAN $1\"; }}\nmain --apply {IP}")
    assert rc == 1 and "--apply needs root" in out and "RAN" not in out, out
