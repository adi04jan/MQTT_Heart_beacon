"""Config signing: ECDSA P-256 / SHA-256 over the config bytes, DER signature, base64url without padding."""
from __future__ import annotations

import base64
import json

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from . import models

_ALG = ec.ECDSA(hashes.SHA256())


def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def unb64u(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def load_key(path: str) -> ec.EllipticCurvePrivateKey:
    with open(path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise ValueError("config signing key must be EC P-256")
    return key


def config_bytes(cfg: dict) -> bytes:
    """Canonical bytes of a config: compact JSON, raw UTF-8, keys in the order given."""
    return json.dumps(cfg, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def sign(key: ec.EllipticCurvePrivateKey, data: bytes) -> str:
    return b64u(key.sign(data, _ALG))


def verify(pub: ec.EllipticCurvePublicKey, data: bytes, sig: str) -> bool:
    try:
        pub.verify(unb64u(sig), data, _ALG)
        return True
    except (InvalidSignature, ValueError):
        return False


def cfg_envelope(key, raw: bytes, id_: str, now: int) -> bytes:
    """The retained `cfg` message for config bytes `raw` (already validated). Raises too_big over 1024 B."""
    n = json.loads(raw)["n"]
    return models.encode("cfg", id_, now, {"n": n, "b": b64u(raw), "s": sign(key, raw)})
