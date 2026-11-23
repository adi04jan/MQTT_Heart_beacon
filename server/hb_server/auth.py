"""Passwords: API logins (scrypt), login/claim attempt limits, the Mosquitto 1.6 password file."""
from __future__ import annotations

import base64
import hashlib
import hmac
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

WINDOW_S = 600
IP_FAILS = 10  # failed logins per client IP per window (attempts in flight count), then refused
PAIR_MAX_S = 60  # per (IP, user) not-before after a failure: 1, 2, 4 ... 60 s
MAX_KEYS = 10_000

log = logging.getLogger("hb.auth")


class TooMany(Exception):
    def __init__(self, retry_after: float):
        super().__init__(retry_after)
        self.retry_after = max(1, math.ceil(retry_after))


class Guard:
    """Login and claim attempt limits. Never sleeps; every check-and-record happens under one lock.

    Login: an attempt is reserved against its client IP before the password check and released on success, so
    parallel guesses count while in flight. Per IP: at most IP_FAILS per window. Per (IP, user): a doubling
    not-before. Per user alone: counted for the admin page, never blocking, so nobody can lock a user out
    from another address. Keys are bounded: expired entries go first, then the oldest.
    """

    def __init__(self, clock=time.time, max_keys: int = MAX_KEYS):
        self.clock, self.max_keys = clock, max_keys
        self.lock = threading.Lock()
        self.times: dict[tuple, list[float]] = {}  # ("ip", ip) | ("user", uid) | (kind, uid) -> recent times
        self.not_before: dict[tuple[str, str], tuple[int, float]] = {}  # (ip, user) -> (failures, not before)

    def _prune(self, now: float) -> None:
        # ponytail: O(keys) per attempt, keys capped at max_keys
        for k in list(self.times):
            v = [t for t in self.times[k] if now - t < WINDOW_S]
            if v:
                self.times[k] = v
            else:
                del self.times[k]
        for k in [k for k, (_, nb) in self.not_before.items() if now - nb >= WINDOW_S]:
            del self.not_before[k]

    def _put(self, d: dict, k, v) -> None:
        d.pop(k, None)
        d[k] = v
        while len(d) > self.max_keys:
            del d[next(iter(d))]  # dicts keep insertion order and _put re-inserts: the front is the oldest

    def take(self, key: tuple, cap: int) -> float:
        """Check and record one attempt for `key`; TooMany once `cap` attempts are inside the window."""
        with self.lock:
            now = self.clock()
            self._prune(now)
            hits = self.times.get(key, [])
            if len(hits) >= cap:
                raise TooMany(hits[0] + WINDOW_S - now)
            self._put(self.times, key, hits + [now])
            return now

    def login_start(self, ip: str, user: str) -> float:
        """Reserve a login attempt; returns the token for login_ok."""
        with self.lock:
            now = self.clock()
            self._prune(now)
            nb = self.not_before.get((ip, user), (0, 0.0))[1]
            if now < nb:
                raise TooMany(nb - now)
            hits = self.times.get(("ip", ip), [])
            if len(hits) >= IP_FAILS:
                raise TooMany(hits[0] + WINDOW_S - now)
            self._put(self.times, ("ip", ip), hits + [now])
            return now

    def login_ok(self, ip: str, user: str, token: float) -> None:
        with self.lock:
            hits = self.times.get(("ip", ip), [])
            if token in hits:
                hits.remove(token)
            self.not_before.pop((ip, user), None)

    def login_failed(self, ip: str, user: str) -> None:
        """The reservation stays as a failure."""
        with self.lock:
            now = self.clock()
            n = self.not_before.get((ip, user), (0, 0.0))[0] + 1
            self._put(self.not_before, (ip, user), (n, now + min(2.0 ** (n - 1), PAIR_MAX_S)))
            self._put(self.times, ("user", user), self.times.get(("user", user), []) + [now])
        log.warning("failed login for %r from %s (%d in a row from there)", user, ip, n)

    def failed_logins(self, user: str) -> int:
        with self.lock:
            now = self.clock()
            return sum(1 for t in self.times.get(("user", user), []) if now - t < WINDOW_S)


def reload_broker(unit: str = "hb-mosquitto.service") -> None:
    """SIGHUP the broker so it re-reads password_file and acl_file (both run as user hb)."""
    out = subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", unit],
                         capture_output=True, text=True, check=True).stdout.strip()
    if out and out != "0":
        os.kill(int(out), signal.SIGHUP)
