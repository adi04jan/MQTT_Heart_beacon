"""Router rules from the plan: fan-out per circle, dedupe, path choice, retry on the other path, 24 h expiry,
receipts, burst coalescing, flush on wake. Pure: time and ids are injected."""
import itertools
import json
import threading

from hb_server import models
from hb_server.db import Db
from hb_server.router import Beacon, Directory, Msg, State, Tick, User, load_state, route, save_state

A, B, C = "HB-AAAAAA", "HB-BBBBBB", "HB-CCCCCC"


def mkstate():
    ids = itertools.count(1)
    return State(
        users={"alice": User("alice", "Alice", "home"), "bob": User("bob", "Bob", "home"),
               "carol": User("carol", "Carol", "other")},
        beacons={A: Beacon(A, "alice", tz=330), B: Beacon(B, "bob", tz=330), C: Beacon(C, "carol")},
        new_id=lambda: f"{next(ids):016x}")


def env(t, id_, **body):
    return models.encode(t, id_, 0, body)


def send(st, topic, raw, now=1000):
    return [(p.topic, json.loads(p.payload) if p.payload else None, p.retain) for p in route(Msg(topic, raw, now), st)]


def tick(st, now):
    return [(p.topic, json.loads(p.payload)) for p in route(Tick(now), st)]


def up(st, did, path="wifi", now=1000):
    """Beacon reports itself reachable on `path`."""
    raw = env("state", "f" * 16, st="away" if path == "wifi" else "near", path=path)
    if path == "wifi":
        return send(st, f"hb/v1/d/{did}/state", raw, now)
    owner = st.beacons[did].owner
    return send(st, f"hb/v1/u/{owner}/up", env("relay", "e" * 16, did=did, m=json.loads(raw)), now)


def only(pubs, topic):
    return [p for t, p, *_ in pubs if t == topic]


EID = "00000000000000aa"


def test_phone_mood_fans_out_to_the_circle_only():
    st = mkstate()
    up(st, B)
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    topics = [t for t, *_ in pubs]
    assert only(pubs, "hb/v1/u/bob/down") == [{"v": 1, "t": "mood", "id": EID, "ts": 1000, "k": "heart", "from": "Alice"}]
    assert only(pubs, f"hb/v1/d/{B}/down") == [{"v": 1, "t": "show", "id": EID, "ts": 1000, "k": "heart"}]
    assert only(pubs, "hb/v1/u/alice/down") == [{"v": 1, "t": "rcpt", "id": EID, "ts": 1000, "to": B, "st": "queued"}]
    assert not any("carol" in t or C in t or A in t for t in topics)  # other circle, and not the sender's own beacon


def test_beacon_mood_is_sent_by_its_owner_and_receipts_go_to_the_owner_phone():
    st = mkstate()
    up(st, B)
    pubs = send(st, f"hb/v1/d/{A}/up", env("mood", EID, k="sorry"))
    assert only(pubs, "hb/v1/u/bob/down")[0]["from"] == "Alice"
    assert only(pubs, f"hb/v1/d/{B}/down")[0]["k"] == "sorry"
    assert only(pubs, "hb/v1/u/alice/down")[0]["t"] == "rcpt"


def test_dedupe_on_source_and_id_across_paths():
    st = mkstate()
    up(st, B)
    first = send(st, f"hb/v1/d/{A}/up", env("mood", EID, k="heart"))
    again = send(st, f"hb/v1/d/{A}/up", env("mood", EID, k="heart"))  # same id on WiFi
    relayed = send(st, "hb/v1/u/alice/up", env("relay", "1" * 16, did=A, m=json.loads(env("mood", EID, k="heart"))))
    assert first and not again and not relayed
    assert len(st.events) == 1 and len(st.deliveries) == 1
    other = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))  # same id, different source: new event
    assert other and len(st.events) == 2


