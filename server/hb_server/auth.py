"""Passwords: API logins (scrypt) and the Mosquitto 1.6 password file the broker reads."""
from __future__ import annotations

import base64
import hashlib
import hmac
import os
import secrets
import signal
import subprocess
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


def reload_broker(unit: str = "hb-mosquitto.service") -> None:
    """SIGHUP the broker so it re-reads password_file and acl_file (both run as user hb)."""
    out = subprocess.run(["systemctl", "show", "-p", "MainPID", "--value", unit],
                         capture_output=True, text=True, check=True).stdout.strip()
    if out and out != "0":
        os.kill(int(out), signal.SIGHUP)
