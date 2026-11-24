"""Router: the one delivery queue (PROTOCOL.md section 4).

`route(event, state) -> [Pub]` is pure: no clock, no I/O, ids from `state.new_id`. `Glue` is the thin shell:
paho-mqtt in, route(), persist to SQLite, publish out, plus a 1 s tick.

Rules: a `mood` from any member (beacon or phone) goes to every *other* member of the sender's circle, as
`mood` to their phones and `show` to their beacons. Dedupe on (source did or uid, id). A beacon's downlink goes
on its most recently reported live path (wifi: `d/{did}/down`; ble: `relay` to the owner's phone). No `shown`
in 10 s: send once on the other path; still nothing after another 10 s: the beacon counts as unreachable and the
delivery waits for it to show up again. Undelivered after 24 h: expired. The sender's phone gets `rcpt`
queued / shown / expired per target beacon. Same `k` to the same beacon is played once per burst: joins a show
still in flight or started < 3 s ago, and a flush on wake plays each `k` once. A delivery is bound to the
beacon's owner when it was made: if the beacon changes owner, is unclaimed, or owner and sender stop sharing a
circle, it expires instead of reaching the new owner. Every `state` and `tele` accepted from a beacon is
forwarded to its owner's phone as `relay{did, m}` (device status); a `cfg` never goes in a `relay`.

Every field read from a payload is type-checked before it is used as a key: untrusted input never raises.
"""
from __future__ import annotations

import json
import logging
import secrets
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

from . import models

log = logging.getLogger("hb.router")
KINDS = frozenset({"heart", "sorry", "annoy"})
# What a beacon may put in `tele` (anything else, or out of range, is dropped before it is stored or shown).
TELE_INTS = {"mv": (0, 10_000), "pct": (0, 100), "rssi": (-150, 0), "cfg": (0, 2**31 - 1),
             "up_s": (0, 2**32 - 1), "heap": (0, 2**32 - 1), "rst": (0, 2**32 - 1)}
TELE_STRS = frozenset({"fw", "hw", "rst"})  # <= 32 characters
RETRY_S = 10
EXPIRE_S = 24 * 3600
BURST_S = 3
UP_STATES = frozenset({"near", "away"})
SUBS = ("hb/v1/d/+/up", "hb/v1/d/+/state", "hb/v1/u/+/up", "hb/v1/u/+/state")


@dataclass
class User:
    uid: str
    name: str
    circle: str


@dataclass
class Beacon:
    did: str
    owner: str
    tz: int = 0
    wifi: bool = False
    ble: bool = False
    path: str | None = None  # most recently reported live path
    st: str = ""
    wake: int = 0
    last_seen: int = 0
    tele: dict = field(default_factory=dict)
    dirty: bool = False

    def live(self) -> str | None:
        if self.path and getattr(self, self.path):
            return self.path
        return "wifi" if self.wifi else "ble" if self.ble else None


@dataclass
class Event:
    src: str  # did or uid that sent the mood
    eid: str
    uid: str  # sending user (a beacon's owner)
    k: str
    ts: int


@dataclass
class Delivery:
    src: str
    eid: str
    did: str  # target beacon
    k: str
    created: int
    status: str = "pending"  # pending | sent | shown | expired
    path: str | None = None
    sent_at: int = 0  # first send of its show
    tried_at: int = 0  # last send
    tries: int = 0
    show_id: str | None = None
    owner: str = ""  # the target beacon's owner when this was made
    dirty: bool = True


@dataclass
class State:
    users: dict[str, User] = field(default_factory=dict)
    beacons: dict[str, Beacon] = field(default_factory=dict)
    deliveries: list[Delivery] = field(default_factory=list)
    events: dict[tuple[str, str], Event] = field(default_factory=dict)
    new_events: list[Event] = field(default_factory=list)
    new_id: Callable[[], str] = lambda: secrets.token_hex(8)


@dataclass(frozen=True)
class Pub:
    topic: str
    payload: bytes
    retain: bool = False


@dataclass(frozen=True)
class Msg:
    topic: str
    payload: bytes
    now: int


@dataclass(frozen=True)
class Tick:
    now: int


