#!/usr/bin/env python3
"""
serial_to_mysql.py  —  USB-serial bridge from the aggregator ESP32 to MySQL.

The aggregator does ALL the ESP-NOW work and stays pinned to the mesh radio
channel, so it cannot also talk to a database. Instead it prints the stitched
positions over USB serial, and this script (running on your desktop) reads them
and INSERTs them into MySQL. Your desktop is the "gateway".

The aggregator frames its output so we can pick data out of ordinary debug text:
    INFLUX <line-protocol>     ← one data line per node
    INFLUX_END                 ← end of a batch; write to the DB now
Any other line is human-readable debug and is echoed to the console.

(The "INFLUX" prefix is just the wire marker the firmware happens to use; the
data lands in MySQL regardless.)

Whatever you type here is forwarded back to the ESP, so you can still answer the
aggregator's mirror y/n prompt while this script is running.

─── Quick-start ────────────────────────────────────────────────────────────
1.  pip install -r requirements.txt
2.  mysql -u root -p < schema.sql          # one-time table setup
3.  Set MYSQL_* env vars (see db.py) if not using localhost/root defaults.
4.  Plug in the aggregator over USB.
5.  python serial_to_mysql.py [SERIAL_PORT]
        e.g.  python serial_to_mysql.py COM5
              python serial_to_mysql.py /dev/ttyUSB0
    With no argument it tries to auto-detect the port.
────────────────────────────────────────────────────────────────────────────
"""

import os
import sys
import time
import threading

import serial
import serial.tools.list_ports

from db import connect, init_db, insert_positions, insert_sensors, prune_old

# ─── Serial / framing config ─────────────────────────────────────────────────

SERIAL_BAUD = int(os.getenv("SERIAL_BAUD", "115200"))

DATA_PREFIX     = "INFLUX "       # must match the aggregator firmware
BATCH_END       = "INFLUX_END"
MAX_BATCH_LINES = 64              # safety cap if an END marker is ever missed

# ─── Retention ───────────────────────────────────────────────────────────────
# At high refresh rates the tables grow fast; trim rows older than this.
RETENTION_MIN = int(os.getenv("RETENTION_MINUTES", "60"))   # 0 = keep everything
PRUNE_EVERY_S = int(os.getenv("PRUNE_EVERY_S", "60"))       # how often to trim


def find_serial_port() -> str:
    """Use the CLI arg if given, else pick the first likely USB-serial port."""
    if len(sys.argv) > 1:
        return sys.argv[1]

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        sys.exit("[!] No serial ports found. Plug in the aggregator, or pass the "
                 "port name explicitly:  python serial_to_mysql.py COM5")

    for p in ports:
        desc = (p.description or "").lower()
        if any(k in desc for k in ("usb", "uart", "cp210", "ch340", "ch910", "silicon")):
            print(f"[serial] Auto-selected {p.device}  ({p.description})")
            return p.device

    print(f"[serial] Auto-selected {ports[0].device}  ({ports[0].description})")
    return ports[0].device


def parse_line_protocol(line: str):
    """
    Parse one InfluxDB line-protocol record into a row dict, e.g.:
        node_position,node_id=0xAB,is_anchor=true x=0.0,y=0.0,altitude=1.2,azimuth=45
    Returns a dict {node_id, is_anchor, x, y, altitude, azimuth} or None.
    """
    try:
        head, fields = line.split(" ", 1)        # tags | fields
    except ValueError:
        return None

    tag_parts = head.split(",")                   # [measurement, tag=val, ...]
    if not tag_parts or tag_parts[0] != "node_position":
        return None

    row = {"node_id": None, "is_anchor": None,
           "x": None, "y": None, "altitude": None, "azimuth": None}

    for kv in tag_parts[1:]:
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        if k == "node_id":
            row["node_id"] = v
        elif k == "is_anchor":
            row["is_anchor"] = (v.lower() == "true")

    for kv in fields.split(","):
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        if k == "azimuth":
            row[k] = int(float(v))
        elif k in ("x", "y", "altitude"):
            row[k] = float(v)

    if row["node_id"] is None or row["is_anchor"] is None:
        return None
    return row


_SENSOR_FLOAT_FIELDS = {"temp", "altitude", "ax", "ay", "az",
                        "gx", "gy", "gz", "cx", "cy", "cz"}
_SENSOR_INT_FIELDS   = {"pressure", "azimuth", "peer_count"}


