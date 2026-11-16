package com.aroleap.cardiodrum.ui

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

/** Formatting helpers shared across the screens. */
object Fmt {
    /** Energy as J, switching to kJ above 1000 J (matches the dashboard). */
    fun energy(j: Double): Pair<String, String> =
        if (j >= 1000.0) String.format(Locale.US, "%.2f", j / 1000.0) to "kJ"
        else j.roundToInt().toString() to "J"

    fun work1(j: Double): String =
        if (j >= 1000.0) String.format(Locale.US, "%.2f kJ", j / 1000.0)
        else "${j.roundToInt()} J"

    fun int(v: Double): String = v.roundToInt().toString()
    fun d1(v: Double): String = String.format(Locale.US, "%.1f", v)
    fun d2(v: Double): String = String.format(Locale.US, "%.2f", v)

    fun duration(sec: Double): String {
        val total = sec.roundToInt()
        val m = total / 60
        val s = total % 60
        return String.format(Locale.US, "%d:%02d", m, s)
    }

    fun distance(m: Double): String =
        if (m >= 1000.0) String.format(Locale.US, "%.2f km", m / 1000.0)
        else String.format(Locale.US, "%d m", m.roundToInt())

    private val dateFmt = SimpleDateFormat("MMM d · HH:mm", Locale.getDefault())
    fun dateTime(epochMs: Long): String = dateFmt.format(Date(epochMs))
}