@dataclass(frozen=True)
class Directory:
    """Users and claimed beacons changed (API / admin): `devs` is [(did, owner, tz)]."""
    users: list[User]
    devs: list[tuple[str, str, int]]
    now: int


def route(ev: Msg | Tick | Directory, st: State) -> list[Pub]:
    out: list[Pub] = []
    if isinstance(ev, Tick):
        _tick(st, ev.now, out)
        return out
    if isinstance(ev, Directory):
        _directory(st, ev.users, ev.devs, ev.now, out)
        return out
    parts = ev.topic.split("/")
    if len(parts) != 5 or parts[:2] != ["hb", "v1"] or parts[2] not in ("d", "u") or parts[4] not in ("up", "state"):
        return out
    kind, who, leaf = parts[2], parts[3], parts[4]
    if kind == "u" and leaf == "state":
        _phone_state(st, who, ev.payload, ev.now, out)
        return out
    try:
        env = models.decode(ev.payload)
    except models.ProtocolError:
        return out
    if kind == "d":
        _from_beacon(st, who, env, "wifi", ev.now, out)
    else:
        _from_phone(st, who, env, ev.now, out)
    return out


def _tele(body: dict) -> dict:
    out = {}
    for k, v in body.items():
        if k in TELE_INTS and models._is_int(v) and TELE_INTS[k][0] <= v <= TELE_INTS[k][1]:
            out[k] = v
        elif k in TELE_STRS and isinstance(v, str) and len(v) <= 32:
            out[k] = v
    return out


def _str(body: dict, key: str) -> str | None:
    v = body.get(key)
    return v if isinstance(v, str) else None


# ---- uplinks ----------------------------------------------------------------------

def _from_beacon(st, did, env, path, now, out):
    b = st.beacons.get(did)
    if b is None:  # unknown or unclaimed
        return
    b.last_seen, b.dirty = now, True
    if env.t == "state":
        _beacon_state(st, b, env, path, now, out)
        return
    _links(st, b, now, out, **{path: True}, path=path)  # hearing from it proves the path
    if env.t == "mood":
        if _str(env.body, "k") in KINDS:
            _new_event(st, did, b.owner, env.id, env.body["k"], now, out)
    elif env.t == "shown":
        _shown(st, did, env.id, now, out)
    elif env.t == "tele":
        b.tele = _tele(env.body)
        _status(st, b, env, b.tele, now, out)
    elif env.t not in models.TYPES:
        _send(st, b, path, models.unknown_t_response(env, st.new_id(), now), now, out)


def _beacon_state(st, b, env, path, now, out):
    body = env.body
    s = _str(body, "st")
    if s not in models.STATES and s != models.LWT_STATE:  # exact lowercase names only (PROTOCOL.md 3)
        return
    p = _str(body, "path") if _str(body, "path") in ("wifi", "ble") else path
    b.st = s
    wake = body.get("wake")
    b.wake = wake if models._is_int(wake) and 0 <= wake <= 0xFFFFFFFF else 0  # epoch s, same range as `ts`
    if s == "lost":  # LWT: only the WiFi session died
        _links(st, b, now, out, wifi=False)
    elif s in UP_STATES:
        _links(st, b, now, out, wifi=p == "wifi", ble=p == "ble", path=p)
        if p == "wifi":  # MQTT (re)connect: give it the time
            out.append(Pub(f"hb/v1/d/{b.did}/down", models.encode("time", st.new_id(), now, {"tz": b.tz})))
    else:  # going to deep sleep, offline or pairing
        _links(st, b, now, out, wifi=False, ble=False)
    fwd = {"st": s}
    if _str(body, "path") in ("wifi", "ble"):
        fwd["path"] = body["path"]
    if "wake" in body:
        fwd["wake"] = b.wake
    _status(st, b, env, fwd, now, out)


def _status(st, b, env, body, now, out):
    """Device status for the owner's phone (PROTOCOL.md 4): the beacon's message, keeping its id, with only
    the fields the router accepted."""
    m = {"v": 1, "t": env.t, "id": env.id, "ts": env.ts, **body}
    out.append(Pub(f"hb/v1/u/{b.owner}/down", models.encode("relay", st.new_id(), now, {"did": b.did, "m": m})))


