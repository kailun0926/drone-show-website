#!/usr/bin/env python3
"""
app.py  —  Local dashboard server for the mesh positioning backend.

Serves a single-page dashboard (static/index.html) and a small JSON API that
reads live data straight from MySQL. Runs on your desktop next to the database.

    pip install -r requirements.txt
    mysql -u root -p < schema.sql        # if not already done
    python dashboard/app.py
    # then open http://localhost:5000

Data appears as soon as serial_to_mysql.py (real hardware) or simulate_nodes.py
(synthetic) starts writing to MySQL.
"""

import os
import sys
import json
from datetime import datetime

from flask import Flask, jsonify, request, send_from_directory

# Allow importing db.py from the project root (one level up from dashboard/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from db import connect, init_db  # noqa: E402

STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

# A node is considered "online" if seen within this many seconds.
ONLINE_WINDOW_S = 12

app = Flask(__name__, static_folder=STATIC_DIR, static_url_path="")


# ─── Helpers ─────────────────────────────────────────────────────────────────

def _query(sql, params=()):
    """Run a read query and return a list of dict rows."""
    conn = connect()
    try:
        with conn.cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()
    finally:
        conn.close()


def _run(cur, sql, params=()):
    """Run a query on an existing cursor — lets one connection serve several."""
    cur.execute(sql, params)
    return cur.fetchall()


def _iso(ts):
    return ts.isoformat() if isinstance(ts, datetime) else ts


def _clean(row):
    """Normalise a row for JSON: booleans, datetimes, JSON columns."""
    out = dict(row)
    if "is_anchor" in out and out["is_anchor"] is not None:
        out["is_anchor"] = bool(out["is_anchor"])
    if "ts" in out:
        out["ts"] = _iso(out["ts"])
    if "peers_json" in out:
        raw = out.pop("peers_json")
        try:
            out["peers"] = json.loads(raw) if raw else []
        except (TypeError, ValueError):
            out["peers"] = []
    return out


# ─── API ─────────────────────────────────────────────────────────────────────

@app.route("/api/overview")
def overview():
    """Latest position + latest sensor reading + online status, per node."""
    # One connection for all three queries instead of three separate ones —
    # keeps the fast (100 ms) dashboard poll cheap, especially over a remote DB.
    conn = connect()
    try:
        with conn.cursor() as cur:
            positions = [_clean(r) for r in _run(cur,
                "SELECT t.* FROM node_position t "
                "JOIN (SELECT node_id, MAX(id) AS mid FROM node_position GROUP BY node_id) m "
                "ON t.id = m.mid"
            )]
            sensors = [_clean(r) for r in _run(cur,
                "SELECT t.* FROM sensor_data t "
                "JOIN (SELECT node_id, MAX(id) AS mid FROM sensor_data GROUP BY node_id) m "
                "ON t.id = m.mid"
            )]
            # last-seen across both tables → online/offline + age
            seen = _run(cur,
                "SELECT node_id, MAX(ts) AS last_seen FROM ("
                "  SELECT node_id, ts FROM node_position "
                "  UNION ALL "
                "  SELECT node_id, ts FROM sensor_data"
                ") u GROUP BY node_id"
            )
    finally:
        conn.close()

    health = []
    for r in seen:
        last = r["last_seen"]
        age = None
        if isinstance(last, datetime):
            age = (datetime.now() - last).total_seconds()
        health.append({
            "node_id":   r["node_id"],
            "last_seen": _iso(last),
            "age_s":     round(age, 1) if age is not None else None,
            "online":    (age is not None and age <= ONLINE_WINDOW_S),
        })

    return jsonify({
        "server_time": datetime.now().isoformat(),
        "online_window_s": ONLINE_WINDOW_S,
        "positions": positions,
        "sensors":   sensors,
        "health":    health,
    })


@app.route("/api/history/positions")
def history_positions():
    """Recent position points per node — used for movement trails on the map.

    Pass ?seconds=N for a short trail window (preferred at high refresh rates),
    or ?minutes=N for a longer one. seconds takes precedence if both are given.
    """
    seconds = request.args.get("seconds", default=None, type=int)
    if seconds is not None:
        window_sql, window_val = "INTERVAL %s SECOND", max(seconds, 1)
        echo = {"seconds": window_val}
    else:
        minutes = request.args.get("minutes", default=10, type=int)
        window_sql, window_val = "INTERVAL %s MINUTE", minutes
        echo = {"minutes": minutes}

    rows = _query(
        "SELECT node_id, is_anchor, x, y, ts FROM node_position "
        "WHERE ts > NOW() - " + window_sql + " ORDER BY ts ASC",
        (window_val,),
    )
    tracks = {}
    for r in rows:
        tracks.setdefault(r["node_id"], []).append({
            "x": r["x"], "y": r["y"], "ts": _iso(r["ts"]),
        })
    return jsonify({**echo, "tracks": tracks})


@app.route("/api/history/sensor")
def history_sensor():
    """Time series of one sensor field across all nodes — for the trend chart."""
    field   = request.args.get("field", default="altitude")
    minutes = request.args.get("minutes", default=30, type=int)

    allowed = {"altitude", "pressure", "temp", "azimuth"}
    if field not in allowed:
        return jsonify({"error": f"field must be one of {sorted(allowed)}"}), 400

    rows = _query(
        f"SELECT node_id, ts, {field} AS value FROM sensor_data "
        "WHERE ts > NOW() - INTERVAL %s MINUTE ORDER BY ts ASC",
        (minutes,),
    )
    series = {}
    for r in rows:
        series.setdefault(r["node_id"], []).append({
            "ts": _iso(r["ts"]), "value": r["value"],
        })
    return jsonify({"field": field, "minutes": minutes, "series": series})


@app.route("/")
def index():
    return send_from_directory(STATIC_DIR, "index.html")


if __name__ == "__main__":
    try:
        init_db()              # create database + tables if needed
    except Exception as e:
        print(f"[dashboard] WARNING: could not initialize database: {e}")
    port = int(os.getenv("DASHBOARD_PORT", "5000"))
    print(f"[dashboard] http://localhost:{port}")
    app.run(host="0.0.0.0", port=port, debug=False, threaded=True)
