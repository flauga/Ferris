package com.aroleap.cardiodrum.ble

import java.util.UUID

/**
 * Nordic UART Service identifiers and protocol constants, mirrored verbatim from
 * the firmware ([include/config.h]) and the Web Bluetooth dashboard. These MUST
 * stay in lockstep with the firmware — changing a UUID or the frame layout here
 * without changing it on the ESP32 silently breaks the link.
 */
object BleConstants {
    const val DEVICE_NAME = "CardioDrum"

    val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
    // app -> esp (write, write-no-response)
    val RX_CHAR_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
    // esp -> app (notify): 16-byte binary data frames + [STATUS]/[INFO] text
    val TX_CHAR_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")

    // Client Characteristic Configuration Descriptor — needed to turn on notifications.
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /** Binary data frame length: u32 ms | f32 rpm | i32 pos | f32 resistance. */
    const val FRAME_LEN = 16

    // ---- app -> esp commands (newline-terminated by the writer) -------------
    fun cmdResistance(pct: Int): String = "R:${pct.coerceIn(0, 100)}"
    const val CMD_RESET = "RESET"
    const val CMD_STATUS = "S"
    const val CMD_PING = "PING"
    const val CMD_STOP = "STOP"

    // Keep-alive cadence (matches the dashboard): silent PING every 3 s, and a
    // full status request every ~30 s. Keeps the OS BLE stack seeing bidirectional
    // traffic and stops the firmware's 30 s peer dead-man from releasing the brake.
    const val KEEPALIVE_PERIOD_MS = 3_000L
    const val STATUS_EVERY_N_PINGS = 10

    // If no data frame arrives for this long while nominally connected, treat the
    // link as dead (Android/Chrome-style late/never onConnectionStateChange). The
    // firmware streams at 25 Hz, so 2.5 s of silence is unambiguous.
    const val FRAME_WATCHDOG_MS = 2_500L

    // Auto-reconnect backoff bounds for an unexpected drop.
    const val RECONNECT_MIN_BACKOFF_MS = 600L
    const val RECONNECT_MAX_BACKOFF_MS = 8_000L
    // Give up after this much continuous downtime (stepping out of range, etc.).
    const val RECONNECT_GIVE_UP_MS = 5 * 60 * 1000L

    // Scan timeout before reporting "device not found".
    const val SCAN_TIMEOUT_MS = 15_000L
}
