package com.heartbeacon.app.ui.heart

import android.os.SystemClock
import android.provider.Settings
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.exponentialDecay
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.platform.ViewConfiguration
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.heartbeacon.app.data.Beacon
import com.heartbeacon.app.data.Event
import com.heartbeacon.app.data.Mark
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import com.heartbeacon.app.data.ServerLink
import com.heartbeacon.app.ui.Format
import com.heartbeacon.app.ui.Tabular
import com.heartbeacon.core.Fx
import com.heartbeacon.core.Taps
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlin.math.hypot

/** The stage is a fixed dark vitrine in both themes: the object and its light are the colour. */
object Stage {
    val ground = Color(0xFF110E12)
    val groundDeep = Color(0xFF0A080B)
    val text = Color(0xFFF3ECEF)
    val text2 = Color(0xFFBDB1B7)
    val label = Color(0xFF9D9197)
    val pipOff = Color(0xFF3A3339)
    val warn = Color(0xFFF2B8B5)
}

private const val REST_YAW = -0.24f
private const val REST_PITCH = 0.07f

/**
 * Your beacon's twin: the brass cage turning under your finger, its LEDs playing what the desk beacon plays, a
 * glanceable dashboard over its left lobe, and the last event at its foot. Tap like the button: once Heart, twice
 * Sorry, three times Annoy; hold for the battery bar.
 */
