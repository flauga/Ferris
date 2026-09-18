#pragma once
// ============================================================================
//  Cardio Drum Trainer — firmware configuration
//  All tunable constants live here. Physics math itself is done in the
//  dashboard JS (see dashboard/cardio_dashboard.html); the physics constants
//  below are a record-of-truth so both sides agree.
// ============================================================================

// esp_bt.h supplies the esp_power_level_t enum (ESP_PWR_LVL_*) used by the BLE
// TX-power settings below, so this header is self-contained and does not depend
// on being included after BLEDevice.h.
#include <esp_bt.h>

// ---- Serial ----------------------------------------------------------------
#define SERIAL_BAUD            115200

// ---- BLE (Nordic UART Service — reuse Ferra's UUIDs verbatim) --------------
#define BLE_DEVICE_NAME        "CardioDrum"
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_RX_UUID           "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // write  (app -> esp)
#define CHAR_TX_UUID           "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // notify (esp -> app)
#define BLE_MTU                512

// ---- BLE link reliability / range -----------------------------------------
// TX power. The radio was previously pinned at -3 dBm as a brownout workaround,
// which costs ~12 dB against the +9 dBm maximum — roughly a 4x reduction in
// usable range. The brownout detector is disabled in setup() and the BLE stack
// is brought up after a settle delay, so the workaround is no longer needed.
// Advertising and the connected link are set independently: advertising is the
// packet a cold/disconnected central has to hear, so it gets full power.
//   Levels: N12 N9 N6 N3 N0 P3 P6 P9  (-12 .. +9 dBm)
#define BLE_TX_POWER_ADV       ESP_PWR_LVL_P9   // +9 dBm — maximise discovery range
#define BLE_TX_POWER_CONN      ESP_PWR_LVL_P9   // +9 dBm — maximise in-session range
#define BLE_TX_POWER_DEFAULT   ESP_PWR_LVL_P9   // applies to any other power type

// NOTE on BLE 5 "Long Range" (LE Coded PHY, up to 4x range): NOT AVAILABLE on
// this hardware. The classic ESP32 is a Bluetooth 4.2 radio
// (CONFIG_BT_BLE_42_FEATURES_SUPPORTED, no BLE_50), so Coded PHY and 2M PHY do
// not exist here. Reaching for them means moving to an ESP32-C3/S3/C6, which
// would also need a Web-Bluetooth-capable central that negotiates Coded PHY.
// Everything below is what IS available on a 4.2 radio.

// Modem sleep. The BLE controller powers the radio down between connection
// events to save current; on a mains/USB-powered device that only buys wake-up
// latency and a slightly higher chance of missing an event on a marginal link.
// Disabling it keeps the receiver consistently hot.
#define BLE_DISABLE_MODEM_SLEEP  1

// Advertise at the connection-relevant power on ALL advertising handles, and
// keep the advertising payload SMALL. A shorter packet spends less time on air,
// which measurably improves reception at the edge of range: fewer bits to
// corrupt, and less chance of colliding with a Wi-Fi burst. The device name is
// moved into the scan response so the advertising packet carries only flags +
// the service UUID.
#define BLE_NAME_IN_SCAN_RESPONSE 1

// Connection parameters requested from the central (units: interval = n*1.25 ms,
// timeout = n*10 ms). A LONGER supervision timeout is the main defence against
// spurious drops: the link only dies if nothing is heard for this whole window,
// so a body passing between the drum and the laptop, or a Wi-Fi burst, is ridden
// out instead of killing the session. 6 s is comfortably under the 32 s spec
// ceiling and is accepted by Windows/Chrome.
//   min 24 -> 30 ms, max 40 -> 50 ms: comfortably faster than the 40 ms frame push.
//   latency 0: do NOT let the peripheral skip connection events. Slave latency
//   saves power but makes a marginal link much likelier to miss the events that
//   would have kept it alive — this device is mains/USB powered, so spend it.
#define BLE_CONN_MIN_INTERVAL  24      // 30 ms
#define BLE_CONN_MAX_INTERVAL  40      // 50 ms
#define BLE_CONN_LATENCY       0       // no skipped connection events
#define BLE_CONN_TIMEOUT       600     // 6000 ms supervision timeout

// Advertising interval (units of 0.625 ms). Fast advertising = the central finds
// the device again quickly after a drop, which is what makes auto-reconnect feel
// instant. 32 -> 20 ms, 64 -> 40 ms is the BT-spec "fast connection" range.
#define BLE_ADV_MIN_INTERVAL   32      // 20 ms
#define BLE_ADV_MAX_INTERVAL   64      // 40 ms

// How long after a disconnect before we re-advertise, and how often we re-assert
// advertising if the stack ever drops out of it silently.
#define BLE_READVERTISE_MS     300     // settle time before re-advertising
#define BLE_ADV_REASSERT_MS    5000    // while unconnected, re-kick advertising this often

