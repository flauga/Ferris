package com.aroleap.cardiodrum.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// Palette mirrored from the Web dashboard.
val Bg = Color(0xFF0B0F14)
val Surface = Color(0xFF111A24)
val SurfaceHi = Color(0xFF16222E)
val TextHi = Color(0xFFE6EDF3)
val TextMid = Color(0xFF9FB0C0)
val TextLow = Color(0xFF64748B)
val AccentBlue = Color(0xFF60A5FA)   // RPM / velocity
val AccentGreen = Color(0xFF4ADE80)  // power / energy
val AccentAmber = Color(0xFFF59E0B)  // resistance
val AccentRed = Color(0xFFF87171)
val Divider = Color(0xFF223040)

private val DarkColors = darkColorScheme(
    primary = AccentGreen,
    onPrimary = Color(0xFF05140A),
    secondary = AccentBlue,
    background = Bg,
    onBackground = TextHi,
    surface = Surface,
    onSurface = TextHi,
    surfaceVariant = SurfaceHi,
    onSurfaceVariant = TextMid,
    error = AccentRed,
    outline = Divider,
)

@Composable
fun CardioTheme(content: @Composable () -> Unit) {
    // Always dark — this is a gym/training HUD; a light theme would wash out the dials.
    MaterialTheme(
        colorScheme = DarkColors,
        typography = Typography(),
        content = content,
    )
}
