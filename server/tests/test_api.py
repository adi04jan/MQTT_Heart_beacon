"""API via TestClient: login, claim rules, settings -> signed config, events, firmware files."""
import json
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from fastapi.testclient import TestClient

from hb_server import api, auth, models, signing
from hb_server.db import Db

A, B = "HB-AAAAAA", "HB-BBBBBB"
NOW = 1_700_000_000


@pytest.fixture
def e(tmp_path):
    pubs, reloads, sleeps = [], [], []
    ctx = api.Ctx(Db(str(tmp_path / "hb.db")), ec.generate_private_key(ec.SECP256R1()), ["203.0.113.10"],
                  passwd=str(tmp_path / "passwd"), router_hash=auth.mosq_hash("router-pass"),
                  fw_dir=str(tmp_path), publish=pubs.extend, reload=lambda: reloads.append(1), clock=lambda: NOW,
                  sleep=sleeps.append)
    pw = {u: api.add_user(ctx, u, u.title()) for u in ("alice", "bob")}
    api.register(ctx, A, assign="alice")
    api.register(ctx, B, assign="bob")
    return SimpleNamespace(ctx=ctx, c=TestClient(api.create_app(ctx)), pw=pw, pubs=pubs, reloads=reloads, sleeps=sleeps,
                           passwd=tmp_path / "passwd")


def login(e, uid):
    r = e.c.post("/api/v1/login", json={"user": uid, "pass": e.pw[uid]})
    assert r.status_code == 200, r.text
    return {"Authorization": "Bearer " + r.json()["token"]}


def passwd(e):
    return dict(ln.split(":", 1) for ln in e.passwd.read_text().splitlines())


def test_login_returns_mqtt_credentials_in_the_broker_file(e):
    r = e.c.post("/api/v1/login", json={"user": "alice", "pass": e.pw["alice"]}).json()
    assert r["uid"] == "alice" and r["mqtt"]["user"] == "alice" and r["mqtt"]["port"] == 8883
    assert auth.mosq_check(r["mqtt"]["pass"], passwd(e)["alice"])
    assert auth.mosq_check("router-pass", passwd(e)["router"])
    assert e.c.post("/api/v1/login", json={"user": "alice", "pass": "nope"}).status_code == 401
    assert e.c.post("/api/v1/login", json={"user": "zed", "pass": "nope"}).status_code == 401
    e.ctx.db.x("UPDATE user SET disabled = 1 WHERE uid = 'alice'")
    assert e.c.post("/api/v1/login", json={"user": "alice", "pass": e.pw["alice"]}).status_code == 401


def test_endpoints_need_a_token(e):
    assert e.c.get("/api/v1/me").status_code == 401
    assert e.c.get("/api/v1/me", headers={"Authorization": "Bearer forged"}).status_code == 401
    assert e.c.post("/api/v1/devices/claim", json={"did": A}).status_code == 401


def test_claim_issues_credential_and_signed_seed_config(e):
    r = e.c.post("/api/v1/devices/claim", json={"did": A}, headers=login(e, "alice"))
    assert r.status_code == 200, r.text
    j = r.json()
    assert (j["host"], j["mport"], j["aport"], j["user"]) == ("203.0.113.10", 8883, 8443, A)
    assert len(j["pass"]) >= 43 and auth.mosq_check(j["pass"], passwd(e)[A])
    env = j["cfg"]
    raw = signing.unb64u(env["b"])
    assert signing.verify(e.ctx.key.public_key(), raw, env["s"])
    cfg = models.parse_config(raw)
    assert (cfg["did"], cfg["n"], env["n"]) == (A, 1, 1)
    assert cfg["sched"] == {"on": True, "tz": 330, "days": "1111110", "start": "09:00", "stop": "19:00"}
    assert cfg["srv"] == {"hosts": ["203.0.113.10"], "mqtt": [8883], "api": 8443}
    assert [(p.topic, p.retain) for p in e.pubs] == [(f"hb/v1/d/{A}/cfg", True)]
    assert json.loads(e.pubs[0].payload) == env and e.reloads


