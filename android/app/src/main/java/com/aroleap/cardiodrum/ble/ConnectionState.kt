package com.aroleap.cardiodrum.ble

/** High-level link state surfaced to the UI. */
sealed interface ConnectionState {
    data object Idle : ConnectionState
    data object Scanning : ConnectionState
    data object Connecting : ConnectionState
    data object Connected : ConnectionState
    data class Reconnecting(val attempt: Int) : ConnectionState
    data class Error(val message: String) : ConnectionState

    val isConnected: Boolean get() = this is Connected

    val label: String
        get() = when (this) {
            Idle -> "Not connected"
            Scanning -> "Scanning…"
            Connecting -> "Connecting…"
            Connected -> "Connected"
            is Reconnecting -> "Reconnecting… (try $attempt)"
            is Error -> message
        }
}