@Composable
fun HeartStage(
    beacon: Beacon?,
    server: ServerLink,
    events: List<Event>,
    bright: Int,
    fmt: Format,
    onSend: (Mood) -> Unit,
    onAdd: () -> Unit,
    modifier: Modifier = Modifier,
    top: @Composable () -> Unit,
) {
    val renderer = remember { CageRenderer() }
    val twin = remember { Twin() }
    val taps = remember { Taps() }
    val scope = rememberCoroutineScope()
    val haptics = LocalHapticFeedback.current
    val ctx = LocalContext.current
    val still = remember { Settings.Global.getFloat(ctx.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f }

    val yaw = remember { Animatable(if (still) REST_YAW else REST_YAW - 1.0f) }
    val pitch = remember { Animatable(REST_PITCH) }
    val squash = remember { Animatable(1f) }
    val ripple = remember { Animatable(0f) }
    var rippleMood by remember { mutableStateOf(Mood.HEART) }
    var clock by remember { mutableLongStateOf(SystemClock.uptimeMillis()) }
    var effectUntil by remember { mutableLongStateOf(0L) }
    var tapCount by remember { mutableIntStateOf(0) }
    var tapSerial by remember { mutableIntStateOf(0) }
    var sentFlash by remember { mutableStateOf<Mood?>(null) }
    var lastTouch by remember { mutableLongStateOf(0L) }

    val present = beacon != null
    val awake = beacon?.presence == Presence.Near || beacon?.presence == Presence.AwayWifi
    val look = when {
        beacon == null -> Look.OFFLINE
        awake -> Look.AWAKE
        beacon.presence is Presence.Asleep -> Look.ASLEEP
        else -> Look.OFFLINE
    }

    fun play(now: Long) {
        twin.playing?.let { effectUntil = it.startMs + (Fx.durationMs(it.kind, it.arg) ?: 0L) }
        clock = now
    }

    // Intro: a half turn into the resting three-quarter view, once.
    LaunchedEffect(Unit) {
        if (!still) yaw.animateTo(REST_YAW, tween(1400, easing = FastOutSlowInEasing))
    }

    // Live moods reach the twin as they reach the beacon; history fetched later never replays.
    val seen = remember { HashSet<String>() }
    var primed by remember { mutableStateOf(false) }
    LaunchedEffect(events) {
        val now = System.currentTimeMillis()
        for (e in events) {
            if (!seen.add(e.id) || !primed) continue
            if (e.from != null && now - e.atEpochMs < 30_000) {
                twin.mood(e.mood, awake, SystemClock.uptimeMillis())
                play(SystemClock.uptimeMillis())
            }
        }
        primed = true
    }
    // The beacon came up: the router flushes what it held, the twin plays it.
    LaunchedEffect(awake) {
        if (awake && twin.waiting > 0) {
            twin.woke(SystemClock.uptimeMillis())
            play(SystemClock.uptimeMillis())
        }
    }

    // Frame clock only while an LED effect plays; pose, squash and ripple animate through their own state.
    LaunchedEffect(Unit) {
        while (true) {
            snapshotFlow { clock < effectUntil }.first { it }
            while (clock < effectUntil) {
                androidx.compose.runtime.withFrameMillis { clock = SystemClock.uptimeMillis() }
            }
        }
    }

    // Multi-tap: commit the count once the button's gap passes with no new tap.
    LaunchedEffect(tapSerial) {
        if (tapSerial == 0) return@LaunchedEffect
        delay(Taps.GAP_MS)
        val n = taps.due(SystemClock.uptimeMillis()) ?: return@LaunchedEffect
        val mood = when (n) { 1 -> Mood.HEART; 2 -> Mood.SORRY; else -> Mood.ANNOY }
        onSend(mood)
        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
        tapCount = 0
        sentFlash = mood
        rippleMood = mood
        if (!still) {
            ripple.snapTo(0f)
            ripple.animateTo(1f, tween(900, easing = LinearEasing))
        }
        delay(1400)
        if (sentFlash == mood) sentFlash = null
    }

    // Back to the resting pose a while after the last touch.
    LaunchedEffect(lastTouch) {
        if (lastTouch == 0L) return@LaunchedEffect
        delay(3500)
        launch { pitch.animateTo(REST_PITCH, spring(stiffness = 40f, dampingRatio = 0.9f)) }
        val target = REST_YAW + Math.round((yaw.value - REST_YAW) / (2 * Math.PI)).toFloat() * (2 * Math.PI).toFloat()
        yaw.animateTo(target, spring(stiffness = 40f, dampingRatio = 0.9f))
    }

    val rgb = remember { IntArray(27) }
    val stateText = beacon?.let { describe(it, fmt) } ?: "No beacon yet"

    Box(
        modifier
            .background(Brush.verticalGradient(listOf(Stage.ground, Stage.groundDeep)))
            .semantics {
                contentDescription = "Your beacon, ${stateText.lowercase()}"
                customActions = listOf(
                    CustomAccessibilityAction("Send a Heart") { onSend(Mood.HEART); true },
                    CustomAccessibilityAction("Send a Sorry") { onSend(Mood.SORRY); true },
                    CustomAccessibilityAction("Send an Annoy") { onSend(Mood.ANNOY); true },
                )
            },
    ) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val wPx = constraints.maxWidth.toFloat()
            val hPx = constraints.maxHeight.toFloat()
            val halfWidth = minOf(wPx * 0.355f, hPx * 0.28f)
            val center = Offset(wPx * 0.585f, hPx * 0.47f)
            fun inside(p: Offset) = hypot(p.x - center.x, p.y - center.y) < halfWidth * 1.15f

            val baseConfig = LocalViewConfiguration.current
            val holdConfig = remember(baseConfig) {
                object : ViewConfiguration by baseConfig {
                    override val longPressTimeoutMillis: Long get() = Taps.HOLD_MS
                }
            }
            CompositionLocalProvider(LocalViewConfiguration provides holdConfig) {
                Canvas(
                    Modifier
                        .fillMaxSize()
                        .pointerInput(Unit) {
                            val tracker = VelocityTracker()
                            detectDragGestures(
                                onDragStart = { tracker.resetTracking(); lastTouch = 0L },
                                onDragEnd = {
                                    val v = tracker.calculateVelocity()
                                    scope.launch { yaw.animateDecay(v.x * 0.0075f, exponentialDecay(1.6f)) }
                                    scope.launch {
                                        pitch.animateDecay(v.y * 0.0045f, exponentialDecay(2.5f))
                                    }
                                    lastTouch = SystemClock.uptimeMillis()
                                },
                            ) { change, drag ->
                                change.consume()
                                tracker.addPosition(change.uptimeMillis, change.position)
                                scope.launch {
                                    yaw.snapTo(yaw.value + drag.x * 0.0075f)
                                    pitch.snapTo((pitch.value + drag.y * 0.0045f).coerceIn(-0.65f, 0.65f))
                                }
                            }
                        }
                        .pointerInput(present) {
                            detectTapGestures(
                                onPress = { p ->
                                    if (!inside(p)) return@detectTapGestures
                                    scope.launch { squash.animateTo(0.955f, tween(70)) }
                                    tryAwaitRelease()
                                    scope.launch { squash.animateTo(1f, spring(dampingRatio = 0.35f, stiffness = 700f)) }
                                },
                                onTap = { p ->
                                    if (!inside(p)) return@detectTapGestures
                                    haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                    tapCount = taps.tap(SystemClock.uptimeMillis())
                                    sentFlash = null
                                    tapSerial++
                                },
                                onLongPress = { p ->
                                    val pct = beacon?.batteryPct
                                    if (!inside(p) || pct == null) return@detectTapGestures
                                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                                    twin.battery(pct, SystemClock.uptimeMillis())
                                    play(SystemClock.uptimeMillis())
                                },
                            )
                        },
                ) {
                    val now = clock
                    twin.frame(now, 100, rgb)
                    // The desk LEDs dim with the brightness setting; a screen needs a floor to stay readable.
                    val k = 0.62f + 0.38f * bright.coerceIn(5, 100) / 100f
                    for (i in rgb.indices) rgb[i] = (rgb[i] * k).toInt()
                    if (ripple.value in 0.001f..0.999f) {
                        val t = ripple.value
                        val c = moodColor(rippleMood)
                        drawCircle(
                            c.copy(alpha = (1f - t) * 0.55f),
                            radius = halfWidth * (1.05f + 0.75f * t),
                            center = center,
                            style = Stroke(width = (1f - t) * 5.dp.toPx() + 1f),
                        )
                    }
                    with(renderer) { draw(rgb, yaw.value, pitch.value, squash.value, look, center, halfWidth) }
                }
            }
        }

        top()

        if (beacon != null) {
            Dashboard(
                beacon, server, fmt,
                Modifier
                    .align(Alignment.CenterStart)
                    .padding(bottom = 28.dp)
                    .fillMaxHeight(0.62f),
            )
        }

        Foot(
            beacon = beacon,
            events = events,
            waiting = twin.waiting,
            tapCount = tapCount,
            sent = sentFlash,
            fmt = fmt,
            onAdd = onAdd,
            modifier = Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(start = 20.dp, end = 20.dp, bottom = 22.dp),
        )
    }
}

