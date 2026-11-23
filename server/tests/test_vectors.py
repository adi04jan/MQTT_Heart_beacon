"""protocol/vectors.json against hb_server.models (the same file the C and Kotlin suites check)."""
import json
from pathlib import Path

import pytest

from hb_server import models as m

VEC = json.loads((Path(__file__).resolve().parents[2] / "protocol" / "vectors.json").read_text(encoding="utf-8"))
ENV, FRAME, CFG = VEC["envelope"], VEC["frame"], VEC["config"]


def by_name(items):
    return pytest.mark.parametrize("v", items, ids=[i["name"] for i in items])


def test_known_types():
    assert m.TYPES == set(ENV["types"])


@by_name(ENV["encode"])
def test_encode_round_trip(v):
    raw = m.encode(v["t"], v["id"], v["ts"], v["body"])
    assert raw == v["json"].encode("utf-8")
    assert m.decode(raw) == m.Envelope(v["t"], v["id"], v["ts"], v["body"])


@by_name(ENV["decode"])
def test_decode(v):
    raw = v["json"].encode("utf-8")
    if "err" in v:
        with pytest.raises(m.ProtocolError) as e:
            m.decode(raw)
        assert e.value.code == v["err"]
    else:
        assert m.decode(raw) == m.Envelope(v["t"], v["id"], v["ts"], v["body"])


@by_name(ENV["unknown_t"])
def test_unknown_t_response(v):
    req = m.decode(v["json"].encode("utf-8"))
    want = v["resp_json"] and v["resp_json"].encode("utf-8")
    assert m.unknown_t_response(req, v["resp_id"], v["resp_ts"]) == want


def test_did_from_mac():
    for v in ENV["did"]:
        assert m.did_from_mac(bytes.fromhex(v["mac"])) == v["did"]


def test_encode_refuses_oversize():
    with pytest.raises(m.ProtocolError):
        m.encode("mood", "0123456789abcdef", 0, {"pad": "x" * 1024})


@by_name(FRAME["split"])
def test_split_and_reassemble(v):
    msg = v["msg"].encode("utf-8")
    frames = m.split_frames(msg, v["seq"], v["mtu"])
    assert [f.hex() for f in frames] == v["frames"]
    r = m.Reassembler()
    outs = [r.push(f, 10 * i) for i, f in enumerate(frames)]
    assert outs[-1] == ("complete", msg)
    assert all(o == ("partial", None) for o in outs[:-1])


@by_name(FRAME["split_reject"])
def test_split_rejects(v):
    with pytest.raises(m.ProtocolError):
        m.split_frames(v["msg"].encode("utf-8"), v["seq"], v["mtu"])


@by_name(FRAME["reassemble"])
def test_reassemble_steps(v):
    r = m.Reassembler()
    for s in v["steps"]:
        out, msg = r.push(bytes.fromhex(s["frame"]), s["at"])
        assert out == s["out"]
        assert msg == (s["msg"].encode("utf-8") if out == "complete" else None)


def test_config_defaults():
    assert m.CONFIG_DEFAULTS == CFG["defaults"]


@by_name(CFG["parse"])
def test_config_parse(v):
    raw = v["json"].encode("utf-8")
    if "err" in v:
        with pytest.raises(m.ProtocolError) as e:
            m.parse_config(raw)
        assert e.value.code == v["err"]
    else:
        assert m.parse_config(raw) == v["cfg"]
