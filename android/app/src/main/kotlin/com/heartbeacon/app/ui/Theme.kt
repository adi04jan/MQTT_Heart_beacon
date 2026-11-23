package com.heartbeacon.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * One accent (rose wine) on near-neutral grounds that carry a trace of the same hue, so the accent
 * belongs to the surface instead of sitting on top of it. Secondary and tertiary are deliberately the
 * neutrals: tonal buttons and chips stay quiet and only the primary action is coloured.
 * No dynamic colour: the single accent is the identity.
 */
private val Light = lightColorScheme(
    primary = Color(0xFF9C2A4D),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFFFD9E1),
    onPrimaryContainer = Color(0xFF3F0018),
    inversePrimary = Color(0xFFFFB1C4),
    secondary = Color(0xFF6A5A60),
    onSecondary = Color(0xFFFFFFFF),
    secondaryContainer = Color(0xFFEDE4E8),
    onSecondaryContainer = Color(0xFF241A1E),
    tertiary = Color(0xFF6A5A60),
    onTertiary = Color(0xFFFFFFFF),
    tertiaryContainer = Color(0xFFEDE4E8),
    onTertiaryContainer = Color(0xFF241A1E),
    background = Color(0xFFFBF8F9),
    onBackground = Color(0xFF1D1A1C),
    surface = Color(0xFFFBF8F9),
    onSurface = Color(0xFF1D1A1C),
    surfaceVariant = Color(0xFFEFE8EB),
    onSurfaceVariant = Color(0xFF574F53),
    surfaceTint = Color(0xFF9C2A4D),
    inverseSurface = Color(0xFF322F31),
    inverseOnSurface = Color(0xFFF5EFF2),
    outline = Color(0xFF8A8186),
    outlineVariant = Color(0xFFDAD2D6),
    surfaceBright = Color(0xFFFBF8F9),
    surfaceDim = Color(0xFFDCD8DA),
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFF5F1F3),
    surfaceContainer = Color(0xFFF0EBEE),
    surfaceContainerHigh = Color(0xFFEAE5E8),
    surfaceContainerHighest = Color(0xFFE4DFE2),
    error = Color(0xFFB3261E),
    onError = Color(0xFFFFFFFF),
    errorContainer = Color(0xFFF9DEDC),
    onErrorContainer = Color(0xFF410E0B),
)

private val Dark = darkColorScheme(
    primary = Color(0xFFFFB1C4),
    onPrimary = Color(0xFF5E1131),
    primaryContainer = Color(0xFF7C2943),
    onPrimaryContainer = Color(0xFFFFD9E1),
    inversePrimary = Color(0xFF9C2A4D),
    secondary = Color(0xFFD6C8CD),
    onSecondary = Color(0xFF3A2E33),
    secondaryContainer = Color(0xFF3A3236),
    onSecondaryContainer = Color(0xFFEDE4E8),
    tertiary = Color(0xFFD6C8CD),
    onTertiary = Color(0xFF3A2E33),
    tertiaryContainer = Color(0xFF3A3236),
    onTertiaryContainer = Color(0xFFEDE4E8),
    background = Color(0xFF141214),
    onBackground = Color(0xFFE8E1E4),
    surface = Color(0xFF141214),
    onSurface = Color(0xFFE8E1E4),
    surfaceVariant = Color(0xFF4A4347),
    onSurfaceVariant = Color(0xFFCFC5CA),
    surfaceTint = Color(0xFFFFB1C4),
    inverseSurface = Color(0xFFE8E1E4),
    inverseOnSurface = Color(0xFF322F31),
    outline = Color(0xFF988F94),
    outlineVariant = Color(0xFF4A4347),
    surfaceBright = Color(0xFF3B373A),
    surfaceDim = Color(0xFF141214),
    surfaceContainerLowest = Color(0xFF0F0D0F),
    surfaceContainerLow = Color(0xFF1D1A1C),
    surfaceContainer = Color(0xFF221E21),
    surfaceContainerHigh = Color(0xFF2C292B),
    surfaceContainerHighest = Color(0xFF373336),
    error = Color(0xFFF2B8B5),
    onError = Color(0xFF601410),
    errorContainer = Color(0xFF8C1D18),
    onErrorContainer = Color(0xFFF9DEDC),
)

// Material scale, system face. Titles a step heavier than stock so section structure reads at a glance.
private val Type = Typography().run {
    copy(
        headlineMedium = headlineMedium.copy(fontWeight = FontWeight.Medium, letterSpacing = (-0.2).sp),
        headlineSmall = headlineSmall.copy(fontWeight = FontWeight.Medium),
        titleLarge = titleLarge.copy(fontWeight = FontWeight.Medium),
        titleMedium = titleMedium.copy(fontWeight = FontWeight.SemiBold),
        labelLarge = labelLarge.copy(fontWeight = FontWeight.SemiBold),
    )
}

/** Spacing steps. Tight inside a group, generous between groups. */
object Space {
    val xs = 4.dp
    val s = 8.dp
    val m = 12.dp
    val l = 16.dp
    val xl = 24.dp
    val xxl = 32.dp
    val screen = 20.dp
}

/** Tabular figures for battery, times and versions so values do not jitter. */
val Tabular = TextStyle(fontFeatureSettings = "tnum")

@Composable
fun HeartTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) Dark else Light, typography = Type, content = content)
}
