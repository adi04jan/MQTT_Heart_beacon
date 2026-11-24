package com.heartbeacon.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.heartbeacon.app.R
import com.heartbeacon.app.data.Mood

/*
 * The app is made of what the beacon is made of. Two metals and the cloth it sits on:
 * brass is the one accent (controls, selection, the primary action), tin is the light ground (the solder),
 * felt is the dark ground (the grey cloth in the photos). The only saturated colours anywhere are the
 * firmware's LED colours, and they always mean a mood or the battery, never decoration.
 * No dynamic colour: the materials are the identity.
 */
private val Light = lightColorScheme(
    primary = Color(0xFF765710),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFF1DFB0),
    onPrimaryContainer = Color(0xFF261A00),
    inversePrimary = Color(0xFFE2BC6A),
    secondary = Color(0xFF5F5B52),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFE6E4DE),
    onSecondaryContainer = Color(0xFF1D1B16),
    tertiary = Color(0xFF5F5B52),
    onTertiary = Color(0xFFFFFFFF),
    tertiaryContainer = Color(0xFFE6E4DE),
    onTertiaryContainer = Color(0xFF1D1B16),
    background = Color(0xFFF0F0EE),
    onBackground = Color(0xFF1A1917),
    surface = Color(0xFFF0F0EE),
    onSurface = Color(0xFF1A1917),
    surfaceVariant = Color(0xFFE3E2DE),
    onSurfaceVariant = Color(0xFF55524B),
    surfaceTint = Color(0xFF765710),
    inverseSurface = Color(0xFF2F2E2B),
    inverseOnSurface = Color(0xFFF2F0EB),
    outline = Color(0xFF7E7A71),
    outlineVariant = Color(0xFFD3D1CB),
    surfaceBright = Color(0xFFFAFAF8),
    surfaceDim = Color(0xFFD9D8D4),
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFF7F7F5),
    surfaceContainer = Color(0xFFFAFAF8),
    surfaceContainerHigh = Color(0xFFE8E7E3),
    surfaceContainerHighest = Color(0xFFE1E0DC),
    error = Color(0xFFB3261E),
    onError = Color(0xFFFFFFFF),
    errorContainer = Color(0xFFF9DEDC),
    onErrorContainer = Color(0xFF410E0B),
)

private val Dark = darkColorScheme(
    primary = Color(0xFFE2BC6A),
    onPrimary = Color(0xFF3A2A00),
    primaryContainer = Color(0xFF55410F),
    onPrimaryContainer = Color(0xFFFBE3AC),
    inversePrimary = Color(0xFF765710),
    secondary = Color(0xFFCBC6BA),
    onSecondary = Color(0xFF322F28),
    secondaryContainer = Color(0xFF36332D),
    onSecondaryContainer = Color(0xFFE8E3D8),
    tertiary = Color(0xFFCBC6BA),
    onTertiary = Color(0xFF322F28),
    tertiaryContainer = Color(0xFF36332D),
    onTertiaryContainer = Color(0xFFE8E3D8),
    background = Color(0xFF131211),
    onBackground = Color(0xFFEAE5DC),
    surface = Color(0xFF131211),
    onSurface = Color(0xFFEAE5DC),
    surfaceVariant = Color(0xFF45423C),
    onSurfaceVariant = Color(0xFFC6C0B4),
    surfaceTint = Color(0xFFE2BC6A),
    inverseSurface = Color(0xFFEAE5DC),
    inverseOnSurface = Color(0xFF2F2E2B),
    outline = Color(0xFF958F84),
    outlineVariant = Color(0xFF3B3934),
    surfaceBright = Color(0xFF393733),
    surfaceDim = Color(0xFF131211),
    surfaceContainerLowest = Color(0xFF0D0C0B),
    surfaceContainerLow = Color(0xFF1A1917),
    surfaceContainer = Color(0xFF1E1D1A),
    surfaceContainerHigh = Color(0xFF292724),
    surfaceContainerHighest = Color(0xFF34322E),
    error = Color(0xFFF2B8B5),
    onError = Color(0xFF601410),
    errorContainer = Color(0xFF8C1D18),
    onErrorContainer = Color(0xFFF9DEDC),
)

