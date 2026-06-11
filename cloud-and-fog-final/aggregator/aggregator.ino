// =====================================================
//  AGGREGATOR FIRMWARE (FOG COMPUTING REVISION)
//  No longer processes raw RSSI. Instead receives
//  pre-solved zone_summary packets from each anchor,
//  stitches them into one global map, then broadcasts.
// =====================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// =====================================================
//  CONFIG
// =====================================================

#define MAX_NODES       8
#define MAX_PEERS       8
#define MAX_ANCHORS     8
#define ANCHOR_ORIGIN   0x80

// ESP-NOW radio channel — must match every anchor and node.
#define ESPNOW_CHANNEL  1

#define N_ITER          400
#define LEARN_RATE      0.008f

#define SOLVE_INTERVAL  100      // ms between solve + broadcast + host stream
#define WARMUP_MS       2000     // startup grace period before the first solve

// When several anchors each estimate a node's position, combine them weighted
// by 1/(dist² + eps) so the NEAREST anchor dominates — a node then tracks
// toward whichever anchor it's closest to, instead of sitting at the centroid.
// This is purely relative (no distance calibration needed). Smaller eps =
// stronger pull to the nearest anchor (and a bit jumpier).
#define PROXIMITY_EPS   0.5f

// =====================================================
//  KNOWN ANCHOR POSITIONS
//  Anchors are FIXED reference points — measure where you place each one and
//  list it here (in metres). Pick one anchor as the origin (0,0) and measure
//  the others' X/Y from it; the axes are your choice, just stay consistent.
//  Each board prints its ID on boot: "Anchor ID (MAC last byte): 0x..".
//  Anchors NOT listed here are ignored by the solver.
//
//      ┌─ REPLACE the ids and coordinates below with YOUR anchors ─┐
// =====================================================

struct AnchorPos { uint8_t id; float x, y; };

const AnchorPos KNOWN_ANCHORS[] = {
    { 0x90, 0.00f, 0.00f },   // Anchor 1 (origin)
    { 0x84, 2.50f, 0.00f },   // Anchor 2
    { 0x80, 1.25f, 2.00f },   // Anchor 3
};
const unsigned N_KNOWN_ANCHORS = sizeof(KNOWN_ANCHORS) / sizeof(KNOWN_ANCHORS[0]);

// =====================================================
//  DATA STRUCTURES
// =====================================================

// Inbound from anchors — a locally-solved zone
typedef struct __attribute__((packed)) zone_node {
    uint8_t id;
    float   lx, ly;
    float   altitude;
    int16_t azimuth;
    bool    isAnchor;
} zone_node;

typedef struct __attribute__((packed)) zone_summary {
    uint8_t   anchor_id;
    uint8_t   nodeCount;
    zone_node nodes[MAX_PEERS];
} zone_summary;

// Outbound to all nodes — final stitched map
typedef struct __attribute__((packed)) node_position {
    uint8_t id;
    float   x, y;
    float   altitude;
    int16_t azimuth;
    bool    isAnchor;
} node_position;

typedef struct __attribute__((packed)) position_map {
    uint8_t       nodeCount;
    node_position positions[MAX_NODES];
} position_map;

// Raw sensor broadcast from nodes/anchors — identical layout to the node
// firmware. The aggregator is on the mesh channel, so it hears these directly
// and forwards them to the host for the sensor_data table.
typedef struct __attribute__((packed)) peer_rssi {
    uint8_t id;
    int8_t  rssi;
} peer_rssi;

typedef struct __attribute__((packed)) sensor_data {
    uint8_t   board_id;
    bool      isAnchor;
    float     temp;
    int32_t   pressure;
    float     ax, ay, az;
    float     gx, gy, gz;
    float     cx, cy, cz;
    int16_t   azimuth;
    uint8_t   peerCount;
    peer_rssi peers[MAX_PEERS];
} sensor_data;

// =====================================================
//  NODE REGISTRY
// =====================================================

struct NodeInfo {
    uint8_t  id;
    bool     active;
    bool     isAnchor;
    float    x, y;
    float    altitude;
    int16_t  azimuth;
    int32_t  pressure;
};

NodeInfo nodes[MAX_NODES];
uint8_t  nodeCount = 0;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// =====================================================
//  ZONE BUFFER
//  Stores the most recent summary from each anchor.
// =====================================================

zone_summary zoneSummaries[MAX_ANCHORS];
bool         zoneReceived[MAX_ANCHORS];
uint8_t      anchorCount = 0;

