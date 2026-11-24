@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.heartbeacon.app.data.DeviceSettings
import kotlinx.coroutines.launch
import java.time.LocalTime

/** Which settings dialog is open. */
enum class SettingsDialog { NAME, WIFI, START, STOP, REMOVE }

/**
 * The beacon's settings, one plate per section, for the Home list under the heart. [note] says when changes reach a
 * beacon that is not up; Wi-Fi changes go over Bluetooth only, so they wait for the phone to be [near].
 */
fun LazyListScope.beaconSettings(
    cur: DeviceSettings,
    fmt: Format,
    note: String?,
    near: Boolean,
    update: (DeviceSettings) -> Unit,
    forget: (String) -> Unit,
    open: (SettingsDialog) -> Unit,
) {
    val start = LocalTime.parse(cur.start)
    val stop = LocalTime.parse(cur.stop)

    item { SectionTitle("Auto on and off") }
    note?.let { item { Note(it) } }
    item {
        Group {
            SwitchRow(
                "Sleep outside these hours",
                "Hearts sent while it sleeps play when it wakes.",
                cur.schedOn,
            ) { update(cur.copy(schedOn = it)) }
            Line()
            DayPlan(start, stop, cur.schedOn, fmt, open)
            Line()
            Days(cur.days, cur.schedOn) { update(cur.copy(days = it)) }
        }
    }

    item { SectionTitle("Light and power") }
    item {
        Group {
            Brightness(cur.bright) { update(cur.copy(bright = it)) }
            Line()
            SwitchRow(
                "Nap when idle",
                "After 90 minutes with no hearts it naps and checks in every 15 minutes. Saves battery; hearts can take up to 15 minutes.",
                cur.napWhenIdle,
            ) { update(cur.copy(napWhenIdle = it)) }
        }
    }

    item { SectionTitle("Wi-Fi") }
    item {
        Group {
            cur.wifi.forEach { ssid ->
                ItemRow(ssid, "Saved on the beacon", trailing = {
                    TextButton(onClick = { forget(ssid) }, enabled = near, modifier = Modifier.semantics { contentDescription = "Forget $ssid" }) {
                        Text("Forget")
                    }
                })
                Line()
            }
            if (cur.wifi.size < 4) {
                val cs = MaterialTheme.colorScheme
                ItemRow(
                    "Add network",
                    if (near) null else "Bring your phone near ${cur.name} first",
                    onClick = if (near) ({ open(SettingsDialog.WIFI) }) else null,
                    trailing = { VectorIcon(Icons.Filled.Add, if (near) cs.primary else cs.onSurface.copy(alpha = 0.38f), 22.dp) },
                )
                Line()
            }
            SwitchRow(
                "Use Wi-Fi when your phone is away",
                "Off keeps the beacon on Bluetooth only. It goes quiet when your phone leaves.",
                cur.useWifiAway,
            ) { update(cur.copy(useWifiAway = it)) }
        }
    }

    item { SectionTitle("Beacon") }
    item {
        Group {
            ItemRow("Name", cur.name, onClick = { open(SettingsDialog.NAME) })
            Line()
            ItemRow(
                if (cur.fw.isEmpty()) "Version not reported yet" else "Version ${cur.fw}",
                cur.fwAvailable?.let { "$it is ready. It installs itself over Wi-Fi with at least 70% battery." } ?: "Up to date",
            )
        }
    }
    item {
        TextButton(
            onClick = { open(SettingsDialog.REMOVE) },
            colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
            modifier = Modifier.padding(start = Space.screen, end = Space.screen, top = Space.s).fillMaxWidth().height(48.dp),
        ) { Text("Remove beacon") }
    }
}

/** Asks before a network is dropped from the beacon; it can only be added back with its password. */
@Composable
fun ForgetDialog(ssid: String, beacon: String, close: () -> Unit, forget: () -> Unit) = AlertDialog(
    onDismissRequest = close,
    title = { Text("Forget $ssid?") },
    text = { Text("$beacon stops joining it. To add it back you need its password.") },
    confirmButton = { TextButton(onClick = { forget(); close() }) { Text("Forget") } },
    dismissButton = { TextButton(onClick = close) { Text("Cancel") } },
)

