@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Done
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.unit.dp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.heartbeacon.app.data.Beacon
import com.heartbeacon.app.data.Event
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.Mark
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import com.heartbeacon.app.data.ServerLink

class HomeVm(private val repo: HeartRepo) : ViewModel() {
    val beacon = repo.beacon
    val events = repo.events
    val server = repo.server
    fun send(m: Mood) = repo.send(m)
}

@Composable
fun HomeScreen(vm: HomeVm, onBeacon: () -> Unit, onAdd: () -> Unit, onSettings: () -> Unit) {
    val beacon by vm.beacon.collectAsStateWithLifecycle()
    val events by vm.events.collectAsStateWithLifecycle()
    val server by vm.server.collectAsStateWithLifecycle()
    val fmt = rememberFormat()
    val now = System.currentTimeMillis()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Heart Beacon") },
                actions = {
                    IconButton(onClick = onSettings) { Icon(Icons.Outlined.Settings, contentDescription = "Settings") }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.surface),
            )
        },
    ) { pad ->
        LazyColumn(
            contentPadding = PaddingValues(
                start = Space.screen,
                end = Space.screen,
                top = pad.calculateTopPadding() + Space.s,
                bottom = pad.calculateBottomPadding() + Space.xxl,
            ),
            modifier = Modifier.fillMaxSize(),
        ) {
            item { beacon?.let { BeaconCard(it, fmt, now, onBeacon) } ?: NoBeacon(onAdd) }
            if (server != ServerLink.CONNECTED) item { ServerNote(server) }
            item { MoodButtons(vm::send) }
            item {
                Text(
                    "Recent",
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(top = Space.xxl, bottom = Space.xs),
                )
            }
            if (events.isEmpty()) {
                item {
                    Text(
                        "Nothing yet. Hearts you send and receive show up here.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(vertical = Space.l),
                    )
                }
            }
            var lastDay: String? = null
            events.take(20).forEach { e ->
                val day = fmt.dayHeader(e.atEpochMs, now)
                if (day != lastDay) {
                    lastDay = day
                    item(key = "day-$day") {
                        Text(
                            day,
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = Space.l, bottom = Space.xs),
                        )
                    }
                }
                item(key = e.id) { EventRow(e, fmt) }
            }
        }
    }
}

