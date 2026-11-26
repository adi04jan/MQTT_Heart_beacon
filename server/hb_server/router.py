"""Router: the one delivery queue (PROTOCOL.md section 4).

`route(event, state) -> [Pub]` is pure: no clock, no I/O, ids from `state.new_id`. `Glue` is the thin shell:
paho-mqtt in, route(), persist to SQLite, publish out, plus a 1 s tick.

Rules: a `mood` from any member (beacon or phone) goes to every *other* member of the sender's circle, as
`mood` to their phones and `show` to their beacons; a phone's mood also glows the sender's own live beacons
(`show` with the mood's id, untracked). Dedupe on (source did or uid, id). A beacon's downlink goes
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
KINDS = frozenset({"heart", "sorry", "annoy", "custom", "together"})
TOGETHER_S = 10  # two members' `together` this close: both circles' beacons breathe in step
LOW_PCT, LOW_RESET_PCT = 15, 30  # battery nudge to the other members once at <= 15 %, again after > 30 %
DAY_TZ_MIN = 330  # ponytail: streak and anniversary days run in IST; a per-circle zone if circles span zones
STREAK_DAYS = 400  # history kept for the streak
# What a beacon may put in `tele` (anything else, or out of range, is dropped before it is stored or shown).
TELE_INTS = {"mv": (0, 10_000), "pct": (0, 100), "rssi": (-150, 0), "cfg": (0, 2**31 - 1),
             "up_s": (0, 2**32 - 1), "heap": (0, 2**32 - 1), "rst": (0, 2**32 - 1)}
TELE_STRS = frozenset({"fw", "hw", "rst"})  # <= 32 characters
RETRY_S = 10
PING_S = 120  # an up beacon that answered neither path is asked `get info` this often while hearts wait
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
    ping_at: int = 0  # last `get info` sent while it was unreachable with hearts waiting (not persisted)
    low_told: bool = False  # the other members were told its battery is low (not persisted: may repeat once)
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
    together: dict[str, int] = field(default_factory=dict)  # uid -> when they last sent `together`
    days: dict[str, set[int]] = field(default_factory=dict)  # uid -> local day numbers they sent a mood on
    streak: dict[str, int] = field(default_factory=dict)  # circle -> days in a row everyone sent one
    day: int = 0  # the local day the streaks were last computed for
    anniv: dict[str, str] = field(default_factory=dict)  # circle -> "MM-DD HH:MM" (setting table)
    fired: dict[str, int] = field(default_factory=dict)  # circle -> year its anniversary last played
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
        k = _kind(env.body)
        if k:
            _new_event(st, did, b.owner, env.id, k, now, out)
    elif env.t == "shown":
        _shown(st, did, env.id, now, out)
    elif env.t == "tele":
        b.tele = _tele(env.body)
        _status(st, b, env, b.tele, now, out)
        pct, owner = b.tele.get("pct"), st.users.get(b.owner)
        if isinstance(pct, int) and owner:  # I10: tell the others once when it runs low
            if pct <= LOW_PCT and not b.low_told:
                b.low_told = True
                _circle_down(st, owner.circle, "batt", {"did": b.did, "name": owner.name, "pct": pct}, now, out,
                             beacons=False, skip_uid=owner.uid)
            elif pct > LOW_RESET_PCT:
                b.low_told = False
    elif env.t not in models.TYPES:
        _send(st, b, path, models.unknown_t_response(env, st.new_id(), now), now, out)


def _beacon_state(st, b, env, path, now, out):
    body = env.body
    s = _str(body, "st")
    if s not in models.STATES and s != models.LWT_STATE:  # exact lowercase names only (PROTOCOL.md 3)
        return
    p = _str(body, "path") if _str(body, "path") in ("wifi", "ble") else path
    was_near = b.st == "near"
    b.st = s
    wake = body.get("wake")
    b.wake = wake if models._is_int(wake) and 0 <= wake <= 0xFFFFFFFF else 0  # epoch s, same range as `ts`
    if s == "lost":  # LWT: only the WiFi session died
        _links(st, b, now, out, wifi=False)
    elif s in UP_STATES:
        _links(st, b, now, out, wifi=p == "wifi", ble=p == "ble", path=p)
        owner = st.users.get(b.owner)
        if owner and st.streak.get(owner.circle):  # its ember, on every connect
            _send(st, b, p, models.encode("streak", st.new_id(), now, {"n": st.streak[owner.circle]}), now, out)
        if p == "wifi":  # MQTT (re)connect: give it the time
            out.append(Pub(f"hb/v1/d/{b.did}/down", models.encode("time", st.new_id(), now, {"tz": b.tz})))
    else:  # going to deep sleep, offline or pairing
        _links(st, b, now, out, wifi=False, ble=False)
    owner = st.users.get(b.owner)
    if owner and was_near != (s == "near"):  # I7: the others' beacons and phones
        _circle_down(st, owner.circle, "presence", {"uid": owner.uid, "near": s == "near"}, now, out,
                     skip_uid=owner.uid)
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
        k = _kind(body)
        if k:
            _new_event(st, uid, uid, env.id, k, now, out)
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

def _kind(body) -> str | None:
    """A mood's `k` as the router keeps it: a custom one carries its colour and pattern (PROTOCOL.md 3)."""
    k = _str(body, "k")
    if k == "custom":
        rgb, pat = body.get("rgb"), body.get("pat")
        ok = isinstance(rgb, str) and models._RGB.fullmatch(rgb) and type(pat) is int and pat in (0, 1)
        return f"custom:{rgb}:{pat}" if ok else "custom:#ff4fa3:0"
    return k if k in KINDS else None


