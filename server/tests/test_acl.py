"""deploy/acl (the shipped file) under Mosquitto 1.6 ACL semantics, then against a real 1.6.9 broker in Docker."""
import re
import subprocess
import threading
import time
from pathlib import Path

import pytest

from hb_server import api, auth

ACL = Path(__file__).resolve().parents[2] / "deploy" / "acl"
A, B = "HB-AAAAAA", "HB-BBBBBB"


def _matches(filt: str, topic: str) -> bool:
    f, t = filt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t) or (part != "+" and part != t[i]):
            return False
    return len(f) == len(t)


def allowed(user: str, topic: str, access: str) -> bool:
    """Mosquitto 1.6 acl_file: `pattern` lines apply to every user with %u = username; `topic` lines belong to
    the preceding `user`. Access is read (subscribe + delivery) or write."""
    rules, owner = [], None
    for line in ACL.read_text().splitlines():
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0] == "user":
            owner = parts[1]
        elif parts[0] in ("topic", "pattern"):
            acc, filt = (parts[1], parts[2]) if len(parts) == 3 else ("readwrite", parts[1])
            if parts[0] == "pattern":
                rules.append((acc, filt.replace("%u", user)))
            elif owner == user:
                rules.append((acc, filt))
    return any(access in acc and _matches(filt, topic) for acc, filt in rules)


def topics(did_or_uid: str):
    return [f"hb/v1/d/{did_or_uid}/{leaf}" for leaf in ("up", "state", "down", "cfg")] + \
           [f"hb/v1/u/{did_or_uid}/{leaf}" for leaf in ("up", "state", "down")]


# Patterns apply to every user, so each login also gets the other class's subtree under its own name
# (u/HB-AAAAAA/..., d/alice/...). No real beacon or user can have that name, and the router drops it.
OWN_D = {("d/{}/up", "write"), ("d/{}/state", "write"), ("d/{}/down", "read"), ("d/{}/cfg", "read")}
OWN_U = {("u/{}/up", "write"), ("u/{}/state", "write"), ("u/{}/down", "read")}


def reach(user):
    return {(t, a) for t in topics(user) for a in ("read", "write") if allowed(user, t, a)}


def expect(user):
    return {(f"hb/v1/{t.format(user)}", a) for t, a in OWN_D | OWN_U}


def test_beacon_reaches_only_its_own_name():
    assert reach(A) == expect(A)
    for t in topics(B) + topics("alice") + ["hb/v1/#", "$SYS/broker/clients", "heart_beacon"]:
        assert not allowed(A, t, "read") and not allowed(A, t, "write"), t


def test_phone_reaches_only_its_own_name():
    assert reach("alice") == expect("alice")
    for t in topics("bob") + topics(A):
        assert not allowed("alice", t, "read") and not allowed("alice", t, "write"), t


def test_the_cross_class_subtrees_are_dead():
    from hb_server import models
    from hb_server.router import Beacon, Msg, State, User, route
    st = State(users={"alice": User("alice", "Alice", "home"), "bob": User("bob", "Bob", "home")},
               beacons={A: Beacon(A, "alice"), B: Beacon(B, "bob", wifi=True, path="wifi")})
    mood = models.encode("mood", "00000000000000aa", 0, {"k": "heart"})
    assert route(Msg(f"hb/v1/u/{A}/up", mood, 1), st) == []  # a beacon posing as a phone
    assert route(Msg(f"hb/v1/u/{A}/state", b'{"on":false}', 1), st) == [] and st.beacons[B].wifi
    assert route(Msg("hb/v1/d/alice/up", mood, 1), st) == []  # a phone posing as a beacon


def test_router_has_the_whole_tree_and_names_cannot_collide():
    assert all(allowed("router", t, a) for t in topics(A) + topics("alice") for a in ("read", "write"))
    assert not allowed("router", "heart_beacon", "read")  # the v1 shared topic lives on the other broker
    assert not api.UID.fullmatch(A) and not re.fullmatch(r"HB-[0-9A-F]{6}", "alice")


# ---- real Mosquitto 1.6.9 -------------------------------------------------------------------

def _docker_up() -> bool:
    try:
        return subprocess.run(["docker", "info"], capture_output=True, timeout=15).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


@pytest.mark.skipif(not _docker_up(), reason="Docker is not running: real-broker ACL test needs eclipse-mosquitto:1.6.9")
def test_real_mosquitto_1_6_9_keeps_beacons_apart(tmp_path):
    import paho.mqtt.client as mqtt

    pw = {u: auth.new_secret() for u in ("router", A, B, "alice")}
    auth.write_passwd(tmp_path / "passwd", [(u, auth.mosq_hash(p)) for u, p in pw.items()])
    (tmp_path / "acl").write_bytes(ACL.read_bytes())
    (tmp_path / "mosquitto.conf").write_text(
        "allow_anonymous false\npassword_file /mosquitto/config/passwd\nacl_file /mosquitto/config/acl\n"
        "listener 1883\n", newline="\n")
    cid = subprocess.run(["docker", "run", "-d", "--rm", "-p", "127.0.0.1::1883", "-v",
                          f"{tmp_path}:/mosquitto/config", "eclipse-mosquitto:1.6.9"],
                         capture_output=True, text=True, check=True).stdout.strip()
    clients = []
    try:
        port = int(subprocess.run(["docker", "port", cid, "1883"], capture_output=True, text=True,
                                  check=True).stdout.split(":")[-1])
        got = {u: [] for u in pw}

        def connect(user):
            c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"t-{user}", protocol=mqtt.MQTTv5)
            c.username_pw_set(user, pw[user])
            c.on_message = lambda cl, ud, m: got[user].append(m.topic)
            for _ in range(50):  # broker start-up
                try:
                    c.connect("127.0.0.1", port)
                    break
                except OSError:
                    time.sleep(0.2)
            c.loop_start()
            clients.append(c)
            return c

        r, a = connect("router"), connect(A)
        connect(B)
        r.subscribe("hb/v1/#", qos=1)
        a.subscribe([("hb/v1/#", 1), (f"hb/v1/d/{B}/#", 1), (f"hb/v1/d/{A}/down", 1)])
        time.sleep(1)
        for t in (f"hb/v1/d/{B}/down", f"hb/v1/d/{B}/cfg", "hb/v1/u/alice/down", f"hb/v1/d/{A}/down"):
            r.publish(t, b"x", qos=1)
        a.publish(f"hb/v1/d/{B}/up", b"spoof", qos=1)
        a.publish(f"hb/v1/d/{A}/up", b"mine", qos=1)
        deadline = time.time() + 5
        while time.time() < deadline and (f"hb/v1/d/{A}/up" not in got["router"]):
            time.sleep(0.1)
        time.sleep(0.5)
        assert sorted(set(got[A])) == [f"hb/v1/d/{A}/down"]  # nothing of B's, nothing of the phone's
        assert f"hb/v1/d/{A}/up" in got["router"] and f"hb/v1/d/{B}/up" not in got["router"]  # no spoofing B
    finally:
        for c in clients:
            c.loop_stop()
            c.disconnect()
        subprocess.run(["docker", "rm", "-f", cid], capture_output=True)
