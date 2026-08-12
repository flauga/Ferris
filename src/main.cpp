// ============================================================================
//  Cardio Drum Trainer — ESP32 firmware
//
//  A rope wound on a drum spins an AS5600 magnetic encoder; a PWM-controlled
//  magnetic brake provides resistance. The dashboard (Web Bluetooth) sets the
//  resistance and reads RPM; all force/velocity/power/energy math is done in
//  the dashboard so calibration lives in one editable place.
//
//  Reuses the Ferra PCB / brake / AS5600 hardware:
//    - BLE Nordic UART Service (same UUIDs as Ferra)
//    - PWM brake on MOTOR_OUT_PIN via the ESP32 ledc peripheral
//    - AS5600 over I2C (default pins, Wire.begin())
//
//  Protocol:
//    esp -> app   data (BLE):  16-byte little-endian binary frame —
//                              uint32 ms | float32 rpm | int32 pos | float32 resistance
//    esp -> app   data (USB):  ASCII mirror [<ms>ms] RPM:<rpm.1> POS:<cumcounts> R:<pct>
//                              (Serial only, for debugging on a plain monitor)
//    esp -> app   status: [STATUS] {"name":"CardioDrum","r":<pct>,"heap":<n>,"fw":"1.0"}
//    esp -> app   info:   [INFO] <message>
//    app -> esp   cmds:   R:<nn> | RESET | S | STOP
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include "AS5600.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "config.h"
#include "soc/soc.h"           // WRITE_PERI_REG
#include "soc/rtc_cntl_reg.h"  // RTC_CNTL_BROWN_OUT_REG

// ---- Globals ---------------------------------------------------------------
AS5600 as5600;                          // default Wire

static BLEServer        *pServer  = nullptr;
static BLECharacteristic *pTxChar = nullptr;   // notify (esp -> app)
static BLECharacteristic *pRxChar = nullptr;   // write  (app -> esp)

static bool deviceConnected    = false;
static bool oldDeviceConnected = false;
// Set in onConnect, actioned in loop(). Never call notify() from inside a BLE
// stack callback — the link isn't fully up yet and it can wedge/drop the GATT
// connection. We defer the connect-time status line to the main loop instead.
static volatile bool pendingConnectStatus = false;

static volatile int currentResistance = 0;     // last commanded 0..100

// ---- Cumulative position tracked entirely in firmware ----------------------
// We own every readAngle() call so the library's internal wrap-detection never
// sees a stale angle. getCumulativePosition() is NOT called — it races with our
// readAngle() calls and causes direction-flip artefacts.
//
// cumCounts: signed 32-bit count of raw encoder ticks from reset.
//            Each full revolution = ±4096 counts (CW positive).
// lastRaw:   the previous 12-bit angle sample (0–4095).
static int32_t  cumCounts = 0;
static uint16_t lastRaw   = 0xFFFF;   // sentinel: no sample yet

// Push one raw angle sample; update cumCounts with wrap-safe delta.
// Returns the signed delta applied this tick (useful for diag).
static int16_t pushAngle(uint16_t raw) {
    if (lastRaw == 0xFFFF) { lastRaw = raw; return 0; }
    int16_t delta = (int16_t)raw - (int16_t)lastRaw;
    // Wrap correction: if the angular jump exceeds ±2048 counts (half a rev),
    // we crossed the 0/4095 boundary. Correct to the shortest-path delta.
    if (delta >  2048) delta -= 4096;
    if (delta < -2048) delta += 4096;
    cumCounts += delta;
    lastRaw = raw;
    return delta;
}

// ---- RPM via position slope ------------------------------------------------
// Sliding-window slope over the last RPM_WINDOW_MS of (time, cumCounts) pairs.
struct PosSnapshot { uint32_t us; int32_t counts; };
static PosSnapshot posRing[RPM_RING_SIZE];
static uint8_t     posRingHead = 0;
static bool        posRingFull = false;

static uint32_t lastPushMs = 0;

// ---- Diagnostic mode -------------------------------------------------------
// Send "DIAG ON" / "DIAG OFF" over Serial or BLE to toggle.
// Each tick emits:  DIAG,<ms>,<raw>,<delta>,<cum>,<rpm>
// DIAG is on by default until the magnet/sensor issue is resolved.
// Send "DIAG OFF" to silence it once behaviour is confirmed correct.
static bool diagMode = true;

