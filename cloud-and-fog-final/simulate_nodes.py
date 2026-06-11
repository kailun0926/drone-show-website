#!/usr/bin/env python3
"""
simulate_nodes.py  —  Mesh node data simulator for backend development.

Generates synthetic data that mirrors what the real hardware produces:
  • sensor_data   — raw packets each node broadcasts (~15 Hz, written at 1 s)
  • node_position — stitched position map the aggregator outputs every 5 s

Writes both streams to a local MySQL instance (see schema.sql / db.py).

─── Quick-start ────────────────────────────────────────────────────────────
1.  Install MySQL and make sure it's running.
2.  pip install -r requirements.txt
3.  mysql -u root -p < schema.sql          # one-time table setup
4.  Set MYSQL_* env vars (see db.py) if not using localhost/root defaults.
5.  python simulate_nodes.py
────────────────────────────────────────────────────────────────────────────
"""

import math
import time
import random
from dataclasses import dataclass, field
from typing import List

from db import connect, init_db, insert_positions, insert_sensors

# ─── Database connection ─────────────────────────────────────────────────────
# All MySQL settings live in db.py (configured via MYSQL_* env vars).

# ─── Timing — matches hardware constants ────────────────────────────────────

SOLVE_INTERVAL  = 5.0   # seconds  (aggregator SOLVE_INTERVAL = 5000 ms)
SENSOR_INTERVAL = 1.0   # seconds  (hardware ~67 ms; 1 s is fine for DB dev)

# ─── Node layout ─────────────────────────────────────────────────────────────

@dataclass
class SimNode:
    node_id:   int
    is_anchor: bool
    x:         float
    y:         float
    # sensor readings — updated each cycle
    altitude:  float = 1.2
    azimuth:   int   = 0
    temp:      float = 25.0
    pressure:  int   = 101325
    ax: float = 0.0;  ay: float = 0.0;  az: float = 1.0
    gx: float = 0.0;  gy: float = 0.0;  gz: float = 0.0
    cx: float = 20.0; cy: float = 5.0;  cz: float = -45.0
    peers: List[dict] = field(default_factory=list)

# Three anchors in a rough triangle (metres, origin = 0xAB)
ANCHORS = [
    SimNode(node_id=0xAB, is_anchor=True,  x=0.0, y=0.0),
    SimNode(node_id=0xCD, is_anchor=True,  x=5.0, y=0.0),
    SimNode(node_id=0xEF, is_anchor=True,  x=2.5, y=4.3),
]

# Three mobile nodes that wander inside the anchor triangle
MOBILE_NODES = [
    SimNode(node_id=0x01, is_anchor=False, x=1.0, y=1.5, azimuth=90),
    SimNode(node_id=0x02, is_anchor=False, x=3.0, y=2.0, azimuth=45),
    SimNode(node_id=0x03, is_anchor=False, x=4.0, y=1.0, azimuth=180),
]

ALL_NODES: List[SimNode] = ANCHORS + MOBILE_NODES

# ─── Simulation helpers ───────────────────────────────────────────────────────

BOUNDS    = (-0.5, 5.5, -0.5, 4.8)  # xmin, xmax, ymin, ymax
WALK_STEP = 0.08                     # metres per solve cycle

def _clamp(v, lo, hi):
    return max(lo, min(hi, v))

def rssi_from_distance(d: float) -> int:
    """Inverse of the anchor's rssiToDistance() with TX_POWER=-50, PATH_LOSS=2.7."""
    if d < 0.01:
        d = 0.01
    return int(-50.0 - 10.0 * 2.7 * math.log10(d))

def update_mobile(node: SimNode):
    node.x = _clamp(node.x + random.uniform(-WALK_STEP, WALK_STEP), BOUNDS[0], BOUNDS[1])
    node.y = _clamp(node.y + random.uniform(-WALK_STEP, WALK_STEP), BOUNDS[2], BOUNDS[3])
    node.azimuth = (node.azimuth + random.randint(-8, 8)) % 360

