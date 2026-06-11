// =====================================================
//  NODE FIRMWARE
//  Unchanged from the previous revision.
//  Nodes are unaware of the fog split — they continue
//  broadcasting sensor_data and receiving position_map.
//  Anchors handle the rest transparently.
// =====================================================

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_BMP085.h>
#include <MPU6500_WE.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_HMC5883_U.h>
#include <Adafruit_NeoPixel.h>

// --- CONFIG ---
#define LED_PIN    D7
#define NUM_LEDS   7
#define MAX_PEERS  8
#define MAX_NODES  8
#define IS_ANCHOR  false

Adafruit_NeoPixel indicator(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- DATA STRUCTS ---
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

sensor_data myData;

position_map lastMap;
bool         mapReceived = false;

struct PeerEntry {
    uint8_t       id;
    float         rssi;
    unsigned long lastSeen;
};
PeerEntry peerTable[MAX_PEERS];
uint8_t   peerTableSize = 0;

Adafruit_BMP085          bmp;
MPU6500_WE               mpu = MPU6500_WE(0x68);
Adafruit_HMC5883_Unified compass = Adafruit_HMC5883_Unified(12345);

unsigned long lastSendTime       = 0;
int           currentInterval    = 67;
unsigned long lastCloseEncounter = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================
//  PEER TABLE
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

void packPeerRSSI_Locked() {
    myData.peerCount = 0;
    unsigned long now = millis();
    for (int i = 0; i < peerTableSize; i++) {
        if (now - peerTable[i].lastSeen < 3000) {
            myData.peers[myData.peerCount++] = {peerTable[i].id, (int8_t)peerTable[i].rssi};
        }
    }
}

// =====================================================
//  MAP HELPERS
// =====================================================

bool getSelfPosition(float &x, float &y, float &alt) {
    if (!mapReceived) return false;
    for (int i = 0; i < lastMap.nodeCount; i++) {
        if (lastMap.positions[i].id == myData.board_id) {
            x = lastMap.positions[i].x;
            y = lastMap.positions[i].y;
            alt = lastMap.positions[i].altitude;
            return true;
        }
    }
    return false;
}

float getDistanceTo(uint8_t targetId) {
    if (!mapReceived) return -1.0f;
    float sx = 0, sy = 0, sa = 0;
    if (!getSelfPosition(sx, sy, sa)) return -1.0f;
    for (int i = 0; i < lastMap.nodeCount; i++) {
        if (lastMap.positions[i].id == targetId) {
            float dx = lastMap.positions[i].x - sx;
            float dy = lastMap.positions[i].y - sy;
            return sqrtf(dx*dx + dy*dy);
        }
    }
    return -1.0f;
}

// =====================================================
//  ESP-NOW CALLBACKS
// =====================================================

void OnDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
    Serial.printf("Shout status: %s\n", status == ESP_NOW_SEND_SUCCESS ? "Success" : "FAIL");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
    if (len == sizeof(sensor_data)) {
        sensor_data other;
        memcpy(&other, incomingData, sizeof(other));
        int8_t rssi = (int8_t)info->rx_ctrl->rssi;

        updatePeerTable(other.board_id, rssi);

        sensor_data snap;
        portENTER_CRITICAL(&mux);
        memcpy(&snap, &myData, sizeof(snap));
        portEXIT_CRITICAL(&mux);

        Serial.printf("Heard node! RSSI: %d | My Press: %d | Their Press: %d\n",
            rssi, snap.pressure, other.pressure);

        // BUG FIX 3: Corrected threshold from -25 to -60 (~2-3 metres)
        bool isRadioClose = (rssi > -20);
        bool isSameFloor  = (abs(snap.pressure - other.pressure) < 10000);

        if (isRadioClose && isSameFloor) {
            lastCloseEncounter = millis();
        }

    } else if (len == sizeof(position_map)) {
        position_map map;
        memcpy(&map, incomingData, sizeof(map));

        portENTER_CRITICAL(&mux);
        memcpy(&lastMap, &map, sizeof(lastMap));
        mapReceived = true;
        portEXIT_CRITICAL(&mux);

        float sx, sy, salt;
        if (getSelfPosition(sx, sy, salt)) {
            Serial.printf("[MAP] My position — x: %+5.2f m  y: %+5.2f m  alt: %+5.2f m\n",
                sx, sy, salt);
        }

        Serial.printf("[MAP] Full swarm map (%d nodes):\n", map.nodeCount);
        for (int i = 0; i < map.nodeCount; i++) {
            node_position &p = map.positions[i];
            Serial.printf("  [%s] 0x%02X | x: %+5.2f  y: %+5.2f  alt: %+5.2f  hdg: %3d°\n",
                p.isAnchor ? "ANCHOR" : "MOBILE",
                p.id, p.x, p.y, p.altitude, p.azimuth);
        }
    }
    // zone_summary packets from anchors are silently ignored (unrecognised size)
}

// =====================================================
//  FAULT HANDLER — sensor missing or timed out
//  One slow sine-wave red breath (~5 s), then deep
//  sleep for ~25 s.  On wake the ESP reboots and
//  setup() retries sensor detection automatically.
// =====================================================

