"""Admin pages on 127.0.0.1 (reached over an SSH tunnel): devices + users, and one device's config."""
from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timedelta, timezone
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import PlainTextResponse, RedirectResponse
from fastapi.templating import Jinja2Templates

from . import api, models
from .router import _tele

IST = timezone(timedelta(minutes=330), "IST")
DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
# (block, heading, [(key, label, hint)]); keys follow models.CONFIG_DEFAULTS. net.wifi is never signed (api).
FORM = [
    ("name", "Identity", [("name", "Name", "Shown in the app, up to 32 bytes")]),
    ("sched", "Schedule", [("sched.on", "Office hours on", "Off means always on"),
                           ("sched.days", "Days", ""),
                           ("sched.start", "Wake at", ""), ("sched.stop", "Sleep at", ""),
                           ("sched.tz", "Time zone", "Minutes east of UTC, 330 = IST")]),
    ("led", "LEDs", [("led.bright", "Brightness", "Percent, 0 to 100")]),
    ("pwr", "Power", [("pwr.idle_nap_min", "Nap when idle", "Minutes, 0 = never"),
                      ("pwr.nap_wake_min", "Nap check-in", "Minutes"),
                      ("pwr.batt_low_mv", "Low battery", "mV"), ("pwr.batt_off_mv", "Battery off", "mV, below low"),
                      ("pwr.batt_cal_ppm", "Divider correction", "ppm")]),
    ("net", "Network", [("net.use_wifi", "Use Wi-Fi when the phone is away", ""),
                        ("net.near_hold_s", "Near hold", "Seconds"), ("net.away_grace_s", "Away grace", "Seconds"),
                        ("net.tele_min", "Telemetry every", "Minutes")]),
    ("ble", "Bluetooth", [("ble.adv_fast_ms", "Fast advertising", "ms"), ("ble.adv_fast_s", "Fast for", "Seconds"),
                          ("ble.adv_slow_ms", "Slow advertising", "ms"), ("ble.conn_ms", "Connection interval", "ms"),
                          ("ble.latency", "Peripheral latency", "Intervals"),
                          ("ble.sup_ms", "Supervision timeout", "ms")]),
    ("srv", "Server", [("srv.hosts", "Addresses", "Comma separated, up to 4"),
                       ("srv.mqtt", "MQTT ports", "Comma separated"), ("srv.api", "API port", "")]),
    ("fw", "Firmware target", [("fw.ver", "Version", "Empty = no update"), ("fw.path", "Path", "e.g. /fw/hb-2.0.0.bin"),
                               ("fw.sha256", "SHA-256", "64 hex digits"), ("fw.min_mv", "Minimum battery", "mV")]),
]


ERR_HINTS = {"pwr.batt_off_mv": "Must be below Low battery.",
             "ble.sup_ms": "Must be longer than 2 x (1 + latency) x connection interval.",
             "fw.sha256": "Leave empty or paste 64 lowercase hex digits.",
             "srv.hosts": "Up to 4 addresses, comma separated.", "srv.mqtt": "1 to 4 ports, comma separated.",
             "sched.start": "Use HH:MM.", "sched.stop": "Use HH:MM.", "name": "Up to 32 bytes.",
             "too_big": "The signed config would exceed 1024 bytes: shorten the name, paths or host list."}


def kind(key: str) -> str:
    if key == "sched.days":
        return "days"
    if key in ("sched.start", "sched.stop"):
        return "time"
    d = _default(key)
    return "bool" if isinstance(d, bool) else "int" if isinstance(d, int) else "list" if isinstance(d, list)         else "text"


def label(key: str) -> str:
    return next((lb for _, _, fields in FORM for k, lb, _ in fields if k == key), key)


def _default(key: str):
    block, _, leaf = key.partition(".")
    d = models.CONFIG_DEFAULTS[block]
    return d[leaf] if leaf else d


def _get(cfg: dict, key: str):
    block, _, leaf = key.partition(".")
    return cfg[block][leaf] if leaf else cfg[block]


def form_to_config(form, base: dict) -> tuple[dict, str | None]:
    """Overlay the posted fields on `base`. Returns (config, first unparseable key or None); an unparseable
    field keeps its raw text so the form shows what was typed."""
    cfg, bad = json.loads(json.dumps(base)), None
    for _, _, fields in FORM:
        for key, _, _ in fields:
            block, _, leaf = key.partition(".")
            raw, default = form.get(key, ""), _default(key)
            if key == "sched.days":
                v = "".join("1" if form.get(f"sched.days.{i}") else "0" for i in range(7))
            elif isinstance(default, bool):
                v = key in form
            elif isinstance(default, int):
                try:
                    v = int(raw.strip())
                except ValueError:
                    v, bad = raw, bad or key
            elif isinstance(default, list):
                items = [s.strip() for s in raw.split(",") if s.strip()]
                try:
                    v = [int(s) for s in items] if key == "srv.mqtt" else items
                except ValueError:
                    v, bad = raw, bad or key
            else:
                v = raw.strip()
            if leaf:
                cfg[block][leaf] = v
            else:
                cfg[block] = v
    return cfg, bad


