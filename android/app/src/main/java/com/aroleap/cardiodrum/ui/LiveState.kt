package com.aroleap.cardiodrum.ui

/** Everything the dashboard renders in real time. */
data class LiveState(
    // Live telemetry (shown whether or not a session is running).
    val rpm: Double = 0.0,
    val power: Double = 0.0,
    val velocity: Double = 0.0,
    val resistance: Int = 0,          // actual enforced value echoed by the firmware
    val positionCounts: Int = 0,
    // Session-scoped accumulators.
    val sessionActive: Boolean = false,
    val energyJ: Double = 0.0,        // cumulative work output — the headline number
    val durationSec: Double = 0.0,    // active streaming time
    val avgPowerW: Double = 0.0,
    val distanceM: Double = 0.0,
    val peakPowerW: Double = 0.0,
    val peakRpm: Double = 0.0,
)

/** Shown once when a session ends; also persisted to history. */
data class SessionSummary(
    val startedAt: Long,
    val durationSec: Double,
    val workJ: Double,
    val avgPowerW: Double,
    val peakPowerW: Double,
    val avgVelocityMs: Double,
    val peakRpm: Double,
    val avgResistancePct: Double,
    val distanceM: Double,
    val sampleCount: Int,
    val previousBestJ: Double,        // best work before this session (0 if first)
) {
    val isNewBest: Boolean get() = workJ > previousBestJ && previousBestJ > 0.0
    val isFirst: Boolean get() = previousBestJ <= 0.0
    /** Signed % difference vs. the previous best (for the comparison line). */
    val vsBestPct: Double
        get() = if (previousBestJ > 0.0) (workJ - previousBestJ) / previousBestJ * 100.0 else 0.0
}