def test_bad_kind_and_bad_envelope_are_dropped():
    st = mkstate()
    assert send(st, "hb/v1/u/alice/up", env("mood", EID, k="hug")) == []
    assert send(st, "hb/v1/u/alice/up", b'{"v":1,"t":"mood"}') == []
    assert send(st, "hb/v1/u/mallory/up", env("mood", EID, k="heart")) == []  # unknown user
    assert send(st, "hb/v1/d/HB-DDDDDD/up", env("mood", EID, k="heart")) == []  # unclaimed beacon


def test_path_choice_follows_the_last_reported_live_path():
    st = mkstate()
    up(st, B, "ble")
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    relay = only(pubs, "hb/v1/u/bob/down")[1]  # [0] is the phone's own mood
    assert relay["t"] == "relay" and relay["did"] == B and relay["m"]["t"] == "show" and relay["m"]["id"] == EID
    assert not only(pubs, f"hb/v1/d/{B}/down")
    up(st, B, "wifi", now=1001)
    pubs = send(st, "hb/v1/u/alice/up", env("mood", "00000000000000bb", k="annoy"), now=1002)
    assert only(pubs, f"hb/v1/d/{B}/down")[0]["t"] == "show"


def test_retry_on_other_path_after_10s_then_wait_for_the_beacon():
    st = mkstate()
    up(st, B, "wifi", now=1000)
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)
    assert tick(st, 1009) == []
    retry = tick(st, 1010)
    assert [(t, p["t"], p["m"]["id"]) for t, p in retry] == [("hb/v1/u/bob/down", "relay", EID)]
    assert tick(st, 1019) == []
    ping = tick(st, 1020)  # both paths tried: pending, unreachable, and asked `get info` on both paths
    assert st.deliveries[0].status == "pending" and st.beacons[B].live() is None
    assert [(t, p["t"]) for t, p in ping] == [(f"hb/v1/d/{B}/down", "get"), ("hb/v1/u/bob/down", "relay")]
    assert ping[1][1]["m"]["t"] == "get"
    assert tick(st, 1020 + 119) == []
    assert [p["t"] for _, p in tick(st, 1020 + 120)] == ["get", "relay"]  # again every 2 min while hearts wait
    flush = send(st, f"hb/v1/d/{B}/up", env("tele", "2" * 16, mv=3900), now=1100)  # any uplink: reachable again
    assert only(flush, f"hb/v1/d/{B}/down") == [{"v": 1, "t": "show", "id": EID, "ts": 1100, "k": "heart"}]


def test_shown_stops_retry_and_sends_receipt():
    st = mkstate()
    up(st, B)
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    pubs = send(st, f"hb/v1/d/{B}/up", env("shown", EID), now=1001)
    assert only(pubs, "hb/v1/u/alice/down") == [{"v": 1, "t": "rcpt", "id": EID, "ts": 1001, "to": B, "st": "shown"}]
    assert send(st, f"hb/v1/d/{B}/up", env("shown", EID), now=1002) == []  # duplicate receipt is idempotent
    assert tick(st, 1030) == []


def test_queued_while_asleep_flushes_on_state_up():
    st = mkstate()
    send(st, f"hb/v1/d/{B}/state", env("state", "3" * 16, st="sleep", path="wifi", wake=5000), now=900)
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)
    assert not only(pubs, f"hb/v1/d/{B}/down") and st.deliveries[0].status == "pending"
    woke = up(st, B, "wifi", now=5000)
    downs = only(woke, f"hb/v1/d/{B}/down")
    assert [p["t"] for p in downs] == ["show", "time"]
    assert downs[1]["tz"] == 330 and downs[1]["ts"] == 5000


def test_expire_after_24h_with_receipt():
    st = mkstate()
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)
    assert tick(st, 1000 + 24 * 3600 - 1) == []
    assert tick(st, 1000 + 24 * 3600) == [
        ("hb/v1/u/alice/down", {"v": 1, "t": "rcpt", "id": EID, "ts": 87400, "to": B, "st": "expired"})]
    assert [p["t"] for p in only(up(st, B, now=90000), f"hb/v1/d/{B}/down")] == ["time"]  # nothing left to flush