def _from_phone(st, uid, env, now, out):
    if uid not in st.users:
        return
    body = env.body
    if env.t == "mood":
        if _str(body, "k") in KINDS:
            _new_event(st, uid, uid, env.id, body["k"], now, out)
        return
    if env.t in ("relay", "link"):
        b = st.beacons.get(_str(body, "did") or "")
        if b is None or b.owner != uid:  # a phone speaks only for its owner's beacons
            return
        if env.t == "link":
            if type(body.get("ble")) is bool:
                _links(st, b, now, out, ble=body["ble"], path="ble" if body["ble"] else None)
            return
        m = body.get("m")
        if not isinstance(m, dict):
            return
        try:
            inner = models.decode(json.dumps(m, separators=(",", ":"), ensure_ascii=False).encode("utf-8"))
        except models.ProtocolError:
            return
        _from_beacon(st, b.did, inner, "ble", now, out)
        return
    if env.t not in models.TYPES:
        out.append(Pub(f"hb/v1/u/{uid}/down", models.unknown_t_response(env, st.new_id(), now)))


def _phone_state(st, uid, payload, now, out):
    """`u/{uid}/state` is `{"on":bool}` (LWT false). A phone without its server link is no BLE path."""
    obj = models._load_object(payload) if len(payload) <= models.MAX_MSG else None
    if obj is not None and obj.get("on") is False:
        for b in st.beacons.values():
            if b.owner == uid and b.ble:
                _links(st, b, now, out, ble=False)


def _links(st, b, now, out, wifi=None, ble=None, path=None):
    """Update a beacon's path flags; flush its queue when it goes from unreachable to reachable."""
    was = b.live()
    if wifi is not None:
        b.wifi = wifi
    if ble is not None:
        b.ble = ble
    if path:
        b.path = path
    b.dirty = True
    if was is None and b.live():
        _flush(st, b, now, out)


# ---- delivery ---------------------------------------------------------------------------

def _new_event(st, src, uid, eid, k, now, out):
    sender = st.users.get(uid)
    if sender is None or (src, eid) in st.events:
        return
    e = Event(src, eid, uid, k, now)
    st.events[(src, eid)] = e
    st.new_events.append(e)
    others = {u.uid for u in st.users.values() if u.circle == sender.circle and u.uid != uid}
    for o in sorted(others):
        out.append(Pub(f"hb/v1/u/{o}/down", models.encode("mood", eid, now, {"k": k, "from": sender.name})))
    for b in sorted(st.beacons.values(), key=lambda b: b.did):
        if b.owner in others:
            d = Delivery(src, eid, b.did, k, now, owner=b.owner)
            st.deliveries.append(d)
            _rcpt(st, d, "queued", now, out)
            _deliver(st, b, d, now, out)


def _deliver(st, b, d, now, out):
    for x in st.deliveries:
        if x is not d and x.did == d.did and x.k == d.k and x.show_id and (
                x.status == "sent" or (x.status == "shown" and now - x.sent_at < BURST_S)):
            d.show_id, d.status, d.path = x.show_id, x.status, x.path
            d.sent_at, d.tried_at, d.tries = x.sent_at, x.tried_at, x.tries
            if d.status == "shown":
                _rcpt(st, d, "shown", now, out)
            return
    path = b.live()
    if path:
        _show(st, b, [d], path, now, out)
    # else: stays pending until the beacon shows up


def _show(st, b, group, path, now, out):
    sid = group[0].eid  # earliest event: a re-flushed group keeps its id, so the beacon's ring dedupes it
    for d in group:
        d.show_id, d.status, d.path = sid, "sent", path
        d.sent_at = d.tried_at = now
        d.tries, d.dirty = 1, True
    _send(st, b, path, models.encode("show", sid, now, {"k": group[0].k}), now, out)


def _flush(st, b, now, out):
    by_k: dict[str, list[Delivery]] = {}
    for d in sorted(st.deliveries, key=lambda d: d.created):
        if d.did == b.did and d.status in ("pending", "sent"):
            by_k.setdefault(d.k, []).append(d)
    for group in by_k.values():
        _show(st, b, group, b.live(), now, out)


def _shown(st, did, sid, now, out):
    for d in st.deliveries:
        if d.did == did and d.show_id == sid and d.status in ("sent", "pending"):
            d.status, d.dirty = "shown", True
            _rcpt(st, d, "shown", now, out)