fun moodColor(m: Mood): Color = when (m) {
    Mood.HEART -> Color(250, 40, 52)
    Mood.SORRY -> Color(235, 161, 23)
    Mood.ANNOY -> Color(23, 231, 235)
}

private fun describe(b: Beacon, fmt: Format): String = when (val p = b.presence) {
    Presence.Near, Presence.AwayWifi -> "Awake"
    is Presence.Asleep -> fmt.presence(p, System.currentTimeMillis())
    Presence.Offline -> "Offline"
}

/** Battery, network and state, readable over the cage's left lobe. */
@Composable
private fun Dashboard(b: Beacon, server: ServerLink, fmt: Format, modifier: Modifier) {
    val now = System.currentTimeMillis()
    Box(
        modifier
            .width(168.dp)
            .background(Brush.horizontalGradient(0f to Stage.ground.copy(alpha = 0.96f), 0.7f to Stage.ground.copy(alpha = 0.78f), 1f to Color.Transparent)),
    ) {
        Column(
            Modifier.padding(start = 20.dp, end = 8.dp).align(Alignment.CenterStart),
            verticalArrangement = Arrangement.spacedBy(18.dp),
        ) {
            Readout("Battery") {
                val pct = b.batteryPct
                if (pct == null) {
                    Value("Unknown", Stage.text2)
                } else {
                    Pips(pct)
                    Spacer(Modifier.height(6.dp))
                    Value("$pct%", if (pct <= 20) Stage.warn else Stage.text)
                }
            }
            Readout("Network") {
                when (b.presence) {
                    Presence.Near -> {
                        Value("Near", Stage.text)
                        Sub("Bluetooth, via phone")
                    }
                    Presence.Offline -> {
                        Value("Offline", Stage.warn)
                        b.lastSeenEpochMs?.let { Sub("Seen ${fmt.ago(it, now)}") }
                    }
                    else -> {
                        Row(verticalAlignment = Alignment.Bottom) {
                            Value("Wi-Fi", Stage.text)
                            b.rssi?.let {
                                Spacer(Modifier.width(8.dp))
                                Bars(it)
                            }
                        }
                        b.rssi?.let { Sub("$it dBm") }
                    }
                }
            }
            Readout("Beacon") {
                when (val p = b.presence) {
                    Presence.Near, Presence.AwayWifi -> Value("Awake", Stage.text)
                    is Presence.Asleep -> {
                        Value("Asleep", Stage.text)
                        p.untilEpochMs?.let { Sub("Wakes ${fmt.clock(it)}") }
                    }
                    Presence.Offline -> Value("Unreachable", Stage.text2)
                }
            }
            if (server != ServerLink.CONNECTED) {
                Readout("This phone") {
                    Value(if (server == ServerLink.OFFLINE) "No connection" else "Connecting", Stage.text2)
                    Sub("Moods wait and go out after")
                }
            }
        }
    }
}