def test_burst_within_3s_plays_once_per_kind():
    st = mkstate()
    up(st, B)
    p1 = send(st, "hb/v1/u/alice/up", env("mood", "000000000000000a", k="heart"), now=1000)
    send(st, f"hb/v1/d/{B}/up", env("shown", "000000000000000a"), now=1001)
    p2 = send(st, f"hb/v1/d/{A}/up", env("mood", "000000000000000b", k="heart"), now=1002)  # within 3 s
    p3 = send(st, "hb/v1/u/alice/up", env("mood", "000000000000000c", k="sorry"), now=1002)  # other kind
    p4 = send(st, "hb/v1/u/alice/up", env("mood", "000000000000000d", k="heart"), now=1004)  # 4 s later
    shows = [only(p, f"hb/v1/d/{B}/down") for p in (p1, p2, p3, p4)]
    assert [len(s) for s in shows] == [1, 0, 1, 1]
    assert [r["st"] for r in only(p2, "hb/v1/u/alice/down")] == ["queued", "shown"]


def test_flush_on_wake_coalesces_each_kind_once():
    st = mkstate()
    for i, k in enumerate(["heart", "heart", "sorry", "heart"]):
        send(st, "hb/v1/u/alice/up", env("mood", f"{i + 1:016x}", k=k), now=1000 + 60 * i)
    pubs = up(st, B, now=4000)
    shows = [p for p in only(pubs, f"hb/v1/d/{B}/down") if p["t"] == "show"]
    assert [(s["id"], s["k"]) for s in shows] == [(f"{1:016x}", "heart"), (f"{3:016x}", "sorry")]
    done = send(st, f"hb/v1/d/{B}/up", env("shown", f"{1:016x}"), now=4001)
    assert sorted(r["id"] for r in only(done, "hb/v1/u/alice/down")) == [f"{i:016x}" for i in (1, 2, 4)]


def test_link_and_relay_only_for_own_beacon():
    st = mkstate()
    assert send(st, "hb/v1/u/alice/up", env("link", "4" * 16, did=B, ble=True)) == []
    assert st.beacons[B].live() is None
    send(st, "hb/v1/u/bob/up", env("link", "4" * 16, did=B, ble=True))
    assert st.beacons[B].live() == "ble"
    send(st, "hb/v1/u/bob/state", b'{"on":false}', now=1001)  # phone lost its server link: no BLE path
    assert st.beacons[B].live() is None


def test_lwt_lost_only_drops_wifi():
    st = mkstate()
    up(st, B, "wifi")
    send(st, "hb/v1/u/bob/up", env("link", "5" * 16, did=B, ble=True))
    send(st, f"hb/v1/d/{B}/state", env("state", "6" * 16, st="lost"), now=1001)
    assert (st.beacons[B].wifi, st.beacons[B].live()) == (False, "ble")


def test_unknown_type_gets_resp_on_the_same_path():
    st = mkstate()
    pubs = send(st, "hb/v1/u/alice/up", env("dance", "7" * 16))
    assert only(pubs, "hb/v1/u/alice/down") == [
        {"v": 1, "t": "resp", "id": f"{1:016x}", "ts": 1000, "re": "7" * 16, "ok": False, "err": "unknown_t"}]
    pubs = send(st, f"hb/v1/d/{B}/up", env("dance", "8" * 16))
    assert only(pubs, f"hb/v1/d/{B}/down")[0]["re"] == "8" * 16


