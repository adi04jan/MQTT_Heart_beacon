"""Public HTTPS API (8443): login, claim, settings, events, firmware files. Shared config ops for admin."""
from __future__ import annotations

import copy
import hashlib
import json
import logging
import re
import secrets
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from . import auth, models, signing
from .db import Db
from .router import Pub

SESSION_S = 365 * 24 * 3600
log = logging.getLogger("hb.api")
CLAIMS = 10  # claim attempts per user per auth.WINDOW_S, then 429 (login limits live in auth.Guard)
UID = re.compile(r"[a-z][a-z0-9_]{0,31}")  # never collides with a did (HB-XXXXXX) in the ACL's %u patterns
DID = re.compile(r"HB-[0-9A-F]{6}")
FW_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,95}")
# What a user may change from the app (plan: Config schema v1, "Who edits"); everything else is admin-only.
USER_KEYS = {"name": None, "led": {"bright"}, "sched": {"on", "tz", "days", "start", "stop"},
             "pwr": {"idle_nap_min"}, "net": {"use_wifi"}}
# Seed for every new beacon: the office schedule, Mon-Sat 09:00-19:00 IST.
SEED_SCHED = {"on": True, "tz": 330, "days": "1111110", "start": "09:00", "stop": "19:00"}


@dataclass
class Ctx:
    db: Db
    key: object  # EC P-256 private key for config signing
    hosts: list[str]  # public address(es) of this server: claim `host`, config `srv.hosts`
    mport: int = 8883
    aport: int = 8443
    passwd: str | None = None  # broker password_file; None = don't write one
    router_hash: str = ""
    fw_dir: str = "fw"
    publish: Callable[[list[Pub]], None] = lambda pubs: None
    refresh: Callable[[], None] = lambda: None
    reload: Callable[[], None] = lambda: None
    clock: Callable[[], float] = time.time
    new_id: Callable[[], str] = field(default=lambda: secrets.token_hex(8))
    guard: auth.Guard | None = None  # shared by API and admin (failed-login counts)

    def __post_init__(self):
        self.guard = self.guard or auth.Guard(self.clock)


# ---- shared operations (API, admin, CLI) ---------------------------------------------------

def add_user(ctx: Ctx, uid: str, name: str, circle: str = "home") -> str:
    """Create a login; returns its generated 20-character password (shown once). Phones log in to MQTT as
    `uid` with `mqtt_pass`. Long random passwords are why failed logins never lock a username."""
    if not UID.fullmatch(uid):
        raise ValueError("uid must match [a-z][a-z0-9_]{0,31}")
    pw, mqtt_pass = auth.new_secret()[:20], auth.new_secret()
    ctx.db.x("INSERT INTO user (uid, name, pw, mqtt_pass, mqtt_hash, circle) VALUES (?, ?, ?, ?, ?, ?)",
             (uid, name, auth.hash_password(pw), mqtt_pass, auth.mosq_hash(mqtt_pass), circle))
    sync_broker(ctx)
    ctx.refresh()
    return pw


def register(ctx: Ctx, did: str, assign: str | None = None) -> None:
    if not DID.fullmatch(did):
        raise ValueError("did must be HB- and 6 uppercase hex digits")
    ctx.db.x("INSERT OR IGNORE INTO device (did, registered) VALUES (?, ?)", (did, int(ctx.clock())))
    if assign is not None:
        assign_to(ctx, did, assign)


def assign_to(ctx: Ctx, did: str, uid: str | None) -> None:
    """Only the assigned user may claim `did`: the did is public (BLE name), so this is the proof of
    possession until the contract has a claim code."""
    if uid is not None and not ctx.db.one("SELECT 1 FROM user WHERE uid = ?", (uid,)):
        raise ValueError(f"no user {uid}")
    ctx.db.x("UPDATE device SET assigned_uid = ? WHERE did = ?", (uid, did))


def sync_broker(ctx: Ctx) -> None:
    """Rewrite the broker's password file from the DB and make the broker re-read it."""
    if not ctx.passwd:
        return
    rows = [("router", ctx.router_hash)]
    rows += [(r["uid"], r["mqtt_hash"]) for r in ctx.db.q("SELECT uid, mqtt_hash FROM user WHERE disabled = 0")]
    rows += [(r["did"], r["mqtt_hash"]) for r in
             ctx.db.q("SELECT did, mqtt_hash FROM device WHERE claimed = 1 AND mqtt_hash IS NOT NULL")]
    auth.write_passwd(ctx.passwd, rows)
    ctx.reload()


