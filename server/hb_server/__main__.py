"""`python -m hb_server serve | adduser UID NAME [--circle C] | register DID --assign UID`. Settings come from the environment
(deploy/hb-server.env): HB_DB, HB_HOSTS, HB_CFG_KEY, HB_PASSWD, HB_ROUTER_PASS, HB_TLS_CERT, HB_TLS_KEY,
HB_FW_DIR, HB_MQTT_LOCAL_PORT, HB_API_PORT, HB_ADMIN_PORT, HB_MQTT_PORT."""
from __future__ import annotations

import argparse
import os
import threading
from pathlib import Path

from . import api, auth, signing
from .db import Db


def _router_pass(path: str) -> str:
    """The router's broker password, made once and kept beside the DB so the CLI can rewrite passwd too."""
    p = Path(path)
    if not p.exists():
        auth.write_private(p, auth.new_secret())
    return p.read_text(encoding="utf-8").strip()


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(prog="hb_server")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("serve")
    a = sub.add_parser("adduser", help="create a login and print its password")
    a.add_argument("uid")
    a.add_argument("name")
    a.add_argument("--circle", default="home", help="moods reach only the same circle (smoke-test users: smoke)")
    r = sub.add_parser("register", help="register a beacon did for the one user allowed to claim it")
    r.add_argument("did")
    r.add_argument("--assign", metavar="UID", required=True)
    args = ap.parse_args(argv)

    env = os.environ
    router_pass = _router_pass(env.get("HB_ROUTER_PASS", "/var/lib/hb/router.pass"))
    ctx = api.Ctx(
        db=Db(env.get("HB_DB", "/var/lib/hb/hb.db")),
        key=signing.load_key(env.get("HB_CFG_KEY", "/etc/hb/cfg-sign.key")),
        hosts=env["HB_HOSTS"].split(),
        mport=int(env.get("HB_MQTT_PORT", "8883")),
        aport=int(env.get("HB_API_PORT", "8443")),
        passwd=env.get("HB_PASSWD", "/var/lib/hb/passwd"),
        router_hash=auth.mosq_hash(router_pass),
        fw_dir=env.get("HB_FW_DIR", "/var/lib/hb/fw"),
        reload=auth.reload_broker,
    )
    if args.cmd == "adduser":
        print(api.add_user(ctx, args.uid, args.name, args.circle))
        return
    if args.cmd == "register":
        api.register(ctx, args.did, args.assign)
        return

    import uvicorn

    from .admin import create_admin
    from .router import Glue

    api.sync_broker(ctx)  # the router's password must be in passwd before it connects
    glue = Glue(ctx.db, "127.0.0.1", int(env.get("HB_MQTT_LOCAL_PORT", "1884")), router_pass)
    ctx.publish, ctx.refresh = glue.publish, glue.refresh
    glue.start()
    admin = uvicorn.Server(uvicorn.Config(create_admin(ctx), host="127.0.0.1",
                                          port=int(env.get("HB_ADMIN_PORT", "8444")), log_level="warning"))
    threading.Thread(target=admin.run, daemon=True, name="admin").start()  # not main thread: no signal handlers
    uvicorn.run(api.create_app(ctx), host="0.0.0.0", port=ctx.aport,
                ssl_certfile=env.get("HB_TLS_CERT", "/etc/hb/leaf.crt"),
                ssl_keyfile=env.get("HB_TLS_KEY", "/etc/hb/leaf.key"))


if __name__ == "__main__":
    main()
