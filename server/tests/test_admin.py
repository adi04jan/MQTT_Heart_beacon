"""Admin pages: loopback/Origin guard, register + assign, add user, config edit -> signed publish, release."""
import json
from types import SimpleNamespace

import pytest
from cryptography.hazmat.primitives.asymmetric import ec
from fastapi.testclient import TestClient

from hb_server import admin, api, models, signing
from hb_server.db import Db

A = "HB-AAAAAA"
URL = "http://127.0.0.1:8444"


@pytest.fixture
def e(tmp_path):
    pubs = []
    ctx = api.Ctx(Db(str(tmp_path / "hb.db")), ec.generate_private_key(ec.SECP256R1()), ["203.0.113.10"],
                  publish=pubs.extend, clock=lambda: 1_700_000_000)
    api.add_user(ctx, "alice", "Alice")
    api.add_user(ctx, "bob", "Bob")
    return SimpleNamespace(ctx=ctx, pubs=pubs, c=TestClient(admin.create_admin(ctx), base_url=URL,
                                                            follow_redirects=False))


def form_of(cfg):
    """What a browser posts for the device form."""
    f = {}
    for _, _, fields in admin.FORM:
        for key, _, _ in fields:
            v, kind = admin._get(cfg, key), admin.kind(key)
            if kind == "days":
                f.update({f"sched.days.{i}": "on" for i, c in enumerate(v) if c == "1"})
            elif kind == "bool":
                if v:
                    f[key] = "on"
            elif kind == "list":
                f[key] = ", ".join(map(str, v))
            else:
                f[key] = str(v)
    return f


def test_loopback_only_and_no_cross_site_posts(e):
    assert e.c.get("/").status_code == 200
    assert TestClient(e.c.app, base_url="http://evil.example:8444").get("/").status_code == 403  # DNS rebinding
    assert e.c.post("/register", data={"did": A, "assign": "alice"},
                    headers={"Origin": "http://evil.example"}).status_code == 403
    assert e.c.post("/register", data={"did": A, "assign": "alice"}, headers={"Origin": URL}).status_code == 303


def test_register_assigns_and_lists(e):
    r = e.c.post("/register", data={"did": "hb-aaaaaa", "assign": "alice"})
    assert (r.status_code, r.headers["location"]) == (303, f"/d/{A}")
    assert e.ctx.db.one("SELECT assigned_uid FROM device WHERE did = ?", (A,))["assigned_uid"] == "alice"
    page = e.c.get("/").text
    assert A in page and "not assigned" not in page
    bad = e.c.post("/register", data={"did": "HB-XYZ", "assign": "alice"})
    assert bad.status_code == 400 and "did must be" in bad.text


def test_add_user_shows_a_long_password_once(e):
    r = e.c.post("/users", data={"uid": "carol", "name": "Carol"})
    pw = r.text.split('class="secret">')[1].split("<")[0]
    assert r.status_code == 200 and len(pw) >= 16
    assert e.c.post("/users", data={"uid": "carol", "name": "Carol"}).text.count("already exists") == 1
    assert e.c.post("/users", data={"uid": "Bad Name", "name": "x"}).status_code == 400


def test_config_edit_signs_and_publishes(e):
    api.register(e.ctx, A, "alice")
    cfg = api.current_config(e.ctx, A)
    f = form_of(cfg) | {"name": "Desk", "led.bright": "35", "sched.stop": "18:30"}
    del f["sched.days.5"]  # Saturday off
    r = e.c.post(f"/d/{A}", data=f)
    assert (r.status_code, r.headers["location"]) == (303, f"/d/{A}?saved=1")
    env = json.loads(e.pubs[-1].payload)
    got = models.parse_config(signing.unb64u(env["b"]))
    assert (got["name"], got["led"]["bright"], got["sched"]["stop"], got["sched"]["days"]) == ("Desk", 35, "18:30", "1111100")
    assert got["net"]["wifi"] == [] and e.pubs[-1].retain
    assert "Saved and published" in e.c.get(f"/d/{A}?saved=1").text


def test_config_errors_name_the_field_and_keep_the_input(e):
    api.register(e.ctx, A, "alice")
    f = form_of(api.current_config(e.ctx, A))
    r = e.c.post(f"/d/{A}", data=f | {"led.bright": "101"})
    assert r.status_code == 400 and 'id="e-led.bright"' in r.text and not e.pubs
    r = e.c.post(f"/d/{A}", data=f | {"pwr.batt_off_mv": "3500"})  # above batt_low_mv
    assert 'id="e-pwr.batt_off_mv"' in r.text and "Must be below Low battery." in r.text
    r = e.c.post(f"/d/{A}", data=f | {"ble.latency": "three"})
    assert r.status_code == 400 and 'value="three"' in r.text and 'id="e-ble.latency"' in r.text


def test_hostile_beacon_state_and_tele_cannot_break_the_pages(e):
    from hb_server import router
    api.register(e.ctx, A, "alice")
    e.ctx.db.x("UPDATE device SET owner = 'alice', claimed = 1 WHERE did = ?", (A,))
    st = router.load_state(e.ctx.db, 1000)
    raw = (b'{"v":1,"t":"state","id":"00000000000000a1","ts":0,"st":"sleep","path":"wifi","wake":1e30}',
           b'{"v":1,"t":"tele","id":"00000000000000a2","ts":0,"mv":"x","pct":1e9,"rssi":-60,"fw":"2.0.0"}')
    for topic, payload in zip((f"hb/v1/d/{A}/state", f"hb/v1/d/{A}/up"), raw):
        router.route(router.Msg(topic, payload, 1000), st)
    router.save_state(e.ctx.db, st)
    status = json.loads(e.ctx.db.one("SELECT status FROM device WHERE did = ?", (A,))["status"])
    assert status["wake"] == 0 and status["tele"] == {"rssi": -60, "fw": "2.0.0"}
    assert e.c.get("/").status_code == 200 and e.c.get(f"/d/{A}").status_code == 200
    # a row written by something else still renders
    e.ctx.db.x("UPDATE device SET status = ? WHERE did = ?",
               (json.dumps({"st": "sleep", "wake": 10**30, "tele": {"mv": "x", "pct": [1]}}), A))
    assert e.c.get("/").status_code == 200 and e.c.get(f"/d/{A}").status_code == 200
    assert admin._when(10**30) == "" and admin._when(float("nan")) == ""


def test_assign_and_release(e):
    api.register(e.ctx, A, "alice")
    e.ctx.db.x("UPDATE device SET owner = 'alice', claimed = 1 WHERE did = ?", (A,))
    assert e.c.post(f"/d/{A}/assign", data={"assign": "bob"}).status_code == 303
    assert e.c.post(f"/d/{A}/release").status_code == 303
    row = e.ctx.db.one("SELECT owner, claimed, assigned_uid FROM device WHERE did = ?", (A,))
    assert (row["owner"], row["claimed"], row["assigned_uid"]) == (None, 0, "bob")
    assert "Released" in e.c.get(f"/d/{A}?released").text
    assert e.c.get("/d/HB-000000").status_code == 404