// Link-loss safety. If the dashboard stops talking to us for this long while the
// link is nominally up, treat the peer as gone and release the brake. Must be
// comfortably longer than the dashboard's keep-alive cadence.
#define BLE_PEER_TIMEOUT_MS    15000   // no command/keep-alive for 15 s -> release brake

// Task watchdog: panics and reboots if loop() wedges (e.g. a hung I2C bus), so a
// dead device comes back advertising instead of staying silent until power-cycled.
// NOTE: the Arduino core already starts the TWDT (CONFIG_ESP_TASK_WDT_INIT=1) at
// CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5 s and subscribes the CPU0 idle task to it.
// initLoopWatchdog() therefore just SUBSCRIBES loop() to that existing instance
// rather than reconfiguring it — reconfiguring would unsubscribe the idle task.
// This value is only the fallback used if the core ever ships with the TWDT off.
#define LOOP_WDT_TIMEOUT_S     10

// ---- AS5600 magnetic encoder ----------------------------------------------
// I2C uses the ESP32 default pins GPIO 21 (SDA) / GPIO 22 (SCL): Ferra calls
// Wire.begin() with no args, which selects exactly this pinout, so we match by
// doing the same — no explicit pin macros needed.
#define AS5600_DIR_PIN         23      // as5600.begin(23), like Ferra
#define COUNTS_PER_REV         4096

// ---- PWM brake (identical to Ferra) ----------------------------------------
#define MOTOR_OUT_PIN          12      // brake PWM output
#define MOTOR_OUT_PIN2         13      // held LOW
#define PWM_CHANNEL            0
#define PWM_FREQ               15000
#define PWM_RESOLUTION         8
#define MAX_DUTY_CYCLE         255
#define SAFE_MIN_DUTY          5
#define SAFE_MAX_DUTY          255

// ---- Sampling --------------------------------------------------------------
#define SAMPLE_PERIOD_MS       40      // 25 Hz data-line push (over BLE)
// Serial diagnostic line rate. MUST stay well under the UART throughput or
// Serial.printf() blocks when the TX FIFO fills, starving the BLE task and
// causing spurious disconnects. 20 ms = 50 Hz ≈ 1.75 KB/s, safe at 115200.
#define DIAG_PERIOD_MS         20

// Diagnostic CSV on Serial defaults OFF. When it was on by default the firmware
// permanently streamed ~50 lines/s into the UART; if nothing drains the port the
// TX FIFO fills, Serial.printf() BLOCKS, loop() stalls, the BLE task is starved
// and the link hits its supervision timeout. Enable it deliberately with
// "DIAG ON" when you actually have a monitor attached.
#define DIAG_DEFAULT_ON        false

// I2C bus health. A wedged AS5600 (loose connector, brown-out on the sensor rail)
// makes readAngle() return a frozen value or block. After this many consecutive
// failed reads the bus is re-initialised rather than letting the loop stall.
#define I2C_TIMEOUT_MS         50      // per-transaction ceiling, so a stuck bus can't hang loop()
// The health probe runs once a second, so this is ALSO the number of seconds a
// dead sensor is tolerated before the bus is re-initialised. Keep it small: 3 s
// is long enough to ride out a single glitched transaction but short enough that
// a loose connector recovers while the rider is still mid-session.
#define I2C_FAIL_LIMIT         3       // consecutive failed probes before a bus recovery

// RPM is derived from our own cumCounts slope over a sliding window.
// RPM_WINDOW_MS: how far back the slope reaches — longer = smoother but more lag.
// RPM_RING_SIZE: must hold at least RPM_WINDOW_MS / 1 ms loop tick snapshots.
// RPM_NOISE_FLOOR: clamp sub-threshold RPM to 0 (eliminates at-rest jitter).
#define RPM_WINDOW_MS          300    // 300 ms window — strong smoothing
#define RPM_RING_SIZE          350     // 350 slots @ ~1 ms/tick covers 350 ms
#define RPM_NOISE_FLOOR        3.0f    // RPM below this → 0 (raised noise floor)

// ---- Physics record-of-truth (mirrors the dashboard; firmware reference) ---
// SINGLE SOURCE OF TRUTH = max rope force, default 7.5 kgf (measured max
// downward pull at the rope when the brake is at full PWM).
// Edit MAX_FORCE_KGF here AND in the dashboard JS to recalibrate.
#define MAX_FORCE_KGF          7.5f
#define GRAVITY                9.80665f
#define MAX_FORCE_N            (MAX_FORCE_KGF * GRAVITY)  // ~73.6 N (reference only)
#define R_EFF_M                0.144f                     // drum_r 0.125 + rope_r 0.019
