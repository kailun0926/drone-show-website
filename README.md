# 📡 Mesh Positioning Network — Term Project Showcase

A self-organizing indoor positioning system built on **ESP32** boards and
**ESP-NOW**. A swarm of battery-powered nodes and fixed anchors cooperatively
work out where everyone is — no GPS, no fixed infrastructure beyond the anchors
— and stream the result to a local **MySQL** database and a live **web
dashboard**.

> Nodes measure who they can hear (RSSI) and their own orientation. Anchors turn
> that into local maps. An aggregator stitches the local maps into one global map
> and hands it to a desktop, which stores it and visualizes it in real time.

🌐 **Project website:** <https://kailun0926.github.io/drone-show-website/> —
an Angular site introducing the system, its 3-level Edge/Fog/Cloud architecture,
future outlook, and the team. (Site source lives in `src/`; the system itself
lives in [`cloud-and-fog-final/`](cloud-and-fog-final).)

---

## ✨ Highlights

- **Fog-computing architecture** — distance solving is pushed out to the anchors
  instead of being centralized, so the aggregator only stitches pre-solved zones.
- **Infrastructure-free ranging** — positions are derived from radio signal
  strength (RSSI) plus a spring-relaxation multilateration solver.
- **Tilt-compensated heading** — each node fuses accelerometer + magnetometer
  data for a true compass bearing.
- **End-to-end pipeline** — firmware → USB serial → Python bridge → MySQL →
  Flask dashboard, all runnable **without hardware** via a built-in simulator.
- **Live dashboard** — position map, per-node sensor cards, network health, and
  historical trend charts. No external dependencies; runs fully offline.

---

## 🗺️ System Architecture

```
   ┌─────────┐   sensor_data (ESP-NOW, ch.1)   ┌──────────┐
   │  NODES  │ ──────────────────────────────▶ │ ANCHORS  │
   │ (mobile)│ ◀───── position_map ──────────  │ (fixed)  │
   └─────────┘                                  └────┬─────┘
        ▲                                            │ zone_summary
        │ position_map (final map)                   ▼  (locally solved)
        │                                       ┌────────────┐
        └────────────────────────────────────  │ AGGREGATOR │
                                                │  (ch.1)    │
                                                └────┬───────┘
                                                     │ USB serial (line protocol)
                                                     ▼
                                         ┌────────────────────────┐
                                         │   DESKTOP (gateway)     │
                                         │  serial_to_mysql.py ──▶ │── MySQL ──▶ dashboard/app.py
                                         └────────────────────────┘                  │
                                                                                      ▼
                                                                          http://localhost:5000
```

**Why USB instead of Wi-Fi for the aggregator?** The ESP-NOW radio is pinned to
one channel (1), but joining a Wi-Fi router would force the radio onto the
router's channel and break the mesh. So the aggregator stays on the mesh channel
and talks to the database over a USB cable; the desktop is the "gateway" to the
network and database.

---

## 📂 Repository Structure

```
.
├── src/                         # Angular project website (GitHub Pages)
├── .github/workflows/           # CI: build & deploy the website to Pages
│
└── cloud-and-fog-final/         # The positioning system itself
    ├── node_firmware/           # Mobile node firmware (sensors + ESP-NOW)
    ├── anchor_firmware/         # Anchor firmware (local zone solver)
    ├── aggregator/              # Aggregator firmware (stitches zones, streams over USB)
    │
    ├── schema.sql               # MySQL tables (node_position, sensor_data)
    ├── db.py                    # Shared MySQL connection + insert helpers
    ├── serial_to_mysql.py       # Hardware → MySQL bridge (reads USB serial)
    ├── simulate_nodes.py        # Synthetic data generator (no hardware needed)
    │
    ├── dashboard/
    │   ├── app.py               # Flask server + JSON API
    │   └── static/              # index.html, style.css, app.js (the dashboard UI)
    │       └── vendor/          # bundled Chart.js + Inter font (offline)
    │
    ├── run_demo.bat             # One-click: simulator + dashboard
    ├── run_dashboard.bat        # One-click: dashboard only
    └── requirements.txt         # Python dependencies
```

---

## 🔧 Hardware

Each **node** (XIAO ESP32-C3 class board) carries:

| Component        | Role                                  |
|------------------|---------------------------------------|
| ESP32            | Compute + ESP-NOW radio               |
| BMP085 / BMP180  | Barometric pressure → altitude / floor|
| MPU6500          | Accelerometer + gyroscope (tilt)      |
| HMC5883L         | Magnetometer (compass heading)        |
| NeoPixel ring    | Status / proximity indicator          |

**Anchors** are the same class of board placed at known fixed points.
The **aggregator** is one more ESP32 connected to the desktop via USB.

---

## ⚙️ How It Works

1. **Nodes** broadcast `sensor_data` (~15 Hz): pressure, IMU, compass heading,
   and the RSSI of every peer they can hear.
2. **Anchors** listen, convert RSSI → distance (log-distance path-loss model),
   and run a local **spring-relaxation solver** to place everything they can
   hear into an anchor-local coordinate frame. They send a compact `zone_summary`
   to the aggregator.
3. The **aggregator** solves the anchors' global positions from inter-anchor
   distances, rotates each anchor's local zone into the global frame, averages
   overlapping observations, and produces one `position_map`. It broadcasts the
   map back to the swarm **and** streams it over USB as InfluxDB-style line
   protocol.
4. The **desktop bridge** (`serial_to_mysql.py`) parses that stream and writes
   it into MySQL.
