package com.aroleap.cardiodrum.data

import androidx.room.Entity
import androidx.room.PrimaryKey

/**
 * A completed training session. [workJ] — the cumulative mechanical work output
 * in joules — is the headline "objective number" used to compare one session
 * against another.
 */
@Entity(tableName = "sessions")
data class SessionEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val startedAt: Long,            // epoch millis when the session started
    val durationSec: Double,        // wall-clock seconds the session ran
    val workJ: Double,              // ∫ power dt — cumulative work output (joules)
    val avgPowerW: Double,
    val peakPowerW: Double,
    val avgVelocityMs: Double,
    val peakRpm: Double,
    val avgResistancePct: Double,
    val distanceM: Double,          // ∫ velocity dt — rope path length
    val sampleCount: Int,
    val note: String = "",
)
