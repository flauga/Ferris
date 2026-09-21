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
//    esp -> app   status: [STATUS] {"name":..,"r":..,"heap":..,"fw":"1.2",
//                              "conn":..,"disc":..,"dreason":..,"up":..,"i2crec":..,
//                              "rst":..,"bor":..,"rsts":..,"bod":..}
//                              dreason = HCI reason of the last drop (0x08 = RF
//                              supervision timeout, 0x13/0x16 = the browser hung
//                              up, 0x3E = never established).
//                              rst/bor/rsts/bod = supply-rail diagnostics: this
//                              boot's reset reason, brownout resets and total
//                              resets since power-on, and whether the brownout
//                              detector is armed.
//    esp -> app   info:   [INFO] <message>
//    app -> esp   cmds:   R:<nn> | RESET | S | STOP | PING | DIAG ON | DIAG OFF
//
//  Link reliability (see include/config.h for the tunables):
//    - TX power at max (+9 dBm) for both advertising and the connected link.
//    - Advertising and scan-response payloads are built EXPLICITLY so the core
//      cannot auto-append the device name and overflow the 31-byte limit (which
//      silently dropped the stack back to a default advertisement).
//    - Supply-rail diagnostics: brownout resets are counted across reboots and
//      reported, because a sagging rail cuts real TX power and is otherwise
//      indistinguishable from an RF range problem.
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
#include <esp_system.h>        // esp_reset_reason() — supply-rail diagnostics
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
// Edge latch for the dead-man above, so the brake is released ONCE per silence
// rather than re-fired on every ~1 ms tick, and so the peer's recovery can be
// logged. Deltas against lastPeerMs MUST be computed as SIGNED - see the
// dead-man block in loop() for the underflow this prevents.
static bool peerSilent = false;

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

// ---- Supply-rail diagnostics -----------------------------------------------
// Brownout resets are counted in RTC_NOINIT memory, which survives a RESET but
// not a power cycle — exactly the lifetime we want. A tally that survived a
// power cycle would report stale history from a previous session; one kept in
// ordinary RAM would be wiped by the very reset it is trying to record.
//
// RTC_NOINIT_ATTR is deliberately NOT zero-initialised by the startup code, so
// it must be validated with a magic word before it is trusted: on the first boot
// after power-on it holds whatever was in RTC RAM.
#define RAIL_DIAG_MAGIC 0x43447631u   // "CDv1" — marks the tally as initialised
RTC_NOINIT_ATTR static uint32_t railDiagMagic;
RTC_NOINIT_ATTR static uint32_t brownoutResets;   // resets attributed to brownout
RTC_NOINIT_ATTR static uint32_t totalResets;      // resets since power-on

// Reset reason for THIS boot, captured once in setup() before anything can
// overwrite it. Reported in [STATUS] as "rst".
static uint8_t  lastResetReason = 0;

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

// ---- Safe millis() deltas --------------------------------------------------
// EVERY timeout in this firmware compares a stale `nowMs` (sampled once at the
// top of loop()) against a timestamp that may have been written LATER by the
// BLE task. With unsigned arithmetic, "later" underflows to ~4.29e9 ms and
// fires the timeout instantly. That bug killed healthy BLE links for weeks and
// presented as a range problem (see the dead-man block in loop()).
//
// msSince() is the only correct way to age a millis() timestamp here:
//   - signed result, so a future timestamp gives a small negative age
//   - correct across the 49.7-day millis() rollover, because the subtraction is
//     done in uint32 and only THEN reinterpreted as signed
// Use it for every timeout; never write `now - then >= LIMIT` directly.
static inline int32_t msSince(uint32_t nowMs, uint32_t thenMs) {
    return (int32_t)(nowMs - thenMs);
}

// True when `thenMs` is at least `limitMs` in the past, underflow-safe.
static inline bool msElapsed(uint32_t nowMs, uint32_t thenMs, uint32_t limitMs) {
    return msSince(nowMs, thenMs) >= (int32_t)limitMs;
}

