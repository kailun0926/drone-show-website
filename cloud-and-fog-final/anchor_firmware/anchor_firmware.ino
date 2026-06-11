// =====================================================
//  ANCHOR FIRMWARE (FOG COMPUTING REVISION)
//  Anchors now run a local spring relaxation on the
//  nodes they can hear, then send a zone_summary to
//  the aggregator instead of raw RSSI data.
// =====================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

#define MAX_PEERS     8
#define MAX_NODES     8
#define MAX_ZONE      (MAX_PEERS + 1)   // self + up to MAX_PEERS others

#define TX_POWER      -50.0f
#define PATH_LOSS     2.7f

#define N_ITER_LOCAL  200               // fewer iterations than the global solver
#define LEARN_RATE    0.008f
#define ZONE_INTERVAL 100               // ms between zone summary broadcasts

// Onboard addressable RGB LED (single WS2812). GPIO8 on the ESP32-C6-DevKitC-1
// and most "C6 WROOM" dev boards. Change if your board wires it elsewhere.
#define STATUS_LED_PIN 8

// =====================================================
//  DATA STRUCTURES
// =====================================================

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

// A single node entry in the zone summary (coordinates are anchor-local)
typedef struct __attribute__((packed)) zone_node {
    uint8_t id;
    float   lx, ly;     // position relative to this anchor (anchor = 0,0)
    float   altitude;
    int16_t azimuth;
    bool    isAnchor;
} zone_node;            // 16 bytes

// Sent to aggregator after each local solve (2 + 8*16 = 130 bytes, well under 250)
typedef struct __attribute__((packed)) zone_summary {
    uint8_t   anchor_id;
    uint8_t   nodeCount;
    zone_node nodes[MAX_PEERS];
} zone_summary;

// Inbound position map from aggregator
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

// =====================================================
//  ZONE SOLVER STATE
//  zoneNodes[0] is always self, pinned at (0, 0).
//  All other slots are discovered neighbours.
// =====================================================

struct ZoneNode {
    uint8_t  id;
    bool     active;
    bool     isAnchor;
    float    lx, ly;
    float    altitude;
    int16_t  azimuth;
    int32_t  pressure;
};

ZoneNode zoneNodes[MAX_ZONE];
uint8_t  zoneCount = 0;

float localDist[MAX_ZONE][MAX_ZONE];
bool  localDistKnown[MAX_ZONE][MAX_ZONE];

// BUG FIX: the zone arrays above are now only ever touched by the loop task.
// Inbound sensor_data is handed off through this queue (same pattern the
// aggregator uses) so the recv callback and the solver never race.
typedef struct {
    sensor_data data;
    int8_t      rssi;
} rssi_event;

QueueHandle_t rssiQueue;

sensor_data myData;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Existing peer table — still used to pack sensor_data for node broadcasts
struct PeerEntry {
    uint8_t       id;
    float         rssi;
    unsigned long lastSeen;
};
PeerEntry peerTable[MAX_PEERS];
uint8_t   peerTableSize = 0;

unsigned long lastSendTime  = 0;
unsigned long lastZoneSend  = 0;
int           currentInterval = 67;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

bool fatalError = false;   // set if the radio fails to start

// Status-LED state
volatile unsigned long lastPeerHeard = 0;   // set in the recv callback (heard a neighbour)
unsigned long          lastZoneFlash = 0;   // set when a zone summary is sent

// =====================================================
//  HELPERS
// =====================================================

float rssiToDistance(int8_t rssi) {
    return powf(10.0f, (TX_POWER - (float)rssi) / (10.0f * PATH_LOSS));
}

// Returns zone index for id, creating a new slot if needed.
// Index 0 is always self — never returned here.
int getZoneIndex(uint8_t id) {
    for (int i = 1; i < zoneCount; i++)
        if (zoneNodes[i].id == id) return i;
    if (zoneCount >= MAX_ZONE) return -1;
    int idx = zoneCount++;
    memset(&zoneNodes[idx], 0, sizeof(ZoneNode));
    zoneNodes[idx].id = id;
    zoneNodes[idx].lx = (float)random(-200, 200) / 100.0f;
    zoneNodes[idx].ly = (float)random(-200, 200) / 100.0f;
    return idx;
}

void updateLocalDist(int a, int b, float d) {
    if (localDistKnown[a][b]) {
        localDist[a][b] = 0.75f * localDist[a][b] + 0.25f * d;
    } else {
        localDist[a][b] = d;
        localDistKnown[a][b] = true;
    }
    // Mirror — apply EMA to reverse entry too (Bug Fix 2 carries over)
    if (localDistKnown[b][a]) {
        localDist[b][a] = 0.75f * localDist[b][a] + 0.25f * localDist[a][b];
    } else {
        localDist[b][a] = localDist[a][b];
        localDistKnown[b][a] = true;
    }
}

// =====================================================
//  STATUS LED  (onboard addressable RGB on the ESP32-C6)
//
//    blue (brief)        booting (set in setup)
//    solid red           fatal error — radio failed to start
//    breathing amber     running but isolated — no neighbours heard
//    steady dim green    healthy — hears neighbours, solving normally
//    bright green blink  heartbeat — a zone summary was just sent
// =====================================================

