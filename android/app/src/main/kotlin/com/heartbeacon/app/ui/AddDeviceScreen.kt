package com.heartbeacon.app.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
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
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.viewModelScope
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.WifiNet
import com.heartbeacon.app.link.BatteryExemption
import kotlinx.coroutines.launch

class AddDeviceVm(private val repo: HeartRepo, start: Int) : ViewModel() {
    var step by mutableIntStateOf(start)
    var did by mutableStateOf<String?>(null)
        private set
    var pairError by mutableStateOf<String?>(null)
        private set
    var networks by mutableStateOf<List<WifiNet>?>(null)
        private set
    var chosen by mutableStateOf<WifiNet?>(null)
    var psk by mutableStateOf("")
    var joining by mutableStateOf(false)
        private set
    var joinError by mutableStateOf<String?>(null)
        private set
    var name by mutableStateOf("Desk")

    init {
        if (start >= WIFI) {
            did = "HB-3FA2C1"
            scan()
        }
    }

    /** The CDM chooser returned (or the fake stood in for it). */
    fun paired(mac: String?) {
        step = PAIRING
        pairError = null
        viewModelScope.launch {
            did = repo.pair(mac)
            if (did == null) {
                pairError = "Could not pair. Hold the button again until it pulses blue, then retry."
                step = HOLD
            } else {
                step = WIFI
                scan()
            }
        }
    }

    fun scan() {
        networks = null
        viewModelScope.launch { networks = repo.scanWifi() }
    }

    fun choose(n: WifiNet) {
        chosen = n
        psk = ""
        joinError = null
    }

    fun join() {
        val n = chosen ?: return
        joining = true
        viewModelScope.launch {
            joinError = repo.joinWifi(n.ssid, psk)
            joining = false
            if (joinError == null) step = NAME
        }
    }

    fun finish(done: () -> Unit) {
        viewModelScope.launch {
            repo.claim(did ?: return@launch, name.trim())
            done()
        }
    }

    companion object {
        const val PERMISSIONS = 0
        const val HOLD = 1
        const val PAIRING = 2
        const val WIFI = 3
        const val NAME = 4
        const val STEPS = 5
    }
}

@Composable
fun AddDeviceScreen(vm: AddDeviceVm, onFind: () -> Unit, onBack: () -> Unit, onDone: () -> Unit) {
    Scaffold(topBar = { BackBar("Add beacon", onBack) }) { pad ->
        Column(Modifier.padding(pad).fillMaxSize()) {
            LinearProgressIndicator(
                progress = { (vm.step + 1f) / AddDeviceVm.STEPS },
                modifier = Modifier.fillMaxWidth().padding(horizontal = Space.screen),
                drawStopIndicator = {},
            )
            Text(
                "Step ${vm.step + 1} of ${AddDeviceVm.STEPS}",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = Space.screen, vertical = Space.s),
            )
            AnimatedContent(
                targetState = vm.step,
                transitionSpec = { fadeIn(tween(220, delayMillis = 90)) togetherWith fadeOut(tween(90)) },
                label = "step",
                modifier = Modifier.weight(1f),
            ) { step ->
                Column(
                    Modifier
                        .fillMaxSize()
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = Space.screen, vertical = Space.l),
                    verticalArrangement = Arrangement.spacedBy(Space.l),
                ) {
                    when (step) {
                        AddDeviceVm.PERMISSIONS -> PermissionsStep { vm.step = AddDeviceVm.HOLD }
                        AddDeviceVm.HOLD -> HoldStep(vm.pairError, onFind)
                        AddDeviceVm.PAIRING -> PairingStep()
                        AddDeviceVm.WIFI -> WifiStep(vm)
                        else -> NameStep(vm) { vm.finish(onDone) }
                    }
                }
            }
        }
    }
}