// ---- Non-blocking Serial ---------------------------------------------------
// Serial.print* BLOCKS once the UART TX FIFO is full, which happens whenever the
// firmware emits faster than the host drains (and always when no monitor is
// attached at all). A blocked loop() starves the BLE task, the peripheral misses
// connection events, and the central eventually times the link out — i.e. the
// USB debug path can kill the wireless link. Every Serial write below is gated
// on there being room in the TX buffer, so output is DROPPED rather than
// allowed to stall the loop. Diagnostics are best-effort; the link is not.
// availableForWrite() reports the free space in the UART TX RING BUFFER, and
// falls back to the 128-byte HARDWARE FIFO when no ring buffer is installed.
// Serial.begin() without an explicit txBufferSize installs none, so the value
// was capped at 128 — and every line longer than that (notably [STATUS], at
// ~236 bytes) failed this check on EVERY call and was silently dropped forever.
// SERIAL_TX_BUFFER_BYTES below installs a real ring buffer so long lines fit.
//
// The check stays: it is what keeps a full buffer from BLOCKING Serial.write()
// and stalling loop(). Only the capacity was wrong, not the idea.
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
        // MUST hold the biggest line sendStatus() can build, plus the newline
        // and NUL. Keep this >= sendStatus()'s own buffer (320): if it is
        // smaller, the JSON is silently truncated mid-object and the dashboard
        // surfaces it only as "Bad STATUS JSON". Adding a [STATUS] field means
        // checking BOTH buffers.
        char buf[352];
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
    // Sized for the FULL line with every field at its widest (32-bit counters
    // printed in full). The rail-diagnostic fields below pushed the worst case
    // past the previous 224, and a truncated line reaches the dashboard only as
    // "Bad STATUS JSON" — see the matching note in sendLine()'s buffer.
    char buf[320];
    // Extra link telemetry so a flaky connection can be diagnosed from the
    // dashboard log instead of needing a serial monitor: lifetime connect/drop
    // counts, the HCI reason code of the last drop, uptime and I2C recoveries.
    //
    // Rail diagnostics (rst/bor/rsts): "rst" is THIS boot's reset reason, "bor"
    // the brownout resets since power-on and "rsts" all resets since power-on.
    // A climbing "bor" mid-session means the supply is sagging, NOT that the RF
    // link is bad — the two are indistinguishable from the dashboard otherwise,
    // because a brownout reset also makes the device vanish and re-advertise.
#if RAIL_DIAG_ENABLE
    snprintf(buf, sizeof(buf),
             "[STATUS] {\"name\":\"%s\",\"r\":%d,\"heap\":%u,\"fw\":\"1.2\","
             "\"conn\":%lu,\"disc\":%lu,\"dreason\":%u,\"up\":%lu,\"i2crec\":%lu,"
             "\"rst\":%u,\"bor\":%lu,\"rsts\":%lu,\"bod\":%d}",
             BLE_DEVICE_NAME, currentResistance, (unsigned)ESP.getFreeHeap(),
             (unsigned long)connectCount, (unsigned long)disconnectCount,
             (unsigned)lastDiscReason,
             (unsigned long)(millis() / 1000), (unsigned long)i2cRecoveries,
             (unsigned)lastResetReason,
             (unsigned long)brownoutResets, (unsigned long)totalResets,
             BROWNOUT_DETECT_ENABLE ? 1 : 0);
#else
    snprintf(buf, sizeof(buf),
             "[STATUS] {\"name\":\"%s\",\"r\":%d,\"heap\":%u,\"fw\":\"1.2\","
             "\"conn\":%lu,\"disc\":%lu,\"dreason\":%u,\"up\":%lu,\"i2crec\":%lu}",
             BLE_DEVICE_NAME, currentResistance, (unsigned)ESP.getFreeHeap(),
             (unsigned long)connectCount, (unsigned long)disconnectCount,
             (unsigned)lastDiscReason,
             (unsigned long)(millis() / 1000), (unsigned long)i2cRecoveries);