def test_malformed_payloads_are_dropped_and_routing_continues():
    st = mkstate()
    up(st, B)
    hostile = [
        ("hb/v1/u/alice/up", env("mood", "1" * 16, k=["heart"])),
        ("hb/v1/u/alice/up", env("mood", "2" * 16, k={"heart": 1})),
        (f"hb/v1/d/{A}/up", env("mood", "3" * 16, k=["heart"])),
        ("hb/v1/u/bob/up", env("link", "4" * 16, did=[B], ble=True)),
        ("hb/v1/u/bob/up", env("relay", "5" * 16, did={"x": B}, m={})),
        ("hb/v1/u/bob/up", env("relay", "6" * 16, did=B, m=[1, 2])),
        (f"hb/v1/d/{B}/state", env("state", "7" * 16, st=["near"], path=["ble"], wake="soon")),
        (f"hb/v1/d/{B}/up", env("tele", "8" * 16, mv=[[[1]]], fw="2.0.0")),
        ("hb/v1/u/bob/state", b"[" * 1024),
        ("hb/v1/u/bob/state", b'{"on":' + b"[" * 900 + b"]" * 900 + b"}"),
        ("hb/v1/u/bob/state", b'{"on":false,"pad":"' + b"x" * 2000 + b'"}'),  # oversize
        ("hb/v1/u/bob/state", b"[false]"),
        ("hb/v1/u/alice/up", b'{"v":1,"t":"mood","id":"' + b"9" * 16 + b'","ts":0,"k":' + b"[" * 980 + b"}"),
        ("hb/v1/u/alice/up", b"x" * 5000),
    ]
    for topic, raw in hostile:
        assert all(t != f"hb/v1/u/alice/down" or p["t"] != "rcpt" for t, p, _ in send(st, topic, raw)), topic
    assert st.events == {} and st.beacons[B].wifi and st.beacons[B].tele == {"fw": "2.0.0"}
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1001)  # still routing
    assert only(pubs, f"hb/v1/d/{B}/down")[0]["id"] == EID


def test_glue_logs_and_drops_a_crashing_message(tmp_path, monkeypatch, caplog):
    from hb_server import router
    db = Db(str(tmp_path / "hb.db"))
    g = router.Glue(db, "127.0.0.1", 1, "pw", clock=lambda: 1000)
    monkeypatch.setattr(router, "route", lambda ev, st: 1 / 0)
    assert g.handle(Msg("hb/v1/u/alice/up", b"{}", 1000)) == []
    assert "router dropped hb/v1/u/alice/up" in caplog.text
    monkeypatch.undo()
    assert g.handle(Tick(1001)) == []  # the glue still works afterwards


def test_reclaim_by_another_user_expires_the_old_owners_queue():
    st = mkstate()
    st.users["dave"] = User("dave", "Dave", "other")
    up(st, B, "ble", now=1000)  # bob's phone carries B
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)  # in flight to bob over BLE
    send(st, f"hb/v1/d/{B}/state", env("state", "9" * 16, st="sleep", path="wifi"), now=1001)
    send(st, "hb/v1/u/alice/up", env("mood", "00000000000000bb", k="sorry"), now=1002)  # queued
    users = list(st.users.values())
    moved = route(Directory(users, [(A, "alice", 330), (B, "dave", 0), (C, "carol", 0)], 1003), st)  # B -> dave
    got = [(p.topic, json.loads(p.payload)) for p in moved]
    assert sorted((t, p["id"], p["st"]) for t, p in got) == [
        ("hb/v1/u/alice/down", EID, "expired"), ("hb/v1/u/alice/down", "00000000000000bb", "expired")]
    later = tick(st, 1020) + up(st, B, "wifi", now=1030) + send(st, f"hb/v1/d/{B}/up", env("shown", EID), now=1031)
    later += tick(st, 1060)
    # nothing to bob, no rcpt at all: only device status, to the new owner
    assert [(t, p["t"], p["m"]["t"]) for t, p, *_ in later if not t.endswith(f"{B}/down")] == [
        ("hb/v1/u/dave/down", "relay", "state")]
    assert [p["t"] for t, p, *_ in later if t.endswith(f"{B}/down")] == ["time"]  # no old show reaches dave's B


