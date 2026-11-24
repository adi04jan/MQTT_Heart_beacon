@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import android.app.Activity
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import com.heartbeacon.app.R
import com.heartbeacon.app.data.Mood
import java.time.ZoneId

@Composable
fun rememberFormat(): Format {
    val locale = LocalContext.current.resources.configuration.locales[0]
    return androidx.compose.runtime.remember(locale) { Format(ZoneId.systemDefault(), locale) }
}

/** Status bar icons: dark over a light surface, light over the stage or a dark surface. */
@Composable
fun StatusBarIcons(dark: Boolean) {
    val view = LocalView.current
    SideEffect {
        (view.context as? Activity)?.window?.let { WindowCompat.getInsetsController(it, view).isAppearanceLightStatusBars = dark }
    }
}

/** For screens on the theme's own surface. */
@Composable
fun SurfaceStatusBar() = StatusBarIcons(MaterialTheme.colorScheme.surface.luminance() > 0.5f)

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

/** A section's heading: set in the dial face, in the text colour. The accent is kept for things you can press. */
@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        style = MaterialTheme.typography.titleMedium,
        color = MaterialTheme.colorScheme.onSurface,
        modifier = modifier
            .padding(start = Space.screen + Space.s, end = Space.screen, top = Space.xl + Space.xs, bottom = Space.s)
            .semantics { heading() },
    )
}

/** One section's rows on a single raised plate, hairlines between them (see [Line]). */
@Composable
fun Group(modifier: Modifier = Modifier, inset: Dp = Space.screen, content: @Composable ColumnScope.() -> Unit) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceContainer,
        shape = MaterialTheme.shapes.large,
        modifier = modifier.padding(horizontal = inset).fillMaxWidth(),
    ) { Column(content = content) }
}

@Composable
fun Line() = HorizontalDivider(Modifier.padding(horizontal = Space.l), color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))

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
        modifier = if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier,
    )
}

@Composable
fun SwitchRow(title: String, supporting: String?, checked: Boolean, onChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = supporting?.let { { Text(it) } },
        trailingContent = {
            val cs = MaterialTheme.colorScheme
            Switch(
                checked = checked,
                onCheckedChange = null,
                colors = SwitchDefaults.colors(checkedTrackColor = cs.brass, checkedThumbColor = cs.onBrass, checkedBorderColor = cs.brass),
            )
        },
        colors = rowColors,
        modifier = Modifier.toggleable(value = checked, role = Role.Switch, onValueChange = onChange),
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

/**
 * A mood's mark for lists, in the button's own notation: one dot for a Heart, two for Sorry, three for Annoy, in
 * that mood's LED colour, on a quiet disc.
 */
@Composable
fun MoodDisc(mood: Mood, size: Dp = 36.dp) {
    val cs = MaterialTheme.colorScheme
    val c = moodColor(mood, onLight = cs.surface.luminance() > 0.5f)
    Row(
        Modifier.size(size).clip(CircleShape).background(cs.surfaceContainerHighest),
        horizontalArrangement = Arrangement.spacedBy(3.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        repeat(mood.ordinal + 1) { Box(Modifier.size(6.dp).clip(CircleShape).background(c)) }
    }
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
