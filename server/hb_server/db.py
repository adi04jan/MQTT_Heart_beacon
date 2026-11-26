"""SQLite (stdlib, WAL). One connection shared by API threads, the admin thread and the MQTT thread."""
from __future__ import annotations

import sqlite3
import threading
from contextlib import contextmanager

SCHEMA = """
CREATE TABLE IF NOT EXISTS user (
  uid TEXT PRIMARY KEY, name TEXT NOT NULL, pw TEXT NOT NULL,
  mqtt_pass TEXT NOT NULL, mqtt_hash TEXT NOT NULL,
  circle TEXT NOT NULL DEFAULT 'home', disabled INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS session (token TEXT PRIMARY KEY, uid TEXT NOT NULL, created INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS device (
  did TEXT PRIMARY KEY, owner TEXT, assigned_uid TEXT, claimed INTEGER NOT NULL DEFAULT 0, mqtt_hash TEXT,
  cfg_n INTEGER NOT NULL DEFAULT 0, cfg TEXT, cfg_env TEXT,
  status TEXT NOT NULL DEFAULT '{}', registered INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS setting (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS event (
  src TEXT NOT NULL, id TEXT NOT NULL, uid TEXT NOT NULL, k TEXT NOT NULL, ts INTEGER NOT NULL,
  PRIMARY KEY (src, id));
CREATE TABLE IF NOT EXISTS delivery (
  src TEXT NOT NULL, eid TEXT NOT NULL, did TEXT NOT NULL, k TEXT NOT NULL, created INTEGER NOT NULL,
  status TEXT NOT NULL, path TEXT, sent_at INTEGER NOT NULL, tried_at INTEGER NOT NULL,
  tries INTEGER NOT NULL, show_id TEXT, owner TEXT NOT NULL, PRIMARY KEY (src, eid, did));
CREATE INDEX IF NOT EXISTS event_ts ON event (ts);
"""


class Db:
    # ponytail: one connection + one global lock; per-thread connections if the API ever gets busy
    def __init__(self, path: str):
        self.con = sqlite3.connect(path, check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        self.con.execute("PRAGMA journal_mode=WAL")
        self.con.execute("PRAGMA synchronous=NORMAL")
        self.con.executescript(SCHEMA)

    @contextmanager
    def tx(self):
        """Lock + transaction: commits on success, rolls back on error."""
        with self.lock, self.con:
            yield self.con

    def q(self, sql: str, args=()) -> list[sqlite3.Row]:
        with self.lock:
            return self.con.execute(sql, args).fetchall()

    def one(self, sql: str, args=()) -> sqlite3.Row | None:
        rows = self.q(sql, args)
        return rows[0] if rows else None

    def x(self, sql: str, args=()) -> int:
        with self.tx() as c:
            return c.execute(sql, args).rowcount