QueueHandle_t zoneQueue;

// ── Sensor registry ──────────────────────────────────────────────────────────
// Most recent sensor_data heard from each board, forwarded to the host so the
// sensor_data table populates from real hardware (not just the simulator).
#define MAX_SENSORS (MAX_NODES + MAX_ANCHORS)
sensor_data sensorRecords[MAX_SENSORS];
bool        sensorValid[MAX_SENSORS];
uint8_t     sensorCount = 0;

QueueHandle_t sensorQueue;

int getZoneBufferIndex(uint8_t anchor_id) {
    for (int i = 0; i < anchorCount; i++)
        if (zoneSummaries[i].anchor_id == anchor_id) return i;
    if (anchorCount >= MAX_ANCHORS) return -1;
    int idx = anchorCount++;
    zoneReceived[idx]            = false;
    zoneSummaries[idx].anchor_id = anchor_id;
    return idx;
}

int getSensorIndex(uint8_t id) {
    for (int i = 0; i < sensorCount; i++)
        if (sensorRecords[i].board_id == id) return i;
    if (sensorCount >= MAX_SENSORS) return -1;
    int idx = sensorCount++;
    sensorValid[idx]            = false;
    sensorRecords[idx].board_id = id;
    return idx;
}

// =====================================================
//  HELPERS
// =====================================================

int findByLogicalId(uint8_t id) {
    for (int i = 0; i < nodeCount; i++)
        if (nodes[i].id == id) return i;
    return -1;
}

// =====================================================
//  ESP-NOW CALLBACKS
// =====================================================

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len == sizeof(zone_summary)) {
        zone_summary summary;
        memcpy(&summary, data, sizeof(summary));
        xQueueSendFromISR(zoneQueue, &summary, NULL);
    } else if (len == sizeof(sensor_data)) {
        // Raw sensor broadcast from a node/anchor — queue it for the host stream.
        // Queued (not stored here) so the registry is only touched in loop().
        sensor_data sd;
        memcpy(&sd, data, sizeof(sd));
        xQueueSendFromISR(sensorQueue, &sd, NULL);
    }
    // other sizes (e.g. our own position_map) are ignored
}

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS)
        Serial.println("[!] Map broadcast failed");
}

// =====================================================
//  ZONE STITCHING
//
//  Phase 1 — Anchor global positions (FIXED)
//    Anchors are known, fixed reference points. We look up
//    each heard anchor's coordinates from KNOWN_ANCHORS
//    instead of solving them from RSSI — so they no longer
//    drift, and they define a stable global frame.
//
//  Phase 2 — Transform each zone into global frame
//    For each anchor zone, the rotation from local→global
//    is computed by comparing the local direction to a
//    visible anchor against that anchor's known global
//    direction. Multiple references are averaged using
//    a circular mean for stability.
//
//  Phase 3 — Average across overlapping zones
//    Mobile nodes visible to multiple anchors have their
//    global positions averaged across all contributions.
// =====================================================

