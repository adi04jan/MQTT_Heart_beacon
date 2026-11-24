@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.heartbeacon.app.R
import com.heartbeacon.app.data.Mood
import java.time.ZoneId
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun rememberFormat(): Format {
    val locale = LocalContext.current.resources.configuration.locales[0]
    return androidx.compose.runtime.remember(locale) { Format(ZoneId.systemDefault(), locale) }
}

@Composable
fun BackBar(title: String, onBack: () -> Unit) {
    TopAppBar(
        title = { Text(title) },
        navigationIcon = {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
        },
        colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface),
    )
}

@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = modifier.padding(start = Space.screen, end = Space.screen, top = Space.xl, bottom = Space.xs),
    )
}

private val rowColors @Composable get() = ListItemDefaults.colors(containerColor = Color.Transparent)

@Composable
fun ItemRow(
    title: String,
    supporting: String? = null,
    onClick: (() -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = supporting?.let { { Text(it) } },
        trailingContent = trailing,
        colors = rowColors,
        modifier = (if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier).padding(horizontal = Space.xs),
    )
}

@Composable
fun SwitchRow(title: String, supporting: String?, checked: Boolean, onChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = supporting?.let { { Text(it) } },
        trailingContent = { Switch(checked = checked, onCheckedChange = null) },
        colors = rowColors,
        modifier = Modifier
            .toggleable(value = checked, role = Role.Switch, onValueChange = onChange)
            .padding(horizontal = Space.xs),
    )
}

/** Label / value pair for read-only facts (phone details, firmware). */
@Composable
fun FactRow(title: String, value: String, onClick: (() -> Unit)? = null, attention: Boolean = false) {
    ItemRow(title = title, onClick = onClick, trailing = {
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium.merge(Tabular),
            color = if (attention) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
        )
    })
}

@Composable
fun MoodIcon(mood: Mood, tint: Color, size: Dp = 20.dp) {
    val res = when (mood) {
        Mood.HEART -> R.drawable.ic_heart
        Mood.SORRY -> R.drawable.ic_sorry
        Mood.ANNOY -> R.drawable.ic_poke
    }
    Icon(painterResource(res), contentDescription = null, tint = tint, modifier = Modifier.size(size))
}

/**
 * A failure in place: what went wrong and what to do next, in the error role with a glyph so it does
 * not rest on colour alone. TalkBack reads it out when it appears.
 */
@Composable
fun ErrorNote(text: String, modifier: Modifier = Modifier) {
    val c = MaterialTheme.colorScheme.error
    Row(modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite }) {
        Icon(painterResource(R.drawable.ic_error), contentDescription = null, tint = c, modifier = Modifier.padding(top = 1.dp).size(18.dp))
        Spacer(Modifier.width(Space.s))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = c)
    }
}

@Composable
fun VectorIcon(v: ImageVector, tint: Color, size: Dp = 18.dp) =
    Icon(v, contentDescription = null, tint = tint, modifier = Modifier.size(size))

/**
 * The beacon itself: its nine pixels on a ring. Lit pixels use [color]; [pulse] breathes them the way
 * the hardware does in the pairing window.
 */
@Composable
fun LedRing(lit: Boolean, color: Color, size: Dp, modifier: Modifier = Modifier, pulse: Boolean = false, body: Boolean = false) {
    val dim = MaterialTheme.colorScheme.outlineVariant
    val face = MaterialTheme.colorScheme.surfaceContainerHigh
    val alpha = if (pulse) {
        rememberInfiniteTransition(label = "pulse").animateFloat(
            initialValue = 0.35f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(tween(1100, easing = FastOutSlowInEasing), RepeatMode.Reverse),
            label = "alpha",
        ).value
    } else {
        1f
    }
    Canvas(modifier.size(size)) {
        val r = this.size.minDimension / 2
        // Drawn as the device (a round face with its pixels inset) when large, so it never reads as a spinner.
        if (body) drawCircle(face, r)
        val dot = r * if (body) 0.085f else 0.17f
        val ring = if (body) r * 0.68f else r - dot
        repeat(9) { i ->
            val a = Math.toRadians(-90.0 + i * 40.0)
            val c = Offset(center.x + ring * cos(a).toFloat(), center.y + ring * sin(a).toFloat())
            drawCircle(if (lit) color.copy(alpha = alpha) else dim, dot, c)
        }
    }
}