def current_config(ctx: Ctx, did: str) -> dict:
    row = ctx.db.one("SELECT cfg FROM device WHERE did = ?", (did,))
    if row and row["cfg"]:
        return json.loads(row["cfg"])
    cfg = {"cv": 1, "n": 1, "did": did, **copy.deepcopy(models.CONFIG_DEFAULTS)}
    cfg["srv"].update(hosts=list(ctx.hosts), mqtt=[ctx.mport], api=ctx.aport)
    cfg["sched"] = dict(SEED_SCHED)
    return cfg


def push_config(ctx: Ctx, did: str, cfg: dict) -> dict:
    """Validate, bump n, sign, store and publish (retained). Returns the `cfg` envelope as an object.

    Raises models.ProtocolError with the failing key (or `too_big` if the signed envelope exceeds 1024 B).
    """
    row = ctx.db.one("SELECT cfg_n, owner, claimed FROM device WHERE did = ?", (did,))
    if row is None:
        raise KeyError(did)
    cfg = {**cfg, "cv": 1, "n": row["cfg_n"] + 1, "did": did}
    # parse_config drops unknown keys, net.wifi among them: no PSK is ever signed or reaches the phone.
    raw = signing.config_bytes(models.parse_config(signing.config_bytes(cfg)))
    env = signing.cfg_envelope(ctx.key, raw, ctx.new_id(), int(ctx.clock()))
    ctx.db.x("UPDATE device SET cfg_n = ?, cfg = ?, cfg_env = ? WHERE did = ?",
             (cfg["n"], raw.decode("utf-8"), env.decode("utf-8"), did))
    ctx.publish([Pub(f"hb/v1/d/{did}/cfg", env, retain=True)])
    ctx.refresh()
    return json.loads(env)


def unclaim(ctx: Ctx, did: str, release: bool) -> None:
    """Delete the beacon's broker credential. `release` (admin) also frees it for another user and drops the
    owner's settings (cfg_n stays, so `n` only ever grows)."""
    ctx.db.x("UPDATE device SET claimed = 0, mqtt_hash = NULL, status = '{}' WHERE did = ?", (did,))
    if release:
        ctx.db.x("UPDATE device SET owner = NULL, cfg = NULL, cfg_env = NULL WHERE did = ?", (did,))
    sync_broker(ctx)
    ctx.publish([Pub(f"hb/v1/d/{did}/cfg", b"", retain=True), Pub(f"hb/v1/d/{did}/state", b"", retain=True)])
    ctx.refresh()


@contextmanager
def _limited():
    try:
        yield
    except auth.TooMany as e:
        raise HTTPException(429, "slow_down", headers={"Retry-After": str(e.retry_after)}) from None


def _token_hash(tok: str) -> str:
    return hashlib.sha256(tok.encode()).hexdigest()


# ---- HTTP -----------------------------------------------------------------------------

class LoginIn(BaseModel):
    user: str = Field(max_length=32)
    password: str = Field(alias="pass", max_length=128)


class ClaimIn(BaseModel):
    did: str = Field(max_length=9)