void stitchZones() {

    // ── Phase 1: Build anchor global positions ──────────────────────────

    uint8_t anchorIds[MAX_ANCHORS];
    float   anchorX[MAX_ANCHORS], anchorY[MAX_ANCHORS];
    float   anchorAlt[MAX_ANCHORS];
    uint8_t nAnchors = 0;
    memset(anchorAlt, 0, sizeof(anchorAlt));

    // Look up an anchor's fixed position in KNOWN_ANCHORS.
    auto knownPos = [&](uint8_t id, float &x, float &y) -> bool {
        for (unsigned k = 0; k < N_KNOWN_ANCHORS; k++)
            if (KNOWN_ANCHORS[k].id == id) {
                x = KNOWN_ANCHORS[k].x; y = KNOWN_ANCHORS[k].y; return true;
            }
        return false;
    };

    // Register an anchor by id using its fixed position. Anchors not listed in
    // KNOWN_ANCHORS are ignored (returns -1).
    auto anchorIdx = [&](uint8_t id) -> int {
        for (int i = 0; i < nAnchors; i++) if (anchorIds[i] == id) return i;
        float kx, ky;
        if (!knownPos(id, kx, ky)) return -1;
        if (nAnchors >= MAX_ANCHORS) return -1;
        int i = nAnchors++;
        anchorIds[i] = id;
        anchorX[i]   = kx;
        anchorY[i]   = ky;
        return i;
    };

    // Register every anchor we hear about (zone senders + anchors seen inside
    // zones) and capture each one's reported altitude.
    for (int z = 0; z < anchorCount; z++) {
        if (!zoneReceived[z]) continue;
        anchorIdx(zoneSummaries[z].anchor_id);
        for (int n = 0; n < zoneSummaries[z].nodeCount; n++) {
            zone_node &zn = zoneSummaries[z].nodes[n];
            if (!zn.isAnchor) continue;
            int bi = anchorIdx(zn.id);
            if (bi >= 0) anchorAlt[bi] = zn.altitude;
        }
    }

    if (nAnchors == 0)
        Serial.println("[STITCH] No known anchors heard — check KNOWN_ANCHORS ids.");

    // ── Phase 2: Transform zone nodes to global frame ───────────────────

    // Accumulator: each mobile node collects global position contributions
    // from every zone it appears in; they are averaged in Phase 3.
    struct Accumulator {
        float   x, y, alt;     // x, y are proximity-weighted sums (÷ wsum in Phase 3)
        int16_t azm;
        float   wsum;          // sum of proximity weights
        int     count;
    };
    Accumulator acc[MAX_NODES];
    uint8_t     accId[MAX_NODES];
    uint8_t     accCount = 0;
    memset(acc,   0, sizeof(acc));
    memset(accId, 0, sizeof(accId));

    auto getAcc = [&](uint8_t id) -> int {
        for (int i = 0; i < accCount; i++) if (accId[i] == id) return i;
        if (accCount >= MAX_NODES) return -1;
        int i = accCount++;
        accId[i] = id;
        acc[i]   = {0.0f, 0.0f, 0.0f, 0, 0.0f, 0};
        return i;
    };

    for (int z = 0; z < anchorCount; z++) {
        if (!zoneReceived[z]) continue;

        // Find this anchor's solved global position
        int ai = -1;
        for (int i = 0; i < nAnchors; i++)
            if (anchorIds[i] == zoneSummaries[z].anchor_id) { ai = i; break; }
        if (ai < 0) continue;

        // Compute rotation local→global using a circular mean over all
        // other anchors visible in this zone (more references = more stable)
        float sinSum = 0.0f, cosSum = 0.0f;
        int   rotRefs = 0;
        for (int n = 0; n < zoneSummaries[z].nodeCount; n++) {
            zone_node &zn = zoneSummaries[z].nodes[n];
            if (!zn.isAnchor) continue;
            int bi = -1;
            for (int i = 0; i < nAnchors; i++)
                if (anchorIds[i] == zn.id) { bi = i; break; }
            if (bi < 0) continue;
            float localAngle  = atan2f(zn.ly, zn.lx);
            float globalAngle = atan2f(anchorY[bi] - anchorY[ai],
                                       anchorX[bi] - anchorX[ai]);
            float theta = globalAngle - localAngle;
            sinSum += sinf(theta);
            cosSum += cosf(theta);
            rotRefs++;
        }

        // Need at least one anchor reference to establish rotation
        if (rotRefs == 0) {
            Serial.printf("[STITCH] Zone 0x%02X skipped — no rotation reference.\n",
                zoneSummaries[z].anchor_id);
            continue;
        }

        float theta = atan2f(sinSum, cosSum);
        float cosT  = cosf(theta);
        float sinT  = sinf(theta);

        // Rotate and translate each mobile node from local to global frame
        for (int n = 0; n < zoneSummaries[z].nodeCount; n++) {
            zone_node &zn = zoneSummaries[z].nodes[n];
            if (zn.isAnchor) continue;

            float gx = anchorX[ai] + zn.lx * cosT - zn.ly * sinT;
            float gy = anchorY[ai] + zn.lx * sinT + zn.ly * cosT;

            int gi = getAcc(zn.id);
            if (gi < 0) continue;

            // Weight this anchor's estimate by proximity: closer anchor (smaller
            // local distance) → larger weight, so the node tracks toward it.
            // Relative only — needs no distance calibration.
            float d2 = zn.lx * zn.lx + zn.ly * zn.ly;   // squared local distance
            float w  = 1.0f / (d2 + PROXIMITY_EPS);

            acc[gi].x    += gx * w;
            acc[gi].y    += gy * w;
            acc[gi].wsum += w;
            acc[gi].alt   = zn.altitude;  // last zone wins (pressure-derived, consistent)
            acc[gi].azm   = zn.azimuth;
            acc[gi].count++;
        }
    }

    // ── Phase 3: Populate node registry ─────────────────────────────────

    nodeCount = 0;

    // Anchors first — their positions come straight from KNOWN_ANCHORS
    for (int i = 0; i < nAnchors && nodeCount < MAX_NODES; i++) {
        NodeInfo &nd = nodes[nodeCount++];
        memset(&nd, 0, sizeof(NodeInfo));
        nd.id       = anchorIds[i];
        nd.active   = true;
        nd.isAnchor = true;
        nd.x        = anchorX[i];
        nd.y        = anchorY[i];
        nd.altitude = anchorAlt[i];   // preserve altitude instead of dropping it to 0
    }

    // Mobile nodes — averaged across all zone contributions
    for (int i = 0; i < accCount && nodeCount < MAX_NODES; i++) {
        if (acc[i].count == 0 || acc[i].wsum <= 0.0f) continue;
        NodeInfo &nd = nodes[nodeCount++];
        memset(&nd, 0, sizeof(NodeInfo));
        nd.id       = accId[i];
        nd.active   = true;
        nd.isAnchor = false;
        nd.x        = acc[i].x / acc[i].wsum;
        nd.y        = acc[i].y / acc[i].wsum;
        nd.altitude = acc[i].alt;
        nd.azimuth  = acc[i].azm;
    }
}

