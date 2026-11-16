package com.aroleap.cardiodrum.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.History
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.runtime.DisposableEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.aroleap.cardiodrum.ble.ConnectionState
import com.aroleap.cardiodrum.data.Physics
import com.aroleap.cardiodrum.ui.theme.AccentAmber
import com.aroleap.cardiodrum.ui.theme.AccentBlue
import com.aroleap.cardiodrum.ui.theme.AccentGreen
import com.aroleap.cardiodrum.ui.theme.AccentRed
import com.aroleap.cardiodrum.ui.theme.Bg
import com.aroleap.cardiodrum.ui.theme.Surface
import com.aroleap.cardiodrum.ui.theme.TextLow
import com.aroleap.cardiodrum.ui.theme.TextMid

@Composable
fun DashboardScreen(
    vm: CardioViewModel,
    onOpenHistory: () -> Unit,
    onRequestConnect: () -> Unit,
) {
    val live by vm.live.collectAsStateWithLifecycle()
    val conn by vm.connectionState.collectAsStateWithLifecycle()
    val commanded by vm.commandedResistance.collectAsStateWithLifecycle()
    val message by vm.lastMessage.collectAsStateWithLifecycle()
    val summary by vm.lastSummary.collectAsStateWithLifecycle()

    val connected = conn.isConnected
    val busy = conn is ConnectionState.Connecting || conn is ConnectionState.Scanning ||
        conn is ConnectionState.Reconnecting

    // Keep the screen awake during a session so the radio/display don't sleep.
    val view = LocalView.current
    DisposableEffect(live.sessionActive || connected) {
        view.keepScreenOn = live.sessionActive || connected
        onDispose { view.keepScreenOn = false }
    }

    Box(Modifier.fillMaxSize().background(Bg)) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            // ---- Header ----
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(
                        "Cardio Drum",
                        color = Color(0xFFE6EDF3),
                        fontSize = 22.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    ConnectionChip(conn)
                }
                IconButton(onClick = onOpenHistory) {
                    Icon(Icons.Filled.History, contentDescription = "Session history", tint = TextMid)
                }
            }

            Spacer(Modifier.height(12.dp))

            // ---- Connect / Disconnect ----
            Button(
                onClick = { if (connected || busy) vm.disconnect() else onRequestConnect() },
                modifier = Modifier.fillMaxWidth().height(50.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (connected || busy) Surface else AccentGreen,
                    contentColor = if (connected || busy) TextMid else Color(0xFF05140A),
                ),
                shape = RoundedCornerShape(14.dp),
            ) {
                Text(
                    if (connected) "Disconnect" else if (busy) "Cancel" else "Connect to CardioDrum",
                    fontWeight = FontWeight.SemiBold,
                )
            }

            Spacer(Modifier.height(16.dp))

            // ---- Gauges ----
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                ArcGauge(
                    value = kotlin.math.abs(live.rpm),
                    max = Physics.RPM_MAX,
                    label = "Speed",
                    unit = "rpm",
                    color = AccentBlue,
                    modifier = Modifier.weight(1f),
                    valueText = Fmt.int(kotlin.math.abs(live.rpm)),
                )
                ArcGauge(
                    value = live.power,
                    max = Physics.POWER_MAX,
                    label = "Power",
                    unit = "W",
                    color = AccentGreen,
                    modifier = Modifier.weight(1f),
                    valueText = Fmt.int(live.power),
                )
            }

            Spacer(Modifier.height(12.dp))

            // ---- Live stats ----
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                StatTile(
                    "Velocity", Fmt.d2(live.velocity), "m/s", AccentBlue,
                    modifier = Modifier.weight(1f),
                )
                StatTile(
                    "Set Force", Fmt.int(Physics.setForce(live.resistance)), "N", AccentAmber,
                    modifier = Modifier.weight(1f),
                )
            }

            Spacer(Modifier.height(16.dp))

            // ---- Resistance ----
            ResistanceCard(
                commanded = commanded,
                enforced = live.resistance,
                enabled = connected,
                onChange = vm::setResistance,
                onRelease = { vm.setResistance(0) },
            )

            Spacer(Modifier.height(16.dp))

            // ---- Session ----
            SessionCard(
                live = live,
                connected = connected,
                onStart = vm::startSession,
                onStop = vm::stopSession,
            )

            if (message != null) {
                Spacer(Modifier.height(12.dp))
                Text(
                    message!!,
                    color = TextLow,
                    fontSize = 11.sp,
                    fontFamily = FontFamily.Monospace,
                )
            }

            Spacer(Modifier.height(24.dp))
        }

        // ---- End-of-session summary ----
        summary?.let {
            SessionSummaryDialog(summary = it, onDismiss = vm::dismissSummary, onViewHistory = {
                vm.dismissSummary(); onOpenHistory()
            })
        }
    }
}

