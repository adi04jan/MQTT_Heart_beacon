@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsTopHeight
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Done
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.heartbeacon.app.data.DeviceSettings
import com.heartbeacon.app.data.Event
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.Mark
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.ui.heart.HeartStage
import com.heartbeacon.app.ui.heart.Stage

class HomeVm(private val repo: HeartRepo) : ViewModel() {
    val beacon = repo.beacon
    val events = repo.events
    val server = repo.server
    val settings = repo.settings
    fun send(m: Mood) = repo.send(m)
    fun update(s: DeviceSettings) = repo.update(s)
    fun forget(ssid: String) = repo.forgetWifi(ssid)
    suspend fun join(ssid: String, psk: String) = repo.joinWifi(ssid, psk)
    fun remove() = repo.remove()
}

/** Home: your beacon's twin on its dark stage, its settings under it, then the recent moods. */
@Composable
fun HomeScreen(vm: HomeVm, onAdd: () -> Unit, onSettings: () -> Unit) {
    val beacon by vm.beacon.collectAsStateWithLifecycle()
    val events by vm.events.collectAsStateWithLifecycle()
    val server by vm.server.collectAsStateWithLifecycle()
    val settings by vm.settings.collectAsStateWithLifecycle()
    val fmt = rememberFormat()
    var dialog by remember { mutableStateOf<SettingsDialog?>(null) }
    val list = rememberLazyListState()
    val cs = MaterialTheme.colorScheme
    val title = beacon?.name ?: "Heart Beacon"

    BoxWithConstraints(Modifier.fillMaxSize().background(cs.surface)) {
        val stageHeight = maxHeight * 0.66f
        val stagePx = with(LocalDensity.current) { stageHeight.toPx() }
        val pinned by remember { derivedStateOf { list.firstVisibleItemIndex > 0 || list.firstVisibleItemScrollOffset > stagePx * 0.82f } }

        // The stage follows the theme, so the theme's icons suit it and the pinned bar alike.
        SurfaceStatusBar()

        LazyColumn(
            state = list,
            contentPadding = PaddingValues(bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding() + Space.xxl),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "stage") {
                HeartStage(
                    beacon = beacon,
                    server = server,
                    events = events,
                    bright = settings?.bright ?: 100,
                    fmt = fmt,
                    onSend = vm::send,
                    onAdd = onAdd,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(stageHeight)
                        .clip(RoundedCornerShape(bottomStart = 28.dp, bottomEnd = 28.dp)),
                ) { StageBar(title, beacon?.did, onSettings) }
            }
            val cur = settings
            if (beacon != null && cur != null) beaconSettings(cur, fmt, vm::update, vm::forget) { dialog = it }

            item { SectionTitle("Recent") }
            if (events.isEmpty()) {
                item {
                    Text(
                        "Nothing yet. Moods you send and receive show up here.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = cs.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = Space.screen + Space.s, vertical = Space.s),
                    )
                }
            }
            val recent = events.take(6)
            if (recent.isNotEmpty()) {
                item {
                    Group {
                        recent.forEachIndexed { i, e ->
                            if (i > 0) Line()
                            EventRow(e, fmt)
                        }
                    }
                }
            }
        }

        // Once the stage starts to leave and until the bar pins, keep its ground under the status bar, so the
        // dashboard scrolling up never runs into the clock.
        val leaving by remember { derivedStateOf { !pinned && list.firstVisibleItemScrollOffset > stagePx * 0.12f } }
        AnimatedVisibility(leaving, enter = fadeIn(tween(160)), exit = fadeOut(tween(120))) {
            Column {
                Box(Modifier.fillMaxWidth().windowInsetsTopHeight(WindowInsets.statusBars).background(Stage.ground))
                Box(Modifier.fillMaxWidth().height(24.dp).background(Brush.verticalGradient(listOf(Stage.ground, Color.Transparent))))
            }
        }

        AnimatedVisibility(pinned, enter = fadeIn(tween(160)), exit = fadeOut(tween(120))) {
            TopAppBar(
                title = { Text(title) },
                actions = { IconButton(onClick = onSettings) { Icon(Icons.Outlined.Settings, contentDescription = "App settings") } },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = cs.surfaceContainer),
            )
        }
    }
    settings?.let { SettingsDialogs(dialog, it, vm::update, vm::join, vm::remove) { dialog = null } }
}

/** The stage's own bar: the beacon's name and did on the dark ground, app settings at the end. */
@Composable
private fun StageBar(title: String, did: String?, onSettings: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().statusBarsPadding().padding(start = 20.dp, end = Space.xs, top = Space.s),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.headlineMedium, color = Stage.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
            did?.let { Text(it, style = MaterialTheme.typography.labelMedium.merge(Tabular), color = Stage.label) }
        }
        IconButton(onClick = onSettings) { Icon(Icons.Outlined.Settings, contentDescription = "App settings", tint = Stage.text) }
    }
}

@Composable
private fun EventRow(e: Event, fmt: Format) {
    val cs = MaterialTheme.colorScheme
    Row(
        Modifier.fillMaxWidth().padding(horizontal = Space.l, vertical = Space.m),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        MoodDisc(e.mood)
        Spacer(Modifier.width(Space.l))
        Column(Modifier.weight(1f)) {
            Text(fmt.event(e.mood, e.from), style = MaterialTheme.typography.bodyLarge)
            Text(
                "${fmt.dayHeader(e.atEpochMs, System.currentTimeMillis())}, ${fmt.clock(e.atEpochMs)}",
                style = MaterialTheme.typography.bodySmall.merge(Tabular),
                color = cs.onSurfaceVariant,
            )
        }
        e.mark?.let { MarkLabel(it, fmt) }
    }
}

@Composable
private fun MarkLabel(m: Mark, fmt: Format) {
    val cs = MaterialTheme.colorScheme
    val color: Color = when (m) {
        Mark.SHOWN -> cs.primary
        Mark.QUEUED -> cs.onSurfaceVariant
        Mark.EXPIRED -> cs.error
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        if (m == Mark.SHOWN) {
            VectorIcon(Icons.Filled.Done, color, 16.dp)
            Spacer(Modifier.width(Space.xs))
        }
        Text(fmt.mark(m), style = MaterialTheme.typography.labelMedium, color = color)
    }
}