@Composable
private fun Readout(label: String, body: @Composable () -> Unit) {
    Column {
        Text(label, style = MaterialTheme.typography.labelMedium, color = Stage.label)
        Spacer(Modifier.height(4.dp))
        body()
    }
}

@Composable
private fun Value(text: String, color: Color) =
    Text(text, style = MaterialTheme.typography.titleMedium.merge(Tabular), color = color, maxLines = 1)

@Composable
private fun Sub(text: String) =
    Text(text, style = MaterialTheme.typography.bodySmall.merge(Tabular), color = Stage.text2, maxLines = 2, overflow = TextOverflow.Ellipsis)

/** The battery as the beacon shows it on a hold: 9 pixels, red through yellow to green, floor(pct * 9 / 100) lit. */
@Composable
private fun Pips(pct: Int) {
    val lit = Fx.barPixels(pct)
    val out = remember { IntArray(27) }
    Fx.frame(Fx.Kind.BATT, Fx.PIXELS, Fx.PIXELS * Fx.BAR_STEP_MS.toLong(), 100, out)
    Row(horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        for (i in 0 until Fx.PIXELS) {
            val c = if (i < lit) Color(out[3 * i], out[3 * i + 1], out[3 * i + 2]) else Stage.pipOff
            Box(Modifier.size(width = 7.dp, height = 12.dp).clip(RoundedCornerShape(2.dp)).background(c))
        }
    }
}

/** Wi-Fi strength as four rising bars (>= -55, -65, -75, else one bar). */
@Composable
private fun Bars(rssi: Int) {
    val n = when { rssi >= -55 -> 4; rssi >= -65 -> 3; rssi >= -75 -> 2; else -> 1 }
    Row(horizontalArrangement = Arrangement.spacedBy(2.dp), verticalAlignment = Alignment.Bottom, modifier = Modifier.padding(bottom = 4.dp)) {
        for (i in 0 until 4) {
            Box(
                Modifier.size(width = 4.dp, height = (5 + 3 * i).dp).clip(RoundedCornerShape(1.dp))
                    .background(if (i < n) Stage.text else Stage.pipOff),
            )
        }
    }
}

/** The stage's foot: what the tap is about to send, or the last thing that happened; the gesture key under it. */
@Composable
private fun Foot(
    beacon: Beacon?,
    events: List<Event>,
    waiting: Int,
    tapCount: Int,
    sent: Mood?,
    fmt: Format,
    onAdd: () -> Unit,
    modifier: Modifier,
) {
    val now = System.currentTimeMillis()
    Column(modifier.semantics { liveRegion = LiveRegionMode.Polite }, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        val line: Pair<String, Color?> = when {
            tapCount > 0 -> when (tapCount) { 1 -> "Heart"; 2 -> "Sorry"; else -> "Annoy" }.let { "$it…" to moodColor(moodOf(tapCount)) }
            sent != null -> "Sent a ${label(sent)}" to moodColor(sent)
            beacon == null -> "No beacon yet. A tap still sends a Heart." to null
            waiting > 0 -> "$waiting waiting. They play when it wakes." to null
            else -> events.firstOrNull()?.let { e ->
                val mark = e.mark?.let { " · ${fmt.mark(it)}" }.orEmpty()
                "${fmt.event(e.mood, e.from)} · ${fmt.ago(e.atEpochMs, now)}$mark" to (if (e.mark == Mark.SHOWN || e.from != null) moodColor(e.mood) else null)
            } ?: ("Nothing yet" to null)
        }
        AnimatedContent(line, transitionSpec = { fadeIn(tween(160)) togetherWith fadeOut(tween(120)) }, label = "foot") { (text, dot) ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (dot != null) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
                    Spacer(Modifier.width(8.dp))
                }
                Text(text, style = MaterialTheme.typography.titleSmall, color = Stage.text, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (beacon == null) {
            FilledTonalButton(onClick = onAdd, modifier = Modifier.padding(top = 8.dp)) { Text("Add beacon") }
        } else {
            Text(
                "Tap for a Heart, twice for Sorry, three times for Annoy. Hold for battery.",
                style = MaterialTheme.typography.bodySmall,
                color = Stage.text2,
            )
        }
    }
}

private fun moodOf(n: Int) = when (n) { 1 -> Mood.HEART; 2 -> Mood.SORRY; else -> Mood.ANNOY }

private fun label(m: Mood) = when (m) { Mood.HEART -> "Heart"; Mood.SORRY -> "Sorry"; Mood.ANNOY -> "Annoy" }