def _when(ts) -> str:
    try:
        return datetime.fromtimestamp(ts, IST).strftime("%d %b %H:%M") if ts else ""
    except (OverflowError, ValueError, OSError, TypeError):
        return ""


def _ago(ts, now) -> str:
    if not ts:
        return "never"
    s = max(0, int(now - ts))
    for unit, n in (("d", 86400), ("h", 3600), ("min", 60)):
        if s >= n:
            return f"{s // n} {unit} ago"
    return "just now"


def create_admin(ctx: api.Ctx) -> FastAPI:
    app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
    tpl = Jinja2Templates(directory=str(Path(__file__).parent / "templates"))
    tpl.env.filters.update(when=_when, ago=lambda ts: _ago(ts, ctx.clock()))
    tpl.env.globals.update(FORM=FORM, DAYS=DAYS, get=_get, kind=kind, label=label, hints=ERR_HINTS)
    db = ctx.db

    @app.middleware("http")
    async def local_only(request: Request, call_next):
        # Loopback only, and no cross-site POSTs: a page in the admin's browser must not drive this
        # (DNS rebinding changes Host, a foreign form changes Origin).
        host = request.headers.get("host", "")
        origin = request.headers.get("origin")
        if host.rsplit(":", 1)[0] not in ("127.0.0.1", "localhost") or (
                request.method == "POST" and origin not in (None, f"http://{host}")):
            return PlainTextResponse("forbidden", status_code=403)
        return await call_next(request)

    def users():
        return db.q("SELECT uid, name, circle, disabled FROM user ORDER BY uid")

    def devices_page(request, **extra):
        rows = []
        for r in db.q("SELECT * FROM device ORDER BY did"):
            st = json.loads(r["status"])
            tele = _tele(st.get("tele") or {})  # rows not written by the router get the same filter
            rows.append({**dict(r), "st": st.get("st", ""), "path": st.get("path"),
                         "live": st.get("wifi") or st.get("ble"), "last_seen": st.get("last_seen", 0),
                         "wake": st.get("wake", 0), "mv": tele.get("mv"), "pct": tele.get("pct"),
                         "fw": tele.get("fw", ""),
                         "name": json.loads(r["cfg"])["name"] if r["cfg"] else ""})
        return tpl.TemplateResponse(request, "devices.html", {"devices": rows, "users": users(), **extra},
                                    status_code=400 if extra.get("error") or extra.get("user_error") else 200)

    @app.get("/")
    def index(request: Request):
        return devices_page(request)

    @app.post("/register")
    async def do_register(request: Request):
        f = await request.form()
        did, uid = f.get("did", "").strip().upper(), f.get("assign", "")
        try:
            api.register(ctx, did, uid or None)
        except ValueError as e:
            return devices_page(request, error=str(e), form=dict(f))
        return RedirectResponse(f"/d/{did}", status_code=303)

    @app.post("/users")
    async def do_add_user(request: Request):
        f = await request.form()
        uid, name = f.get("uid", "").strip(), f.get("name", "").strip()
        try:
            if not name:
                raise ValueError("Give the user a display name.")
            pw = api.add_user(ctx, uid, name)
        except sqlite3.IntegrityError:
            return devices_page(request, user_error=f"User {uid} already exists.", uform=dict(f))
        except ValueError as e:
            return devices_page(request, user_error=str(e), uform=dict(f))
        # Rendered, not redirected: the password must not land in a URL or the history.
        return devices_page(request, new_user={"uid": uid, "pw": pw})

    def device_page(request, did, cfg=None, error=None, status=200):
        row = db.one("SELECT * FROM device WHERE did = ?", (did,))
        if row is None:
            return PlainTextResponse("no such beacon", status_code=404)
        st = json.loads(row["status"])
        return tpl.TemplateResponse(request, "device.html", {
            "d": dict(row), "st": st, "tele": _tele(st.get("tele") or {}), "users": users(),
            "cfg": cfg or api.current_config(ctx, did), "error": error,
            "saved": request.query_params.get("saved"), "released": "released" in request.query_params},
            status_code=status)

    @app.get("/d/{did}")
    def device(request: Request, did: str):
        return device_page(request, did)

    @app.post("/d/{did}")
    async def save(request: Request, did: str):
        f = await request.form()
        cfg, bad = form_to_config(f, api.current_config(ctx, did))
        if bad:
            return device_page(request, did, cfg, bad, 400)
        try:
            env = api.push_config(ctx, did, cfg)
        except models.ProtocolError as e:
            return device_page(request, did, cfg, e.code, 400)
        except KeyError:
            return PlainTextResponse("no such beacon", status_code=404)
        return RedirectResponse(f"/d/{did}?saved={env['n']}", status_code=303)

    @app.post("/d/{did}/assign")
    async def assign(request: Request, did: str):
        uid = (await request.form()).get("assign") or None
        api.assign_to(ctx, did, uid)
        return RedirectResponse(f"/d/{did}", status_code=303)

    @app.post("/d/{did}/release")
    def release(did: str):
        api.unclaim(ctx, did, release=True)
        return RedirectResponse(f"/d/{did}?released", status_code=303)

    return app
