#!/usr/bin/env python3
"""
db.py  —  Shared MySQL connection + insert helpers.

Used by both serial_to_mysql.py (real hardware) and simulate_nodes.py
(synthetic data) so they always agree on connection settings and table layout.

Configuration comes from environment variables (with sensible localhost
defaults), so you never have to hard-code credentials:

    MYSQL_HOST      default "localhost"
    MYSQL_PORT      default 3306
    MYSQL_USER      default "root"
    MYSQL_PASSWORD  default ""
    MYSQL_DATABASE  default "mesh_nodes"
"""

import os
import json

import pymysql

# ─── Connection settings ─────────────────────────────────────────────────────

DB_HOST     = os.getenv("MYSQL_HOST",     "140.118.235.181")
DB_PORT     = int(os.getenv("MYSQL_PORT", "3306"))
DB_USER     = os.getenv("MYSQL_USER",     "aiot")
DB_PASSWORD = os.getenv("MYSQL_PASSWORD", "aiotpassword")
DB_NAME     = os.getenv("MYSQL_DATABASE", "aiot")


def connect():
    """Open an autocommit connection to the mesh_nodes database."""
    return pymysql.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASSWORD,
        database=DB_NAME,
        autocommit=True,
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
    )


def _connect_server():
    """Connect to the MySQL server WITHOUT selecting a database.

    Used by init_db() so we can create the database before it exists.
    """
    return pymysql.connect(
        host=DB_HOST,
        port=DB_PORT,
        user=DB_USER,
        password=DB_PASSWORD,
        autocommit=True,
        charset="utf8mb4",
    )


def init_db(verbose=True):
    """Create the database and tables if they don't exist yet.

    Idempotent — safe to call on every startup. The CREATE TABLE statements are
    read from schema.sql (single source of truth); the database name honours the
    MYSQL_DATABASE env var rather than the literal name in the .sql file.
    """
    schema_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "schema.sql")
    with open(schema_path, "r", encoding="utf-8") as f:
        raw = f.read()

    # Drop full-line "--" comments, then split into individual statements and
    # keep only the CREATE TABLE ones (we handle the database ourselves below).
    cleaned = "\n".join(
        ln for ln in raw.splitlines() if not ln.strip().startswith("--")
    )
    statements   = [s.strip() for s in cleaned.split(";") if s.strip()]
    table_stmts  = [s for s in statements if s.upper().startswith("CREATE TABLE")]

    conn = _connect_server()
    try:
        with conn.cursor() as cur:
            cur.execute(
                f"CREATE DATABASE IF NOT EXISTS `{DB_NAME}` "
                "CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
            )
            cur.execute(f"USE `{DB_NAME}`")
            for stmt in table_stmts:
                cur.execute(stmt)
    finally:
        conn.close()

    if verbose:
        print(f"[db] Database '{DB_NAME}' ready ({len(table_stmts)} tables ensured).")


# ─── Insert helpers ──────────────────────────────────────────────────────────

_POSITION_SQL = (
    "INSERT INTO node_position (node_id, is_anchor, x, y, altitude, azimuth) "
    "VALUES (%(node_id)s, %(is_anchor)s, %(x)s, %(y)s, %(altitude)s, %(azimuth)s)"
)

_SENSOR_SQL = (
    "INSERT INTO sensor_data "
    "(node_id, is_anchor, temp, pressure, altitude, azimuth, "
    " ax, ay, az, gx, gy, gz, cx, cy, cz, peer_count, peers_json) "
    "VALUES (%(node_id)s, %(is_anchor)s, %(temp)s, %(pressure)s, %(altitude)s, %(azimuth)s, "
    " %(ax)s, %(ay)s, %(az)s, %(gx)s, %(gy)s, %(gz)s, %(cx)s, %(cy)s, %(cz)s, "
    " %(peer_count)s, %(peers_json)s)"
)


def insert_positions(conn, rows):
    """rows: list of dicts with node_id, is_anchor, x, y, altitude, azimuth."""
    if not rows:
        return
    with conn.cursor() as cur:
        cur.executemany(_POSITION_SQL, rows)


def insert_sensors(conn, rows):
    """rows: list of sensor dicts. 'peers' (a list) is JSON-encoded automatically."""
    if not rows:
        return
    prepared = []
    for r in rows:
        r = dict(r)
        peers = r.pop("peers", None)
        r["peers_json"] = json.dumps(peers) if peers is not None else None
        prepared.append(r)
    with conn.cursor() as cur:
        cur.executemany(_SENSOR_SQL, prepared)


def prune_old(conn, minutes):
    """Delete rows older than `minutes` from both data tables.

    Keeps the tables from ballooning under high-rate ingestion. Returns the
    number of rows removed. A non-positive `minutes` is a no-op (keep all).
    Table names are literals here (not user input), so the f-string is safe.
    """
    if not minutes or minutes <= 0:
        return 0
    removed = 0
    with conn.cursor() as cur:
        for table in ("node_position", "sensor_data"):
            cur.execute(
                f"DELETE FROM `{table}` WHERE ts < NOW() - INTERVAL %s MINUTE",
                (minutes,),
            )
            removed += cur.rowcount
    return removed