def create_app(ctx: Ctx) -> FastAPI:
    app = FastAPI(title="Heart Beacon", docs_url=None, redoc_url=None, openapi_url=None)
    db = ctx.db

    def me(authorization: str = Header("")):
        tok = authorization.removeprefix("Bearer ").strip()
        row = db.one("SELECT u.* FROM session s JOIN user u ON u.uid = s.uid "
                     "WHERE s.token = ? AND s.created > ? AND u.disabled = 0",
                     (_token_hash(tok), int(ctx.clock()) - SESSION_S)) if tok else None
        if row is None:
            raise HTTPException(401, "login")
        return row

    def own(did: str, user) -> None:
        if not db.one("SELECT 1 FROM device WHERE did = ? AND owner = ? AND claimed = 1", (did, user["uid"])):
            raise HTTPException(404, "not_yours")

    @app.post("/api/v1/login")
    def login(body: LoginIn, request: Request):
        ip = request.client.host
        with _limited():
            token = ctx.guard.login_start(ip)
        row = db.one("SELECT * FROM user WHERE uid = ? AND disabled = 0", (body.user,))
        # Unknown user and wrong password look the same: same answer, and a hash check either way.
        if not auth.check_password(body.password, row["pw"] if row else auth.DUMMY_HASH) or row is None:
            log.warning("failed login for %r from %s", body.user, ip)
            raise HTTPException(401, "bad_login")
        ctx.guard.login_ok(ip, token)
        tok = auth.new_secret()
        db.x("INSERT INTO session VALUES (?, ?, ?)", (_token_hash(tok), row["uid"], int(ctx.clock())))
        return {"token": tok, "uid": row["uid"], "name": row["name"],
                "mqtt": {"host": ctx.hosts[0], "port": ctx.mport, "user": row["uid"], "pass": row["mqtt_pass"]}}

    @app.get("/api/v1/me")
    def get_me(user=Depends(me)):
        devs = [{"did": r["did"], "name": json.loads(r["cfg"])["name"] if r["cfg"] else "",
                 "cfg_n": r["cfg_n"], **json.loads(r["status"])}
                for r in db.q("SELECT * FROM device WHERE owner = ? AND claimed = 1", (user["uid"],))]
        return {"uid": user["uid"], "name": user["name"], "circle": user["circle"], "devices": devs}

    @app.post("/api/v1/devices/claim")
    def claim(body: ClaimIn, user=Depends(me)):
        with _limited():  # authenticated, so per user is enough
            ctx.guard.take_claim(user["uid"], CLAIMS)
        if not DID.fullmatch(body.did):
            raise HTTPException(422, "bad_did")
        row = db.one("SELECT * FROM device WHERE did = ?", (body.did,))
        if row is None or row["assigned_uid"] != user["uid"]:  # unregistered looks the same: no did probing
            raise HTTPException(403, "not_assigned")
        if row["owner"] not in (None, user["uid"]):
            raise HTTPException(409, "claimed")  # admin must release it first
        pw = auth.new_secret()
        new_owner = row["owner"] != user["uid"]
        db.x("UPDATE device SET owner = ?, claimed = 1, mqtt_hash = ?, "
             "cfg = CASE WHEN ? THEN NULL ELSE cfg END, cfg_env = CASE WHEN ? THEN NULL ELSE cfg_env END "
             "WHERE did = ?", (user["uid"], auth.mosq_hash(pw), new_owner, new_owner, body.did))
        sync_broker(ctx)
        cfg = push_config(ctx, body.did, current_config(ctx, body.did))
        return {"did": body.did, "host": ctx.hosts[0], "mport": ctx.mport, "aport": ctx.aport,
                "user": body.did, "pass": pw, "cfg": cfg}

    @app.delete("/api/v1/devices/{did}/claim")
    def delete_claim(did: str, user=Depends(me)):
        own(did, user)
        unclaim(ctx, did, release=False)
        return {"did": did, "claimed": False}

    @app.get("/api/v1/devices/{did}/settings")
    def get_settings(did: str, user=Depends(me)):
        own(did, user)
        cfg = current_config(ctx, did)
        out = {"n": cfg["n"]}
        for k, sub in USER_KEYS.items():
            out[k] = cfg[k] if sub is None else {kk: cfg[k][kk] for kk in sorted(sub)}
        return out

    @app.api_route("/api/v1/devices/{did}/settings", methods=["PATCH", "PUT"])
    def set_settings(did: str, body: dict, user=Depends(me)):
        own(did, user)
        cfg = current_config(ctx, did)
        for k, v in body.items():
            if k not in USER_KEYS:
                raise HTTPException(403, f"not_editable:{k}")
            if USER_KEYS[k] is None:
                cfg[k] = v
                continue
            if not isinstance(v, dict):
                raise HTTPException(422, k)
            for kk, vv in v.items():
                if kk not in USER_KEYS[k]:
                    raise HTTPException(403, f"not_editable:{k}.{kk}")
                cfg[k][kk] = vv
        try:
            return {"cfg": push_config(ctx, did, cfg)}
        except models.ProtocolError as e:
            raise HTTPException(422, e.code)

    @app.get("/api/v1/events")
    def events(limit: int = 50, user=Depends(me)):
        rows = db.q("SELECT e.*, u.name FROM event e JOIN user u ON u.uid = e.uid "
                    "WHERE u.circle = ? ORDER BY e.ts DESC LIMIT ?", (user["circle"], max(1, min(limit, 200))))
        return [{"id": r["id"], "k": r["k"], "ts": r["ts"], "uid": r["uid"], "from": r["name"], "src": r["src"],
                 "to": [{"did": d["did"], "st": d["status"]} for d in
                        db.q("SELECT did, status FROM delivery WHERE src = ? AND eid = ? ORDER BY did",
                             (r["src"], r["id"]))]}
                for r in rows]

    @app.get("/fw/{name}")
    def firmware(name: str):
        p = Path(ctx.fw_dir) / name
        if not FW_NAME.fullmatch(name) or not p.is_file():
            raise HTTPException(404)
        return FileResponse(p, media_type="application/octet-stream")

    return app
