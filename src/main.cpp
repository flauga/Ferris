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
//    app -> esp   cmds:   R:<nn> | RESET | S | STOP | PING | DIAG ON | DIAG OFF
//
//  Link reliability (see include/config.h for the tunables):
//    - TX power at max (+9 dBm) for both advertising and the connected link.
//    - 6 s supervision timeout and zero slave latency, re-requested on connect,
//      so brief RF blockage (a body passing through the path, a Wi-Fi burst)
//      is ridden out instead of dropping the session.
//    - Fast advertising (20-40 ms) and a self-healing advertising re-assert, so
//      the dashboard's auto-reconnect finds the device again almost instantly.
//    - A task watchdog reboots the board if loop() ever wedges, and a stuck I2C
//      bus is detected and recovered rather than blocking the BLE task.
//    - A peer timeout releases the brake if the dashboard goes quiet while the
//      link is nominally still up.
// ============================================================================

#include <Arduino.h>
#include <stdarg.h>            // va_list — non-blocking serialPrintf()
#include <Wire.h>
#include "AS5600.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "config.h"
#include "soc/soc.h"           // WRITE_PERI_REG
#include "soc/rtc_cntl_reg.h"  // RTC_CNTL_BROWN_OUT_REG
#include <esp_task_wdt.h>      // loop watchdog — reboot instead of wedging silently
#include <esp_bt.h>            // esp_ble_tx_power_set / esp_power_level_t
// NOTE: esp_wifi.h is deliberately NOT included. Calling esp_wifi_stop() pulled
// the whole Wi-Fi driver into the link (+74 KB flash, 87% -> 93% of the
// partition) just to stop a driver this firmware never starts. Wi-Fi is never
// initialised here, so there is no coexistence to disable — the band is already
// uncontended. If a future change adds Wi-Fi, revisit this.

// ---- Globals ---------------------------------------------------------------
AS5600 as5600;                          // default Wire

static BLEServer        *pServer  = nullptr;
static BLECharacteristic *pTxChar = nullptr;   // notify (esp -> app)
static BLECharacteristic *pRxChar = nullptr;   // write  (app -> esp)

static volatile bool deviceConnected = false;   // written by the BLE task, read by loop()
static bool oldDeviceConnected = false;
// Set in onConnect, actioned in loop(). Never call notify() from inside a BLE
// stack callback — the link isn't fully up yet and it can wedge/drop the GATT
// connection. We defer the connect-time status line to the main loop instead.
static volatile bool pendingConnectStatus = false;

static volatile int currentResistance = 0;     // last ACTUATED value 0..100
// Requested resistance, written by the BLE task (command handler / disconnect)
// and actuated by loop(). scaleResistance() does ledcWrite() THEN stores
// currentResistance; that pair is not atomic, so calling it from two tasks let
// the PWM duty and the reported value diverge (brake engaged while reporting 0).
// Single owner: only loop() calls scaleResistance().
static volatile int requestedResistance = 0;
static volatile bool resistanceDirty     = false;

// ---- Link bookkeeping ------------------------------------------------------
// Peer address of the current central, captured in onConnect so the connection
// parameters can be re-requested from loop() if the central renegotiates them
// (Windows in particular likes to impose its own after the initial accept).
static esp_bd_addr_t  peerAddr;
// Conn id of the CURRENT connection, captured in onConnect. BLEServer::m_connId
// is never reset on disconnect, so getConnId() can name a newer connection than
// the one we meant to close — closing the link the dashboard just brought up.
static volatile uint16_t peerConnId = 0;
static volatile bool  peerAddrValid  = false;
static volatile bool  pendingConnParams = false;  // re-request conn params from loop()

// millis() of the last thing we heard FROM the peer (any RX write, including the
// dashboard's keep-alive). Drives BLE_PEER_TIMEOUT_MS brake release.
static volatile uint32_t lastPeerMs = 0;