def _kbody(k: str) -> dict:
    if k.startswith("custom:"):
        _, rgb, pat = k.split(":")
        return {"k": "custom", "rgb": rgb, "pat": int(pat)}
    return {"k": k}


def _day(now: int) -> int:
    return (now + DAY_TZ_MIN * 60) // 86400


def _circle_down(st, circle, t, body, now, out, beacons=True, phones=True, skip_uid=None):
    """One message to every member's phone and live beacon in `circle` (fresh ids), but `skip_uid`'s."""
    members = {u.uid for u in st.users.values() if u.circle == circle and u.uid != skip_uid}
    if phones:
        for uid in sorted(members):
            out.append(Pub(f"hb/v1/u/{uid}/down", models.encode(t, st.new_id(), now, body)))
    if beacons:
        for b in sorted(st.beacons.values(), key=lambda b: b.did):
            if b.owner in members and b.live():
                _send(st, b, b.live(), models.encode(t, st.new_id(), now, body), now, out)


def _streak(st, circle, today) -> int:
    members = [u.uid for u in st.users.values() if u.circle == circle]
    if len(members) < 2:
        return 0

    def full(d):
        return all(d in st.days.get(m, ()) for m in members)

    d, n = (today if full(today) else today - 1), 0
    while full(d):
        n, d = n + 1, d - 1
    return n


def _streaks(st, now, out, circles=None):
    """Recompute the circles' streaks; tell everyone in a circle whose streak changed (I5)."""
    st.day = _day(now)
    for c in circles or {u.circle for u in st.users.values()}:
        n = _streak(st, c, st.day)
        if n != st.streak.get(c, 0):
            st.streak[c] = n
            _circle_down(st, c, "streak", {"n": n}, now, out)


def _new_event(st, src, uid, eid, k, now, out):
    sender = st.users.get(uid)
    if sender is None or (src, eid) in st.events:
        return
    e = Event(src, eid, uid, k, now)
    st.events[(src, eid)] = e
    st.new_events.append(e)
    others = {u.uid for u in st.users.values() if u.circle == sender.circle and u.uid != uid}
    for o in sorted(others):
        out.append(Pub(f"hb/v1/u/{o}/down", models.encode("mood", eid, now, {**_kbody(k), "from": sender.name})))
    for b in sorted(st.beacons.values(), key=lambda b: b.did):
        if b.owner in others:
            d = Delivery(src, eid, b.did, k, now, owner=b.owner)
            st.deliveries.append(d)
            _rcpt(st, d, "queued", now, out)
            _deliver(st, b, d, now, out)
    if src == uid:  # from a phone: the sender's own live beacons glow too, on whatever path (a press glows itself).
        # The mood's id, so a beacon that already played the phone's BLE copy dedupes this one; a glow, not a
        # delivery (no rcpt, no retry; its `shown` matches nothing).
        for b in sorted(st.beacons.values(), key=lambda b: b.did):
            if b.owner == uid and b.live():
                _send(st, b, b.live(), models.encode("show", eid, now, _kbody(k)), now, out)
    if k == "together":  # I3: a second member within 10 s: every beacon of both breathes in step
        other = next((u for u, t in st.together.items() if u != uid and now - t <= TOGETHER_S
                      and st.users.get(u) and st.users[u].circle == sender.circle), None)
        if other:
            st.together.pop(other, None)
            for b in sorted(st.beacons.values(), key=lambda b: b.did):
                if b.owner in (uid, other) and b.live():
                    _send(st, b, b.live(), models.encode("show", st.new_id(), now, {"k": "together", "at": now + 2}),
                          now, out)
        else:
            st.together[uid] = now
    st.days.setdefault(uid, set()).add(_day(now))
    _streaks(st, now, out, {sender.circle})


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
    _send(st, b, path, models.encode("show", sid, now, _kbody(group[0].k)), now, out)


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
            ev = st.events.get((d.src, d.eid))
            if ev and ev.k != "together":  # I2: the sender's own cage gives one soft pulse
                for ob in sorted(st.beacons.values(), key=lambda b: b.did):
                    if ob.owner == ev.uid and ob.live():
                        _send(st, ob, ob.live(), models.encode("show", st.new_id(), now, {"k": "echo"}), now, out)