5. The **dashboard** reads MySQL and renders everything live.

---

## 🚀 Getting Started

All commands below are run inside `cloud-and-fog-final/`.

### 1. Prerequisites
- Python 3.9+
- MySQL server running locally
- (For real hardware) Arduino IDE / `arduino-cli` with the ESP32 core and the
  Adafruit BMP085, MPU6500_WE, Adafruit HMC5883, and Adafruit NeoPixel libraries.

### 2. Install
```bash
cd cloud-and-fog-final
pip install -r requirements.txt
```
The database and tables are **created automatically** on first run (see
`init_db()` in `db.py`), so there's no manual setup step. If you'd rather create
them by hand, `schema.sql` is still provided: `mysql -u root -p < schema.sql`.

### 3. Run the demo (no hardware)
**Windows:** double-click **`run_demo.bat`** — it starts the simulator and the
dashboard and opens your browser.

**Any OS:**
```bash
python simulate_nodes.py      # terminal 1
python dashboard/app.py       # terminal 2  → http://localhost:5000
```

### 4. Run with real hardware
1. Flash `node_firmware`, `anchor_firmware`, and `aggregator` to their boards.
2. Plug the aggregator into the desktop via USB.
3. Start the bridge, then the dashboard:
   ```bash
   python serial_to_mysql.py          # auto-detects the port, or pass e.g. COM5
   ```
   then double-click **`run_dashboard.bat`** (or `python dashboard/app.py`).

> Only one program can hold the serial port at a time — close the Arduino Serial
> Monitor before running the bridge.

---

## 🖥️ Dashboard

A single-page web app served by Flask, reading live data straight from MySQL.
**Light/dark theme toggle** (remembers your choice and respects your OS setting),
and a **fully offline** build — Chart.js and the Inter font are bundled locally in
`dashboard/static/vendor/`, so it works on a LAN with no internet.

| Section            | What it shows                                                                 |
|--------------------|------------------------------------------------------------------------------|
| **KPI row**        | Total nodes, online now, anchors, mobile nodes — at-a-glance counts          |
| **Position Map**   | High-DPI top-down 2D map: anchors (squares), nodes (circles) + heading, movement trails, north indicator |
| **Sensor Cards**   | Temp, pressure, altitude, heading, peer count per node — each with inline **altitude & pressure sparklines** |
| **Network Health** | Online/offline status, role, last-seen, peer count                           |
| **Historical Trend** | Chart.js line chart; selectable metric (altitude / pressure / temp / heading) and window (10/30/60 min). Breaks the line across data gaps, with a **Smooth** toggle (moving average) for noisy signals |

### Refresh rate

The dashboard polls on two timers, defined at the top of
`dashboard/static/app.js`:

```js
const OVERVIEW_MS = 2000;   // live: KPIs, health, cards, map
const HISTORY_MS  = 8000;   // trails, trend chart, sparklines
```

Lower these to refresh faster (e.g. `1000` / `4000`). Note that the **position
map only changes as fast as new data is written** — the aggregator solves every
`SOLVE_INTERVAL` (5 s by default), so polling faster than that re-fetches
identical rows. For genuinely faster position updates, lower `SOLVE_INTERVAL` in
the aggregator firmware too. Each poll opens a short-lived MySQL connection, so
very aggressive intervals (sub-500 ms) add load for little benefit.

---

## 🔩 Configuration

All configurable via environment variables (sensible localhost defaults):

| Variable          | Default       | Used by                         |
|-------------------|---------------|---------------------------------|
| `MYSQL_HOST`      | `localhost`   | all DB scripts                  |
| `MYSQL_PORT`      | `3306`        | all DB scripts                  |
| `MYSQL_USER`      | `root`        | all DB scripts                  |
| `MYSQL_PASSWORD`  | *(empty)*     | all DB scripts                  |
| `MYSQL_DATABASE`  | `mesh_nodes`  | all DB scripts                  |
| `SERIAL_BAUD`     | `115200`      | `serial_to_mysql.py`            |
| `DASHBOARD_PORT`  | `5000`        | `dashboard/app.py`              |

---

## 🌐 Project Website

The Angular site in `src/` is built and deployed to GitHub Pages automatically
on every push to `main` (see `.github/workflows/deploy.yml`).

```bash
npm install
npm start        # dev server → http://localhost:4200
```

---

## 🛠️ Engineering Notes

A few non-obvious bugs were found and fixed during development — good examples of
embedded-systems gotchas:

- **Radio-channel conflict:** the aggregator's Wi-Fi connection silently moved
  its radio off the ESP-NOW channel, deafening it to the mesh. Resolved by
  decoupling the database link onto USB.
- **One-shot map mirror:** a coordinate flip was guarded so it only applied once,
  but the map is rebuilt every solve cycle — so the fix had to reapply each cycle.
- **Cross-task data race:** anchor zone arrays were written from the ESP-NOW
  receive callback while the solver read them. Fixed with a queue hand-off so all
  state changes happen on one task.

---

## 📜 Notes & Limitations

- The dashboard uses Flask's development server — ideal for local/LAN use. A
  production deployment would put it behind a proper WSGI server.
- RSSI-based ranging is inherently noisy; accuracy depends on anchor placement,
  calibration of the path-loss model, and environment.
- The aggregator and all peers must share one ESP-NOW channel (default 1).

---

*Built as a term project demonstrating distributed sensing, embedded firmware,
sensor fusion, and a full data-to-dashboard pipeline.*