def test_concurrent_refreshes_apply_the_newest_directory(tmp_path):
    from hb_server import router
    db = Db(str(tmp_path / "hb.db"))
    for uid in ("bob", "dave"):
        db.x("INSERT INTO user (uid, name, pw, mqtt_pass, mqtt_hash) VALUES (?, ?, '', '', '')", (uid, uid))
    db.x("INSERT INTO device (did, owner, claimed, registered) VALUES (?, 'bob', 1, 0)", (B,))
    first_loaded, gate, calls = threading.Event(), threading.Event(), []

    def loader(d):
        snap = router.load_directory(d)
        calls.append(snap)
        if len(calls) == 1:  # the first refresh holds its old snapshot until the second one has had its chance
            first_loaded.set()
            gate.wait(5)
        return snap

    g = router.Glue(db, "127.0.0.1", 1, "pw", clock=lambda: 1000, loader=loader)
    t1 = threading.Thread(target=g.refresh)
    t1.start()
    assert first_loaded.wait(5)
    db.x("UPDATE device SET owner = 'dave' WHERE did = ?", (B,))  # a re-claim lands meanwhile
    t2 = threading.Thread(target=g.refresh)
    t2.start()
    t2.join(0.5)  # unfixed: t2 loads and applies the new owner now, and t1 then overwrites it
    gate.set()
    t1.join(5)
    t2.join(5)
    assert g.state.beacons[B].owner == "dave"


def test_reclaim_by_the_same_owner_keeps_the_queue():
    st = mkstate()
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)
    devs = [(A, "alice", 330), (B, "bob", 330), (C, "carol", 0)]
    assert route(Directory(list(st.users.values()), devs, 1001), st) == []
    assert only(up(st, B, now=1002), f"hb/v1/d/{B}/down")[0]["t"] == "show"


def test_queue_survives_restart(tmp_path):
    db = Db(str(tmp_path / "hb.db"))
    db.x("INSERT INTO user (uid, name, pw, mqtt_pass, mqtt_hash) VALUES ('alice', 'Alice', '', '', '')")
    db.x("INSERT INTO user (uid, name, pw, mqtt_pass, mqtt_hash) VALUES ('bob', 'Bob', '', '', '')")
    for did, owner in ((A, "alice"), (B, "bob")):
        db.x("INSERT INTO device (did, owner, claimed, registered) VALUES (?, ?, 1, 0)", (did, owner))
    st = load_state(db, 1000)
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=1000)
    save_state(db, st)
    st2 = load_state(Db(str(tmp_path / "hb.db")), 2000)
    assert [(d.eid, d.did, d.status) for d in st2.deliveries] == [(EID, B, "pending")]
    assert send(st2, "hb/v1/u/alice/up", env("mood", EID, k="heart"), now=2001) == []  # dedupe survives too
    assert only(up(st2, B, now=2002), f"hb/v1/d/{B}/down")[0]["id"] == EID


def test_state_st_must_be_an_exact_lowercase_name():
    st = mkstate()
    for bad in ("NEAR", "Away", "awake", ""):
        pubs = send(st, f"hb/v1/d/{B}/state", env("state", "a" * 16, st=bad, path="wifi"))
        assert pubs == [] and st.beacons[B].live() is None, bad  # dropped: no time, no status, no path
    for good in models.STATES:
        assert send(st, f"hb/v1/d/{B}/state", env("state", "b" * 16, st=good, path="wifi")), good
    assert st.beacons[B].st == "lowbatt"


def test_state_and_tele_reach_the_owner_phone_as_relay():
    st = mkstate()
    pubs = send(st, f"hb/v1/d/{B}/state", env("state", "c" * 16, st="away", path="wifi", wake=0, junk=[1]))
    relays = [p for p in only(pubs, "hb/v1/u/bob/down") if p["t"] == "relay"]
    assert relays == [{"v": 1, "t": "relay", "id": relays[0]["id"], "ts": 1000, "did": B,
                       "m": {"v": 1, "t": "state", "id": "c" * 16, "ts": 0, "st": "away", "path": "wifi", "wake": 0}}]
    pubs = send(st, f"hb/v1/d/{B}/up", env("tele", "d" * 16, mv=3912, pct=71, fw="2.0.0", heap=[1]), now=1001)
    assert [p["m"] for p in only(pubs, "hb/v1/u/bob/down")] == [
        {"v": 1, "t": "tele", "id": "d" * 16, "ts": 0, "mv": 3912, "pct": 71, "fw": "2.0.0"}]  # as stored
    lwt = send(st, f"hb/v1/d/{B}/state", env("state", "e" * 16, st="lost"), now=1002)
    assert [p["m"]["st"] for p in only(lwt, "hb/v1/u/bob/down")] == ["lost"]
    # Over BLE (relayed up by bob's phone) it comes back down too, keeping the beacon's id.
    tele = json.loads(env("tele", "f" * 16, mv=3900))
    back = send(st, "hb/v1/u/bob/up", env("relay", "1" * 16, did=B, m=tele), now=1003)
    assert [p["m"]["id"] for p in only(back, "hb/v1/u/bob/down")] == ["f" * 16]
    assert all("alice" not in t and "carol" not in t for t, *_ in pubs + lwt + back)