// =====================================================
//  MAP RESOLUTION
//  Anchors are fixed, so stitchZones() already yields positions in the correct
//  global frame. No post-alignment (translation/rotation/mirror) is needed —
//  the anchor layout itself defines the origin and orientation.
// =====================================================

void resolveMap() {
    stitchZones();
}

// =====================================================
//  MAP BROADCAST (unchanged)
// =====================================================

void broadcastMap() {
    position_map map;
    map.nodeCount = nodeCount;
    for (int i = 0; i < nodeCount; i++) {
        map.positions[i] = {
            nodes[i].id, nodes[i].x, nodes[i].y,
            nodes[i].altitude, nodes[i].azimuth, nodes[i].isAnchor
        };
    }
    esp_now_send(broadcastAddress, (uint8_t*)&map, sizeof(map));
    Serial.printf("[MAP] Broadcast sent — %d nodes.\n", nodeCount);
}

// =====================================================
//  SERIAL OUTPUT
// =====================================================

void printPositions() {
    Serial.println("\n========================================");
    Serial.println("  NODE POSITIONS (stitched from zones)");
    Serial.println("  Frame = fixed anchor layout (KNOWN_ANCHORS)");
    Serial.println("========================================");
    for (int i = 0; i < nodeCount; i++) {
        Serial.printf("  [%s] ID 0x%02X | x: %+5.2f m  y: %+5.2f m  z: %+5.2f m  | hdg: %3d°\n",
            nodes[i].isAnchor ? "ANCHOR" : "MOBILE",
            nodes[i].id, nodes[i].x, nodes[i].y,
            nodes[i].altitude, nodes[i].azimuth);
    }
    Serial.println("========================================\n");
}

// =====================================================
//  HOST STREAM (over USB serial)
//
//  This board has no internet access (it is pinned to the
//  ESP-NOW channel). After every resolveMap() it streams
//  the stitched positions as InfluxDB v2 Line Protocol
//  over the USB serial port to the desktop, which reads
//  them and does the actual HTTP POST to InfluxDB.
//
//  Framing (so the host can pick data out from debug text):
//    - each data line is prefixed with "INFLUX "
//    - an "INFLUX_END" line marks the end of a batch, telling
//      the host "flush what you have to InfluxDB now"
//  Any line WITHOUT the prefix is human-readable debug and
//  is ignored by the host script.
//
//  Line Protocol format (after the prefix):
//    measurement,tag=value field=value
// =====================================================

void streamPositionsToHost() {
    // One line per node.
    // Example: INFLUX node_position,node_id=0xAB,is_anchor=true x=0.00,y=0.00,altitude=1.20,azimuth=45
    for (int i = 0; i < nodeCount; i++) {
        NodeInfo &n = nodes[i];
        char line[128];
        snprintf(line, sizeof(line),
            "node_position,node_id=0x%02X,is_anchor=%s "
            "x=%.3f,y=%.3f,altitude=%.3f,azimuth=%d",
            n.id,
            n.isAnchor ? "true" : "false",
            n.x, n.y, n.altitude, n.azimuth);
        Serial.print("INFLUX ");
        Serial.println(line);
    }
    // NOTE: INFLUX_END is printed by the caller, after the sensor stream too,
    // so positions and sensors land in the same host batch.
}