@Composable
private fun Heading(title: String, body: String) {
    Column(verticalArrangement = Arrangement.spacedBy(Space.s)) {
        Text(title, style = MaterialTheme.typography.headlineSmall)
        Text(body, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun PermissionsStep(next: () -> Unit) {
    val ctx = LocalContext.current
    fun granted(p: String) = ContextCompat.checkSelfPermission(ctx, p) == PackageManager.PERMISSION_GRANTED
    var nearby by remember { mutableStateOf(granted(Manifest.permission.BLUETOOTH_CONNECT)) }
    var notify by remember { mutableStateOf(granted(Manifest.permission.POST_NOTIFICATIONS)) }
    var exempt by remember { mutableStateOf(BatteryExemption.isExempt(ctx)) }
    LifecycleResumeEffect(Unit) {
        nearby = granted(Manifest.permission.BLUETOOTH_CONNECT)
        notify = granted(Manifest.permission.POST_NOTIFICATIONS)
        exempt = BatteryExemption.isExempt(ctx)
        onPauseOrDispose {}
    }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        nearby = granted(Manifest.permission.BLUETOOTH_CONNECT)
        notify = granted(Manifest.permission.POST_NOTIFICATIONS)
    }
    val exemption = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        exempt = BatteryExemption.isExempt(ctx)
    }

    Heading("Before we look for it", "Three things let your beacon reach you, even with the app closed.")
    Column {
        Need("Nearby devices", "Talk to your beacon over Bluetooth", nearby) { ask.launch(Manifest.permission.BLUETOOTH_CONNECT) }
        Need("Notifications", "Tell you when a heart arrives", notify) { ask.launch(Manifest.permission.POST_NOTIFICATIONS) }
        Need("Run in background", "Keep the link alive when the screen is off", exempt) {
            exemption.launch(BatteryExemption.requestIntent(ctx))
        }
    }
    Spacer(Modifier.height(Space.s))
    Button(onClick = next, enabled = nearby, modifier = Modifier.fillMaxWidth().height(52.dp)) { Text("Continue") }
    if (!nearby) {
        Text(
            "Nearby devices is required. The other two can wait.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun Need(title: String, why: String, ok: Boolean, allow: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = Space.m), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = Space.l)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(why, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (ok) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                VectorIcon(Icons.Filled.CheckCircle, MaterialTheme.colorScheme.primary, 18.dp)
                Spacer(Modifier.width(Space.xs))
                Text("Allowed", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
            }
        } else {
            FilledTonalButton(onClick = allow) { Text("Allow") }
        }
    }
}

/** The colour the hardware shows in its pairing window. A depiction of the device, not a UI accent. */
private val PairBlue @Composable get() =
    if (MaterialTheme.colorScheme.surface.red < 0.5f) Color(0xFF9DB8FF) else Color(0xFF3D63C9)

@Composable
private fun HoldStep(error: String?, onFind: () -> Unit) {
    Box(Modifier.fillMaxWidth().padding(vertical = Space.xl), contentAlignment = Alignment.Center) {
        LedRing(lit = true, color = PairBlue, size = 168.dp, pulse = true, body = true)
    }
    Heading(
        "Hold its button for 5 seconds",
        "Keep holding until the lights pulse blue. It then stays ready to pair for 2 minutes.",
    )
    error?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error) }
    Spacer(Modifier.height(Space.s))
    Button(onClick = onFind, modifier = Modifier.fillMaxWidth().height(52.dp)) { Text("Find my beacon") }
    Text(
        "Android lists the beacons it can see. Pick the one whose name starts with HB-.",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun PairingStep() {
    Box(Modifier.fillMaxWidth().padding(vertical = Space.xl), contentAlignment = Alignment.Center) {
        LedRing(lit = true, color = PairBlue, size = 168.dp, pulse = true, body = true)
        CircularProgressIndicator(Modifier.size(56.dp), strokeWidth = 3.dp)
    }
    Heading("Pairing", "If Android asks to pair with your beacon, tap Pair.")
}

@Composable
private fun WifiStep(vm: AddDeviceVm) {
    Heading("Choose its Wi-Fi", "Your beacon uses this when your phone is not around. It found these networks itself.")
    val list = vm.networks
    if (list == null) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
            Spacer(Modifier.width(Space.m))
            Text("Your beacon is looking…", style = MaterialTheme.typography.bodyMedium)
        }
    } else {
        Column {
            list.forEach { n ->
                val selected = vm.chosen == n
                Row(
                    Modifier
                        .fillMaxWidth()
                        .selectable(selected, role = Role.RadioButton) { vm.choose(n) }
                        .padding(vertical = Space.m),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    RadioButton(selected = selected, onClick = null, modifier = Modifier.padding(end = Space.l))
                    Column(Modifier.weight(1f)) {
                        Text(n.ssid, style = MaterialTheme.typography.bodyLarge)
                        Text(
                            signal(n.rssi),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    if (n.secure) VectorIcon(Icons.Outlined.Lock, MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        vm.chosen?.let { n ->
            if (n.secure) {
                OutlinedTextField(
                    value = vm.psk,
                    onValueChange = { vm.psk = it },
                    label = { Text("Password for ${n.ssid}") },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                    isError = vm.joinError != null,
                    supportingText = { Text(vm.joinError ?: "Sent to the beacon only. Never stored on the server.") },
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            Button(
                onClick = vm::join,
                enabled = !vm.joining && (!n.secure || vm.psk.length >= 8),
                modifier = Modifier.fillMaxWidth().height(52.dp),
            ) {
                if (vm.joining) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp) else Text("Join ${n.ssid}")
            }
        }
    }
    Row {
        TextButton(onClick = vm::scan) { Text("Scan again") }
        Spacer(Modifier.weight(1f))
        TextButton(onClick = { vm.step = AddDeviceVm.NAME }) { Text("Skip for now") }
    }
}

private fun signal(rssi: Int) = when {
    rssi >= -55 -> "Strong signal"
    rssi >= -70 -> "Good signal"
    else -> "Weak signal"
}

@Composable
private fun NameStep(vm: AddDeviceVm, done: () -> Unit) {
    Heading("Name your beacon", "Everyone in your circle sees this name.")
    OutlinedTextField(
        value = vm.name,
        onValueChange = { if (it.encodeToByteArray().size <= 32) vm.name = it },
        label = { Text("Name") },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    Button(onClick = done, enabled = vm.name.isNotBlank(), modifier = Modifier.fillMaxWidth().height(52.dp)) { Text("Done") }
}
