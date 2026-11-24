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
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.runtime.toMutableStateList
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.heartbeacon.app.data.ApiError
import com.heartbeacon.app.data.FakeRepo
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import com.heartbeacon.app.data.RealRepo
import com.heartbeacon.app.data.ServerLink
import com.heartbeacon.app.link.BeaconService
import com.heartbeacon.app.link.Cdm
import com.heartbeacon.app.ui.AddDeviceScreen
import com.heartbeacon.app.ui.AddDeviceVm
import com.heartbeacon.app.ui.AppSettingsScreen
import com.heartbeacon.app.ui.AppSettingsVm
import com.heartbeacon.app.ui.HeartTheme
import com.heartbeacon.app.ui.HomeScreen
import com.heartbeacon.app.ui.HomeVm
import com.heartbeacon.app.ui.LoginScreen
import com.heartbeacon.app.ui.LoginVm
import kotlinx.coroutines.launch

enum class Route { HOME, ADD, SETTINGS }

private class Entry(val route: Route, val n: Int)

private val StackSaver = listSaver<SnapshotStateList<Entry>, Int>(
    save = { s -> s.flatMap { listOf(it.route.ordinal, it.n) } },
    restore = { l -> l.chunked(2).map { Entry(Route.entries[it[0]], it[1]) }.toMutableStateList() },
)

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
        // Debug-only switch to FakeRepo for screenshots: --es screen login|home|add|settings --ei step N
        // --es status near|away|asleep|offline --es server offline|connecting --es fail 401|403|409|429|net|tls
        // --es fx heart|sorry|annoy (a mood from BulBul arrives 2.5 s after start)
        if (BuildConfig.DEBUG && savedInstanceState == null && intent.hasExtra("screen")) {
            val screen = intent.getStringExtra("screen")
            start = Route.entries.firstOrNull { it.name.equals(screen, ignoreCase = true) } ?: Route.HOME
            step = intent.getIntExtra("step", 0)
            val presence = when (intent.getStringExtra("status")) {
                "away" -> Presence.AwayWifi
                "asleep" -> Presence.Asleep(
                    java.time.ZonedDateTime.now().plusDays(1).withHour(9).withMinute(0).withSecond(0).toInstant().toEpochMilli(),
                )
                "offline" -> Presence.Offline
                else -> Presence.Near
            }
            val link = ServerLink.entries.firstOrNull { it.name.equals(intent.getStringExtra("server"), ignoreCase = true) }
            val fail = when (intent.getStringExtra("fail")) {
                "401" -> ApiError(401, "bad_login")
                "403" -> ApiError(403, "not_assigned")
                "409" -> ApiError(409, "claimed")
                "429" -> ApiError(429, "slow_down", 42)
                "net" -> java.net.ConnectException("refused")
                "tls" -> javax.net.ssl.SSLHandshakeException("not our CA")
                else -> null
            }
            (app.repo as? RealRepo)?.close()
            val fake = FakeRepo(app.scope, presence, loggedIn = screen != "login", link = link ?: ServerLink.CONNECTED, fail = fail)
            app.repo = fake
            Mood.entries.firstOrNull { it.wire == intent.getStringExtra("fx") }?.let { m ->
                app.scope.launch {
                    kotlinx.coroutines.delay(2_500)
                    fake.incoming(m)
                }
            }
        }
        BeaconService.start(this)
        setContent { HeartTheme { AppRoot(app, start, step, ::findBeacon) } }
    }

    // uiMode is handled here (Appearance setting, system dark mode): Compose re-themes itself; only the system bar
    // scrims are chosen once, so pick them again.
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        enableEdgeToEdge()
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
    // Saved, so a resize or a process restart keeps the screen you were on (and each Add keeps its own view model).
    var next by rememberSaveable { mutableIntStateOf(1) }
    val stack = rememberSaveable(saver = StackSaver) {
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
                onAdd = { push(Route.ADD) },
                onSettings = { push(Route.SETTINGS) },
            )
            Route.ADD -> {
                val vm = viewModel(key = "add-${e.n}") { AddDeviceVm(repo, if (e.n == 1) addStep else 0) }
                AddDeviceScreen(vm, onFind = { find(vm) }, onBack = ::pop, onDone = ::home)
            }
            Route.SETTINGS -> AppSettingsScreen(viewModel { AppSettingsVm(repo, app.settings) }, onBack = ::pop)
        }
    }
}