def claimer(e):
    return lambda did, h: e.c.post("/api/v1/devices/claim", json={"did": did}, headers=h)


def test_claim_rules(e):
    alice, bob = login(e, "alice"), login(e, "bob")
    claim = claimer(e)
    assert claim("HB-123456", alice).json()["detail"] == "not_assigned"  # unregistered looks the same: 403
    assert claim("HB-12345", alice).status_code == 422
    assert claim("hb-aaaaaa", alice).status_code == 422
    assert claim(A, bob).status_code == 403  # the did is public (BLE name): only its assignee may claim
    first = claim(A, alice).json()
    again = claim(A, alice).json()  # same owner may re-claim (after a factory reset): new password, n bumps
    assert again["pass"] != first["pass"] and not auth.mosq_check(first["pass"], passwd(e)[A])
    assert again["cfg"]["n"] == 2
    assert e.c.delete(f"/api/v1/devices/{A}/claim", headers=bob).status_code == 404
    assert e.c.delete(f"/api/v1/devices/{A}/claim", headers=alice).status_code == 200
    assert A not in passwd(e)  # credential deleted
    assert (f"hb/v1/d/{A}/cfg", b"", True) in [(p.topic, p.payload, p.retain) for p in e.pubs]
    api.assign_to(e.ctx, A, "bob")
    assert claim(A, bob).status_code == 409  # still alice's until the admin releases it
    api.unclaim(e.ctx, A, release=True)
    assert claim(A, bob).status_code == 200


def test_release_and_reclaim_by_another_user_starts_from_the_seed(e):
    alice, claim = login(e, "alice"), claimer(e)
    claim(A, alice)
    e.c.patch(f"/api/v1/devices/{A}/settings", headers=alice, json={"name": "Alice desk", "sched": {"stop": "17:00"}})
    api.unclaim(e.ctx, A, release=True)
    row = e.ctx.db.one("SELECT cfg, cfg_env, cfg_n FROM device WHERE did = ?", (A,))
    assert (row["cfg"], row["cfg_env"], row["cfg_n"]) == (None, None, 2)
    api.assign_to(e.ctx, A, "bob")
    bob = login(e, "bob")
    cfg = models.parse_config(signing.unb64u(claim(A, bob).json()["cfg"]["b"]))
    assert (cfg["n"], cfg["name"], cfg["sched"]["stop"]) == (3, "", "19:00")  # n keeps growing, settings reset
    assert e.c.get(f"/api/v1/devices/{A}/settings", headers=bob).json()["name"] == ""


def test_bad_logins_from_one_ip_do_not_lock_the_user_out_elsewhere(e):
    app = e.c.app
    x, y = TestClient(app, client=("192.0.2.1", 1000)), TestClient(app, client=("192.0.2.2", 1000))
    for _ in range(api.IP_FAILS):
        assert x.post("/api/v1/login", json={"user": "alice", "pass": "guess"}).status_code == 401
    assert e.sleeps == [0.25, 0.5, 1.0, 2.0, 4.0, 4.0, 4.0, 4.0, 4.0]  # backoff for the name, capped
    assert x.post("/api/v1/login", json={"user": "bob", "pass": e.pw["bob"]}).status_code == 429  # IP X is out
    assert y.post("/api/v1/login", json={"user": "alice", "pass": e.pw["alice"]}).status_code == 200  # A is not


def test_success_resets_the_users_backoff(e):
    for _ in range(3):
        e.c.post("/api/v1/login", json={"user": "alice", "pass": "guess"})
    login(e, "alice")
    e.sleeps.clear()
    login(e, "alice")
    assert e.sleeps == []


def test_claim_attempts_are_limited_per_user(e):
    h = login(e, "bob")
    codes = [e.c.post("/api/v1/devices/claim", json={"did": A}, headers=h).status_code for _ in range(api.CLAIMS + 1)]
    assert codes == [403] * api.CLAIMS + [429]
    assert e.c.post("/api/v1/devices/claim", json={"did": A}, headers=login(e, "alice")).status_code == 200


