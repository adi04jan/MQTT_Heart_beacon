"""Passwords: API logins (scrypt), login/claim attempt limits, the Mosquitto 1.6 password file."""
from __future__ import annotations

import base64
import hashlib
import hmac
import ipaddress
import logging
import math
import os
import secrets
import signal
import subprocess
import threading
import time
from pathlib import Path

_SCRYPT = dict(n=2**14, r=8, p=1)


def new_secret() -> str:
    """32 random bytes, base64url: MQTT passwords and session tokens."""
    return secrets.token_urlsafe(32)


def hash_password(pw: str) -> str:
    salt = os.urandom(16)
    h = hashlib.scrypt(pw.encode(), salt=salt, **_SCRYPT)
    return f"scrypt${salt.hex()}${h.hex()}"


def check_password(pw: str, stored: str) -> bool:
    try:
        kind, salt, h = stored.split("$")
    except ValueError:
        return False
    if kind != "scrypt":
        return False
    got = hashlib.scrypt(pw.encode(), salt=bytes.fromhex(salt), **_SCRYPT)
    return hmac.compare_digest(got.hex(), h)


# ---- Mosquitto 1.6 ------------------------------------------------------------
# mosquitto_passwd 1.6.x writes `user:$6$<b64 salt>$<b64 sha512(password || salt)>` with a 12-byte salt and
# standard base64 (src/mosquitto_passwd.c output_new_password, PW_SHA512 = 6); the broker recomputes the same
# digest (src/security_default.c pw__digest). 1.6 has no $7$ (PBKDF2), that arrived in 2.0.

def mosq_hash(pw: str, salt: bytes | None = None) -> str:
    salt = os.urandom(12) if salt is None else salt
    digest = hashlib.sha512(pw.encode() + salt).digest()
    return f"$6${base64.b64encode(salt).decode()}${base64.b64encode(digest).decode()}"


def mosq_check(pw: str, stored: str) -> bool:
    parts = stored.split("$")
    if len(parts) != 4 or parts[1] != "6":
        return False
    return hmac.compare_digest(mosq_hash(pw, base64.b64decode(parts[2])), stored)


def write_passwd(path: str | Path, entries: list[tuple[str, str]]) -> None:
    """Atomically replace the broker's password file with `user:hash` lines."""
    path = Path(path)
    for user, h in entries:
        if not user or any(c in user for c in ":\n+#/") or not h.startswith("$6$"):
            raise ValueError(f"bad passwd entry for {user!r}")
    tmp = path.with_suffix(".tmp")
    tmp.write_text("".join(f"{u}:{h}\n" for u, h in entries), encoding="utf-8", newline="\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


# ---- attempt limits ----------------------------------------------------------------
# Threat model: a home server with 2 admin-created users holding generated 20-character passwords. Guessing is
# hopeless, so this limits resource abuse (one scrypt per attempt) per client address, not password guessing.

WINDOW_S = 600
IP_FAILS = 10  # failed logins per address per window (attempts in flight count), then 429
MAX_KEYS = 10_000

log = logging.getLogger("hb.auth")
DUMMY_HASH = hash_password(secrets.token_urlsafe(16))  # checked when the user does not exist: same timing


class TooMany(Exception):
    def __init__(self, retry_after: float):
        super().__init__(retry_after)
        self.retry_after = max(1, math.ceil(retry_after))


def bucket(ip: str) -> str:
    """The unit the limit applies to: IPv4 as is, IPv6 per /64 (one subscriber owns the whole /64)."""
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return ip
    if a.version == 4:
        return str(a)
    if a.ipv4_mapped:
        return str(a.ipv4_mapped)
    return str(ipaddress.ip_network(f"{a}/64", strict=False))


class Guard:
    """Per-address login limit and per-user claim limit. Never sleeps; every check-and-record is under one lock.

    A login attempt is reserved against its address before the password check, so parallel guesses count while
    in flight; a success deletes the address's entry. Full table: evict the unblocked entry with the fewest
    failures (oldest first). A blocked address is never evicted; only when every entry is blocked do new
    addresses get 429.
    """

    def __init__(self, clock=time.time, max_keys: int = MAX_KEYS):
        self.clock, self.max_keys = clock, max_keys
        self.lock = threading.Lock()
        self.ips: dict[str, list[float]] = {}  # address bucket -> attempts (in flight or failed)
        self.claims: dict[str, list[float]] = {}  # authenticated user -> claim attempts (bounded by users)

    def _prune(self, now: float) -> None:
        # ponytail: O(entries) per attempt, entries capped at max_keys
        for table in (self.ips, self.claims):
            for k in list(table):
                v = [t for t in table[k] if now - t < WINDOW_S]
                if v:
                    table[k] = v
                else:
                    del table[k]

    def take_claim(self, uid: str, cap: int) -> None:
        with self.lock:
            now = self.clock()
            self._prune(now)
            hits = self.claims.get(uid, [])
            if len(hits) >= cap:
                raise TooMany(hits[0] + WINDOW_S - now)
            self.claims[uid] = hits + [now]

    def login_start(self, ip: str) -> None:
        """Reserve a login attempt for `ip`; it stays as a failure unless login_ok follows."""
        b = bucket(ip)
        with self.lock:
            now = self.clock()
            self._prune(now)
            hits = self.ips.get(b, [])
            if len(hits) >= IP_FAILS:
                raise TooMany(hits[0] + WINDOW_S - now)
            if b not in self.ips and len(self.ips) >= self.max_keys:
                free = [k for k, v in self.ips.items() if len(v) < IP_FAILS]
                if not free:
                    log.warning("login table full of blocked addresses: refusing %s", b)
                    raise TooMany(min(v[0] for v in self.ips.values()) + WINDOW_S - now)
                del self.ips[min(free, key=lambda k: (len(self.ips[k]), self.ips[k][0]))]
            self.ips[b] = hits + [now]

    def login_ok(self, ip: str) -> None:
        with self.lock:
            self.ips.pop(bucket(ip), None)


def reload_broker(unit: str = "hb-mosquitto.service") -> None:
    """SIGHUP the broker so it re-reads password_file and acl_file (both run as user hb)."""
    out = subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", unit],
                         capture_output=True, text=True, check=True).stdout.strip()
    if out and out != "0":
        os.kill(int(out), signal.SIGHUP)