/** A quiet line under a section heading: how the section behaves right now. */
@Composable
private fun Note(text: String) = Text(
    text,
    style = MaterialTheme.typography.bodyMedium,
    color = MaterialTheme.colorScheme.onSurfaceVariant,
    modifier = Modifier.padding(start = Space.screen + Space.s, end = Space.screen + Space.s, bottom = Space.m),
)

/** The open settings dialog, if any. */
@Composable
fun SettingsDialogs(
    dialog: SettingsDialog?,
    cur: DeviceSettings,
    update: (DeviceSettings) -> Unit,
    join: suspend (String, String) -> String?,
    remove: () -> Unit,
    close: () -> Unit,
) {
    when (dialog) {
        SettingsDialog.NAME -> NameDialog(cur.name, close) { update(cur.copy(name = it)); close() }
        SettingsDialog.WIFI -> WifiDialog(close, join)
        SettingsDialog.START -> TimeDialog("Wakes at", cur.start, close) { update(cur.copy(start = it)); close() }
        SettingsDialog.STOP -> TimeDialog("Sleeps at", cur.stop, close) { update(cur.copy(stop = it)); close() }
        SettingsDialog.REMOVE -> AlertDialog(
            onDismissRequest = close,
            title = { Text("Remove ${cur.name}?") },
            text = { Text("It is reset to factory settings and leaves your account. You can add it again later.") },
            confirmButton = {
                TextButton(onClick = { remove(); close() }) {
                    Text("Remove", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = close) { Text("Cancel") } },
        )
        null -> Unit
    }
}

/** The day as a bar, midnight to midnight, with the hours the beacon is awake filled in; the two times under it. */
@Composable
private fun DayPlan(start: LocalTime, stop: LocalTime, enabled: Boolean, fmt: Format, open: (SettingsDialog) -> Unit) {
    val cs = MaterialTheme.colorScheme
    val fill = if (enabled) cs.brass else cs.onSurface.copy(alpha = 0.28f)
    val track = cs.surfaceContainerHighest
    val tick = cs.outline
    val spans = awakeSpans(start, stop)
    Column(Modifier.padding(top = Space.l)) {
        Canvas(
            Modifier
                .padding(horizontal = Space.l)
                .fillMaxWidth()
                .height(22.dp)
                .semantics { contentDescription = "Awake from ${fmt.clock(start)} to ${fmt.sleeps(start, stop)}" },
        ) {
            val h = 10.dp.toPx()
            val round = CornerRadius(h / 2)
            drawRoundRect(track, size = Size(size.width, h), cornerRadius = round)
            for ((a, b) in spans) {
                drawRoundRect(fill, topLeft = Offset(size.width * a, 0f), size = Size(size.width * (b - a), h), cornerRadius = round)
            }
            for (i in 1..3) {
                val x = size.width * i / 4f
                drawLine(tick, Offset(x, h + 4.dp.toPx()), Offset(x, h + 10.dp.toPx()), strokeWidth = 1.dp.toPx())
            }
        }
        Row(Modifier.padding(horizontal = Space.l).fillMaxWidth().clearAndSetSemantics {}) {
            Spacer(Modifier.weight(0.5f))
            for (hour in listOf(6, 12, 18)) {
                Text(
                    fmt.clock(LocalTime.of(hour, 0)),
                    style = MaterialTheme.typography.labelSmall.merge(Tabular),
                    color = cs.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    modifier = Modifier.weight(1f),
                )
            }
            Spacer(Modifier.weight(0.5f))
        }
        Row(Modifier.padding(top = Space.xs)) {
            TimeCell("Wakes", fmt.clock(start), Modifier.weight(1f)) { open(SettingsDialog.START) }
            TimeCell("Sleeps", fmt.sleeps(start, stop), Modifier.weight(1f)) { open(SettingsDialog.STOP) }
        }
    }
}

@Composable
private fun TimeCell(label: String, value: String, modifier: Modifier, onClick: () -> Unit) {
    Column(modifier.clickable(role = Role.Button, onClickLabel = "Change", onClick = onClick).padding(horizontal = Space.l, vertical = Space.m)) {
        Text(label, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.titleLarge.merge(Tabular), color = MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun Days(days: String, enabled: Boolean, onChange: (String) -> Unit) {
    val labels = listOf("M", "T", "W", "T", "F", "S", "S")
    val names = listOf("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
    val cs = MaterialTheme.colorScheme
    Row(Modifier.fillMaxWidth().padding(horizontal = Space.s, vertical = Space.m)) {
        labels.forEachIndexed { i, l ->
            val on = days.getOrNull(i) == '1'
            Box(
                Modifier
                    .weight(1f)
                    .height(48.dp)
                    .toggleable(
                        value = on,
                        enabled = enabled,
                        role = Role.Checkbox,
                        onValueChange = { c -> onChange(days.padEnd(7, '0').replaceRange(i, i + 1, if (c) "1" else "0")) },
                    )
                    .semantics { contentDescription = names[i] },
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier
                        .size(40.dp)
                        .alpha(if (enabled) 1f else 0.38f)
                        .clip(CircleShape)
                        .background(if (on) cs.brass else Color.Transparent)
                        .border(1.dp, if (on) Color.Transparent else cs.outlineVariant, CircleShape),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(l, style = MaterialTheme.typography.labelLarge, color = if (on) cs.onBrass else cs.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun Brightness(value: Int, onDone: (Int) -> Unit) {
    var v by remember(value) { mutableFloatStateOf(value.toFloat()) }
    Column(Modifier.padding(start = Space.l, end = Space.l, top = Space.l, bottom = Space.xs)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Brightness", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
            Text("${v.toInt()}%", style = MaterialTheme.typography.titleMedium.merge(Tabular), color = MaterialTheme.colorScheme.onSurface)
        }
        Slider(
            value = v,
            onValueChange = { v = it },
            valueRange = 5f..100f,
            onValueChangeFinished = { onDone(v.toInt()) },
            colors = SliderDefaults.colors(thumbColor = MaterialTheme.colorScheme.brass, activeTrackColor = MaterialTheme.colorScheme.brass),
            modifier = Modifier.semantics { contentDescription = "Brightness" },
        )
    }
}

@Composable
private fun NameDialog(current: String, dismiss: () -> Unit, save: (String) -> Unit) {
    var name by remember { mutableStateOf(current) }
    AlertDialog(
        onDismissRequest = dismiss,
        title = { Text("Name") },
        text = {
            OutlinedTextField(name, { if (it.encodeToByteArray().size <= 32) name = it }, singleLine = true, modifier = Modifier.fillMaxWidth())
        },
        confirmButton = { TextButton(onClick = { save(name.trim()) }, enabled = name.isNotBlank()) { Text("Save") } },
        dismissButton = { TextButton(onClick = dismiss) { Text("Cancel") } },
    )
}

@Composable
private fun WifiDialog(dismiss: () -> Unit, join: suspend (String, String) -> String?) {
    var ssid by remember { mutableStateOf("") }
    var psk by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    AlertDialog(
        onDismissRequest = dismiss,
        title = { Text("Add network") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(Space.s)) {
                OutlinedTextField(ssid, { ssid = it }, label = { Text("Network name") }, singleLine = true)
                OutlinedTextField(
                    psk, { psk = it },
                    label = { Text("Password") },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    isError = error != null,
                    supportingText = { Text(error ?: "Sent to the beacon only.") },
                )
            }
        },
        confirmButton = {
            TextButton(
                enabled = !busy && ssid.isNotBlank() && (psk.isEmpty() || psk.length >= 8),
                onClick = {
                    busy = true
                    scope.launch {
                        error = join(ssid.trim(), psk)
                        busy = false
                        if (error == null) dismiss()
                    }
                },
            ) { Text(if (busy) "Joining…" else "Join") }
        },
        dismissButton = { TextButton(onClick = dismiss) { Text("Cancel") } },
    )
}

@Composable
private fun TimeDialog(title: String, current: String, dismiss: () -> Unit, save: (String) -> Unit) {
    val t = LocalTime.parse(current)
    val state = rememberTimePickerState(t.hour, t.minute, is24Hour = android.text.format.DateFormat.is24HourFormat(LocalContext.current))
    AlertDialog(
        onDismissRequest = dismiss,
        title = { Text(title) },
        text = { TimePicker(state) },
        confirmButton = { TextButton(onClick = { save("%02d:%02d".format(state.hour, state.minute)) }) { Text("Save") } },
        dismissButton = { TextButton(onClick = dismiss) { Text("Cancel") } },
    )
}
