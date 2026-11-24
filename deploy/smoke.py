"""End-to-end smoke test of a deployed Heart Beacon v2 server, run from any client:

    HB_SMOKE_A_PASS=... HB_SMOKE_B_PASS=... python deploy/smoke.py <ip> --ca ca.crt

Needs paho-mqtt 2.x (server/.venv has it). The test users smoke_a and smoke_b (circle `smoke`, so real users never
see them) and the test beacon HB-000001 assigned to smoke_b come from the admin CLI (RUNBOOK.md step 7). smoke_a
plays a phone; this script also plays smoke_b's beacon, claiming it as smoke_b on every run and answering every
`show` with `shown` like the firmware. One PASS/FAIL line per check; exit 0 only when all pass.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request

import paho.mqtt.client as mqtt

WAIT = 10  # s per expected message (the router retries a show after 10 s)
BURST = 3  # router coalescing window (PROTOCOL.md 4)


class Fail(Exception):
    pass


def ok(msg: str) -> None:
    print(f"PASS {msg}", flush=True)


def env(t: str, id_: str | None = None, **body) -> bytes:
    m = {"v": 1, "t": t, "id": id_ or secrets.token_hex(8), "ts": int(time.time()), **body}
    return json.dumps(m, separators=(",", ":")).encode()


def tls_checks(host: str, port: int, pinned: ssl.SSLContext) -> None:
    with socket.create_connection((host, port), WAIT) as s, pinned.wrap_socket(s, server_hostname=host):
        ok(f"{port}: TLS with the pinned CA, IP SAN {host}")
    other = "192.0.2.1" if host != "192.0.2.1" else "192.0.2.2"
    for what, ctx, name in (("without the pinned CA (system trust store)", ssl.create_default_context(), host),
                            (f"for another IP ({other}: SAN mismatch)", pinned, other)):
        try:
            with socket.create_connection((host, port), WAIT) as s, ctx.wrap_socket(s, server_hostname=name):
                pass
        except ssl.SSLCertVerificationError:
            ok(f"{port}: refused {what}")
            continue
        raise Fail(f"{port}: TLS accepted {what}")


def api(base: str, ctx: ssl.SSLContext, path: str, body: dict, token: str | None = None) -> dict:
    hdrs = {"Content-Type": "application/json", **({"Authorization": f"Bearer {token}"} if token else {})}
    req = urllib.request.Request(base + path, json.dumps(body).encode(), hdrs, method="POST")
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=WAIT) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        raise Fail(f"POST {path}: HTTP {e.code} {e.read()[:200].decode(errors='replace')}") from None


class Client:
    """One MQTT 5 session over TLS; keeps every message it gets. `beacon`: answers each `show` with `shown`."""

    def __init__(self, host, port, ctx, user, pw, cid, subs, beacon=False):
        self.msgs: list[tuple[str, dict | None]] = []
        self.up = f"hb/v1/d/{user}/up"
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid, protocol=mqtt.MQTTv5)
        self.c.username_pw_set(user, pw)
        self.c.tls_set_context(ctx)
        done, rc = threading.Event(), []
        self.c.on_connect = lambda c, u, f, reason, p: (rc.append(reason), done.set())
        self.c.on_subscribe = lambda c, u, mid, codes, p: done.set()

        def on_message(c, u, m):
            try:
                body = json.loads(m.payload) if m.payload else None
            except ValueError:
                body = None
            self.msgs.append((m.topic, body))
            if beacon and body and body.get("t") == "show":
                c.publish(self.up, env("shown", body["id"]), qos=1)

        self.c.on_message = on_message
        self.c.connect(host, port, keepalive=60, clean_start=True)
        self.c.loop_start()
        if not done.wait(WAIT) or rc[0].is_failure:
            self.close()
            raise Fail(f"MQTT connect as {user}: {rc[0] if rc else 'no CONNACK'}")
        done.clear()
        self.c.subscribe([(t, 1) for t in subs])
        if not done.wait(WAIT):
            raise Fail(f"MQTT subscribe as {user}: no SUBACK")

    def pub(self, topic: str, payload: bytes, retain: bool = False) -> None:
        self.c.publish(topic, payload, qos=1, retain=retain).wait_for_publish(WAIT)

    def expect(self, what: str, pred, timeout: float = WAIT) -> dict:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for t, m in list(self.msgs):
                if m and pred(t, m):
                    return m
            time.sleep(0.05)
        raise Fail(f"no {what} within {timeout:g} s")

    def shows(self) -> list[dict]:
        return [m for t, m in list(self.msgs) if m and t.endswith("/down") and m.get("t") == "show"]

    def close(self) -> None:
        self.c.disconnect()
        self.c.loop_stop()


def run(a) -> None:
    pinned = ssl.create_default_context(cafile=a.ca)  # only our CA; hostname (IP SAN) checked
    for port in (a.mport, a.aport):
        tls_checks(a.host, port, pinned)

    base = f"https://{a.host}:{a.aport}"
    la = api(base, pinned, "/api/v1/login", {"user": a.a, "pass": a.pa})
    lb = api(base, pinned, "/api/v1/login", {"user": a.b, "pass": a.pb})
    ok(f"{a.aport}: API login as {a.a} and {a.b}")
    claim = api(base, pinned, "/api/v1/devices/claim", {"did": a.did}, lb["token"])
    ok(f"{a.did} claimed by {a.b}: beacon credential + signed config n={claim['cfg']['n']}")

    clients = []
    try:
        def connect(user, pw, cid, subs, beacon=False):
            c = Client(a.host, a.mport, pinned, user, pw, cid, subs, beacon)
            clients.append(c)
            return c

        def beacon_up():
            c = connect(a.did, claim["pass"], f"d-{a.did}", [f"hb/v1/d/{a.did}/down", f"hb/v1/d/{a.did}/cfg"], True)
            c.pub(f"hb/v1/d/{a.did}/state", env("state", st="away", path="wifi"), retain=True)
            return c

        pa = connect(la["mqtt"]["user"], la["mqtt"]["pass"], f"u-{a.a}-smoke", [f"hb/v1/u/{a.a}/down"])
        pb = connect(lb["mqtt"]["user"], lb["mqtt"]["pass"], f"u-{a.b}-smoke", [f"hb/v1/u/{a.b}/down"])
        beacon = beacon_up()
        beacon.expect("retained cfg", lambda t, m: t.endswith("/cfg") and m.get("n") == claim["cfg"]["n"])
        ok(f"{a.mport}: phone and beacon logins; beacon got its signed config (retained)")
        time.sleep(1)
        if beacon.shows():  # leftovers of an earlier failed run, flushed and acked: let their burst window pass
            time.sleep(BURST + 0.5)

        def rcpt(eid, st):
            return lambda t, m: m.get("t") == "rcpt" and m.get("id") == eid and m.get("st") == st \
                and m.get("to") == a.did

        e1 = secrets.token_hex(8)
        pa.pub(f"hb/v1/u/{a.a}/up", env("mood", e1, k="heart"))
        beacon.expect("show on the beacon topic", lambda t, m: m.get("t") == "show" and m.get("id") == e1
                      and m.get("k") == "heart")
        shown_at = time.monotonic()
        ok(f"heart from {a.a}'s phone reached hb/v1/d/{a.did}/down as show")
        pb.expect("mood on the owner's phone", lambda t, m: m.get("t") == "mood" and m.get("id") == e1)
        ok(f"{a.b}'s phone got the mood")
        pa.expect("rcpt shown", rcpt(e1, "shown"))
        ok(f"beacon's shown -> rcpt shown to {a.a}")

        beacon.pub(f"hb/v1/d/{a.did}/state", env("state", st="sleep", path="wifi"), retain=True)
        pb.expect("sleep state relayed to the owner", lambda t, m: m.get("t") == "relay"
                  and m.get("m", {}).get("t") == "state" and m.get("m", {}).get("st") == "sleep")
        beacon.close()
        clients.remove(beacon)
        time.sleep(max(0.0, shown_at + BURST + 0.5 - time.monotonic()))  # else they join e1's burst (played once)
        e2, e3 = secrets.token_hex(8), secrets.token_hex(8)
        for e in (e2, e3):
            pa.pub(f"hb/v1/u/{a.a}/up", env("mood", e, k="heart"))
            pa.expect("rcpt queued", rcpt(e, "queued"))
        ok("beacon offline: 2 hearts queued")

        beacon = beacon_up()
        first = beacon.expect("show after reconnect", lambda t, m: m.get("t") == "show")
        time.sleep(BURST)
        shows = [(m["id"], m.get("k")) for m in beacon.shows()]
        if shows != [(e2, "heart")]:
            raise Fail(f"after reconnect expected one show {e2} (coalesced), got {shows}")
        ok(f"reconnect: both offline hearts delivered as one show (id {first['id']})")
        for e in (e2, e3):
            pa.expect("rcpt shown", rcpt(e, "shown"))
        ok(f"both offline hearts receipted shown to {a.a}")
        beacon.pub(f"hb/v1/d/{a.did}/state", env("state", st="sleep", path="wifi"), retain=True)
    finally:
        for c in clients:
            c.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="the server's public IP (the leaf certificate's IP SAN)")
    ap.add_argument("--ca", required=True, help="ca.crt, the CA pinned in the firmware and app")
    ap.add_argument("--mport", type=int, default=8883)
    ap.add_argument("--aport", type=int, default=8443)
    ap.add_argument("--a", default="smoke_a", help="phone user")
    ap.add_argument("--b", default="smoke_b", help="user the test beacon is assigned to")
    ap.add_argument("--did", default="HB-000001", help="test beacon did")
    a = ap.parse_args(argv)
    a.pa, a.pb = os.environ.get("HB_SMOKE_A_PASS"), os.environ.get("HB_SMOKE_B_PASS")
    if not (a.pa and a.pb):
        ap.error("set HB_SMOKE_A_PASS and HB_SMOKE_B_PASS (printed by hb-admin adduser)")
    try:
        run(a)
    except (Fail, OSError, KeyError) as e:
        print(f"FAIL {e}", flush=True)
        return 1
    print("SMOKE PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