// One line per mobile node we've heard, carrying its raw sensor readings.
// Anchors are skipped — they have no sensors (pressure/temp would be 0).
// Example: INFLUX sensor_data,node_id=0x01,is_anchor=false temp=24.5,pressure=101325,...,peers="0xAB:-55|0xCD:-60"
void streamSensorsToHost() {
    for (int i = 0; i < sensorCount; i++) {
        if (!sensorValid[i]) continue;
        sensor_data &s = sensorRecords[i];
        if (s.isAnchor) continue;

        // Encode the peer list compactly as a quoted string: "0xCD:-55|0xEF:-60"
        char peersBuf[96];
        int off = 0;
        peersBuf[0] = '\0';
        for (int p = 0; p < s.peerCount && p < MAX_PEERS; p++) {
            int n = snprintf(peersBuf + off, sizeof(peersBuf) - off, "%s0x%02X:%d",
                             p ? "|" : "", s.peers[p].id, s.peers[p].rssi);
            if (n < 0 || n >= (int)(sizeof(peersBuf) - off)) break;
            off += n;
        }

        // Altitude from pressure (same barometric formula the sim/anchors use)
        float altitude = 44330.0f * (1.0f - powf(s.pressure / 101325.0f, 0.1903f));

        char line[256];
        snprintf(line, sizeof(line),
            "sensor_data,node_id=0x%02X,is_anchor=false "
            "temp=%.2f,pressure=%d,altitude=%.3f,azimuth=%d,"
            "ax=%.4f,ay=%.4f,az=%.4f,gx=%.4f,gy=%.4f,gz=%.4f,"
            "cx=%.3f,cy=%.3f,cz=%.3f,peer_count=%d,peers=\"%s\"",
            s.board_id,
            s.temp, (int)s.pressure, altitude, s.azimuth,
            s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
            s.cx, s.cy, s.cz, s.peerCount, peersBuf);
        Serial.print("INFLUX ");
        Serial.println(line);
    }
}

// =====================================================
//  SETUP & LOOP
// =====================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    memset(nodes,         0, sizeof(nodes));
    memset(zoneSummaries, 0, sizeof(zoneSummaries));
    memset(zoneReceived,  0, sizeof(zoneReceived));
    memset(sensorRecords, 0, sizeof(sensorRecords));
    memset(sensorValid,   0, sizeof(sensorValid));

    zoneQueue   = xQueueCreate(20, sizeof(zone_summary));
    sensorQueue = xQueueCreate(20, sizeof(sensor_data));

    // Pin the radio to the ESP-NOW channel — this board never joins WiFi,
    // so it stays put and stays reachable by the anchors/nodes.
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[!] ESP-NOW init failed");
        return;
    }

    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb(OnDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[!] Broadcast peer registration failed");
        return;
    }

    Serial.println("\n[AGGREGATOR] Ready (fog mode). Waiting for zone summaries...");
    Serial.printf("[AGGREGATOR] Origin anchor: 0x%02X\n", ANCHOR_ORIGIN);
    Serial.printf("[AGGREGATOR] Warming up for %d seconds...\n\n", WARMUP_MS / 1000);
}

void loop() {
    static unsigned long lastSolve = 0;
    unsigned long now = millis();

    // Drain the zone queue and update the zone buffer
    zone_summary summary;
    while (xQueueReceive(zoneQueue, &summary, 0) == pdTRUE) {
        int zi = getZoneBufferIndex(summary.anchor_id);
        if (zi < 0) continue;
        zoneSummaries[zi] = summary;
        zoneReceived[zi]  = true;
        Serial.printf("[ZONE] Received summary from anchor 0x%02X (%d nodes)\n",
            summary.anchor_id, summary.nodeCount);
    }

    // Drain the sensor queue — keep the latest reading from each board
    sensor_data sd;
    while (xQueueReceive(sensorQueue, &sd, 0) == pdTRUE) {
        int si = getSensorIndex(sd.board_id);
        if (si < 0) continue;
        sensorRecords[si] = sd;
        sensorValid[si]   = true;
    }

    // Stitch and broadcast once we have at least one zone and the warmup is done
    bool hasZone = false;
    for (int i = 0; i < anchorCount; i++) if (zoneReceived[i]) { hasZone = true; break; }

    if (now > WARMUP_MS && now - lastSolve > SOLVE_INTERVAL && hasZone) {
        lastSolve = now;

        resolveMap();
        printPositions();
        broadcastMap();
        streamPositionsToHost();          // node_position lines
        streamSensorsToHost();            // sensor_data lines
        Serial.println("INFLUX_END");     // one delimiter for the whole batch
    }
}