def update_sensors(node: SimNode):
    node.temp     = 24.0  + random.gauss(0, 0.2)
    node.pressure = 101325 + random.randint(-60, 60)
    node.altitude = 44330.0 * (1.0 - (node.pressure / 101325.0) ** 0.1903)
    node.ax = random.gauss(0, 0.03);  node.ay = random.gauss(0, 0.03)
    node.az = 1.0 + random.gauss(0, 0.02)
    node.gx = random.gauss(0, 0.8);   node.gy = random.gauss(0, 0.8)
    node.gz = random.gauss(0, 0.8)
    node.cx = 20.0 + random.gauss(0, 1.2)
    node.cy =  5.0 + random.gauss(0, 1.2)
    node.cz = -45.0 + random.gauss(0, 1.2)

def build_peer_rssi(node: SimNode) -> List[dict]:
    """Returns the RSSI list this node would broadcast (distance-derived)."""
    peers = []
    for other in ALL_NODES:
        if other.node_id == node.node_id:
            continue
        d    = math.hypot(node.x - other.x, node.y - other.y)
        rssi = rssi_from_distance(d)
        if rssi > -90:   # only include nodes in radio range
            peers.append({"id": other.node_id, "rssi": rssi})
    return peers[:8]     # MAX_PEERS = 8

# ─── MySQL write helpers ──────────────────────────────────────────────────────

def write_sensor_batch(conn, nodes: List[SimNode]):
    """Writes one sensor_data row per node (mirrors the 67 ms broadcasts)."""
    rows = []
    for n in nodes:
        rows.append({
            "node_id":   f"0x{n.node_id:02X}",
            "is_anchor": n.is_anchor,
            "temp":      round(n.temp, 2),
            "pressure":  n.pressure,
            "altitude":  round(n.altitude, 3),
            "azimuth":   n.azimuth,
            "ax": round(n.ax, 4), "ay": round(n.ay, 4), "az": round(n.az, 4),
            "gx": round(n.gx, 4), "gy": round(n.gy, 4), "gz": round(n.gz, 4),
            "cx": round(n.cx, 3), "cy": round(n.cy, 3), "cz": round(n.cz, 3),
            "peer_count": len(n.peers),
            # Stored as a JSON column: [{"id":"0xCD","rssi":-55}, ...]
            "peers": [{"id": f"0x{p['id']:02X}", "rssi": p["rssi"]} for p in n.peers],
        })
    insert_sensors(conn, rows)

def write_position_map(conn, nodes: List[SimNode]):
    """Writes the stitched position_map (aggregator output) to MySQL."""
    rows = []
    for n in nodes:
        rows.append({
            "node_id":   f"0x{n.node_id:02X}",
            "is_anchor": n.is_anchor,
            "x":         round(n.x, 3),
            "y":         round(n.y, 3),
            "altitude":  round(n.altitude, 3),
            "azimuth":   n.azimuth,
        })
    insert_positions(conn, rows)

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    try:
        init_db()              # create database + tables if needed
        conn = connect()
    except Exception as e:
        print(f"[ERROR] Cannot reach MySQL: {e}")
        print("        Check that MySQL is running and your MYSQL_* env vars")
        print("        are correct (see db.py).")
        return

    print(f"[SIM] Connected to MySQL")
    print(f"[SIM] {len(ANCHORS)} anchors, {len(MOBILE_NODES)} mobile nodes")
    print(f"[SIM] sensor_data every {SENSOR_INTERVAL}s  |  position_map every {SOLVE_INTERVAL}s")
    print(f"[SIM] Ctrl-C to stop.\n")

    last_sensor = 0.0
    last_solve  = 0.0

    while True:
        now = time.time()

        # ── Sensor data burst ──────────────────────────────────────────────
        if now - last_sensor >= SENSOR_INTERVAL:
            last_sensor = now
            for n in ALL_NODES:
                update_sensors(n)
                n.peers = build_peer_rssi(n)
            write_sensor_batch(conn, ALL_NODES)
            print(f"[sensor_data]   wrote {len(ALL_NODES)} rows")

        # ── Position map (aggregator solve output) ─────────────────────────
        if now - last_solve >= SOLVE_INTERVAL:
            last_solve = now
            for n in MOBILE_NODES:
                update_mobile(n)
            write_position_map(conn, ALL_NODES)
            positions = "  ".join(
                f"0x{n.node_id:02X}({n.x:+.2f},{n.y:+.2f})" for n in ALL_NODES
            )
            print(f"[node_position] wrote {len(ALL_NODES)} rows  —  {positions}")

        time.sleep(0.1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[SIM] Stopped.")