def test_mood_from_is_the_senders_display_name():
    st = mkstate()
    st.users["alice"].name = "Asha ♥"
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    assert [p["from"] for p in only(pubs, "hb/v1/u/bob/down")] == ["Asha ♥"]  # not the uid


def test_cfg_is_never_sent_in_a_relay():
    from hb_server import router
    st, out = mkstate(), []
    router._send(st, st.beacons[B], "ble", env("cfg", "2" * 16, n=1, b="e30", s="x"), 1000, out)
    assert out == []
    router._send(st, st.beacons[B], "ble", env("show", "3" * 16, k="heart"), 1000, out)
    assert [json.loads(p.payload)["m"]["t"] for p in out] == ["show"]


def test_glue_picks_up_users_and_claims_written_by_the_admin_cli(tmp_path):
    from hb_server import router
    g = router.Glue(Db(str(tmp_path / "hb.db")), "127.0.0.1", 1, "pw", clock=lambda: 1000)
    g.poll()
    assert g.state.users == {}
    cli = Db(str(tmp_path / "hb.db"))  # another process: `hb-admin adduser` / `register` while the server runs
    cli.x("INSERT INTO user (uid, name, pw, mqtt_pass, mqtt_hash) VALUES ('bob', 'Bob', '', '', '')")
    cli.x("INSERT INTO device (did, owner, claimed, registered) VALUES (?, 'bob', 1, 0)", (B,))
    g.poll()
    assert set(g.state.users) == {"bob"} and g.state.beacons[B].owner == "bob"


# ---- review ideas 2026-11-26: custom, echo, together, presence, battery nudge, streak, anniversary ----

def test_custom_mood_carries_its_colour_to_the_show_and_the_phone():
    st = mkstate()
    up(st, B)
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="custom", rgb="#0a14c8", pat=1))
    assert only(pubs, f"hb/v1/d/{B}/down")[0] == {"v": 1, "t": "show", "id": EID, "ts": 1000, "k": "custom",
                                                    "rgb": "#0a14c8", "pat": 1}
    assert only(pubs, "hb/v1/u/bob/down")[0]["rgb"] == "#0a14c8"


def test_shown_echoes_a_soft_pulse_on_the_senders_own_beacon():
    st = mkstate()
    up(st, A)
    up(st, B)
    send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    pubs = send(st, f"hb/v1/d/{B}/up", env("shown", EID), now=1001)
    echo = only(pubs, f"hb/v1/d/{A}/down")
    assert [p["k"] for p in echo] == ["echo"] and echo[0]["id"] != EID


def test_together_within_10s_breathes_both_circles_beacons_in_step():
    st = mkstate()
    up(st, A)
    up(st, B)
    send(st, "hb/v1/u/alice/up", env("mood", "00000000000000b1", k="together"), now=1000)
    pubs = send(st, f"hb/v1/d/{B}/up", env("mood", "00000000000000b2", k="together"), now=1006)
    synced = [p for t, p, *_ in pubs if t in (f"hb/v1/d/{A}/down", f"hb/v1/d/{B}/down") and p.get("at")]
    assert len(synced) == 2 and all(p["k"] == "together" and p["at"] == 1008 for p in synced)
    late = send(st, "hb/v1/u/alice/up", env("mood", "00000000000000b3", k="together"), now=1100)
    assert not any(p.get("at") for _, p, *_ in late)  # alone again: just an invitation


