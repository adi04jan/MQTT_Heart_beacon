package com.heartbeacon.app.ui

import com.heartbeacon.app.data.Appearance
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.Settings
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.ViewModel
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewModelScope
import com.heartbeacon.app.BuildConfig
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.ServerLink
import com.heartbeacon.app.data.SettingsStore
import com.heartbeacon.app.link.BatteryExemption
import kotlinx.coroutines.launch

class AppSettingsVm(private val repo: HeartRepo, private val store: SettingsStore) : ViewModel() {
    val session = repo.session
    val server = repo.server
    val local = store.flow
    fun notify(m: Mood, on: Boolean) = viewModelScope.launch { store.setNotify(m, on) }
    fun appearance(a: Appearance) = viewModelScope.launch { store.setAppearance(a) }
    fun logout() = repo.logout()
}

/** Read live from the OS each time the screen resumes; what support asks for first. */
data class PhoneInfo(
    val model: String,
    val android: String,
    val bluetooth: String,
    val notifications: Boolean,
    val exempt: Boolean,
) {
    companion object {
        fun read(ctx: Context) = PhoneInfo(
            model = "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}",
            android = "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})",
            bluetooth = ctx.getSystemService(BluetoothManager::class.java)?.adapter?.let { if (it.isEnabled) "On" else "Off" }
                ?: "Not available",
            notifications = NotificationManagerCompat.from(ctx).areNotificationsEnabled(),
            exempt = BatteryExemption.isExempt(ctx),
        )
    }
}

@Composable
fun AppSettingsScreen(vm: AppSettingsVm, onBack: () -> Unit) {
    val ctx = LocalContext.current
    val session by vm.session.collectAsStateWithLifecycle()
    val server by vm.server.collectAsStateWithLifecycle()
    val local by vm.local.collectAsStateWithLifecycle(initialValue = null)
    var phone by remember { mutableStateOf(PhoneInfo.read(ctx)) }
    LifecycleResumeEffect(Unit) {
        phone = PhoneInfo.read(ctx)
        onPauseOrDispose {}
    }

    SurfaceStatusBar()
    Scaffold(topBar = { BackBar("Settings", onBack) }) { pad ->
        LazyColumn(Modifier.fillMaxSize().padding(bottom = pad.calculateBottomPadding()), contentPadding = PaddingValues(top = pad.calculateTopPadding(), bottom = Space.xxl)) {
            item { SectionTitle("Appearance") }
            item {
                Group {
                    SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(Space.l)) {
                        Appearance.entries.forEachIndexed { i, a ->
                            SegmentedButton(
                                selected = (local?.appearance ?: Appearance.SYSTEM) == a,
                                onClick = { vm.appearance(a) },
                                shape = SegmentedButtonDefaults.itemShape(i, Appearance.entries.size),
                                label = { Text(when (a) { Appearance.SYSTEM -> "Phone"; Appearance.LIGHT -> "Light"; Appearance.DARK -> "Dark" }) },
                            )
                        }
                    }
                }
            }

            item { SectionTitle("Notifications") }
            item {
                Group {
                    Mood.entries.forEachIndexed { i, m ->
                        val label = when (m) {
                            Mood.HEART -> "Heart"
                            Mood.SORRY -> "Sorry"
                            Mood.ANNOY -> "Annoy"
                        }
                        if (i > 0) Line()
                        SwitchRow(label, if (m == Mood.HEART) "When someone in your circle sends one" else null, local?.notify?.get(m) ?: true) {
                            vm.notify(m, it)
                        }
                    }
                }
            }

            item { SectionTitle("This phone") }
            item {
              Group {
                FactRow("Model", phone.model)
                Line()
                FactRow("Android", phone.android)
                Line()
                FactRow("App version", BuildConfig.VERSION_NAME)
                Line()
                FactRow("Bluetooth", phone.bluetooth, attention = phone.bluetooth != "On")
                Line()
                FactRow(
                    "Notifications",
                    if (phone.notifications) "Allowed" else "Off",
                    attention = !phone.notifications,
                    onClick = {
                        ctx.startActivity(
                            Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(Settings.EXTRA_APP_PACKAGE, ctx.packageName),
                        )
                    },
                )
                Line()
                FactRow(
                    "Battery optimisation",
                    if (phone.exempt) "Not restricted" else "Restricted",
                    attention = !phone.exempt,
                    onClick = if (phone.exempt) null else ({ ctx.startActivity(BatteryExemption.requestIntent(ctx)) }),
                )
                Line()
                val text = when (server) {
                    ServerLink.CONNECTED -> "Connected"
                    ServerLink.CONNECTING -> "Connecting"
                    ServerLink.OFFLINE -> "Offline"
                }
                FactRow("Server link", text, attention = server == ServerLink.OFFLINE)
              }
            }
            if (!phone.exempt) {
                item {
                    Text(
                        "With battery optimisation on, Android may pause the link while the screen is off. Tap the row to allow background use.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(start = Space.screen + Space.s, end = Space.screen + Space.s, top = Space.m),
                    )
                }
            }

            item { SectionTitle("Account") }
            item {
                session?.let { Group { ItemRow(it.user, "Signed in on ${it.host}") } }
            }
            item {
                OutlinedButton(
                    onClick = vm::logout,
                    modifier = Modifier.padding(horizontal = Space.screen, vertical = Space.l).fillMaxWidth().height(52.dp),
                ) { Text("Log out") }
            }
        }
    }
}
