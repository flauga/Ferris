package com.aroleap.cardiodrum.data

import kotlin.math.PI
import kotlin.math.abs

/**
 * Force / velocity / power / energy math — the single calibration block, mirrored
 * from the firmware's physics record-of-truth (include/config.h) and the
 * dashboard JS. The firmware emits only raw signals (rpm, cumulative counts,
 * commanded resistance); everything derived lives here so calibration is in one
 * editable place.
 *
 * To recalibrate, edit [MAX_FORCE_KGF] / [R_EFF_M] here AND in the firmware.
 */
object Physics {
    const val MAX_FORCE_KGF = 7.5          // measured max rope force at full PWM
    const val GRAVITY = 9.80665
    const val MAX_FORCE_N = MAX_FORCE_KGF * GRAVITY   // ≈ 73.6 N
    const val R_EFF_M = 0.144              // drum radius 0.125 + rope radius 0.019
    const val COUNTS_PER_REV = 4096.0

    // Display ceilings for the dials (editable).
    const val RPM_MAX = 300.0
    const val POWER_MAX = 350.0            // ≈ 73.6 N × ~4.5 m/s (300 rpm) ≈ 333 W

    /** Rope linear speed in m/s. Always ≥ 0 (direction is irrelevant to work). */
    fun velocity(rpm: Double): Double = abs(rpm) * (2.0 * PI / 60.0) * R_EFF_M

    /** Commanded brake force in N for a resistance setting of 0–100 %. */
    fun setForce(resistancePct: Int): Double = (resistancePct / 100.0) * MAX_FORCE_N

    /** Instantaneous mechanical power in W: commanded force × rope speed. */
    fun power(resistancePct: Int, rpm: Double): Double = setForce(resistancePct) * velocity(rpm)

    /** Rope path length in metres for a given change in encoder counts. */
    fun distanceMeters(deltaCounts: Int): Double =
        abs(deltaCounts) / COUNTS_PER_REV * 2.0 * PI * R_EFF_M
}