// Advertising state. advertising==false while a central is connected. The
// re-assert timer re-kicks advertising periodically while unconnected, so a
// silently-failed startAdvertising() can never leave the device invisible.
// (There is deliberately no `advertising` flag. One existed but was written and
// never read: the stack gives no reliable "am I still advertising" query, so the
// self-heal below re-kicks advertising on a timer instead of trying to detect it.)
static uint32_t advReassertMs    = 0;
// Settle-window latch for the connection-parameter re-request. File scope so the
// connect edge in loop() can reset it per connection (see SEV 7 note there).
static uint32_t connParamsAtMs   = 0;
// These are all written from the BLE stack callbacks and read from loop(), so
// they must be volatile: without it the compiler is free to hoist the reads out
// of the loop and the disconnect would never be observed.
static volatile uint32_t disconnectAtMs  = 0;   // millis() of the drop; 0 = nothing pending
static volatile uint32_t connectCount    = 0;   // lifetime connections (diagnostic)
static volatile uint32_t disconnectCount = 0;   // lifetime drops (diagnostic)
static volatile uint16_t lastDiscReason  = 0;   // HCI reason code of the last drop

// ---- I2C / encoder health --------------------------------------------------
static uint32_t i2cFailCount   = 0;   // consecutive failed reads
static uint32_t i2cRecoveries  = 0;   // lifetime bus re-inits (diagnostic)

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
// MUST be wide enough for RPM_RING_SIZE. These were uint8_t while the ring is
// 350 entries: the head wrapped at 256, so slots 256..349 were never written,
// posRingFull was set after 256 ticks instead of 350, and `uint8_t entries =
// RPM_RING_SIZE` truncated 350 to 94 — collapsing the RPM slope window from the
// intended 300 ms to ~94 ms and making RPM about three times noisier than the
// configuration says. A static_assert below keeps this honest if the size grows.
static uint16_t    posRingHead = 0;
static bool        posRingFull = false;
static_assert(RPM_RING_SIZE <= UINT16_MAX,
              "posRingHead/entries types must be widened for this RPM_RING_SIZE");

static uint32_t lastPushMs = 0;

// ---- Diagnostic mode -------------------------------------------------------
// Send "DIAG ON" / "DIAG OFF" over Serial or BLE to toggle.
// Each tick emits:  DIAG,<ms>,<raw>,<delta>,<cum>,<rpm>
// OFF by default (DIAG_DEFAULT_ON): streaming ~50 lines/s into a UART that
// nothing is draining fills the TX FIFO, at which point Serial.printf() BLOCKS,
// loop() stalls, the BLE task is starved and the link dies on its supervision
// timeout. Turn it on deliberately when a monitor is actually attached.
static bool diagMode = DIAG_DEFAULT_ON;

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

// Ask loop() to actuate a new brake value. Safe to call from a BLE callback:
// it only touches two volatile scalars and never touches the PWM peripheral.
static inline void requestResistance(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    requestedResistance = pct;
    resistanceDirty     = true;
}