def test_wifi_psks_never_enter_a_signed_config(e):
    claimer(e)(A, login(e, "alice"))
    cfg = api.current_config(e.ctx, A)
    cfg["net"]["wifi"] = [{"ssid": "Office", "psk": "secret-psk-123"}]
    env = api.push_config(e.ctx, A, cfg)
    assert b"secret" not in signing.unb64u(env["b"]) and models.parse_config(signing.unb64u(env["b"]))["net"]["wifi"] == []


def test_settings_user_keys_only_and_signed_publish(e):
    h = login(e, "alice")
    e.c.post("/api/v1/devices/claim", json={"did": A}, headers=h)
    s = e.c.get(f"/api/v1/devices/{A}/settings", headers=h).json()
    assert s == {"n": 1, "name": "", "led": {"bright": 60},
                 "sched": {"days": "1111110", "on": True, "start": "09:00", "stop": "19:00", "tz": 330},
                 "pwr": {"idle_nap_min": 90}, "net": {"use_wifi": True}}
    e.pubs.clear()
    r = e.c.patch(f"/api/v1/devices/{A}/settings", headers=h, json={"name": "Desk", "sched": {"stop": "18:30"}})
    assert r.status_code == 200, r.text
    cfg = models.parse_config(signing.unb64u(r.json()["cfg"]["b"]))
    assert (cfg["n"], cfg["name"], cfg["sched"]["stop"], cfg["sched"]["start"]) == (2, "Desk", "18:30", "09:00")
    assert [(p.topic, p.retain) for p in e.pubs] == [(f"hb/v1/d/{A}/cfg", True)]
    assert e.c.put(f"/api/v1/devices/{A}/settings", headers=h, json={"led": {"bright": 10}}).json()["cfg"]["n"] == 3
    bad = lambda body: e.c.patch(f"/api/v1/devices/{A}/settings", headers=h, json=body)  # noqa: E731
    assert bad({"pwr": {"batt_off_mv": 3000}}).json()["detail"] == "not_editable:pwr.batt_off_mv"
    assert bad({"srv": {"hosts": []}}).status_code == 403
    assert bad({"led": {"bright": 101}}).json()["detail"] == "led.bright"
    assert bad({"sched": {"start": "25:00"}}).json()["detail"] == "sched.start"
    assert bad({"led": 5}).status_code == 422
    assert e.c.get(f"/api/v1/devices/{A}/settings", headers=h).json()["n"] == 3  # rejects did not bump n
    assert e.c.get(f"/api/v1/devices/{A}/settings", headers=login(e, "bob")).status_code == 404


def test_me_and_events(e):
    h = login(e, "alice")
    e.c.post("/api/v1/devices/claim", json={"did": A}, headers=h)
    me = e.c.get("/api/v1/me", headers=h).json()
    assert me["uid"] == "alice" and [d["did"] for d in me["devices"]] == [A]
    db = e.ctx.db
    db.x("INSERT INTO event VALUES (?, ?, ?, ?, ?)", (A, "00000000000000aa", "alice", "heart", NOW - 5))
    db.x("INSERT INTO delivery VALUES (?, ?, ?, ?, ?, 'shown', 'wifi', 0, 0, 1, ?, 'bob')",
         (A, "00000000000000aa", B, "heart", NOW - 5, "00000000000000aa"))
    ev = e.c.get("/api/v1/events", headers=login(e, "bob")).json()
    assert ev == [{"id": "00000000000000aa", "k": "heart", "ts": NOW - 5, "uid": "alice", "from": "Alice", "src": A,
                   "to": [{"did": B, "st": "shown"}]}]


def test_firmware_files_are_public_and_confined(e, tmp_path):
    (tmp_path / "hb-2.0.0.bin").write_bytes(b"\xe9fw")
    assert e.c.get("/fw/hb-2.0.0.bin").content == b"\xe9fw"
    assert e.c.get("/fw/missing.bin").status_code == 404
    assert e.c.get("/fw/..%2Fhb.db").status_code == 404
    assert e.c.get("/fw/.hidden").status_code == 404