void setStatusRGB(uint8_t r, uint8_t g, uint8_t b) {
    // Core helper (esp32-hal) drives the onboard WS2812. This board's LED has
    // its red and green channels swapped vs. the standard WS2812 order, so we
    // pass green into the red slot and vice versa to get the right colours.
    // (If your board shows correct colours already, change this back to r, g, b.)
    neopixelWrite(STATUS_LED_PIN, g, r, b);
}

void updateStatusLed() {
    static unsigned long lastLed = 0;
    unsigned long now = millis();
    if (now - lastLed < 25) return;          // ~40 Hz is plenty; avoids hammering the LED
    lastLed = now;

    if (fatalError) { setStatusRGB(60, 0, 0); return; }                 // solid red

    if (now - lastZoneFlash < 120) { setStatusRGB(0, 120, 0); return; } // heartbeat blink

    bool haveNeighbors = (zoneCount >= 2);
    bool heardRecently = (lastPeerHeard != 0) && (now - lastPeerHeard < 5000);

    if (haveNeighbors && heardRecently) {
        setStatusRGB(0, 30, 0);                                         // steady dim green
    } else {
        float b = sinf((now % 2000) / 2000.0f * 2.0f * PI) * 0.5f + 0.5f;
        setStatusRGB((uint8_t)(70 * b), (uint8_t)(22 * b), 0);          // breathing amber
    }
}

// =====================================================
//  PEER TABLE (for outbound sensor_data only)
// =====================================================

void updatePeerTable(uint8_t id, int8_t rssi) {
    unsigned long now = millis();
    int oldestIndex = -1;
    unsigned long oldestTime = now;

    portENTER_CRITICAL(&mux);

    for (int i = 0; i < peerTableSize; i++) {
        if (peerTable[i].id == id) {
            peerTable[i].rssi = (0.7f * peerTable[i].rssi) + (0.3f * (float)rssi);
            peerTable[i].lastSeen = now;
            portEXIT_CRITICAL(&mux);
            return;
        }
        if (peerTable[i].lastSeen < oldestTime) {
            oldestTime  = peerTable[i].lastSeen;
            oldestIndex = i;
        }
    }

    if (peerTableSize < MAX_PEERS) {
        peerTable[peerTableSize++] = {id, (float)rssi, now};
    } else if ((now - oldestTime > 3000) && (oldestIndex != -1)) {
        peerTable[oldestIndex] = {id, (float)rssi, now};
    }

    portEXIT_CRITICAL(&mux);
}

void packPeerRSSI() {
    unsigned long now = millis();
    portENTER_CRITICAL(&mux);
    myData.peerCount = 0;
    for (int i = 0; i < peerTableSize; i++) {
        if (now - peerTable[i].lastSeen < 3000) {
            myData.peers[myData.peerCount++] = {peerTable[i].id, (int8_t)peerTable[i].rssi};
        }
    }
    portEXIT_CRITICAL(&mux);
}

// =====================================================
//  LOCAL ZONE SOLVER
// =====================================================

// Drain queued sensor_data packets and fold them into the zone arrays.
// Runs only in the loop task, so it cannot race with runLocalSolver().
void processRssiQueue() {
    rssi_event ev;
    while (xQueueReceive(rssiQueue, &ev, 0) == pdTRUE) {
        // Update local zone: distance from self (index 0) to this node
        int idx = getZoneIndex(ev.data.board_id);
        if (idx < 0) continue;

        zoneNodes[idx].isAnchor = ev.data.isAnchor;
        zoneNodes[idx].azimuth  = ev.data.azimuth;
        zoneNodes[idx].pressure = ev.data.pressure;
        zoneNodes[idx].altitude = 44330.0f * (1.0f - powf(ev.data.pressure / 101325.0f, 0.1903f));

        updateLocalDist(0, idx, rssiToDistance(ev.rssi));

        // Also extract inter-node distances from the sender's peer list.
        // This is what allows the local solver to place nodes relative to
        // each other, not just relative to this anchor.
        for (int i = 0; i < ev.data.peerCount; i++) {
            int peerIdx = getZoneIndex(ev.data.peers[i].id);
            if (peerIdx < 0 || peerIdx == idx) continue;
            updateLocalDist(idx, peerIdx, rssiToDistance(ev.data.peers[i].rssi));
        }
    }
}

void runLocalSolver() {
    // zoneNodes[0] is self, pinned at (0,0) — start loop from index 1
    for (int iter = 0; iter < N_ITER_LOCAL; iter++) {
        for (int i = 1; i < zoneCount; i++) {
            float fx = 0.0f, fy = 0.0f;
            for (int j = 0; j < zoneCount; j++) {
                if (i == j || !localDistKnown[i][j]) continue;
                float dx = zoneNodes[j].lx - zoneNodes[i].lx;
                float dy = zoneNodes[j].ly - zoneNodes[i].ly;
                float actual = sqrtf(dx*dx + dy*dy);
                if (actual < 0.001f) actual = 0.001f;
                float error = actual - localDist[i][j];
                fx += error * (dx / actual);
                fy += error * (dy / actual);
            }
            zoneNodes[i].lx += LEARN_RATE * fx;
            zoneNodes[i].ly += LEARN_RATE * fy;
        }
    }
}

