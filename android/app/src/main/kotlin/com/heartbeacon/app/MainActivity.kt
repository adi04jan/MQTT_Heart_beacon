package com.heartbeacon.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.heartbeacon.app.data.FakeRepo
import com.heartbeacon.app.data.Presence
import com.heartbeacon.app.link.BeaconService
import com.heartbeacon.app.link.Cdm
import com.heartbeacon.app.ui.AddDeviceScreen
import com.heartbeacon.app.ui.AddDeviceVm
import com.heartbeacon.app.ui.AppSettingsScreen
import com.heartbeacon.app.ui.AppSettingsVm
import com.heartbeacon.app.ui.DeviceSettingsScreen
import com.heartbeacon.app.ui.DeviceSettingsVm
import com.heartbeacon.app.ui.HeartTheme
import com.heartbeacon.app.ui.HomeScreen
import com.heartbeacon.app.ui.HomeVm
import com.heartbeacon.app.ui.LoginScreen
import com.heartbeacon.app.ui.LoginVm

enum class Route { HOME, ADD, DEVICE, SETTINGS }

private class Entry(val route: Route, val n: Int)

class MainActivity : ComponentActivity() {
    private var pairing: AddDeviceVm? = null

    private val chooser = registerForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) {
        // CDM reports the outcome through its callback (Cdm.associate); nothing to read here.
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val app = application as App
        var start = Route.HOME
        var step = 0
        // Debug-only entry points for screenshots: --es screen home|add|device|settings --ei step N --es status ...
        if (BuildConfig.DEBUG && savedInstanceState == null && intent.hasExtra("screen")) {
            val screen = intent.getStringExtra("screen")
            start = Route.entries.firstOrNull { it.name.equals(screen, ignoreCase = true) } ?: Route.HOME
            step = intent.getIntExtra("step", 0)
            val presence = when (intent.getStringExtra("status")) {
                "away" -> Presence.AwayWifi
                "asleep" -> Presence.Asleep(System.currentTimeMillis() + 15 * 3_600_000L)
                "offline" -> Presence.Offline
                else -> Presence.Near
            }
            app.repo = FakeRepo(app.scope, presence, loggedIn = screen != "login")
        }
        BeaconService.start(this)
        setContent { HeartTheme { AppRoot(app, start, step, ::findBeacon) } }
    }

    private fun findBeacon(vm: AddDeviceVm) {
        val app = application as App
        if (app.repo is FakeRepo) return vm.paired(null)
        pairing = vm
        Cdm.associate(
            this,
            launch = { chooser.launch(IntentSenderRequest.Builder(it).build()) },
            onMac = { mac -> pairing?.paired(mac) },
            onFail = { pairing?.paired(null) },
        )
    }
}

@Composable
private fun AppRoot(app: App, start: Route, addStep: Int, find: (AddDeviceVm) -> Unit) {
    val repo = app.repo
    val session by repo.session.collectAsStateWithLifecycle()
    if (session == null) {
        LoginScreen(viewModel { LoginVm(repo) })
        return
    }
    var next by remember { mutableIntStateOf(1) }
    val stack = remember {
        mutableStateListOf(Entry(Route.HOME, 0)).apply { if (start != Route.HOME) add(Entry(start, 1)) }
    }
    fun push(r: Route) = stack.add(Entry(r, ++next))
    fun pop() {
        if (stack.size > 1) stack.removeAt(stack.lastIndex)
    }
    fun home() {
        stack.clear()
        stack.add(Entry(Route.HOME, ++next))
    }
    BackHandler(stack.size > 1) { pop() }

    AnimatedContent(
        targetState = stack.last(),
        transitionSpec = { fadeIn(tween(220, delayMillis = 90)) togetherWith fadeOut(tween(90)) },
        label = "nav",
    ) { e ->
        when (e.route) {
            Route.HOME -> HomeScreen(
                viewModel { HomeVm(repo) },
                onBeacon = { push(Route.DEVICE) },
                onAdd = { push(Route.ADD) },
                onSettings = { push(Route.SETTINGS) },
            )
            Route.ADD -> {
                val vm = viewModel(key = "add-${e.n}") { AddDeviceVm(repo, if (e.n == 1) addStep else 0) }
                AddDeviceScreen(vm, onFind = { find(vm) }, onBack = ::pop, onDone = ::home)
            }
            Route.DEVICE -> DeviceSettingsScreen(viewModel { DeviceSettingsVm(repo) }, onBack = ::pop, onRemoved = ::home)
            Route.SETTINGS -> AppSettingsScreen(viewModel { AppSettingsVm(repo, app.settings) }, onBack = ::pop)
        }
    }
}
