@file:OptIn(ExperimentalMaterial3Api::class)

package com.heartbeacon.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MultiChoiceSegmentedButtonRow
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TimePicker
import androidx.compose.material3.rememberTimePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.heartbeacon.app.data.DeviceSettings
import com.heartbeacon.app.data.HeartRepo
import java.time.LocalTime

class DeviceSettingsVm(private val repo: HeartRepo) : ViewModel() {
    val settings = repo.settings
    fun update(s: DeviceSettings) = repo.update(s)
    fun forget(ssid: String) = repo.forgetWifi(ssid)
    suspend fun join(ssid: String, psk: String) = repo.joinWifi(ssid, psk)
    fun remove() = repo.remove()
}

private enum class Dialog { NAME, WIFI, START, STOP, REMOVE }

@Composable
fun DeviceSettingsScreen(vm: DeviceSettingsVm, onBack: () -> Unit, onRemoved: () -> Unit) {
    val s by vm.settings.collectAsStateWithLifecycle()
    val cur = s ?: return
    var dialog by remember { mutableStateOf<Dialog?>(null) }
    val fmt = rememberFormat()
    fun hhmm(t: String) = fmt.clock(LocalTime.parse(t))

    Scaffold(topBar = { BackBar(cur.name, onBack) }) { pad ->
        LazyColumn(Modifier.fillMaxSize().padding(bottom = pad.calculateBottomPadding()), contentPadding = PaddingValues(top = pad.calculateTopPadding(), bottom = Space.xxl)) {
            item {
                ItemRow("Name", cur.name, onClick = { dialog = Dialog.NAME })
            }

            item { SectionTitle("Wi-Fi") }
            cur.wifi.forEach { ssid ->
                item(key = "wifi-$ssid") {
                    ItemRow(ssid, "Saved on the beacon", trailing = { TextButton(onClick = { vm.forget(ssid) }) { Text("Forget") } })
                }
            }
            if (cur.wifi.size < 4) {
                item {
                    ItemRow("Add network", onClick = { dialog = Dialog.WIFI }, trailing = {
                        VectorIcon(Icons.Filled.Add, MaterialTheme.colorScheme.onSurfaceVariant, 22.dp)
                    })
                }
            }
            item {
                SwitchRow(
                    "Use Wi-Fi when your phone is away",
                    "Off keeps the beacon on Bluetooth only. It goes quiet when your phone leaves.",
                    cur.useWifiAway,
                ) { vm.update(cur.copy(useWifiAway = it)) }
            }

            item { SectionTitle("Auto on and off") }
            item {
                SwitchRow(
                    "Sleep outside these hours",
                    "Hearts sent while it sleeps play when it wakes.",
                    cur.schedOn,
                ) { vm.update(cur.copy(schedOn = it)) }
            }
            item { Days(cur.days, cur.schedOn) { vm.update(cur.copy(days = it)) } }
            item {
                Row {
                    Column(Modifier.weight(1f)) { ItemRow("Wakes", hhmm(cur.start), onClick = { dialog = Dialog.START }) }
                    Column(Modifier.weight(1f)) {
                        ItemRow("Sleeps", fmt.sleeps(LocalTime.parse(cur.start), LocalTime.parse(cur.stop)), onClick = { dialog = Dialog.STOP })
                    }
                }
            }

            item { SectionTitle("Light and power") }
            item { Brightness(cur.bright) { vm.update(cur.copy(bright = it)) } }
            item {
                SwitchRow(
                    "Nap when idle",
                    "After 90 minutes with no hearts it naps and checks in every 15 minutes. Saves battery; hearts can take up to 15 minutes.",
                    cur.napWhenIdle,
                ) { vm.update(cur.copy(napWhenIdle = it)) }
            }

            item { SectionTitle("Firmware") }
            item {
                ItemRow(
                    if (cur.fw.isEmpty()) "Version not reported yet" else "Version ${cur.fw}",
                    cur.fwAvailable?.let { "$it is ready. Installs over Wi-Fi with at least 70% battery." } ?: "Up to date",
                    trailing = cur.fwAvailable?.let { { FilledTonalButton(onClick = {}) { Text("Update") } } },
                )
            }

            item {
                OutlinedButton(
                    onClick = { dialog = Dialog.REMOVE },
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    modifier = Modifier.padding(start = Space.screen, end = Space.screen, top = Space.xxl).fillMaxWidth().height(48.dp),
                ) { Text("Remove beacon") }
            }
        }
    }

    when (dialog) {
        Dialog.NAME -> NameDialog(cur.name, { dialog = null }) { vm.update(cur.copy(name = it)); dialog = null }
        Dialog.WIFI -> WifiDialog({ dialog = null }, vm::join)
        Dialog.START -> TimeDialog("Wakes at", cur.start, { dialog = null }) { vm.update(cur.copy(start = it)); dialog = null }
        Dialog.STOP -> TimeDialog("Sleeps at", cur.stop, { dialog = null }) { vm.update(cur.copy(stop = it)); dialog = null }
        Dialog.REMOVE -> AlertDialog(
            onDismissRequest = { dialog = null },
            title = { Text("Remove ${cur.name}?") },
            text = { Text("It is reset to factory settings and leaves your account. You can add it again later.") },
            confirmButton = {
                TextButton(onClick = { vm.remove(); dialog = null; onRemoved() }) {
                    Text("Remove", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { dialog = null }) { Text("Cancel") } },
        )
        null -> Unit
    }
}

@Composable
private fun Days(days: String, enabled: Boolean, onChange: (String) -> Unit) {
    val labels = listOf("M", "T", "W", "T", "F", "S", "S")
    val names = listOf("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
    MultiChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(horizontal = Space.screen, vertical = Space.s)) {
        labels.forEachIndexed { i, l ->
            val on = days.getOrNull(i) == '1'
            SegmentedButton(
                checked = on,
                onCheckedChange = { c -> onChange(days.padEnd(7, '0').replaceRange(i, i + 1, if (c) "1" else "0")) },
                shape = SegmentedButtonDefaults.itemShape(i, 7),
                colors = SegmentedButtonDefaults.colors(
                    activeContainerColor = MaterialTheme.colorScheme.primaryContainer,
                    activeContentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                ),
                enabled = enabled,
                icon = {},
                label = { Text(l) },
                modifier = Modifier.semantics { contentDescription = names[i] },
            )
        }
    }
}

@Composable
private fun Brightness(value: Int, onDone: (Int) -> Unit) {
    var v by remember(value) { mutableFloatStateOf(value.toFloat()) }
    Column(Modifier.padding(horizontal = Space.screen, vertical = Space.s)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Brightness", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
            Text("${v.toInt()}%", style = MaterialTheme.typography.bodyMedium.merge(Tabular), color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Slider(value = v, onValueChange = { v = it }, valueRange = 5f..100f, onValueChangeFinished = { onDone(v.toInt()) })
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
    val scope = androidx.compose.runtime.rememberCoroutineScope()
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
    val state = rememberTimePickerState(t.hour, t.minute, is24Hour = android.text.format.DateFormat.is24HourFormat(androidx.compose.ui.platform.LocalContext.current))
    AlertDialog(
        onDismissRequest = dismiss,
        title = { Text(title) },
        text = { TimePicker(state) },
        confirmButton = { TextButton(onClick = { save("%02d:%02d".format(state.hour, state.minute)) }) { Text("Save") } },
        dismissButton = { TextButton(onClick = dismiss) { Text("Cancel") } },
    )
}