// ---- PWM brake (re-ranged from Ferra's scaleResistance, now 0..100) --------
static void scaleResistance(int resistance) {
    resistance = constrain(resistance, 0, 100);
    // resistance 0 => true 0 duty so the brake fully releases.
    int duty = (resistance == 0)
                 ? 0
                 : map(resistance, 1, 100, SAFE_MIN_DUTY, SAFE_MAX_DUTY);
    ledcWrite(MOTOR_OUT_PIN, duty);   // 3.x LEDC is pin-based
    currentResistance = resistance;
}

// ---- RPM via position slope ------------------------------------------------
// Record a (time, cumCounts) snapshot, then return the slope over the oldest
// snapshot that is at least RPM_WINDOW_MS behind the current one.
// All I2C reads happen before this call so cumCounts is already updated.
static float computeRPM(uint32_t nowUs) {
    posRing[posRingHead] = { nowUs, cumCounts };
    posRingHead = (posRingHead + 1) % RPM_RING_SIZE;
    if (posRingHead == 0) posRingFull = true;

    uint8_t entries = posRingFull ? RPM_RING_SIZE : posRingHead;
    if (entries < 2) return 0.0f;

    // The ring is a circular buffer; oldest entry index:
    uint8_t oldestIdx = posRingFull
        ? posRingHead                                     // head just wrapped
        : 0;

    // Walk forward from oldest to find the youngest entry still >= windowUs old.
    // That gives the longest window ≤ RPM_WINDOW_MS we can actually form.
    uint32_t windowUs = RPM_WINDOW_MS * 1000UL;
    uint8_t  refIdx   = oldestIdx;
    for (uint8_t i = 0; i < entries - 1; i++) {
        uint8_t idx = (oldestIdx + i) % RPM_RING_SIZE;
        if (nowUs - posRing[idx].us >= windowUs) refIdx = idx;
        else break;   // entries are time-ordered; once inside window we're done
    }

    uint32_t dt = nowUs - posRing[refIdx].us;
    if (dt < 5000) return 0.0f;   // need at least 5 ms of history

    int32_t dc = cumCounts - posRing[refIdx].counts;
    float rpm = fabsf(((float)dc / COUNTS_PER_REV) * (60.0e6f / (float)dt));
    if (rpm < RPM_NOISE_FLOOR) rpm = 0.0f;
    return rpm;
}

// ---- BLE send helpers ------------------------------------------------------
// Text helper for [STATUS]/[INFO] lines: Serial + notify fan-out, newline-added.
// The dashboard distinguishes text from data frames by length/content (text
// lines start with '[' and are > 16 bytes; data frames are exactly 16 bytes).
static void sendLine(const char *s) {
    Serial.println(s);
    if (deviceConnected && pTxChar) {
        char buf[160];
        int n = snprintf(buf, sizeof(buf), "%s\n", s);
        if (n < 0) return;
        if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
        pTxChar->setValue((uint8_t *)buf, n);
        pTxChar->notify();
    }
}

// Binary data frame (16 bytes, little-endian): ms u32 | rpm f32 | pos i32 | r f32.
// ESP32 is little-endian, so we pack by direct member assignment. Also mirrors
// the frame as an ASCII line on USB Serial for debugging on a plain monitor.
static void sendFrame(uint32_t ms, float rpm, int32_t pos, int resistance) {
    if (deviceConnected && pTxChar) {
        struct __attribute__((packed)) {
            uint32_t ms;
            float    rpm;
            int32_t  pos;
            float    resistance;
        } frame = { ms, rpm, pos, (float)resistance };
        pTxChar->setValue((uint8_t *)&frame, sizeof(frame));
        pTxChar->notify();
    }
    // ASCII mirror on USB Serial only (not over BLE).
    Serial.printf("[%lums] RPM:%.1f POS:%ld R:%d\n",
                  (unsigned long)ms, rpm, (long)pos, resistance);
}

static void sendStatus() {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "[STATUS] {\"name\":\"%s\",\"r\":%d,\"heap\":%u,\"fw\":\"1.0\"}",
             BLE_DEVICE_NAME, currentResistance, (unsigned)ESP.getFreeHeap());
    sendLine(buf);
}

