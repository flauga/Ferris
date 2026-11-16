package com.aroleap.cardiodrum.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.aroleap.cardiodrum.ui.theme.Divider
import com.aroleap.cardiodrum.ui.theme.Surface
import com.aroleap.cardiodrum.ui.theme.TextLow
import com.aroleap.cardiodrum.ui.theme.TextMid
import androidx.compose.foundation.Canvas
import kotlin.math.cos
import kotlin.math.sin

/**
 * A 270° arc gauge with a value track, a needle and a big numeric readout.
 * [value] is clamped to [0, max]; the sweep is smoothed with a short animation.
 */
@Composable
fun ArcGauge(
    value: Double,
    max: Double,
    label: String,
    unit: String,
    color: Color,
    modifier: Modifier = Modifier,
    valueText: String = Fmt.int(value),
) {
    val frac = (value / max).coerceIn(0.0, 1.0).toFloat()
    val animated by animateFloatAsState(
        targetValue = frac, animationSpec = tween(250), label = "gauge"
    )
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(18.dp))
            .background(Surface)
            .padding(14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Box(
                modifier = Modifier.fillMaxWidth().aspectRatio(1.15f),
                contentAlignment = Alignment.Center,
            ) {
                Canvas(modifier = Modifier.fillMaxWidth().aspectRatio(1.15f)) {
                    val startAngle = 135f
                    val sweepFull = 270f
                    // Fit the full circle inside the box: diameter = min dimension,
                    // centered. Prevents the 270° arc from clipping top/bottom.
                    val diameter = minOf(size.width, size.height)
                    val stroke = diameter * 0.11f
                    val arcSide = diameter - stroke
                    val topLeft = Offset(
                        (size.width - arcSide) / 2f,
                        (size.height - arcSide) / 2f,
                    )
                    val arcSize = Size(arcSide, arcSide)

                    // Track
                    drawArc(
                        color = Divider,
                        startAngle = startAngle,
                        sweepAngle = sweepFull,
                        useCenter = false,
                        topLeft = topLeft,
                        size = arcSize,
                        style = Stroke(width = stroke, cap = StrokeCap.Round),
                    )
                    // Value
                    drawArc(
                        brush = Brush.sweepGradient(
                            0f to color.copy(alpha = 0.6f), 1f to color
                        ),
                        startAngle = startAngle,
                        sweepAngle = sweepFull * animated,
                        useCenter = false,
                        topLeft = topLeft,
                        size = arcSize,
                        style = Stroke(width = stroke, cap = StrokeCap.Round),
                    )
                    // Needle
                    val cx = topLeft.x + arcSize.width / 2f
                    val cy = topLeft.y + arcSize.height / 2f
                    val needleAngle = Math.toRadians((startAngle + sweepFull * animated).toDouble())
                    val r = arcSize.width / 2f - stroke
                    drawLine(
                        color = color,
                        start = Offset(cx, cy),
                        end = Offset(
                            cx + (r * cos(needleAngle)).toFloat(),
                            cy + (r * sin(needleAngle)).toFloat(),
                        ),
                        strokeWidth = stroke * 0.5f,
                        cap = StrokeCap.Round,
                    )
                }
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        valueText,
                        color = color,
                        fontSize = 34.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                    )
                    Text(unit, color = TextMid, fontSize = 12.sp)
                }
            }
            Text(
                label.uppercase(),
                color = TextLow,
                fontSize = 11.sp,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}

/** Compact labelled value tile. */
@Composable
fun StatTile(
    label: String,
    value: String,
    unit: String,
    accent: Color,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(14.dp))
            .background(Surface)
            .padding(14.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(8.dp).clip(RoundedCornerShape(2.dp)).background(accent),
            )
            Text(
                "  ${label.uppercase()}",
                color = TextLow,
                fontSize = 10.sp,
                fontWeight = FontWeight.SemiBold,
            )
        }
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                value,
                color = Color(0xFFE6EDF3),
                fontSize = 24.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
            )
            if (unit.isNotEmpty()) {
                Text("  $unit", color = TextMid, fontSize = 12.sp,
                    modifier = Modifier.padding(bottom = 3.dp))
            }
        }
    }
}
