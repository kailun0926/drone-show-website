#!/usr/bin/env python3
"""
debug_mysql.py  —  Diagnose MySQL connectivity for the mesh backend.

Uses the SAME settings as the rest of the project (imported from db.py), so it
exercises the exact connection your real scripts use. It checks, in order:

  1. Can we reach the MySQL *server* at all? (host/port/credentials)
  2. Does the target *database* exist and can we USE it?
  3. Are the expected tables present, and how many rows do they hold?

Run it with the same Python you run the other scripts with:

    python debug_mysql.py
"""

import sys

try:
    import pymysql
except ModuleNotFoundError:
    sys.exit("[FAIL] PyMySQL is not installed in this interpreter.\n"
             "       Fix:  python -m pip install pymysql")

# Pull the project's real connection settings so we test what actually runs.
from db import DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_NAME


def mask(pw: str) -> str:
    if not pw:
        return "(empty)"
    return pw[0] + "*" * (len(pw) - 1)


def hint_for(err) -> str:
    """Map common MySQL/PyMySQL errors to a plain-language cause + fix."""
    code = err.args[0] if err.args else None
    return {
        2003: "Can't reach the server. Is MySQL running? Is the host/port right? "
              "Firewall blocking it?",
        2005: "Unknown host — check MYSQL_HOST.",
        1045: "Access denied — wrong username or password (check MYSQL_USER / "
              "MYSQL_PASSWORD).",
        1049: "The server is reachable, but the database doesn't exist yet. "
              "init_db() (run any main script once) will create it, or create it "
              "by hand.",
        1044: "User exists but lacks privileges on this database.",
    }.get(code, "See the error message above.")


def main():
    print("=" * 60)
    print(" MySQL connection diagnostics")
    print("=" * 60)
    print(f"  host     : {DB_HOST}")
    print(f"  port     : {DB_PORT}")
    print(f"  user     : {DB_USER}")
    print(f"  password : {mask(DB_PASSWORD)}")
    print(f"  database : {DB_NAME}")
    print("-" * 60)

    # ── Step 1: reach the server (no database selected) ──────────────────────
    print("[1/3] Connecting to the MySQL server (no database)...")
    try:
        server = pymysql.connect(
            host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASSWORD,
            connect_timeout=5, charset="utf8mb4",
        )
    except pymysql.err.MySQLError as e:
        print(f"      [FAIL] {e}")
        print(f"      → {hint_for(e)}")
        sys.exit(1)

    with server.cursor() as cur:
        cur.execute("SELECT VERSION()")
        version = cur.fetchone()[0]
    print(f"      [OK] Server reachable. MySQL version: {version}")

    # ── Step 2: does the target database exist? ──────────────────────────────
    print(f"[2/3] Checking database '{DB_NAME}'...")
    with server.cursor() as cur:
        cur.execute("SHOW DATABASES LIKE %s", (DB_NAME,))
        exists = cur.fetchone() is not None
    server.close()

    if not exists:
        print(f"      [WARN] Database '{DB_NAME}' does not exist yet.")
        print("      → It will be created automatically the first time you run "
              "serial_to_mysql.py / simulate_nodes.py / dashboard/app.py "
              "(they call init_db()).")
        sys.exit(0)
    print(f"      [OK] Database '{DB_NAME}' exists.")

    # ── Step 3: connect to the database and inspect tables ───────────────────
    print(f"[3/3] Connecting to '{DB_NAME}' and inspecting tables...")
    try:
        conn = pymysql.connect(
            host=DB_HOST, port=DB_PORT, user=DB_USER, password=DB_PASSWORD,
            database=DB_NAME, connect_timeout=5, charset="utf8mb4",
        )
    except pymysql.err.MySQLError as e:
        print(f"      [FAIL] {e}")
        print(f"      → {hint_for(e)}")
        sys.exit(1)

    with conn.cursor() as cur:
        cur.execute("SHOW TABLES")
        tables = [row[0] for row in cur.fetchall()]
        print(f"      [OK] Connected. Tables present: {tables or '(none)'}")

        for t in ("node_position", "sensor_data"):
            if t in tables:
                cur.execute(f"SELECT COUNT(*) FROM `{t}`")
                count = cur.fetchone()[0]
                print(f"           - {t}: {count} rows")
            else:
                print(f"           - {t}: MISSING (will be created by init_db())")
    conn.close()

    print("-" * 60)
    print(" [DONE] MySQL is reachable and usable. If the app still shows no")
    print("        data, the problem is upstream (no rows being written yet).")


if __name__ == "__main__":
    main()