// ---- Command handling ------------------------------------------------------
static void handleCommand(String cmd) {
    cmd.trim();                  // strips trailing \r / \n / spaces
    if (cmd.length() == 0) return;

    String upper = cmd;
    upper.toUpperCase();

    if (upper.startsWith("R:")) {
        int v = cmd.substring(2).toInt();
        scaleResistance(v);
        sendLine((String("[INFO] resistance set ") + currentResistance).c_str());
    } else if (upper == "RESET") {
        cumCounts = 0;
        lastRaw   = 0xFFFF;   // re-seed on next loop tick
        sendLine("[INFO] encoder position reset");
    } else if (upper == "S") {
        sendStatus();
    } else if (upper == "STOP") {
        scaleResistance(0);
        sendLine("[INFO] brake released (STOP)");
    } else if (upper == "DIAG ON") {
        diagMode = true;
        sendLine("[INFO] diagnostic logging ON — CSV on Serial: DIAG,ms,raw,cum,rpm");
    } else if (upper == "DIAG OFF") {
        diagMode = false;
        sendLine("[INFO] diagnostic logging OFF");
    } else {
        sendLine((String("[INFO] unknown command: ") + cmd).c_str());
    }
}

// RX callback — accumulate bytes and split on '\n' so multi-line / partial
// writes are handled cleanly.
class RxCallback : public BLECharacteristicCallbacks {
    String rxBuf;
    void onWrite(BLECharacteristic *c) override {
        String v = c->getValue();   // 3.x returns Arduino String
        for (size_t i = 0; i < v.length(); i++) {
            char ch = v[i];
            if (ch == '\n') {
                handleCommand(rxBuf);
                rxBuf = "";
            } else {
                rxBuf += ch;
                if (rxBuf.length() > 64) rxBuf = "";   // guard runaway input
            }
        }
        // Tolerate a final line with no trailing newline.
        if (rxBuf.length() > 0) {
            handleCommand(rxBuf);
            rxBuf = "";
        }
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    // Bluedroid overload that carries the connect params (peer address). We use
    // this instead of the no-arg onConnect so we can negotiate connection
    // parameters against the actual peer.
    void onConnect(BLEServer *pSrv, esp_ble_gatts_cb_param_t *param) override {
        deviceConnected = true;
        // Ask the central for a relaxed connection interval (~30–50 ms) with a
        // little slave latency and a generous 4 s supervision timeout. This
        // gives the notify queue time to drain and stops the link from timing
        // out under the 25 Hz frame push. Units: interval = n*1.25 ms,
        // timeout = n*10 ms.  min=24→30ms, max=40→50ms, latency=2, timeout=400→4s.
        pSrv->requestConnParams(param->connect.remote_bda, 24, 40, 2, 400);
        // Do NOT notify() here — defer the status line to the main loop.
        pendingConnectStatus = true;
    }
    void onDisconnect(BLEServer *) override {
        deviceConnected = false;
        scaleResistance(0);           // SAFETY: release the brake on disconnect
    }
};

// ---- Setup -----------------------------------------------------------------
void setup() {
    // Disable the hardware brownout detector. The BLE stack startup draws a
    // current spike that sags a marginal USB supply below the ~2.97V trip
    // threshold, causing a reset loop. Disabling it here is safe for a
    // permanently USB/mains-powered device — the chip resets naturally if
    // power is actually lost, just without the extra supervisor trip.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println(F("[INFO] Cardio Drum Trainer — ESP32"));

    // Encoder
    Wire.begin();                                  // default SDA 21 / SCL 22
    as5600.begin(AS5600_DIR_PIN);
    as5600.setDirection(AS5600_CLOCK_WISE);
    Serial.print(F("[INFO] AS5600 connected: "));
    Serial.println(as5600.isConnected() ? F("yes") : F("no"));

    // Magnet diagnostic. Read the STATUS register ONCE per sample and decode
    // all three bits from that single read — the library's magnetDetected()/
    // magnetTooWeak()/magnetTooStrong() each issue a SEPARATE I2C read, which
    // can return inconsistent bits on back-to-back transactions right after
    // power-on. Poll a few times so an intermittent field is visible.
    //
    //   STATUS bits:  MD=0x20 (detect)  ML=0x10 (too weak)  MH=0x08 (too strong)
    //   Healthy: MD=1, ML=0, MH=0, AGC ~ 40-80 (for 5V; 0-128 typ range).
    //   MD=0 + ML=1 with a mid AGC and a valid-looking rawAngle is the classic
    //   signature of a present-but-wrong-geometry magnet — almost always an
    //   AXIALLY-magnetised magnet where a DIAMETRICALLY-magnetised one is
    //   required. The chip still emits an angle from the bad field, which is
    //   why earlier logs showed a moving (but garbage) position.
    delay(100);   // let the sensor settle before reading status
    for (int i = 0; i < 5; i++) {
        uint8_t  st  = as5600.readStatus();
        uint8_t  agc = as5600.readAGC();
        uint16_t ang = as5600.readAngle();
        Serial.printf("[INFO] Magnet sample %d: STATUS=0x%02X  MD=%d ML=%d MH=%d  AGC=%d  rawAngle=%d\n",
            i,
            st,
            (st & 0x20) ? 1 : 0,    // MD — magnet detected
            (st & 0x10) ? 1 : 0,    // ML — too weak / too far
            (st & 0x08) ? 1 : 0,    // MH — too strong / too close
            (int)agc,
            (int)ang);
        delay(50);
    }
    lastRaw = as5600.readAngle();   // seed lastRaw after the diagnostic reads

    // Brake PWM
    pinMode(MOTOR_OUT_PIN2, OUTPUT);
    digitalWrite(MOTOR_OUT_PIN2, LOW);
    // arduino-esp32 core 3.x LEDC API: pin-based, no explicit channel.
    ledcAttach(MOTOR_OUT_PIN, PWM_FREQ, PWM_RESOLUTION);
    scaleResistance(0);                            // brake released at boot

    // Small pause before BLE init — lets the supply rail stabilise after the
    // AS5600/ledc setup current spike so the BLE stack startup doesn't trigger
    // the brownout detector on marginal USB power sources.
    delay(200);

    // BLE (Ferra-identical sequence)
    BLEDevice::init(BLE_DEVICE_NAME);
    // Reduce TX power from the default +9 dBm to -3 dBm — more than enough
    // for a device used within a few metres, and cuts the current draw that
    // was triggering the brownout detector on marginal USB supplies.
    BLEDevice::setPower(ESP_PWR_LVL_N3);   // -3 dBm  (options: N12 N9 N6 N3 N0 P3 P6 P9)
    BLEDevice::setMTU(BLE_MTU);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pTxChar = pService->createCharacteristic(
        CHAR_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());

    pRxChar = pService->createCharacteristic(
        CHAR_RX_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pRxChar->setCallbacks(new RxCallback());

    pService->start();
    pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
    pServer->getAdvertising()->start();
    Serial.println(F("[INFO] BLE advertising as CardioDrum — waiting for a client."));

    lastPushMs = millis();
}

// ---- Loop ------------------------------------------------------------------
void loop() {
    uint32_t nowMs = millis();
    uint32_t nowUs = micros();

    // Single I2C read per tick — we own every angle sample.
    uint16_t rawAngle = as5600.readAngle();   // 0–4095
    int16_t  delta    = pushAngle(rawAngle);  // updates cumCounts
    float    rpm      = computeRPM(nowUs);

    // Diagnostic CSV — Serial only. Rate-limited to DIAG_PERIOD_MS: emitting a
    // full line every ~1 ms tick floods the 115200-baud UART, and once the TX
    // FIFO fills Serial.printf() BLOCKS. A blocked loop starves the BLE task,
    // which then misses connection events and hits the supervision timeout →
    // spurious disconnect. Throttling keeps the loop non-blocking.
    // Columns: ms, raw(0-4095), delta, cum_counts, rpm
    static uint32_t lastDiagMs = 0;
    if (diagMode && (nowMs - lastDiagMs >= DIAG_PERIOD_MS)) {
        lastDiagMs = nowMs;
        Serial.printf("DIAG,%lu,%u,%d,%ld,%.2f\n",
                      (unsigned long)nowMs, rawAngle, (int)delta,
                      (long)cumCounts, rpm);
    }

    // Fixed-rate data push.
    if (nowMs - lastPushMs >= SAMPLE_PERIOD_MS) {
        lastPushMs += SAMPLE_PERIOD_MS;
        if (nowMs - lastPushMs > 2 * SAMPLE_PERIOD_MS) lastPushMs = nowMs;

        sendFrame(nowMs, rpm, cumCounts, currentResistance);
    }

    // Re-advertise after a disconnect (Ferra pattern).
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        Serial.println(F("[INFO] re-advertising"));
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    // Deferred connect-time status line — sent from the loop, NOT from the
    // onConnect callback (notify() inside the stack callback can drop the link).
    if (pendingConnectStatus && deviceConnected) {
        pendingConnectStatus = false;
        sendStatus();
    }

    delay(1);   // ~1 ms tick
}