def _send(st, b, path, raw, now, out):
    if path == "wifi":
        out.append(Pub(f"hb/v1/d/{b.did}/down", raw))
    else:
        m = json.loads(raw)
        if m["t"] == "cfg":  # the phone gets the signed cfg from the API, never in a relay (PROTOCOL.md 7)
            return
        out.append(Pub(f"hb/v1/u/{b.owner}/down", models.encode("relay", st.new_id(), now, {"did": b.did, "m": m})))


def _ping(st, b, now, out):
    """`get info` on both paths: the beacon's `resp` comes back on whichever works (PROTOCOL.md 3)."""
    b.ping_at = now
    raw = models.encode("get", st.new_id(), now, {"what": "info"})
    _send(st, b, "wifi", raw, now, out)
    _send(st, b, "ble", raw, now, out)


def _rcpt(st, d, status, now, out):
    sender = st.events.get((d.src, d.eid))
    if sender:
        out.append(Pub(f"hb/v1/u/{sender.uid}/down", models.encode("rcpt", d.eid, now, {"to": d.did, "st": status})))


def _tick(st, now, out):
    if st.users and _day(now) != st.day:  # a new day can break a streak
        _streaks(st, now, out)
    if st.anniv:  # I9: once a year, on the minute
        t = time.gmtime(now + DAY_TZ_MIN * 60)
        stamp = f"{t.tm_mon:02d}-{t.tm_mday:02d} {t.tm_hour:02d}:{t.tm_min:02d}"
        for c, when in st.anniv.items():
            if when == stamp and st.fired.get(c) != t.tm_year:
                st.fired[c] = t.tm_year
                _circle_down(st, c, "show", {"k": "celebrate"}, now, out, phones=False)
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
            _ping(st, b, now, out)
            continue
        other = "ble" if d.path == "wifi" else "wifi"
        for x in group:
            x.path, x.tried_at, x.tries, x.dirty = other, now, x.tries + 1, True
        _send(st, b, other, models.encode("show", d.show_id, now, {"k": d.k}), now, out)
    # An up beacon that went quiet with hearts waiting: ask it again every PING_S instead of waiting for its next
    # `tele` (up to net.tele_min). Any answer is an uplink, and _links flushes the queue (review S1).
    for b in st.beacons.values():
        if (b.live() is None and b.st in UP_STATES and now - b.ping_at >= PING_S
                and any(d.did == b.did and d.status == "pending" for d in st.deliveries)):
            _ping(st, b, now, out)
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


def load_anniv(db) -> dict[str, str]:
    """circle -> "MM-DD HH:MM" from the setting table (admin page)."""
    return {r["key"][len("anniv:"):]: r["value"] for r in db.q("SELECT key, value FROM setting WHERE key LIKE 'anniv:%'")}


def load_state(db, now: int) -> State:
    st = State()
    for r in db.q("SELECT * FROM event WHERE ts > ?", (now - EXPIRE_S,)):
        st.events[(r["src"], r["id"])] = Event(r["src"], r["id"], r["uid"], r["k"], r["ts"])
    for r in db.q("SELECT * FROM delivery WHERE created > ? OR status IN ('pending', 'sent')", (now - EXPIRE_S,)):
        st.deliveries.append(Delivery(r["src"], r["eid"], r["did"], r["k"], r["created"], r["status"], r["path"],
                                      r["sent_at"], r["tried_at"], r["tries"], r["show_id"], r["owner"], False))
    _directory(st, *load_directory(db), now, [])  # receipts for anything stale are lost across a restart
    for r in db.q("SELECT uid, ts FROM event WHERE ts > ?", (now - STREAK_DAYS * 86400,)):
        st.days.setdefault(r["uid"], set()).add(_day(r["ts"]))
    st.anniv = load_anniv(db)
    _streaks(st, now, [])
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
            self.state.anniv = load_anniv(self.db)  # the admin's anniversary dates (I9)