// ---- RPM via position slope ------------------------------------------------
// Record a (time, cumCounts) snapshot, then return the slope over the oldest
// snapshot that is at least RPM_WINDOW_MS behind the current one.
// All I2C reads happen before this call so cumCounts is already updated.
static float computeRPM(uint32_t nowUs) {
    posRing[posRingHead] = { nowUs, cumCounts };
    posRingHead = (posRingHead + 1) % RPM_RING_SIZE;
    if (posRingHead == 0) posRingFull = true;

    uint16_t entries = posRingFull ? RPM_RING_SIZE : posRingHead;
    if (entries < 2) return 0.0f;

    // The ring is a circular buffer; oldest entry index:
    uint16_t oldestIdx = posRingFull
        ? posRingHead                                     // head just wrapped
        : 0;

    // Walk forward from oldest to find the youngest entry still >= windowUs old.
    // That gives the longest window ≤ RPM_WINDOW_MS we can actually form.
    uint32_t windowUs = RPM_WINDOW_MS * 1000UL;
    uint16_t refIdx   = oldestIdx;
    for (uint16_t i = 0; i + 1 < entries; i++) {   // i+1<entries: no underflow if entries==0
        uint16_t idx = (oldestIdx + i) % RPM_RING_SIZE;
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

// ---- Non-blocking Serial ---------------------------------------------------
// Serial.print* BLOCKS once the UART TX FIFO is full, which happens whenever the
// firmware emits faster than the host drains (and always when no monitor is
// attached at all). A blocked loop() starves the BLE task, the peripheral misses
// connection events, and the central eventually times the link out — i.e. the
// USB debug path can kill the wireless link. Every Serial write below is gated
// on there being room in the TX buffer, so output is DROPPED rather than
// allowed to stall the loop. Diagnostics are best-effort; the link is not.
static inline bool serialHasRoom(size_t need) {
    return Serial && (size_t)Serial.availableForWrite() >= need;
}

static void serialLine(const char *s) {
    size_t n = strlen(s) + 1;                  // + newline
    if (!serialHasRoom(n)) return;             // no room: drop, never block
    Serial.println(s);
}

// printf-style variant with the same drop-don't-block guarantee.
static void serialPrintf(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (!serialHasRoom((size_t)n)) return;
    Serial.write((const uint8_t *)buf, n);
}

// ---- I2C / encoder health --------------------------------------------------
// Read one angle sample, tracking consecutive failures. The AS5600 library has
// no error return on readAngle(), so a wedged bus shows up as a frozen value; we
// use the library's own isConnected() probe (a cheap 1-byte transaction) only
// when a read looks suspicious, to avoid doubling the per-tick I2C traffic.
//
// Returns true if the sample is trustworthy. On a sustained failure the bus is
// re-initialised — a loose connector or a sensor-rail glitch then recovers on
// its own instead of leaving the device streaming a frozen position forever.
static void recoverI2C() {
    // This does ~50-155 ms of blocking work (Wire.end/begin plus AS5600 re-init
    // with a bounded-but-slow bus). Feed the watchdog first so a recovery can
    // never itself look like a wedged loop.
    esp_task_wdt_reset();
    i2cRecoveries++;
    Wire.end();
    delay(5);
    Wire.begin();
    Wire.setTimeOut(I2C_TIMEOUT_MS);
    as5600.begin(AS5600_DIR_PIN);
    as5600.setDirection(AS5600_CLOCK_WISE);
    lastRaw       = 0xFFFF;      // re-seed; don't fabricate a delta across the gap
    i2cFailCount  = 0;
    serialPrintf("[INFO] I2C bus recovered (#%lu)\n", (unsigned long)i2cRecoveries);
}

// ---- BLE send helpers ------------------------------------------------------
// Text helper for [STATUS]/[INFO] lines: Serial + notify fan-out, newline-added.
// The dashboard distinguishes text from data frames by length/content (text
// lines start with '[' and are > 16 bytes; data frames are exactly 16 bytes).
static void sendLine(const char *s) {
    serialLine(s);                      // drop-don't-block: never stall the BLE task
    if (deviceConnected && pTxChar) {
        // MUST hold the biggest line sendStatus() can build (~155 bytes) plus
        // the newline and NUL. At 160 this had 3 bytes of headroom: one more
        // [STATUS] field and the JSON would be silently truncated mid-object,
        // which the dashboard surfaces only as "Bad STATUS JSON".
        char buf[256];
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
    // ASCII mirror on USB Serial only (not over BLE), and only while diagnostics
    // are enabled. Emitting this unconditionally at 25 Hz was pure UART pressure
    // on a port nobody is reading during a real session.
    if (diagMode) {
        serialPrintf("[%lums] RPM:%.1f POS:%ld R:%d\n",
                     (unsigned long)ms, rpm, (long)pos, resistance);
    }
}

static void sendStatus() {
    char buf[224];
    // Extra link telemetry so a flaky connection can be diagnosed from the
    // dashboard log instead of needing a serial monitor: lifetime connect/drop
    // counts, the HCI reason code of the last drop, uptime and I2C recoveries.
    snprintf(buf, sizeof(buf),
             "[STATUS] {\"name\":\"%s\",\"r\":%d,\"heap\":%u,\"fw\":\"1.1\","
             "\"conn\":%lu,\"disc\":%lu,\"dreason\":%u,\"up\":%lu,\"i2crec\":%lu}",
             BLE_DEVICE_NAME, currentResistance, (unsigned)ESP.getFreeHeap(),
             (unsigned long)connectCount, (unsigned long)disconnectCount,
             (unsigned)lastDiscReason,
             (unsigned long)(millis() / 1000), (unsigned long)i2cRecoveries);
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
        requestResistance(v);        // actuated by loop() — see requestedResistance
        sendLine((String("[INFO] resistance set ") + requestedResistance).c_str());
    } else if (upper == "RESET") {
        cumCounts = 0;
        lastRaw   = 0xFFFF;   // re-seed on next loop tick
        sendLine("[INFO] encoder position reset");
    } else if (upper == "S") {
        sendStatus();
    } else if (upper == "PING") {
        // Keep-alive. Deliberately silent: the point is only to give the OS BLE
        // stack bidirectional traffic (and to feed lastPeerMs) without spending
        // a notify slot on a [STATUS] payload every few seconds.
    } else if (upper == "STOP") {
        requestResistance(0);
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
        // Any write is proof the peer is alive — feeds the BLE_PEER_TIMEOUT_MS
        // dead-man check in loop(), which releases the brake if the dashboard
        // goes silent while the link is still nominally up.
        lastPeerMs = millis();
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
        // NOTE: deliberately NO "flush the remainder" here. Dispatching whatever
        // is left over at the end of a write defeats the buffering entirely: a
        // command split across two BLE writes ("R:4", then "5" plus a newline —
        // what a fragmented write on a marginal link looks like) would execute as
        // handleCommand("R:4"), silently setting 4 % instead of 45 %. The
        // remainder stays in rxBuf and is completed by the next write. Every
        // command this firmware accepts is newline-terminated by the dashboard
        // (writeCmd appends a newline), so nothing is lost by waiting for it.
    }
};

class ServerCallbacks : public BLEServerCallbacks {
    // Bluedroid overload that carries the connect params (peer address). We use
    // this instead of the no-arg onConnect so we can negotiate connection
    // parameters against the actual peer, and keep the address around so the
    // request can be repeated from loop() if the central renegotiates.
    void onConnect(BLEServer *pSrv, esp_ble_gatts_cb_param_t *param) override {
        deviceConnected = true;
        // Explicit load/store rather than ++: `++` on a volatile is deprecated
        // (the RMW is not atomic). Single writer — this callback — so this is safe.
        connectCount = connectCount + 1;
        memcpy(peerAddr, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        peerConnId    = param->connect.conn_id;
        peerAddrValid = true;
        lastPeerMs    = millis();

        // Ask the central for a link tuned for reliability over power:
        //   ~30-50 ms interval (comfortably faster than the 40 ms frame push),
        //   ZERO slave latency (never skip a connection event — skipping is what
        //   makes a marginal link fail), and a 6 s supervision timeout so a brief
        //   RF blockage is ridden out rather than killing the session.
        // Units: interval = n*1.25 ms, timeout = n*10 ms.
        pSrv->requestConnParams(peerAddr,
                                BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                BLE_CONN_LATENCY,      BLE_CONN_TIMEOUT);

        // Do NOT notify() here — defer the status line to the main loop.
        pendingConnectStatus = true;
        // Windows frequently accepts the request and then imposes its own
        // parameters a moment later; loop() re-asserts them once, shortly after
        // the link settles. See pendingConnParams.
        pendingConnParams    = true;
    }

    // Bluedroid overload carrying the disconnect params, so the HCI reason code
    // can be recorded. Common ones: 0x08 supervision timeout (out of range /
    // interference), 0x13 remote user terminated (the dashboard disconnected),
    // 0x16 local host terminated, 0x3E failed to establish.
    void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
        deviceConnected = false;
        disconnectCount = disconnectCount + 1;   // see note in onConnect re: volatile ++
        lastDiscReason  = param ? param->disconnect.reason : 0;
        peerAddrValid   = false;
        pendingConnParams = false;
        requestResistance(0);         // SAFETY: release the brake on disconnect
                                      // (actuated by loop() on its next ~1 ms tick)
        // Re-advertising is driven from loop() (see BLE_READVERTISE_MS) rather
        // than with a blocking delay() inside the stack callback.
        disconnectAtMs  = millis();
        if (disconnectAtMs == 0) disconnectAtMs = 1;   // 0 is the "nothing pending" sentinel
    }
};

// ---- Advertising -----------------------------------------------------------
// Single place that starts advertising and records that we did. Called at boot,
// after every disconnect, and by the periodic re-assert in loop(). Starting
// advertising while it is already running is harmless, which is what makes the
// re-assert a safe blanket fix for a stack that silently fell out of it.
static void startAdvertisingNow() {
    if (!pServer) return;
    pServer->startAdvertising();
    advReassertMs = millis();
}

// ---- Loop watchdog ---------------------------------------------------------
// Subscribe the Arduino loop task to the Task Watchdog Timer. If loop() stops
// feeding it for LOOP_WDT_TIMEOUT_S the chip panics and reboots, which is a far
// better failure mode than a silently wedged device: a reboot releases the brake
// and comes back advertising within a couple of seconds.
//
// The Arduino core may have already initialised the TWDT (depending on
// CONFIG_ESP_TASK_WDT_INIT), so esp_task_wdt_init() can return ESP_ERR_INVALID_STATE;
// in that case reconfigure the existing instance instead of giving up.
static void initLoopWatchdog() {
    // The Arduino core builds with CONFIG_ESP_TASK_WDT_INIT=1, so the TWDT is
    // ALREADY running (5 s, panic on timeout) and already watching the CPU0 idle
    // task via CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0. So:
    //   - Do NOT call esp_task_wdt_init()/reconfigure() with idle_core_mask = 0;
    //     that would silently UNSUBSCRIBE the idle task the core registered.
    //   - We only need to subscribe the loop task to the existing instance.
    // The stock 5 s timeout is already shorter than LOOP_WDT_TIMEOUT_S would be,
    // and a shorter timeout is strictly safer here, so we simply adopt it rather
    // than reconfiguring a watchdog that other subsystems are also relying on.
    esp_err_t err = esp_task_wdt_status(NULL);
    if (err == ESP_ERR_NOT_FOUND) {                 // not yet subscribed
        err = esp_task_wdt_add(NULL);               // NULL = the calling (loop) task
    } else if (err == ESP_ERR_INVALID_STATE) {      // TWDT not running at all
        esp_task_wdt_config_t cfg = {
            .timeout_ms     = LOOP_WDT_TIMEOUT_S * 1000U,
            .idle_core_mask = 0,
            .trigger_panic  = true,
        };
        if (esp_task_wdt_init(&cfg) == ESP_OK) err = esp_task_wdt_add(NULL);
    }
    if (err == ESP_OK) Serial.println(F("[INFO] loop watchdog armed"));
    else               Serial.printf("[INFO] loop watchdog unavailable (%d)\n", (int)err);
}

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

    // Arm the loop watchdog EARLY. Armed at the end of setup() it did not cover
    // setup() at all — including BLEDevice::init(), which is the most likely
    // place for the stack to wedge on a bad boot. Everything after this point
    // feeds it, so a hang anywhere in bring-up now reboots into a retry instead
    // of leaving a silent, non-advertising board.
    initLoopWatchdog();

    // Encoder
    Wire.begin();                                  // default SDA 21 / SCL 22
    // Bound every I2C transaction. Without this a wedged bus (loose connector,
    // sensor rail glitch) blocks readAngle() indefinitely, which stalls loop(),
    // starves the BLE task and drops the link. With it, a bad read fails fast
    // and recoverI2C() puts the bus back.
    Wire.setTimeOut(I2C_TIMEOUT_MS);
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
        esp_task_wdt_reset();       // this loop spends ~300 ms in delay()
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

    // 2.4 GHz band: nothing to do. This firmware never calls WiFi.begin() or
    // esp_wifi_init(), so the Wi-Fi driver is never started and BLE already has
    // the radio to itself — there is no coexistence arbitration to disable.
    // (An explicit esp_wifi_stop() would link the entire Wi-Fi driver for no
    // benefit; see the note by the includes.)

    // BLE (Ferra-identical sequence)
    esp_task_wdt_reset();
    BLEDevice::init(BLE_DEVICE_NAME);
    // TX power: MAXIMUM on every power type. The radio used to be pinned at
    // -3 dBm as a brownout workaround, costing ~12 dB against the +9 dBm max —
    // roughly a 4x reduction in usable range, and the single biggest cause of
    // drops when the laptop is across the room. The brownout detector is
    // disabled above and the BLE stack is started after a settle delay, so the
    // workaround is no longer needed. Advertising power is set separately
    // because the advertising packet is what a disconnected central must hear
    // before it can reconnect at all.
    BLEDevice::setPower(BLE_TX_POWER_DEFAULT, ESP_BLE_PWR_TYPE_DEFAULT);
    BLEDevice::setPower(BLE_TX_POWER_ADV,     ESP_BLE_PWR_TYPE_ADV);
    BLEDevice::setPower(BLE_TX_POWER_ADV,      ESP_BLE_PWR_TYPE_SCAN);
    // Every connection handle gets full power too. ESP_BLE_PWR_TYPE_DEFAULT does
    // not always propagate to an already-open handle, so set each explicitly.
    BLEDevice::setPower(BLE_TX_POWER_CONN, ESP_BLE_PWR_TYPE_CONN_HDL0);
    BLEDevice::setPower(BLE_TX_POWER_CONN, ESP_BLE_PWR_TYPE_CONN_HDL1);
    BLEDevice::setPower(BLE_TX_POWER_CONN, ESP_BLE_PWR_TYPE_CONN_HDL2);
#if BLE_DISABLE_MODEM_SLEEP
    // Keep the receiver hot between connection events. Modem sleep saves current
    // that a mains/USB-powered trainer does not need to save, and costs wake-up
    // latency that makes a marginal link likelier to miss an event.
    esp_bt_sleep_disable();
#endif
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

    // Advertising tuned for fast rediscovery: a 20-40 ms interval is the BT-spec
    // "fast connection" range, so after a drop the central finds us again almost
    // immediately instead of waiting out a slow beacon. Scan response carries the
    // name, and setMinPreferred(0x06) is the well-known workaround that stops iOS
    // and some Windows stacks from imposing a sluggish connection interval.
    BLEAdvertising *pAdv = pServer->getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
#if BLE_NAME_IN_SCAN_RESPONSE
    // Keep the ADVERTISING packet minimal (flags + 128-bit service UUID) and put
    // the device name in the SCAN RESPONSE instead. A 128-bit UUID already eats
    // 18 of the 31 payload bytes; adding "CardioDrum" on top risks overflowing
    // the packet, and a shorter packet is on air for less time — fewer bits to
    // corrupt and less chance of colliding with a Wi-Fi burst, which is exactly
    // what limits reception at the edge of range.
    {
        BLEAdvertisementData scanRsp;
        scanRsp.setName(BLE_DEVICE_NAME);
        pAdv->setScanResponseData(scanRsp);
    }
#endif
    pAdv->setMinInterval(BLE_ADV_MIN_INTERVAL);
    pAdv->setMaxInterval(BLE_ADV_MAX_INTERVAL);
    pAdv->setMinPreferred(0x06);
    pAdv->setMaxPreferred(0x12);
    startAdvertisingNow();
    Serial.println(F("[INFO] BLE advertising as CardioDrum — waiting for a client."));

    esp_task_wdt_reset();          // bring-up done; hand a clean slate to loop()

    lastPushMs  = millis();
    lastPeerMs  = millis();
}

// ---- Loop ------------------------------------------------------------------
void loop() {
    uint32_t nowMs = millis();
    uint32_t nowUs = micros();

    // Actuate any resistance requested by the BLE task. loop() is the SOLE
    // caller of scaleResistance(), so the ledcWrite + currentResistance store
    // can never interleave with another task's call to the same pair.
    if (resistanceDirty) {
        resistanceDirty = false;
        scaleResistance(requestedResistance);
    }

    // Feed the task watchdog. Everything below is non-blocking by construction
    // (bounded I2C, drop-don't-block Serial, no delay() on the disconnect path),
    // so reaching here every tick is the normal case; missing it means loop()
    // genuinely wedged and a reboot is the right answer.
    esp_task_wdt_reset();

    // Single I2C read per tick — we own every angle sample.
    uint16_t rawAngle = as5600.readAngle();   // 0-4095

    // Encoder health. The AS5600 library gives readAngle() no error channel, so a
    // dead bus reads as a value that never changes. A frozen reading is normal
    // when the drum is simply at rest, so a stuck value alone proves nothing —
    // we only treat it as a fault when the cheap isConnected() probe also fails.
    // Probing is rate-limited so it never doubles the per-tick I2C traffic.
    static uint32_t lastProbeMs = 0;
    if (nowMs - lastProbeMs >= 1000) {
        lastProbeMs = nowMs;
        if (!as5600.isConnected()) {
            if (++i2cFailCount >= I2C_FAIL_LIMIT) recoverI2C();
        } else {
            i2cFailCount = 0;
        }
    }

    int16_t  delta    = pushAngle(rawAngle);  // updates cumCounts
    float    rpm      = computeRPM(nowUs);

    // Diagnostic CSV — Serial only, and OFF by default (see DIAG_DEFAULT_ON).
    // Rate-limited to DIAG_PERIOD_MS and routed through serialPrintf(), which
    // DROPS output when the UART TX buffer is full rather than blocking. A
    // blocked loop starves the BLE task, which then misses connection events and
    // hits the supervision timeout -> spurious disconnect.
    // Columns: ms, raw(0-4095), delta, cum_counts, rpm
    static uint32_t lastDiagMs = 0;
    if (diagMode && (nowMs - lastDiagMs >= DIAG_PERIOD_MS)) {
        lastDiagMs = nowMs;
        serialPrintf("DIAG,%lu,%u,%d,%ld,%.2f\n",
                     (unsigned long)nowMs, rawAngle, (int)delta,
                     (long)cumCounts, rpm);
    }

    // Fixed-rate data push.
    if (nowMs - lastPushMs >= SAMPLE_PERIOD_MS) {
        lastPushMs += SAMPLE_PERIOD_MS;
        // Resync whenever we are still a WHOLE period behind after that step,
        // i.e. more than one push was missed. The old test (> 2 periods) let a
        // ~100 ms stall leave a 60 ms residual, so the loop emitted a burst of
        // back-to-back notifies on consecutive ~1 ms ticks to catch up. Those
        // pile into the controller queue and notify() DROPS silently on
        // ESP_ERR_NO_MEM — losing exactly the frames we were trying to deliver.
        // Skipping straight to now costs one gap instead of a lossy burst.
        if (nowMs - lastPushMs >= SAMPLE_PERIOD_MS) lastPushMs = nowMs;

        sendFrame(nowMs, rpm, cumCounts, currentResistance);
    }

    // ---- Link management ---------------------------------------------------
    // Re-advertise after a disconnect. Deferred by BLE_READVERTISE_MS from the
    // callback via a timestamp rather than a blocking delay(500) inside loop():
    // that delay stalled the encoder sampling and the data push for half a
    // second on every drop, and delayed nothing useful.
    if (!deviceConnected && disconnectAtMs && (nowMs - disconnectAtMs >= BLE_READVERTISE_MS)) {
        disconnectAtMs = 0;
        startAdvertisingNow();
        serialPrintf("[INFO] re-advertising (drop #%lu, reason 0x%02X)\n",
                     (unsigned long)disconnectCount, (unsigned)lastDiscReason);
    }

    // Advertising self-heal. Re-kick advertising on a slow timer whenever we are
    // unconnected, regardless of whether a re-advertise is pending. Gating this
    // on !disconnectAtMs was wrong: onDisconnect() runs on the BLE task and can
    // re-set disconnectAtMs at any moment, which would gate off the ONLY thing
    // that recovers a silently-failed startAdvertising(). A device that fails to
    // start advertising while unconnected is invisible forever — the loop
    // watchdog cannot catch it, because loop() is running perfectly happily.
    // startAdvertising() on an already-advertising stack is a no-op, so running
    // this unconditionally costs nothing.
    if (!deviceConnected && (nowMs - advReassertMs >= BLE_ADV_REASSERT_MS)) {
        startAdvertisingNow();
    }

    // Re-assert the connection parameters shortly after the link comes up.
    // Windows commonly accepts the request in onConnect and then imposes its own
    // a moment later; asking again once the link has settled usually sticks, and
    // costs one control PDU if it was already honoured.
    if (pendingConnParams && deviceConnected && peerAddrValid) {
        if (connParamsAtMs == 0) {
            connParamsAtMs = nowMs;
        } else if (nowMs - connParamsAtMs >= 1000) {
            pendingConnParams = false;
            connParamsAtMs    = 0;
            pServer->requestConnParams(peerAddr,
                                       BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                       BLE_CONN_LATENCY,      BLE_CONN_TIMEOUT);
        }
    } else if (!deviceConnected) {
        connParamsAtMs = 0;
    }

    // Peer dead-man. If the link is nominally up but the dashboard has said
    // nothing for BLE_PEER_TIMEOUT_MS (much longer than its keep-alive cadence),
    // the peer is gone in every sense that matters — a half-open link where our
    // notifies vanish into the void. Release the brake so a rider is never left
    // pulling against a load nobody is controlling, and drop the link so the
    // dashboard's reconnect logic gets a clean slate to work with.
    if (deviceConnected && (nowMs - lastPeerMs >= BLE_PEER_TIMEOUT_MS)) {
        serialPrintf("[INFO] peer silent for %lu ms — releasing brake, dropping link\n",
                     (unsigned long)(nowMs - lastPeerMs));
        scaleResistance(0);                 // SAFETY first (we ARE loop(); direct is fine)
        lastPeerMs = nowMs;                 // don't re-fire every tick
        // Close THIS connection by the id captured on connect, not
        // getConnId() — that is never reset and can name a newer link.
        if (pServer && peerAddrValid) pServer->disconnect(peerConnId);
    }

    // Track connect/disconnect edges for anything that needs them.
    if (!deviceConnected && oldDeviceConnected) oldDeviceConnected = false;
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = true;
        // Fresh connection: restart the conn-param settle window. Without this
        // the latch can still hold the PREVIOUS connection's timestamp, making
        // the re-request fire instantly instead of after the 1 s settle — which
        // is exactly when Windows is most likely to override it.
        connParamsAtMs = 0;
    }

    // Deferred connect-time status line — sent from the loop, NOT from the
    // onConnect callback (notify() inside the stack callback can drop the link).
    if (pendingConnectStatus && deviceConnected) {
        pendingConnectStatus = false;
        sendStatus();
    }

    // Serial commands, so the board can be driven from a monitor with no BLE
    // client attached (useful when diagnosing a link problem).
    static String serialBuf;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            if (serialBuf.length()) { handleCommand(serialBuf); serialBuf = ""; }
        } else {
            serialBuf += ch;
            if (serialBuf.length() > 64) serialBuf = "";
        }
    }

    delay(1);   // ~1 ms tick
}