@Composable
private fun ConnectionChip(conn: ConnectionState) {
    val color = when {
        conn.isConnected -> AccentGreen
        conn is ConnectionState.Error -> AccentRed
        conn is ConnectionState.Idle -> TextLow
        else -> AccentAmber
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).clip(RoundedCornerShape(4.dp)).background(color))
        Spacer(Modifier.width(6.dp))
        Text(conn.label, color = TextMid, fontSize = 12.sp)
    }
}

@Composable
private fun ResistanceCard(
    commanded: Int,
    enforced: Int,
    enabled: Boolean,
    onChange: (Int) -> Unit,
    onRelease: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Surface).padding(16.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("RESISTANCE", color = TextLow, fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f))
            Text(
                "$commanded%",
                color = AccentAmber,
                fontSize = 26.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
            )
        }
        Slider(
            value = commanded.toFloat(),
            onValueChange = { onChange(it.toInt()) },
            valueRange = 0f..100f,
            enabled = enabled,
            colors = SliderDefaults.colors(
                thumbColor = AccentAmber,
                activeTrackColor = AccentAmber,
                inactiveTrackColor = Color(0xFF223040),
            ),
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                if (enabled) "Brake enforced at $enforced%" else "Connect to control the brake",
                color = TextLow, fontSize = 11.sp, modifier = Modifier.weight(1f),
            )
            TextButton(onClick = onRelease, enabled = enabled) {
                Text("Release (0%)", color = if (enabled) AccentRed else TextLow, fontSize = 12.sp)
            }
        }
    }
}

@Composable
private fun SessionCard(
    live: LiveState,
    connected: Boolean,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Surface).padding(16.dp),
    ) {
        Text(
            if (live.sessionActive) "SESSION — RECORDING" else "SESSION",
            color = if (live.sessionActive) AccentGreen else TextLow,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
        )
        Spacer(Modifier.height(8.dp))

        // Headline: cumulative work output (the objective comparison number).
        val (eVal, eUnit) = Fmt.energy(live.energyJ)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                eVal,
                color = Color(0xFFE6EDF3),
                fontSize = 46.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
            )
            Text("  $eUnit", color = TextMid, fontSize = 18.sp,
                modifier = Modifier.padding(bottom = 7.dp))
        }
        Text("Cumulative work output", color = TextLow, fontSize = 11.sp)

        Spacer(Modifier.height(14.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MiniStat("Time", Fmt.duration(live.durationSec), Modifier.weight(1f))
            MiniStat("Avg Power", "${Fmt.int(live.avgPowerW)} W", Modifier.weight(1f))
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            MiniStat("Distance", Fmt.distance(live.distanceM), Modifier.weight(1f))
            MiniStat("Peak Power", "${Fmt.int(live.peakPowerW)} W", Modifier.weight(1f))
        }

        Spacer(Modifier.height(16.dp))
        if (live.sessionActive) {
            Button(
                onClick = onStop,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                colors = ButtonDefaults.buttonColors(containerColor = AccentRed, contentColor = Color.White),
                shape = RoundedCornerShape(14.dp),
            ) {
                Icon(Icons.Filled.Stop, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Stop Session", fontWeight = FontWeight.SemiBold)
            }
        } else {
            Button(
                onClick = onStart,
                enabled = connected,
                modifier = Modifier.fillMaxWidth().height(50.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = AccentGreen, contentColor = Color(0xFF05140A),
                    disabledContainerColor = Surface, disabledContentColor = TextLow,
                ),
                shape = RoundedCornerShape(14.dp),
            ) {
                Icon(Icons.Filled.PlayArrow, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text(if (connected) "Start Session" else "Connect to start", fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

@Composable
private fun MiniStat(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier.clip(RoundedCornerShape(12.dp)).background(Bg).padding(12.dp),
    ) {
        Text(label.uppercase(), color = TextLow, fontSize = 9.sp, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(3.dp))
        Text(value, color = Color(0xFFE6EDF3), fontSize = 18.sp, fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace)
    }
}