void broadcastZoneSummary() {
    runLocalSolver();

    zone_summary summary;
    summary.anchor_id = myData.board_id;
    summary.nodeCount = 0;

    // Pack all non-self zone nodes into the summary
    for (int i = 1; i < zoneCount && summary.nodeCount < MAX_PEERS; i++) {
        zone_node &zn       = summary.nodes[summary.nodeCount++];
        zn.id       = zoneNodes[i].id;
        zn.lx       = zoneNodes[i].lx;
        zn.ly       = zoneNodes[i].ly;
        zn.altitude = zoneNodes[i].altitude;
        zn.azimuth  = zoneNodes[i].azimuth;
        zn.isAnchor = zoneNodes[i].isAnchor;
    }

    esp_now_send(broadcastAddress, (uint8_t*)&summary, sizeof(summary));
    lastZoneFlash = millis();   // trigger the green heartbeat blink
    Serial.printf("[ZONE] Sent summary — %d nodes in zone.\n", summary.nodeCount);
}

// =====================================================
//  CALLBACKS
// =====================================================

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
    if (len == sizeof(sensor_data)) {
        sensor_data other;
        memcpy(&other, incomingData, sizeof(other));
        int8_t rssi = (int8_t)info->rx_ctrl->rssi;

        // Keep the peer table up to date for outbound sensor_data
        // (peerTable is already mutex-protected, safe to touch here).
        updatePeerTable(other.board_id, rssi);
        lastPeerHeard = millis();   // drives the status LED (heard a neighbour)

        // Hand the packet to the loop task. All zone-array updates happen
        // there so they never run concurrently with the solver.
        rssi_event ev;
        ev.data = other;
        ev.rssi = rssi;
        xQueueSendFromISR(rssiQueue, &ev, NULL);

    } else if (len == sizeof(position_map)) {
        // Final stitched map from aggregator — log own position
        position_map map;
        memcpy(&map, incomingData, sizeof(map));
        for (int i = 0; i < map.nodeCount; i++) {
            if (map.positions[i].id == myData.board_id) {
                Serial.printf("[MAP] My global position — x: %+5.2f m  y: %+5.2f m\n",
                    map.positions[i].x, map.positions[i].y);
            }
        }
    }
    // zone_summary from other anchors: RSSI captured passively at radio level;
    // no explicit handling needed here for the local solver.
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
    Serial.begin(115200);

    setStatusRGB(0, 0, 60);   // boot indicator: blue

    // BUG FIX 4: Lock to channel 1 to match all other nodes
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    myData.board_id = mac[5];
    myData.isAnchor = true;
    myData.temp     = 0;
    myData.pressure = 0;
    myData.ax = myData.ay = myData.az = 0;
    myData.gx = myData.gy = myData.gz = 0;
    myData.cx = myData.cy = myData.cz = 0;
    myData.azimuth = 0;

    // Initialise zone solver — slot 0 is always self, pinned at (0, 0)
    zoneCount = 1;
    memset(zoneNodes,       0, sizeof(zoneNodes));
    memset(localDist,       0, sizeof(localDist));
    memset(localDistKnown,  0, sizeof(localDistKnown));
    zoneNodes[0].id       = myData.board_id;
    zoneNodes[0].active   = true;
    zoneNodes[0].isAnchor = true;
    zoneNodes[0].lx       = 0.0f;
    zoneNodes[0].ly       = 0.0f;

    rssiQueue = xQueueCreate(20, sizeof(rssi_event));

    Serial.printf("Anchor ID (MAC last byte): 0x%02X\n", myData.board_id);

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        fatalError = true;
        return;
    }

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Peer registration failed");
        fatalError = true;
        return;
    }

    Serial.println("Anchor ready (fog mode).");
}

// =====================================================
//  LOOP
// =====================================================

void loop() {
    updateStatusLed();        // status indicator — runs even on fatal error (solid red)
    if (fatalError) return;   // radio failed to start — nothing else to do

    unsigned long now = millis();

    // 1. Regular sensor_data broadcast so mobile nodes can hear this anchor
    //    and include it in their own peer RSSI lists
    if (now - lastSendTime >= (unsigned long)currentInterval) {
        lastSendTime    = now;
        currentInterval = 67 + random(0, 6);
        packPeerRSSI();
        esp_now_send(broadcastAddress, (uint8_t*)&myData, sizeof(myData));
    }

    // 2. Fold any received packets into the zone arrays (loop task only)
    processRssiQueue();

    // 3. Local solve + zone summary broadcast for the aggregator
    if (now - lastZoneSend >= ZONE_INTERVAL && zoneCount >= 2) {
        lastZoneSend = now;
        broadcastZoneSummary();
    }
}
