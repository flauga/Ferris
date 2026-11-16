package com.aroleap.cardiodrum.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import com.aroleap.cardiodrum.ui.theme.AccentAmber
import com.aroleap.cardiodrum.ui.theme.AccentGreen
import com.aroleap.cardiodrum.ui.theme.Surface
import com.aroleap.cardiodrum.ui.theme.SurfaceHi
import com.aroleap.cardiodrum.ui.theme.TextLow
import com.aroleap.cardiodrum.ui.theme.TextMid
import kotlin.math.abs

@Composable
fun SessionSummaryDialog(
    summary: SessionSummary,
    onDismiss: () -> Unit,
    onViewHistory: () -> Unit,
) {
    Dialog(onDismissRequest = onDismiss) {
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(Surface)
                .padding(22.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text("SESSION COMPLETE", color = TextLow, fontSize = 11.sp,
                fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(10.dp))

            // Headline work output.
            val (eVal, eUnit) = Fmt.energy(summary.workJ)
            Row(verticalAlignment = Alignment.Bottom) {
                Text(eVal, color = AccentGreen, fontSize = 56.sp, fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace)
                Text(" $eUnit", color = TextMid, fontSize = 20.sp,
                    modifier = Modifier.padding(bottom = 9.dp))
            }
            Text("Total work output", color = TextLow, fontSize = 12.sp)

            Spacer(Modifier.height(12.dp))
            // Comparison line — the point of the objective number.
            ComparisonBadge(summary)

            Spacer(Modifier.height(18.dp))
            SummaryGrid(summary)

            Spacer(Modifier.height(18.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = onViewHistory) { Text("View history", color = TextMid) }
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = onDismiss) {
                    Text("Done", color = AccentGreen, fontWeight = FontWeight.SemiBold)
                }
            }
        }
    }
}

@Composable
private fun ComparisonBadge(s: SessionSummary) {
    val (text, color) = when {
        s.isFirst -> "First recorded session — your new baseline" to TextMid
        s.isNewBest -> "New best! +${Fmt.d1(s.vsBestPct)}% vs previous best" to AccentGreen
        s.vsBestPct >= 0 -> "+${Fmt.d1(s.vsBestPct)}% vs your best" to AccentGreen
        else -> "${Fmt.d1(s.vsBestPct)}% vs your best (${Fmt.work1(s.previousBestJ)})" to AccentAmber
    }
    Text(
        text, color = color, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier
            .clip(RoundedCornerShape(10.dp))
            .background(SurfaceHi)
            .padding(horizontal = 14.dp, vertical = 8.dp),
    )
}

@Composable
private fun SummaryGrid(s: SessionSummary) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MiniStat("Duration", Fmt.duration(s.durationSec), Modifier.weight(1f))
            MiniStat("Avg Power", "${Fmt.int(s.avgPowerW)} W", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MiniStat("Peak Power", "${Fmt.int(s.peakPowerW)} W", Modifier.weight(1f))
            MiniStat("Avg Speed", "${Fmt.int(s.avgVelocityMs / (2 * Math.PI / 60 * 0.144))} rpm", Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MiniStat("Distance", Fmt.distance(s.distanceM), Modifier.weight(1f))
            MiniStat("Avg Resistance", "${Fmt.int(s.avgResistancePct)}%", Modifier.weight(1f))
        }
    }
}

// Reuse a small tile consistent with the dashboard's MiniStat (local copy to keep
// this file self-contained).
@Composable
private fun MiniStat(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier.clip(RoundedCornerShape(12.dp)).background(SurfaceHi).padding(12.dp),
    ) {
        Text(label.uppercase(), color = TextLow, fontSize = 9.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(3.dp))
        Text(value, color = Color(0xFFE6EDF3), fontSize = 16.sp, fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace)
    }
}