def _send(st, b, path, raw, now, out):
    if path == "wifi":
        out.append(Pub(f"hb/v1/d/{b.did}/down", raw))
    else:
        m = json.loads(raw)
        if m["t"] == "cfg":  # the phone gets the signed cfg from the API, never in a relay (PROTOCOL.md 7)
            return
        out.append(Pub(f"hb/v1/u/{b.owner}/down", models.encode("relay", st.new_id(), now, {"did": b.did, "m": m})))


def _rcpt(st, d, status, now, out):
    sender = st.events.get((d.src, d.eid))
    if sender:
        out.append(Pub(f"hb/v1/u/{sender.uid}/down", models.encode("rcpt", d.eid, now, {"to": d.did, "st": status})))


def _tick(st, now, out):
    for d in st.deliveries:
        if d.status in ("pending", "sent") and now - d.created >= EXPIRE_S:
            d.status, d.dirty = "expired", True
            _rcpt(st, d, "expired", now, out)
    done = set()
    for d in st.deliveries:
        key = (d.did, d.show_id)
        if d.status != "sent" or key in done or now - d.tried_at < RETRY_S:
            continue
        done.add(key)
        b = st.beacons.get(d.did)
        if b is None:
            continue
        group = [x for x in st.deliveries if (x.did, x.show_id) == key and x.status == "sent"]
        if d.tries >= 2:  # both paths tried: unreachable until it reports in again
            for x in group:
                x.status, x.dirty = "pending", True
            b.wifi = b.ble = False
            b.dirty = True
            continue
        other = "ble" if d.path == "wifi" else "wifi"
        for x in group:
            x.path, x.tried_at, x.tries, x.dirty = other, now, x.tries + 1, True
        _send(st, b, other, models.encode("show", d.show_id, now, {"k": d.k}), now, out)
    # ponytail: linear scans every second; fine for a handful of beacons, index by did if that grows
    st.deliveries = [d for d in st.deliveries
                     if d.dirty or d.status in ("pending", "sent") or now - d.created < EXPIRE_S]
    st.events = {k: e for k, e in st.events.items() if now - e.ts < EXPIRE_S}


# ---- persistence + MQTT glue ------------------------------------------------------------------

def load_directory(db) -> tuple[list[User], list[tuple[str, str, int]]]:
    users = [User(r["uid"], r["name"], r["circle"]) for r in db.q("SELECT * FROM user WHERE disabled = 0")]
    devs = []
    for r in db.q("SELECT did, owner, cfg FROM device WHERE claimed = 1 AND owner IS NOT NULL"):
        tz = json.loads(r["cfg"])["sched"]["tz"] if r["cfg"] else 0
        devs.append((r["did"], r["owner"], tz))
    return users, devs


def _directory(st, users, devs, now, out):
    st.users = {u.uid: u for u in users}
    beacons = {}
    for did, owner, tz in devs:
        b = st.beacons.get(did)
        if b is None or b.owner != owner:  # new owner: forget the old owner's links too
            b = Beacon(did, owner, dirty=True)
        b.tz = tz
        beacons[did] = b
    st.beacons = beacons
    for d in st.deliveries:
        if d.status in ("pending", "sent") and not _still_valid(st, d):
            d.status, d.dirty = "expired", True
            _rcpt(st, d, "expired", now, out)


def _still_valid(st, d) -> bool:
    """Same owner as when it was made, and owner and sender still share a circle."""
    b, e = st.beacons.get(d.did), st.events.get((d.src, d.eid))
    owner, sender = st.users.get(d.owner), e and st.users.get(e.uid)
    return bool(b and b.owner == d.owner and owner and sender and owner.circle == sender.circle)


_STATUS = ("wifi", "ble", "path", "st", "wake", "last_seen", "tele")


