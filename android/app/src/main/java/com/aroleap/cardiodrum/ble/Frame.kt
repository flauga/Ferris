package com.aroleap.cardiodrum.ble

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * One 16-byte little-endian data frame from the firmware:
 *   uint32 ms | float32 rpm | int32 pos (cumulative counts) | float32 resistance
 */
data class Frame(
    val ms: Long,          // device millis() timestamp (unsigned 32-bit)
    val rpm: Double,
    val positionCounts: Int,
    val resistancePct: Int,
) {
    companion object {
        /** Parse a notification payload, or null if it isn't a 16-byte data frame. */
        fun parse(bytes: ByteArray): Frame? {
            // Text lines ([STATUS]/[INFO]) start with '[' and are longer than 16 B;
            // data frames are exactly 16 B. Length is the reliable discriminator.
            if (bytes.size != BleConstants.FRAME_LEN) return null
            val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val ms = bb.int.toLong() and 0xFFFFFFFFL
            val rpm = bb.float.toDouble()
            val pos = bb.int
            val resistance = Math.round(bb.float).toInt()
            return Frame(ms, rpm, pos, resistance)
        }
    }
}
