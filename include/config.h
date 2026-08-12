#pragma once
// ============================================================================
//  Cardio Drum Trainer — firmware configuration
//  All tunable constants live here. Physics math itself is done in the
//  dashboard JS (see dashboard/cardio_dashboard.html); the physics constants
//  below are a record-of-truth so both sides agree.
// ============================================================================

// ---- Serial ----------------------------------------------------------------
#define SERIAL_BAUD            115200

// ---- BLE (Nordic UART Service — reuse Ferra's UUIDs verbatim) --------------
#define BLE_DEVICE_NAME        "CardioDrum"
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_RX_UUID           "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // write  (app -> esp)
#define CHAR_TX_UUID           "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // notify (esp -> app)
#define BLE_MTU                512

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

// RPM is derived from our own cumCounts slope over a sliding window.
// RPM_WINDOW_MS: how far back the slope reaches — longer = smoother but more lag.
// RPM_RING_SIZE: must hold at least RPM_WINDOW_MS / 1 ms loop tick snapshots.
// RPM_NOISE_FLOOR: clamp sub-threshold RPM to 0 (eliminates at-rest jitter).
#define RPM_WINDOW_MS          300     // 300 ms window — strong smoothing
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
