"""Router rules from the plan: fan-out per circle, dedupe, path choice, retry on the other path, 24 h expiry,
receipts, burst coalescing, flush on wake. Pure: time and ids are injected."""
import itertools
import json

from hb_server import models
from hb_server.db import Db
from hb_server.router import Beacon, Msg, State, Tick, User, load_state, route, save_state

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
    raw = env("state", "f" * 16, st="AWAY" if path == "wifi" else "NEAR", path=path)
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
    assert tick(st, 1020) == []  # both paths tried: pending, beacon unreachable
    assert st.deliveries[0].status == "pending" and st.beacons[B].live() is None
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
    send(st, f"hb/v1/d/{B}/state", env("state", "3" * 16, st="SLEEP", path="wifi", wake=5000), now=900)
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
