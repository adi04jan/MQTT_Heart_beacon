"""Heart Beacon wire models: envelope, BLE frames, config v1. Normative spec: protocol/PROTOCOL.md."""
from __future__ import annotations

import copy
import json
import re
from dataclasses import dataclass, field

MAX_MSG = 1024
TYPES = frozenset({
    "mood", "show", "shown", "rcpt", "tele", "state", "cfg", "time", "sleep", "get", "resp",
    "claim", "wifi.scan", "wifi.list", "wifi.set", "wifi.forget", "wifi.use", "link", "relay", "reset",
})
_HEADER = ("v", "t", "id", "ts")
_ID = re.compile(r"[0-9a-f]{16}")


class ProtocolError(ValueError):
    """`code` is the PROTOCOL.md error name (envelope) or failing key path (config)."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


def _is_int(x) -> bool:
    return type(x) is int  # bool is an int subclass and is not accepted


def _reject_constant(name):
    raise ValueError(name)  # NaN / Infinity are not JSON


def _load_object(raw: bytes):
    """Strict RFC 8259 object or None."""
    try:
        obj = json.loads(raw.decode("utf-8"), parse_constant=_reject_constant)
    except (ValueError, RecursionError):  # JSONDecodeError, UnicodeDecodeError; deep nesting on Python 3.11
        return None
    return obj if isinstance(obj, dict) else None


# ---- envelope ---------------------------------------------------------------

@dataclass(frozen=True)
class Envelope:
    t: str
    id: str
    ts: int
    body: dict = field(default_factory=dict)


def encode(t: str, id: str, ts: int, body: dict | None = None) -> bytes:
    obj = {"v": 1, "t": t, "id": id, "ts": ts, **(body or {})}
    raw = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(raw) > MAX_MSG:
        raise ProtocolError("too_big")
    return raw


def decode(raw: bytes) -> Envelope:
    if len(raw) > MAX_MSG:
        raise ProtocolError("too_big")
    obj = _load_object(raw)
    if obj is None:
        raise ProtocolError("bad_json")
    if obj.get("v") != 1 or not _is_int(obj.get("v")):
        raise ProtocolError("bad_v")
    t = obj.get("t")
    if not isinstance(t, str) or not t:
        raise ProtocolError("bad_t")
    id_ = obj.get("id")
    if not isinstance(id_, str) or not _ID.fullmatch(id_):
        raise ProtocolError("bad_id")
    ts = obj.get("ts")
    if not _is_int(ts) or not 0 <= ts <= 0xFFFFFFFF:
        raise ProtocolError("bad_ts")
    return Envelope(t, id_, ts, {k: v for k, v in obj.items() if k not in _HEADER})


def unknown_t_response(req: Envelope, resp_id: str, now: int) -> bytes | None:
    """The `resp ok:false` a receiver owes for an unknown `t`, or None for a known one."""
    if req.t in TYPES:
        return None
    return encode("resp", resp_id, now, {"re": req.id, "ok": False, "err": "unknown_t"})


def did_from_mac(mac: bytes) -> str:
    return "HB-" + mac[3:6].hex().upper()


# ---- BLE frames ---------------------------------------------------------------

MIN_MTU = 23
TIMEOUT_MS = 2000
MORE = 0x80


def split_frames(msg: bytes, seq: int, mtu: int) -> list[bytes]:
    if mtu < MIN_MTU or not 1 <= len(msg) <= MAX_MSG or not 0 <= seq <= 127:
        raise ProtocolError("bad_arg")
    n = mtu - 4
    chunks = [msg[i:i + n] for i in range(0, len(msg), n)]
    return [bytes([seq | (MORE if i < len(chunks) - 1 else 0)]) + c for i, c in enumerate(chunks)]


class Reassembler:
    """PROTOCOL.md 6.1 receiver. push() returns (outcome, message or None)."""

    IDLE, COLLECT, SKIP = range(3)

    def __init__(self):
        self.reset()

    def reset(self):
        self.state, self.seq, self.last, self.buf = self.IDLE, 0, 0, bytearray()

    def push(self, frame: bytes, now_ms: int) -> tuple[str, bytes | None]:
        if not frame:
            return "empty", None
        seq, more, payload = frame[0] & 0x7F, bool(frame[0] & MORE), frame[1:]
        if self.state != self.IDLE and (now_ms - self.last) & 0xFFFFFFFF > TIMEOUT_MS:
            self.state = self.SKIP if seq == self.seq else self.IDLE
        if self.state != self.IDLE and seq != self.seq:
            self.state = self.IDLE
        if self.state == self.SKIP:
            self.last = now_ms
            if not more:
                self.state = self.IDLE
            return "skip", None
        if self.state == self.IDLE:
            self.state, self.seq, self.buf = self.COLLECT, seq, bytearray()
        self.last = now_ms
        if len(self.buf) + len(payload) > MAX_MSG:
            self.state, self.buf = (self.SKIP if more else self.IDLE), bytearray()
            return "oversize", None
        self.buf += payload
        if more:
            return "partial", None
        self.state = self.IDLE
        return "complete", bytes(self.buf)


# ---- config v1 ------------------------------------------------------------------

def _int(lo, hi):
    return lambda v: _is_int(v) and lo <= v <= hi


def _text(lo, hi):
    return lambda v: isinstance(v, str) and lo <= len(v.encode("utf-8")) <= hi


def _list(ok, lo, hi):
    return lambda v: isinstance(v, list) and lo <= len(v) <= hi and all(ok(x) for x in v)


def _is_bool(v):
    return type(v) is bool


_HHMM = re.compile(r"([01][0-9]|2[0-3]):[0-5][0-9]")
_DAYS = re.compile(r"[01]{7}")
_SHA = re.compile(r"[0-9a-f]{64}")
_DID = re.compile(r"HB-[0-9A-F]{6}")
_mv = _int(3000, 4200)
_port = _int(1, 65535)

# block -> key -> (default, validator). Order is the error-reporting order.
_SCHEMA = {
    "srv": {"hosts": ([], _list(_text(1, 64), 0, 4)), "mqtt": ([8883], _list(_port, 1, 4)), "api": (8443, _port)},
    "name": ("", _text(0, 32)),
    "led": {"bright": (60, _int(0, 100))},
    "sched": {"on": (False, _is_bool), "tz": (0, _int(-720, 840)),
              "days": ("1111100", lambda v: isinstance(v, str) and bool(_DAYS.fullmatch(v))),
              "start": ("09:00", lambda v: isinstance(v, str) and bool(_HHMM.fullmatch(v))),
              "stop": ("19:00", lambda v: isinstance(v, str) and bool(_HHMM.fullmatch(v)))},
    "pwr": {"idle_nap_min": (90, _int(0, 1440)), "nap_wake_min": (15, _int(1, 1440)),
            "batt_low_mv": (3450, _mv), "batt_off_mv": (3300, _mv), "batt_cal_ppm": (0, _int(-200000, 200000))},
    "ble": {"adv_fast_ms": (100, _int(20, 10240)), "adv_fast_s": (30, _int(0, 600)),
            "adv_slow_ms": (500, _int(20, 10240)), "conn_ms": (500, _int(8, 4000)),
            "latency": (3, _int(0, 499)), "sup_ms": (6000, _int(100, 32000))},
    "net": {"use_wifi": (True, _is_bool), "near_hold_s": (30, _int(0, 3600)), "away_grace_s": (60, _int(0, 3600)),
            "tele_min": (30, _int(1, 1440)), "wifi": ([], None)},  # wifi checked in _wifi()
    "fw": {"ver": ("", _text(0, 16)), "path": ("", _text(0, 96)),
           "sha256": ("", lambda v: v == "" or (isinstance(v, str) and bool(_SHA.fullmatch(v)))),
           "min_mv": (3700, _mv)},
}

CONFIG_DEFAULTS = {k: ({kk: d for kk, (d, _) in s.items()} if isinstance(s, dict) else s[0])
                   for k, s in _SCHEMA.items()}


def _wifi(v):
    """Normalised network list, or None if invalid."""
    if not isinstance(v, list) or len(v) > 4:
        return None
    out = []
    for net in v:
        if not isinstance(net, dict):
            return None
        ssid, psk = net.get("ssid"), net.get("psk", "")
        if not _text(1, 32)(ssid) or not (psk == "" or _text(8, 63)(psk)):
            return None
        out.append({"ssid": ssid, "psk": psk})
    return out


def parse_config(raw: bytes) -> dict:
    """Validated config v1 with defaults filled in and unknown keys dropped."""
    if len(raw) > MAX_MSG:
        raise ProtocolError("size")
    obj = _load_object(raw)
    if obj is None:
        raise ProtocolError("json")
    if obj.get("cv") != 1 or not _is_int(obj.get("cv")):
        raise ProtocolError("cv")
    if not _int(1, 2**31 - 1)(obj.get("n")):
        raise ProtocolError("n")
    if not isinstance(obj.get("did"), str) or not _DID.fullmatch(obj["did"]):
        raise ProtocolError("did")
    cfg = {"cv": 1, "n": obj["n"], "did": obj["did"]}
    for name, spec in _SCHEMA.items():
        if not isinstance(spec, dict):  # top-level leaf
            default, ok = spec
            v = obj.get(name, default)
            if not ok(v):
                raise ProtocolError(name)
            cfg[name] = v
            continue
        given = obj.get(name, {})
        if not isinstance(given, dict):
            raise ProtocolError(name)
        block = {}
        for key, (default, ok) in spec.items():
            v = given.get(key, copy.deepcopy(default))
            if ok is None:
                v = _wifi(v)
                if v is None:
                    raise ProtocolError(f"{name}.{key}")
            elif not ok(v):
                raise ProtocolError(f"{name}.{key}")
            block[key] = v
        if name == "pwr" and block["batt_off_mv"] >= block["batt_low_mv"]:
            raise ProtocolError("pwr.batt_off_mv")
        if name == "ble" and block["sup_ms"] <= 2 * (1 + block["latency"]) * block["conn_ms"]:
            raise ProtocolError("ble.sup_ms")
        cfg[name] = block
    return cfg