/**
 * Jost (SIL OFL, bundled): the geometric lettering of instrument dials, for names, headings, big figures and
 * buttons. Body copy, labels and fields stay on the system face.
 */
val Jost = FontFamily(
    Font(R.font.jost, FontWeight.Light),
    Font(R.font.jost, FontWeight.Normal),
    Font(R.font.jost, FontWeight.Medium),
)

private val Type = Typography().run {
    copy(
        displaySmall = displaySmall.copy(fontFamily = Jost, fontWeight = FontWeight.Light, fontSize = 40.sp, lineHeight = 46.sp, letterSpacing = (-0.4).sp),
        headlineLarge = headlineLarge.copy(fontFamily = Jost, fontWeight = FontWeight.Normal, letterSpacing = (-0.3).sp),
        headlineMedium = headlineMedium.copy(fontFamily = Jost, fontWeight = FontWeight.Normal, fontSize = 30.sp, lineHeight = 36.sp, letterSpacing = (-0.2).sp),
        headlineSmall = headlineSmall.copy(fontFamily = Jost, fontWeight = FontWeight.Normal, fontSize = 26.sp, lineHeight = 32.sp),
        titleLarge = titleLarge.copy(fontFamily = Jost, fontWeight = FontWeight.Medium, fontSize = 23.sp),
        titleMedium = titleMedium.copy(fontFamily = Jost, fontWeight = FontWeight.Medium, fontSize = 19.sp, lineHeight = 26.sp, letterSpacing = 0.sp),
        titleSmall = titleSmall.copy(fontWeight = FontWeight.Medium),
        labelLarge = labelLarge.copy(fontFamily = Jost, fontWeight = FontWeight.Medium, fontSize = 16.sp, letterSpacing = 0.2.sp),
    )
}

private val Shape = Shapes(
    extraSmall = RoundedCornerShape(6.dp),
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(22.dp),
    extraLarge = RoundedCornerShape(28.dp),
)

/**
 * Brass as a fill, for controls that are a filled shape (the main button, a switch that is on, a slider, a chosen
 * day): real brass with dark lettering on tin, the scheme's own bright brass on felt. The scheme's darker primary
 * stays for brass text and icons, which need the contrast.
 */
val ColorScheme.brass: Color get() = if (surface.luminance() > 0.5f) Color(0xFFA57C1F) else primary
val ColorScheme.onBrass: Color get() = if (surface.luminance() > 0.5f) Color(0xFF231700) else onPrimary

@Composable
fun brassButton() = ButtonDefaults.buttonColors(
    containerColor = MaterialTheme.colorScheme.brass,
    contentColor = MaterialTheme.colorScheme.onBrass,
)

/** Spacing steps. Tight inside a group, generous between groups. */
object Space {
    val xs = 4.dp
    val s = 8.dp
    val m = 12.dp
    val l = 16.dp
    val xl = 24.dp
    val xxl = 32.dp
    val screen = 16.dp
}

/** Tabular figures for battery, times and versions so values do not jitter. */
val Tabular = TextStyle(fontFeatureSettings = "tnum")

/**
 * A mood's colour as the LEDs show it (hb_fx.c), for marks on the dark stage; [onLight] gives the same hue deep
 * enough to read on the tin ground.
 */
fun moodColor(m: Mood, onLight: Boolean = false): Color = when (m) {
    Mood.HEART -> if (onLight) Color(0xFFC81E2B) else Color(250, 52, 60)
    Mood.SORRY -> if (onLight) Color(0xFFB85C00) else Color(235, 161, 23)
    Mood.ANNOY -> if (onLight) Color(0xFF00757A) else Color(23, 231, 235)
}

@Composable
fun HeartTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) Dark else Light, typography = Type, shapes = Shape, content = content)
}