@Composable
private fun BeaconCard(b: Beacon, fmt: Format, now: Long, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val near = b.presence == Presence.Near
    Surface(onClick = onClick, shape = RoundedCornerShape(24.dp), color = cs.surfaceContainerLow) {
        Column(Modifier.padding(Space.screen)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                LedRing(lit = near, color = cs.primary, size = 44.dp)
                Spacer(Modifier.width(Space.l))
                Column(Modifier.weight(1f)) {
                    Text(b.name, style = MaterialTheme.typography.headlineSmall)
                    Text(b.did, style = MaterialTheme.typography.bodySmall.merge(Tabular), color = cs.onSurfaceVariant)
                }
                Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = "Beacon settings", tint = cs.onSurfaceVariant)
            }
            Spacer(Modifier.height(Space.xl))
            Row(verticalAlignment = Alignment.CenterVertically) {
                val dot = when (b.presence) {
                    Presence.Near -> cs.primary
                    Presence.AwayWifi -> cs.onSurfaceVariant
                    is Presence.Asleep -> cs.outline
                    Presence.Offline -> cs.error
                }
                Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
                Spacer(Modifier.width(Space.s))
                Text(fmt.presence(b.presence, now), style = MaterialTheme.typography.titleMedium)
            }
            Text(
                fmt.presenceDetail(b.presence),
                style = MaterialTheme.typography.bodyMedium,
                color = cs.onSurfaceVariant,
                modifier = Modifier.padding(start = Space.l, top = 2.dp),
            )
            HorizontalDivider(Modifier.padding(vertical = Space.l), color = cs.outlineVariant)
            Row {
                Fact("Battery", b.batteryPct?.let { "$it%" } ?: "Unknown", Modifier.weight(1f)) {
                    b.batteryPct?.let { BatteryMeter(it) }
                }
                Fact("Last seen", b.lastSeenEpochMs?.let { fmt.ago(it, now) } ?: "Never", Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun Fact(label: String, value: String, modifier: Modifier, lead: (@Composable () -> Unit)? = null) {
    Column(modifier) {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(2.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            lead?.let { it(); Spacer(Modifier.width(Space.s)) }
            Text(value, style = MaterialTheme.typography.titleMedium.merge(Tabular))
        }
    }
}

@Composable
private fun BatteryMeter(pct: Int) {
    val cs = MaterialTheme.colorScheme
    val fill = if (pct <= 20) cs.error else cs.onSurfaceVariant
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(width = 22.dp, height = 11.dp)
                .clip(RoundedCornerShape(3.dp))
                .background(cs.outlineVariant),
        ) {
            Box(
                Modifier
                    .fillMaxHeight()
                    .fillMaxWidth(pct.coerceIn(0, 100) / 100f)
                    .background(fill),
            )
        }
        Box(Modifier.size(width = 2.dp, height = 5.dp).background(cs.outlineVariant))
    }
}

@Composable
private fun NoBeacon(onAdd: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Surface(shape = RoundedCornerShape(24.dp), color = cs.surfaceContainerLow) {
        Column(Modifier.fillMaxWidth().padding(Space.screen), verticalArrangement = Arrangement.spacedBy(Space.s)) {
            LedRing(lit = false, color = cs.primary, size = 44.dp)
            Spacer(Modifier.height(Space.xs))
            Text("No beacon yet", style = MaterialTheme.typography.titleLarge)
            Text(
                "You can still send hearts. Add your beacon to see them light up on your desk.",
                style = MaterialTheme.typography.bodyMedium,
                color = cs.onSurfaceVariant,
            )
            Spacer(Modifier.height(Space.xs))
            FilledTonalButton(onClick = onAdd) { Text("Add beacon") }
        }
    }
}

@Composable
private fun ServerNote(s: ServerLink) {
    Text(
        if (s == ServerLink.CONNECTING) "Connecting to the server…" else "No connection. What you send goes out when it is back.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = Space.l),
    )
}

@Composable
private fun MoodButtons(send: (Mood) -> Unit) {
    val haptics = LocalHapticFeedback.current
    val tap = { m: Mood -> haptics.performHapticFeedback(HapticFeedbackType.LongPress); send(m) }
    val cs = MaterialTheme.colorScheme
    Column(Modifier.padding(top = Space.xl), verticalArrangement = Arrangement.spacedBy(Space.s)) {
        Button(onClick = { tap(Mood.HEART) }, modifier = Modifier.fillMaxWidth().height(60.dp)) {
            MoodIcon(Mood.HEART, cs.onPrimary)
            Spacer(Modifier.width(Space.s))
            Text("Send a Heart", style = MaterialTheme.typography.titleMedium)
        }
        Row(horizontalArrangement = Arrangement.spacedBy(Space.s)) {
            for (m in listOf(Mood.SORRY, Mood.ANNOY)) {
                FilledTonalButton(onClick = { tap(m) }, modifier = Modifier.weight(1f).height(52.dp)) {
                    MoodIcon(m, cs.onSecondaryContainer, size = 18.dp)
                    Spacer(Modifier.width(Space.s))
                    Text(if (m == Mood.SORRY) "Sorry" else "Annoy")
                }
            }
        }
        Text(
            "Everyone in your circle gets it, on their beacon and phone.",
            style = MaterialTheme.typography.bodySmall,
            color = cs.onSurfaceVariant,
            modifier = Modifier.padding(top = Space.xs),
        )
    }
}

@Composable
private fun EventRow(e: Event, fmt: Format) {
    val cs = MaterialTheme.colorScheme
    Row(Modifier.fillMaxWidth().padding(vertical = Space.s), verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(36.dp).clip(CircleShape).background(cs.surfaceContainerHigh), contentAlignment = Alignment.Center) {
            MoodIcon(e.mood, if (e.mood == Mood.HEART) cs.primary else cs.onSurfaceVariant, size = 18.dp)
        }
        Spacer(Modifier.width(Space.m))
        Column(Modifier.weight(1f)) {
            Text(fmt.event(e.mood, e.from), style = MaterialTheme.typography.bodyLarge)
            Text(fmt.clock(e.atEpochMs), style = MaterialTheme.typography.bodySmall.merge(Tabular), color = cs.onSurfaceVariant)
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
