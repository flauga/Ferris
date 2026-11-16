package com.aroleap.cardiodrum.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aroleap.cardiodrum.data.SessionEntity
import com.aroleap.cardiodrum.ui.theme.AccentAmber
import com.aroleap.cardiodrum.ui.theme.AccentGreen
import com.aroleap.cardiodrum.ui.theme.Bg
import com.aroleap.cardiodrum.ui.theme.Surface
import com.aroleap.cardiodrum.ui.theme.TextLow
import com.aroleap.cardiodrum.ui.theme.TextMid

@Composable
fun HistoryScreen(
    vm: CardioViewModel,
    onBack: () -> Unit,
) {
    val sessions by vm.sessions.collectAsStateWithLifecycle()
    val best = sessions.maxOfOrNull { it.workJ } ?: 0.0

    Column(Modifier.fillMaxSize().background(Bg)) {
        Row(
            Modifier.fillMaxWidth().padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back", tint = TextMid)
            }
            Text("Session History", color = Color(0xFFE6EDF3), fontSize = 20.sp,
                fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
        }

        if (sessions.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    "No sessions yet.\nStart and stop a session to record your first\nwork-output number.",
                    color = TextLow, fontSize = 14.sp,
                )
            }
            return@Column
        }

        // Quick comparison header: best-ever work output.
        Text(
            "Compare total work output (J) across sessions — higher is more work done.",
            color = TextLow, fontSize = 12.sp,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        )

        LazyColumn(
            Modifier.fillMaxSize().padding(horizontal = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(vertical = 8.dp),
        ) {
            items(sessions, key = { it.id }) { s ->
                SessionRow(
                    s = s,
                    best = best,
                    isBest = s.workJ >= best && best > 0.0,
                    onDelete = { vm.deleteSession(s) },
                )
            }
        }
    }
}

@Composable
private fun SessionRow(
    s: SessionEntity,
    best: Double,
    isBest: Boolean,
    onDelete: () -> Unit,
) {
    val frac = if (best > 0.0) (s.workJ / best).coerceIn(0.0, 1.0).toFloat() else 0f
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Surface).padding(14.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(Fmt.dateTime(s.startedAt), color = TextMid, fontSize = 12.sp)
                    if (isBest) {
                        Spacer(Modifier.size(6.dp))
                        Icon(Icons.Filled.EmojiEvents, contentDescription = "Best",
                            tint = AccentAmber, modifier = Modifier.size(14.dp))
                    }
                }
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        Fmt.work1(s.workJ),
                        color = Color(0xFFE6EDF3),
                        fontSize = 28.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
            IconButton(onClick = onDelete) {
                Icon(Icons.Filled.DeleteOutline, contentDescription = "Delete", tint = TextLow)
            }
        }

        // Relative-work bar.
        Box(
            Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp))
                .background(Color(0xFF223040)),
        ) {
            Box(
                Modifier.fillMaxHeight().fillMaxWidth(frac).clip(RoundedCornerShape(3.dp))
                    .background(if (isBest) AccentAmber else AccentGreen),
            )
        }

        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Meta("Time", Fmt.duration(s.durationSec))
            Meta("Avg", "${Fmt.int(s.avgPowerW)} W")
            Meta("Peak", "${Fmt.int(s.peakPowerW)} W")
            Meta("Dist", Fmt.distance(s.distanceM))
        }
    }
}

@Composable
private fun Meta(label: String, value: String) {
    Column {
        Text(label.uppercase(), color = TextLow, fontSize = 9.sp, fontWeight = FontWeight.SemiBold)
        Text(value, color = TextMid, fontSize = 13.sp, fontFamily = FontFamily.Monospace)
    }
}