#define SENSOR_INIT_TIMEOUT_MS 10000      // max ms to bring all sensors up
#define FAULT_BLINK_ON_MS      1000       // onboard LED on duration
#define FAULT_SLEEP_US         29000000ULL // 29 s deep sleep (= ~30 s cycle)
#define LED_BUILTIN D10

// XIAO ESP32-C3 onboard LED is active LOW on GPIO15 (LED_BUILTIN).
// HIGH = off, LOW = on.

void breathingFaultSleep(const char* reason) {
    Serial.printf("[FAULT] %s — blinking onboard LED, then sleeping 29 s.\n", reason);

    pinMode(LED_BUILTIN, OUTPUT);

    digitalWrite(LED_BUILTIN, LOW);    // on
    delay(FAULT_BLINK_ON_MS);
    digitalWrite(LED_BUILTIN, HIGH);   // off

    esp_sleep_enable_timer_wakeup(FAULT_SLEEP_US);
    esp_deep_sleep_start();
    // execution never reaches here; ESP reboots on wake
}

// =====================================================
//  SETUP
// =====================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    unsigned long setupStart = millis();   // used for 10 s sensor timeout

    indicator.begin();
    indicator.setBrightness(10);
    indicator.fill(indicator.Color(0, 0, 10));
    indicator.show();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    myData.board_id = mac[5];
    myData.isAnchor = IS_ANCHOR;

    Serial.printf("Board ID (MAC last byte): 0x%02X\n", myData.board_id);

    Wire.begin(D4, D5);

    // --- Sensor detection (must all succeed within SENSOR_INIT_TIMEOUT_MS) ---

    if (millis() - setupStart > SENSOR_INIT_TIMEOUT_MS || !bmp.begin()) {
        breathingFaultSleep("BMP085 not detected");
    }
    Serial.println("[SENSOR] BMP085 OK");

    if (millis() - setupStart > SENSOR_INIT_TIMEOUT_MS || !mpu.init()) {
        breathingFaultSleep("MPU6500 not detected");
    }
    Serial.println("[SENSOR] MPU6500 OK");
    delay(250);

    if (millis() - setupStart > SENSOR_INIT_TIMEOUT_MS || !compass.begin()) {
        breathingFaultSleep("HMC5883 not detected");
    }
    Serial.println("[SENSOR] HMC5883 OK");

    if (esp_now_init() != ESP_OK) {
        indicator.fill(indicator.Color(10, 0, 0));
        indicator.show();
        while (true) { delay(10); }
    }

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    peerInfo.ifidx   = WIFI_IF_STA;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        indicator.fill(indicator.Color(10, 0, 0));
        indicator.show();
        while (true) { delay(10); }
    }
}

// =====================================================
//  MAIN LOOP
// =====================================================

void loop() {
    unsigned long now = millis();

    if (now - lastSendTime >= (unsigned long)currentInterval) {
        lastSendTime    = now;
        currentInterval = 67 + random(0, 6);

        static int frameCount = 0;
        frameCount++;

        bool isTooClose = (lastCloseEncounter != 0) && (now - lastCloseEncounter < 1500);

        if (isTooClose) {
            indicator.fill(frameCount % 4 < 2 ? indicator.Color(100, 0, 0) : indicator.Color(0, 0, 0));
        } else {
            if (frameCount >= 1 && frameCount <= 3) {
                indicator.fill(indicator.Color(0, 50, 0));
            } else {
                indicator.fill(indicator.Color(0, 0, 0));
            }
        }
        indicator.show();

        if (frameCount >= 15) frameCount = 0;

        float   t = bmp.readTemperature();
        int32_t p = bmp.readPressure();

        xyzFloat gVal = mpu.getGValues();
        xyzFloat gyr  = mpu.getGyrValues();

        sensors_event_t event;
        compass.getEvent(&event);

        float roll  = atan2(gVal.y, gVal.z);
        float pitch = atan2(-gVal.x, sqrt(gVal.y * gVal.y + gVal.z * gVal.z));
        float magX  = event.magnetic.x * cos(pitch) + event.magnetic.z * sin(pitch);
        float magY  = event.magnetic.x * sin(roll) * sin(pitch)
                    + event.magnetic.y * cos(roll)
                    - event.magnetic.z * sin(roll) * cos(pitch);

        float heading = atan2(magY, magX);
        if (heading < 0) heading += 2.0f * PI;
        int16_t azm = (int16_t)(heading * 180.0f / PI);

        portENTER_CRITICAL(&mux);
        myData.temp     = t;
        myData.pressure = p;
        myData.ax = gVal.x; myData.ay = gVal.y; myData.az = gVal.z;
        myData.gx = gyr.x;  myData.gy = gyr.y;  myData.gz = gyr.z;
        myData.cx = event.magnetic.x;
        myData.cy = event.magnetic.y;
        myData.cz = event.magnetic.z;
        myData.azimuth = azm;
        packPeerRSSI_Locked();
        portEXIT_CRITICAL(&mux);

        esp_now_send(broadcastAddress, (uint8_t*)&myData, sizeof(myData));
    }
}