def load_state(db, now: int) -> State:
    st = State()
    for r in db.q("SELECT * FROM event WHERE ts > ?", (now - EXPIRE_S,)):
        st.events[(r["src"], r["id"])] = Event(r["src"], r["id"], r["uid"], r["k"], r["ts"])
    for r in db.q("SELECT * FROM delivery WHERE created > ? OR status IN ('pending', 'sent')", (now - EXPIRE_S,)):
        st.deliveries.append(Delivery(r["src"], r["eid"], r["did"], r["k"], r["created"], r["status"], r["path"],
                                      r["sent_at"], r["tried_at"], r["tries"], r["show_id"], r["owner"], False))
    _directory(st, *load_directory(db), now, [])  # receipts for anything stale are lost across a restart
    for r in db.q("SELECT did, status FROM device WHERE claimed = 1"):
        b = st.beacons.get(r["did"])
        if b:
            for k, v in json.loads(r["status"]).items():
                if k in _STATUS:
                    setattr(b, k, v)
    return st


def save_state(db, st: State) -> None:
    with db.tx() as c:
        for e in st.new_events:
            c.execute("INSERT OR IGNORE INTO event VALUES (?, ?, ?, ?, ?)", (e.src, e.eid, e.uid, e.k, e.ts))
        for d in st.deliveries:
            if d.dirty:
                c.execute("INSERT OR REPLACE INTO delivery VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                          (d.src, d.eid, d.did, d.k, d.created, d.status, d.path, d.sent_at, d.tried_at, d.tries,
                           d.show_id, d.owner))
        for b in st.beacons.values():
            if b.dirty:
                c.execute("UPDATE device SET status = ? WHERE did = ?",
                          (json.dumps({k: getattr(b, k) for k in _STATUS}), b.did))
    st.new_events.clear()
    for d in st.deliveries:
        d.dirty = False
    for b in st.beacons.values():
        b.dirty = False


class Glue:
    """paho-mqtt <-> route(). One lock around route + persist; publishes go out after the lock."""

    def __init__(self, db, host: str, port: int, password: str, clock=time.time, loader=load_directory):
        import paho.mqtt.client as mqtt

        self.db, self.host, self.port, self.clock, self.loader = db, host, port, clock, loader
        self.lock = threading.RLock()
        self.state = load_state(db, int(clock()))
        self.seen = self._data_version()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="router", protocol=mqtt.MQTTv5)
        self.client.username_pw_set("router", password)
        self.client.on_connect = self._on_connect
        self.client.on_message = lambda c, u, m: self.handle(Msg(m.topic, m.payload, int(self.clock())))

    def start(self) -> None:
        self.client.connect_async(self.host, self.port, keepalive=60)
        self.client.loop_start()
        threading.Thread(target=self._ticker, daemon=True, name="router-tick").start()

    def _ticker(self) -> None:
        while True:
            time.sleep(1)
            self.poll()

    def _data_version(self) -> int:
        return self.db.one("PRAGMA data_version")[0]  # changes when another connection commits

    def poll(self) -> None:
        """One tick. First picks up users and claims written by another process (the admin CLI)."""
        v = self._data_version()
        if v != self.seen:
            self.seen = v
            self.refresh()
        self.handle(Tick(int(self.clock())))

    def _on_connect(self, client, userdata, flags, reason, props) -> None:
        if reason.is_failure:
            return
        client.subscribe([(t, 1) for t in SUBS])
        # retained configs survive in the broker's persistence; republishing covers a lost broker DB
        rows = self.db.q("SELECT did, cfg_env FROM device WHERE claimed = 1 AND cfg_env IS NOT NULL")
        self.publish([Pub(f"hb/v1/d/{r['did']}/cfg", r["cfg_env"].encode(), retain=True) for r in rows])

    def handle(self, ev: Msg | Tick | Directory) -> list[Pub]:
        """Never raises: an exception here would kill paho's network thread or the ticker."""
        try:
            with self.lock:
                pubs = route(ev, self.state)
                save_state(self.db, self.state)
        except Exception:
            log.exception("router dropped %s", getattr(ev, "topic", type(ev).__name__))
            return []
        self.publish(pubs)
        return pubs

    def publish(self, pubs: list[Pub]) -> None:
        for p in pubs:
            self.client.publish(p.topic, p.payload, qos=1, retain=p.retain)

    def refresh(self) -> None:
        """Pick up users and claims changed by the API or admin. Load and apply under one lock, or an older
        snapshot from a concurrent refresh could land after a newer one."""
        with self.lock:
            self.handle(Directory(*self.loader(self.db), int(self.clock())))