def test_presence_and_battery_nudge_reach_the_other_member_only():
    st = mkstate()
    up(st, B)
    pubs = up(st, A, "ble")  # alice's phone is near her beacon
    pres = only(pubs, "hb/v1/u/bob/down")
    assert [p for p in pres if p["t"] == "presence"] == [
        {"v": 1, "t": "presence", "id": pres[0]["id"], "ts": 1000, "uid": "alice", "near": True}]
    assert any(p["t"] == "presence" for p in only(pubs, f"hb/v1/d/{B}/down"))
    assert not only(pubs, "hb/v1/u/alice/down") or all(p["t"] != "presence" for p in only(pubs, "hb/v1/u/alice/down"))
    low = send(st, f"hb/v1/d/{A}/up", env("tele", "3" * 16, mv=3400, pct=12), now=1100)
    assert [p for p in only(low, "hb/v1/u/bob/down") if p["t"] == "batt"][0]["pct"] == 12
    again = send(st, f"hb/v1/d/{A}/up", env("tele", "4" * 16, mv=3400, pct=11), now=1200)
    assert not any(p["t"] == "batt" for p in only(again, "hb/v1/u/bob/down"))  # once


def test_streak_counts_days_both_sent_and_breaks_on_a_missed_day():
    st = mkstate()
    up(st, A)
    up(st, B)
    day = 86400
    t0 = 20000 * day  # midnight IST is 18:30 UTC; these stay mid-day
    for i in range(3):
        send(st, "hb/v1/u/alice/up", env("mood", f"{i:015x}a", k="heart"), now=t0 + i * day)
        pubs = send(st, "hb/v1/u/bob/up", env("mood", f"{i:015x}b", k="heart"), now=t0 + i * day + 60)
    assert st.streak["home"] == 3
    assert any(p["t"] == "streak" and p["n"] == 3 for p in only(pubs, f"hb/v1/d/{A}/down"))
    tick(st, t0 + 5 * day)  # two days with nothing: broken
    assert st.streak.get("home", 0) == 0


def test_anniversary_plays_celebrate_once_on_its_minute():
    import calendar
    st = mkstate()
    up(st, A)
    up(st, B)
    st.anniv = {"home": "02-14 09:00"}
    at = calendar.timegm((2027, 2, 14, 9, 0, 0)) - 330 * 60  # 09:00 IST
    pubs = tick(st, at + 10)
    assert sorted(t for t, p in pubs if p.get("k") == "celebrate") == [f"hb/v1/d/{A}/down", f"hb/v1/d/{B}/down"]
    assert not [p for _, p in tick(st, at + 20) if p.get("k") == "celebrate"]


def test_phone_mood_glows_the_senders_own_live_beacon_on_any_path():
    """2026-11-26: Bluetooth off on the phone, beacon on WiFi, a tap in the app logged but the beacon stayed dark:
    the app only glowed it over BLE. The router does it on whatever path the beacon is live on, with the mood's id,
    so a beacon that already played it from the phone over BLE dedupes this copy."""
    st = mkstate()
    up(st, A)
    pubs = send(st, "hb/v1/u/alice/up", env("mood", EID, k="heart"))
    assert only(pubs, f"hb/v1/d/{A}/down") == [{"v": 1, "t": "show", "id": EID, "ts": 1000, "k": "heart"}]
    assert all(p.get("to") != A for p in only(pubs, "hb/v1/u/alice/down"))  # a glow, not a delivery: no rcpt
    assert only(send(st, f"hb/v1/d/{A}/up", env("shown", EID), now=1001), "hb/v1/u/alice/down") == []
    button = send(st, f"hb/v1/d/{A}/up", env("mood", "00000000000000ab", k="heart"), now=1002)
    assert only(button, f"hb/v1/d/{A}/down") == []  # a press already glows its own beacon
    near = mkstate()
    up(near, A, path="ble")
    relay = only(send(near, "hb/v1/u/alice/up", env("mood", EID, k="custom", rgb="#0a14c8", pat=1)), "hb/v1/u/alice/down")
    assert [r["m"] for r in relay if r["t"] == "relay"] == [
        {"v": 1, "t": "show", "id": EID, "ts": 1000, "k": "custom", "rgb": "#0a14c8", "pat": 1}]
