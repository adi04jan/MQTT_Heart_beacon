"""Mosquitto 1.6 password-file format, login hashes, config signatures."""
import json

import pytest
from cryptography.hazmat.primitives.asymmetric import ec

from hb_server import auth, models, signing

# Written by the real `mosquitto_passwd -b f HB-3FA2C1 placeholder-pass` (Mosquitto 1.6.9, Ubuntu 20.04).
REAL_1_6_9 = "HB-3FA2C1:$6$ZhsxPajMZwHpIQWQ$qE2uNeVqX0xexapr07aCqmA0WjFzXqEOOujI53lxL/iJTJ/jIUNmAd/409jQSltbUKFWyQnGoSuE8tqUMAoBNQ=="


def test_mosq_hash_matches_mosquitto_passwd_1_6_9():
    user, stored = REAL_1_6_9.split(":", 1)
    assert auth.mosq_check("placeholder-pass", stored)
    assert not auth.mosq_check("placeholder-pasS", stored)
    salt = __import__("base64").b64decode(stored.split("$")[2])
    assert auth.mosq_hash("placeholder-pass", salt) == stored  # byte-identical to the tool's line


def test_mosq_hash_shape():
    h = auth.mosq_hash(auth.new_secret())
    _, six, salt, digest = h.split("$")
    assert (six, len(salt), len(digest)) == ("6", 16, 88)  # 12-byte salt, 64-byte SHA-512, padded base64


def test_write_passwd(tmp_path):
    p = tmp_path / "passwd"
    rows = [("router", auth.mosq_hash("r")), ("HB-3FA2C1", auth.mosq_hash("d")), ("alice", auth.mosq_hash("a"))]
    auth.write_passwd(p, rows)
    lines = p.read_text().splitlines()
    assert [ln.split(":")[0] for ln in lines] == ["router", "HB-3FA2C1", "alice"]
    assert all(auth.mosq_check(pw, ln.split(":", 1)[1]) for pw, ln in zip("rda", lines))
    assert p.read_bytes().endswith(b"\n") and b"\r" not in p.read_bytes()
    for bad in ("a:b", "a+", "#", "x/y", ""):
        with pytest.raises(ValueError):
            auth.write_passwd(p, [(bad, auth.mosq_hash("x"))])


def test_login_password_hash():
    h = auth.hash_password("correct horse")
    assert auth.check_password("correct horse", h)
    assert not auth.check_password("correct horsE", h)
    assert not auth.check_password("x", "garbage")


def test_config_signature_round_trip_and_tamper():
    key = ec.generate_private_key(ec.SECP256R1())
    raw = signing.config_bytes({"cv": 1, "n": 3, "did": "HB-3FA2C1"})
    env = json.loads(signing.cfg_envelope(key, raw, "0123456789abcdef", 1700000000))
    assert (env["t"], env["n"]) == ("cfg", 3) and "=" not in env["b"] + env["s"]
    data = signing.unb64u(env["b"])
    assert data == raw and models.parse_config(data)["n"] == 3
    pub = key.public_key()
    assert signing.verify(pub, data, env["s"])
    assert not signing.verify(pub, data.replace(b'"n":3', b'"n":4'), env["s"])
    assert not signing.verify(ec.generate_private_key(ec.SECP256R1()).public_key(), data, env["s"])
    assert not signing.verify(pub, data, "AAAA")


def test_signature_is_der_sha256_verifiable_by_openssl_style_consumers():
    """mbedTLS mbedtls_pk_verify and Java SHA256withECDSA both take the DER form."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

    key = ec.generate_private_key(ec.SECP256R1())
    sig = signing.unb64u(signing.sign(key, b"x"))
    assert sig[0] == 0x30 and len(sig) <= 72
    decode_dss_signature(sig)  # raises if not DER
    key.public_key().verify(sig, b"x", ec.ECDSA(hashes.SHA256()))