#endif
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
    } else if (upper == "REBOOT") {
        // Software reset. Useful during range testing (recover the board without
        // physical access) and the only way to exercise the RTC-persisted rail
        // counters: pulling EN low resets the whole RTC domain and reports
        // POWERON_RESET, which legitimately clears them, so EN cannot test them.
        sendLine("[INFO] rebooting");
        requestResistance(0);            // SAFETY: never reboot with the brake on
        scaleResistance(0);              // actuate immediately; loop() won't run again
        delay(50);                       // let the line drain before the reset
        esp_restart();
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
    // ---- Supply rail: record the reset cause, THEN decide on the detector ---
    // Order matters. esp_reset_reason() must be read before anything touches the
    // brownout register, and the tally must be updated before BLE starts so a
    // boot loop is still counted if the stack never comes up.
    lastResetReason = (uint8_t)esp_reset_reason();
    if (railDiagMagic != RAIL_DIAG_MAGIC) {
        // First boot after a power cycle: RTC_NOINIT holds garbage. Seed it.
        railDiagMagic  = RAIL_DIAG_MAGIC;
        brownoutResets = 0;
        totalResets    = 0;
    } else {
        totalResets++;
        if (lastResetReason == ESP_RST_BROWNOUT) brownoutResets++;
    }

    // The hardware brownout detector. BLE stack startup draws a current spike
    // that sags a marginal USB supply below the ~2.97V trip threshold, causing a
    // reset loop — which is why this was previously disabled unconditionally.
    //
    // Disabling it does NOT fix the rail, it only hides it: a sagging VDD3P3_RF
    // reduces PA output power, so the +9 dBm configured below is not what
    // actually goes on air. Re-arm the detector (BROWNOUT_DETECT_ENABLE 1) after
    // improving the supply to CONFIRM the fix — no resets with it armed means
    // the rail is clean and the TX power setting is real.
#if BROWNOUT_DETECT_ENABLE
    // Left at its hardware default (armed). Nothing to do.
#else
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

    // Install an explicit TX ring buffer. Without one, availableForWrite() is
    // limited to the 128-byte hardware FIFO and serialHasRoom() rejects every
    // line longer than that — which silently suppressed [STATUS] on Serial.
    // Must be >= the longest line sendLine() can emit (see its buffer).
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(SERIAL_BAUD);
    delay(200);
    Serial.println(F("[INFO] Cardio Drum Trainer — ESP32"));

#if RAIL_DIAG_ENABLE
    // Boot-time rail report. Printed with the plain (blocking) Serial API on
    // purpose: this is bring-up, the BLE task does not exist yet, so there is no
    // link for a stalled write to starve, and this line must never be dropped.
    {
        const char *why;
        switch (lastResetReason) {
            case ESP_RST_POWERON:  why = "power-on";                break;
            case ESP_RST_BROWNOUT: why = "BROWNOUT (supply sag!)";  break;
            case ESP_RST_SW:       why = "software";                break;
            case ESP_RST_PANIC:    why = "panic/exception";         break;
            case ESP_RST_TASK_WDT: why = "task watchdog";           break;
            case ESP_RST_INT_WDT:  why = "interrupt watchdog";      break;
            case ESP_RST_WDT:      why = "other watchdog";          break;
            case ESP_RST_DEEPSLEEP:why = "deep-sleep wake";         break;
            case ESP_RST_EXT:      why = "external pin";            break;
            case ESP_RST_SDIO:     why = "SDIO";                    break;
            default:               why = "unknown";                 break;
        }
        Serial.printf("[INFO] reset reason: %s (%u) | brownout resets: %lu of %lu since power-on\n",
                      why, (unsigned)lastResetReason,
                      (unsigned long)brownoutResets, (unsigned long)totalResets);
        Serial.printf("[INFO] brownout detector: %s\n",
                      BROWNOUT_DETECT_ENABLE ? "ARMED (rail-confirmation mode)"
                                             : "disabled (suppressed, rail unverified)");
        if (brownoutResets > 0) {
            Serial.println(F("[INFO] *** Supply rail is sagging. TX power is NOT the +9 dBm configured: ***"));
            Serial.println(F("[INFO] *** a drooping VDD3P3_RF cuts real PA output. Fix the supply    ***"));
            Serial.println(F("[INFO] *** (bulk cap at the module, better cable/PSU) before blaming RF.***"));
        }
    }
#endif

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
    //
    // Check the return: this call fails silently if the controller is not yet in
    // a state to accept it, and a silent failure is indistinguishable from a
    // working call — which made the "is modem sleep actually off?" question
    // unanswerable. It also costs ~40 mA, so see the A/B note in config.h: if
    // range is BETTER with this compiled out, the supply rail is the real limit.
    {
        esp_err_t slpErr = esp_bt_sleep_disable();
        Serial.printf("[INFO] modem sleep disable: %s\n",
                      slpErr == ESP_OK ? "ok" : esp_err_to_name(slpErr));
    }
#else
    Serial.println(F("[INFO] modem sleep left ENABLED (rail A/B experiment)"));
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
#if BLE_NAME_IN_SCAN_RESPONSE
    // ---- Explicit advertising payload (overflow-proof) ---------------------
    // The previous code called addServiceUUID() + setScanResponseData() and
    // TRUSTED the core to keep the name out of the advertising packet. It does
    // not. BLEAdvertising::start() builds the adv payload itself and appends the
    // device name set in BLEDevice::init() whenever m_advData was never supplied
    // by the caller; setScanResponseData() only populates the SCAN RESPONSE and
    // does nothing to suppress that. The result was
    //     flags(3) + 128-bit UUID(18) + "CardioDrum"(12) = 33 bytes
    // against the 31-byte legacy-advertising limit. esp_ble_gap_config_adv_data()
    // then fails with ESP_ERR_INVALID_ARG and the stack advertises a DEFAULT
    // payload — which is why discovery and range were worse here than on other
    // boards using the same PCB.
    //
    // Fix: supply BOTH payloads explicitly so nothing is auto-appended, and move
    // the 128-bit service UUID into the scan response alongside the name. The
    // advertising packet then carries only flags — it is tiny, spends minimal
    // time on air (fewer bits to corrupt, less chance of colliding with a Wi-Fi
    // burst) and cannot overflow.
    //
    // Discovery still works: the dashboard's requestDevice() filters on
    // {namePrefix:'CardioDrum'} OR {services:[SERVICE_UUID]}, and Web Bluetooth
    // matches BOTH filters against scan-response data as well as the advertising
    // packet. Centrals that scan actively (every OS Web Bluetooth runs on) issue
    // a SCAN_REQ and get the name + UUID in the SCAN_RSP.
    {
        BLEAdvertisementData advData;
        // BR/EDR not supported + LE General Discoverable. Set explicitly: an
        // advertisement with no flags at all is treated as non-discoverable by
        // some centrals.
        advData.setFlags(0x06);
        pAdv->setAdvertisementData(advData);

        BLEAdvertisementData scanRsp;
        scanRsp.setName(BLE_DEVICE_NAME);
        scanRsp.setCompleteServices(BLEUUID(SERVICE_UUID));
        pAdv->setScanResponseData(scanRsp);
    }
    pAdv->setScanResponse(true);
#else
    // Legacy layout: UUID in the advertising packet. Kept only as an escape
    // hatch for a central that does not scan actively; it is 21 of 31 bytes and
    // leaves no room for the name, so the name is unavailable pre-connection.
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
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
    if (msElapsed(nowMs, lastProbeMs, 1000)) {
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
    if (diagMode && msElapsed(nowMs, lastDiagMs, DIAG_PERIOD_MS)) {
        lastDiagMs = nowMs;
        serialPrintf("DIAG,%lu,%u,%d,%ld,%.2f\n",
                     (unsigned long)nowMs, rawAngle, (int)delta,
                     (long)cumCounts, rpm);
    }

    // Fixed-rate data push.
    if (msElapsed(nowMs, lastPushMs, SAMPLE_PERIOD_MS)) {
        lastPushMs += SAMPLE_PERIOD_MS;
        // Resync whenever we are still a WHOLE period behind after that step,
        // i.e. more than one push was missed. The old test (> 2 periods) let a
        // ~100 ms stall leave a 60 ms residual, so the loop emitted a burst of
        // back-to-back notifies on consecutive ~1 ms ticks to catch up. Those
        // pile into the controller queue and notify() DROPS silently on
        // ESP_ERR_NO_MEM — losing exactly the frames we were trying to deliver.
        // Skipping straight to now costs one gap instead of a lossy burst.
        if (msElapsed(nowMs, lastPushMs, SAMPLE_PERIOD_MS)) lastPushMs = nowMs;

        sendFrame(nowMs, rpm, cumCounts, currentResistance);
    }

    // ---- Link management ---------------------------------------------------
    // Re-advertise after a disconnect. Deferred by BLE_READVERTISE_MS from the
    // callback via a timestamp rather than a blocking delay(500) inside loop():
    // that delay stalled the encoder sampling and the data push for half a
    // second on every drop, and delayed nothing useful.
    if (!deviceConnected && disconnectAtMs && msElapsed(nowMs, disconnectAtMs, BLE_READVERTISE_MS)) {
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
    if (!deviceConnected && msElapsed(nowMs, advReassertMs, BLE_ADV_REASSERT_MS)) {
        startAdvertisingNow();
    }

    // Re-assert the connection parameters shortly after the link comes up.
    // Windows commonly accepts the request in onConnect and then imposes its own
    // a moment later; asking again once the link has settled usually sticks, and
    // costs one control PDU if it was already honoured.
    if (pendingConnParams && deviceConnected && peerAddrValid) {
        if (connParamsAtMs == 0) {
            connParamsAtMs = nowMs;
        } else if (msElapsed(nowMs, connParamsAtMs, 1000)) {
            pendingConnParams = false;
            connParamsAtMs    = 0;
            pServer->requestConnParams(peerAddr,
                                       BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL,
                                       BLE_CONN_LATENCY,      BLE_CONN_TIMEOUT);
        }
    } else if (!deviceConnected) {
        connParamsAtMs = 0;
    }

    // ---- Peer dead-man ----------------------------------------------------
    // Purpose: if the dashboard goes silent while the link is nominally up (a
    // half-open link where our notifies vanish into the void), RELEASE THE
    // BRAKE so a rider is never left pulling against a load nobody controls.
    //
    // Two bugs lived here, both of which killed healthy links:
    //
    // 1. UNSIGNED UNDERFLOW - the cause of the "random" disconnects.
    //    `nowMs` is sampled ONCE at the top of loop(), but lastPeerMs is written
    //    from the BLE task in onWrite() with its OWN, later millis(). A keep-alive
    //    landing mid-iteration therefore made lastPeerMs GREATER than nowMs, and
    //    the unsigned subtraction wrapped to ~4.29e9 ms - instantly past the
    //    threshold. Observed live on a healthy link carrying 1875 frames at
    //    24.4 Hz:
    //        [INFO] peer silent for 4294967295 ms - releasing brake, dropping link
    //    (4294967295 == UINT32_MAX == a 1 ms underflow.) The wrap made the failure
    //    look random and load-dependent, which is what disguised it as an RF or
    //    range problem.
    //    Fix: read the timestamp ONCE into a local and compare as SIGNED, so a
    //    peer timestamp from the future gives a small negative age instead of a
    //    huge positive one. This is the standard safe idiom for millis() deltas
    //    and stays correct across the 49.7-day rollover.
    //
    // 2. IT DROPPED THE LINK. Releasing the brake is the safety requirement;
    //    tearing down a live GATT connection is a far bigger hammer than that
    //    goal needs, and it forced a full reconnect (losing the encoder zero)
    //    every time it misfired. Now the brake is released and the link is LEFT
    //    UP: if the peer really is gone, the supervision timeout reaps the
    //    connection on its own schedule; if it is not, we have cost nothing.
    {
        // Single read of the volatile: re-reading between the test and the log
        // could otherwise report a different age than the one we acted on.
        const uint32_t peerMs = lastPeerMs;
        const int32_t  ageMs  = msSince(nowMs, peerMs);      // signed: future => negative

        if (deviceConnected && ageMs >= (int32_t)BLE_PEER_TIMEOUT_MS) {
            if (!peerSilent) {              // edge-triggered: act once per silence
                peerSilent = true;
                serialPrintf("[INFO] peer silent for %ld ms - releasing brake (link left up)\n",
                             (long)ageMs);
                scaleResistance(0);         // SAFETY (we ARE loop(); direct is fine)
            }
        } else if (peerSilent && ageMs < (int32_t)BLE_PEER_TIMEOUT_MS) {
            peerSilent = false;             // peer spoke again - re-arm
            serialPrintf("[INFO] peer alive again (age %ld ms)\n", (long)ageMs);
        }
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