def _parse_peers(v: str):
    """Decode a peers field like "0xCD:-55|0xEF:-60" into [{id, rssi}, ...]."""
    v = v.strip().strip('"')
    if not v:
        return []
    out = []
    for tok in v.split("|"):
        if ":" not in tok:
            continue
        pid, rssi = tok.rsplit(":", 1)
        try:
            out.append({"id": pid, "rssi": int(rssi)})
        except ValueError:
            continue
    return out


def parse_sensor_protocol(line: str):
    """
    Parse a sensor_data line-protocol record into a row dict for insert_sensors().
    e.g.: sensor_data,node_id=0x01,is_anchor=false temp=24.5,pressure=101325,...
    Returns a dict, or None if it isn't a valid sensor_data line.
    """
    try:
        head, fields = line.split(" ", 1)
    except ValueError:
        return None

    tag_parts = head.split(",")
    if not tag_parts or tag_parts[0] != "sensor_data":
        return None

    row = {k: None for k in (
        "node_id", "is_anchor", "temp", "pressure", "altitude", "azimuth",
        "ax", "ay", "az", "gx", "gy", "gz", "cx", "cy", "cz",
        "peer_count", "peers")}

    for kv in tag_parts[1:]:
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        if k == "node_id":
            row["node_id"] = v
        elif k == "is_anchor":
            row["is_anchor"] = (v.lower() == "true")

    # peers is a quoted string with "|" separators, so a plain comma-split of the
    # field list is safe (its value contains no commas).
    for kv in fields.split(","):
        if "=" not in kv:
            continue
        k, v = kv.split("=", 1)
        if k in _SENSOR_FLOAT_FIELDS:
            row[k] = float(v)
        elif k in _SENSOR_INT_FIELDS:
            row[k] = int(float(v))
        elif k == "peers":
            row["peers"] = _parse_peers(v)

    if row["node_id"] is None or row["is_anchor"] is None:
        return None
    return row


def stdin_forwarder(ser: serial.Serial):
    """Forward console input to the ESP so the mirror y/n prompt still works."""
    try:
        for line in sys.stdin:
            ser.write(line.encode("utf-8", errors="ignore"))
    except Exception:
        pass  # stdin closed / not a tty — fine, just stop forwarding


def main():
    port = find_serial_port()

    try:
        ser = serial.Serial(port, SERIAL_BAUD, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"[!] Could not open {port}: {e}")

    try:
        init_db()              # create database + tables if needed
        conn = connect()
    except Exception as e:
        ser.close()
        sys.exit(f"[!] Could not connect to MySQL: {e}")

    print(f"[serial] Listening on {port} @ {SERIAL_BAUD} baud")
    print(f"[mysql]  Connected — writing to node_position + sensor_data")

    threading.Thread(target=stdin_forwarder, args=(ser,), daemon=True).start()

    pos_batch = []
    sensor_batch = []

    def flush():
        if pos_batch:
            try:
                insert_positions(conn, pos_batch)
                print(f"[mysql]  Inserted {len(pos_batch)} position rows.")
            except Exception as e:
                print(f"[mysql]  Position insert failed: {e}")
            finally:
                pos_batch.clear()
        if sensor_batch:
            try:
                insert_sensors(conn, sensor_batch)
                print(f"[mysql]  Inserted {len(sensor_batch)} sensor rows.")
            except Exception as e:
                print(f"[mysql]  Sensor insert failed: {e}")
            finally:
                sensor_batch.clear()

    last_prune = time.monotonic()

    try:
        while True:
            # Periodic retention cleanup — runs even when the serial line is idle.
            now_s = time.monotonic()
            if RETENTION_MIN > 0 and now_s - last_prune >= PRUNE_EVERY_S:
                last_prune = now_s
                try:
                    n = prune_old(conn, RETENTION_MIN)
                    if n:
                        print(f"[mysql]  Pruned {n} rows older than {RETENTION_MIN} min.")
                except Exception as e:
                    print(f"[mysql]  Prune failed: {e}")

            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")

            if line == BATCH_END:
                flush()
            elif line.startswith(DATA_PREFIX):
                payload = line[len(DATA_PREFIX):]
                measurement = payload.split(",", 1)[0]
                if measurement == "node_position":
                    row = parse_line_protocol(payload)
                    if row:
                        pos_batch.append(row)
                elif measurement == "sensor_data":
                    row = parse_sensor_protocol(payload)
                    if row:
                        sensor_batch.append(row)
                if len(pos_batch) + len(sensor_batch) >= MAX_BATCH_LINES:
                    print("[mysql]  Batch cap hit — flushing early.")
                    flush()
            else:
                print(f"[esp] {line}")

    except KeyboardInterrupt:
        print("\n[serial] Stopping.")
    finally:
        flush()
        ser.close()
        conn.close()


if __name__ == "__main__":
    main()
