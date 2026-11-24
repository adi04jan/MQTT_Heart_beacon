@file:OptIn(ExperimentalLayoutApi::class)

package com.heartbeacon.app.ui.heart

import android.graphics.RuntimeShader
import android.os.SystemClock
import android.provider.Settings
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
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
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
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
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ShaderBrush
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.platform.ViewConfiguration
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.heartbeacon.app.data.Beacon
import com.heartbeacon.app.data.Event
import com.heartbeacon.app.data.Mark
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import com.heartbeacon.app.data.ServerLink
import com.heartbeacon.app.ui.Format
import com.heartbeacon.app.ui.Jost
import com.heartbeacon.app.ui.Tabular
import com.heartbeacon.app.ui.moodColor
import com.heartbeacon.core.Fx
import com.heartbeacon.core.Taps
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlin.math.hypot

/** The stage is the grey cloth the beacon sits on, lit from above, in both themes: the object and its light are the colour. */
object Stage {
    val ground = Color(0xFF101112)
    val text = Color(0xFFF2EEE6)
    val text2 = Color(0xFFC2BBAF)
    val label = Color(0xFFA29B8F)
    val pipOff = Color(0xFF3C3934)
    val warn = Color(0xFFF2B8B5)
    val brass = Color(0xFFE2BC6A)
    val onBrass = Color(0xFF2E2100)
}

private const val REST_YAW = -0.26f
private const val REST_PITCH = 0.06f

// Felt under a soft overhead lamp: a pool of light where the beacon stands, the cloth's grain, dark beyond.
private const val FELT = """
uniform float2 spot;
uniform float reach;
half4 main(float2 p) {
    float2 d = (p - spot) / reach;
    float pool = exp(-dot(d, d) * 1.45);
    float n = fract(sin(dot(floor(p / 2.0), float2(12.9898, 78.233))) * 43758.5453);
    float m = fract(sin(dot(floor(p / float2(9.0, 3.0)), float2(39.346, 11.135))) * 24634.6345);
    float v = mix(0.062, 0.235, pool);
    v += (n - 0.5) * 0.050 * (0.35 + pool) + (m - 0.5) * 0.026 * (0.2 + pool);
    return half4(v * 0.975, v * 0.99, v * 1.03, 1.0);
}
"""

/** The cloth ground, its light pooled at ([x], [y]) as fractions of the size, reaching [reach] widths out. */
fun Modifier.felt(x: Float, y: Float, reach: Float = 0.78f): Modifier = drawWithCache {
    val shader = RuntimeShader(FELT)
    shader.setFloatUniform("spot", size.width * x, size.height * y)
    shader.setFloatUniform("reach", size.width * reach)
    val brush = ShaderBrush(shader)
    onDrawBehind { drawRect(brush) }
}

@Composable
private fun animationsOff(): Boolean {
    val ctx = LocalContext.current
    return remember { Settings.Global.getFloat(ctx.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f }
}

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
    val still = animationsOff()

    val yaw = remember { Animatable(if (still) REST_YAW else REST_YAW - 1.1f) }
    val pitch = remember { Animatable(REST_PITCH) }
    val squash = remember { Animatable(1f) }
    val wave = remember { Animatable(0f) }
    var waveMood by remember { mutableStateOf(Mood.HEART) }
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

    // Intro: a turn into the resting three-quarter view, once.
    LaunchedEffect(Unit) {
        if (!still) yaw.animateTo(REST_YAW, tween(1500, easing = FastOutSlowInEasing))
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

    // Frame clock only while an LED effect plays; pose, squash and wave animate through their own state.
    LaunchedEffect(Unit) {
        while (true) {
            snapshotFlow { clock < effectUntil }.first { it }
            while (clock < effectUntil) {
                withFrameMillis { clock = SystemClock.uptimeMillis() }
            }
        }
    }

    // Multi-tap: commit the count once the button's gap passes with no new tap.
    LaunchedEffect(tapSerial) {
        if (tapSerial == 0) return@LaunchedEffect
        delay(Taps.GAP_MS)
        val n = taps.due(SystemClock.uptimeMillis()) ?: return@LaunchedEffect
        val mood = moodOf(n)
        onSend(mood)
        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
        tapCount = 0
        sentFlash = mood
        waveMood = mood
        if (!still) {
            wave.snapTo(0f)
            wave.animateTo(1f, tween(1300, easing = LinearEasing))
        }
        delay(900)
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
            .background(Stage.ground)
            .felt(0.60f, 0.50f)
            // Top and foot stay dark enough for the name and the last event to read on the lit cloth.
            .drawBehind {
                drawRect(Brush.verticalGradient(0f to Color(0f, 0f, 0f, 0.5f), 0.2f to Color.Transparent, 0.68f to Color.Transparent, 1f to Color(0f, 0f, 0f, 0.62f)))
            }
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
            val halfWidth = minOf(wPx * 0.35f, hPx * 0.27f)
            val center = Offset(wPx * 0.60f, hPx * 0.455f)
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
                                    scope.launch { squash.animateTo(0.95f, tween(70)) }
                                    tryAwaitRelease()
                                    scope.launch { squash.animateTo(1f, spring(dampingRatio = 0.38f, stiffness = 650f)) }
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
                    with(renderer) {
                        if (wave.value in 0.001f..0.999f) wave(wave.value, moodColor(waveMood), yaw.value, pitch.value, center, halfWidth)
                        draw(rgb, yaw.value, pitch.value, squash.value, look, center, halfWidth)
                    }
                }
            }
        }

        top()

        if (beacon != null) {
            Dashboard(beacon, fmt, Modifier.align(Alignment.CenterStart).padding(top = 92.dp, bottom = 24.dp))
        }

        Foot(
            beacon = beacon,
            server = server,
            events = events,
            waiting = twin.waiting,
            tapCount = tapCount,
            sent = sentFlash,
            fmt = fmt,
            onAdd = onAdd,
            modifier = Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(start = 20.dp, end = 20.dp, bottom = 24.dp),
        )
    }
}

/**
 * The beacon as a picture, for screens that are about it but not live: unlit at sign-in, playing [play] (the
 * firmware's own effect, such as the blue pairing pulse) while adding one. Not interactive. [fit] is the cage's
 * half width as a share of the height, [at] its centre down the height.
 */
@Composable
fun BeaconFigure(play: Fx.Kind?, modifier: Modifier = Modifier, fit: Float = 0.36f, at: Float = 0.46f) {
    val renderer = remember { CageRenderer() }
    val still = animationsOff()
    val yaw = remember { Animatable(if (still) REST_YAW else REST_YAW - 0.9f) }
    var clock by remember { mutableLongStateOf(Fx.PULSE_MS / 2L) }
    val rgb = remember { IntArray(27) }
    LaunchedEffect(Unit) {
        if (!still) yaw.animateTo(REST_YAW, tween(1500, easing = FastOutSlowInEasing))
    }
    LaunchedEffect(play) {
        if (play == null || still) return@LaunchedEffect
        val t0 = SystemClock.uptimeMillis()
        while (true) withFrameMillis { clock = SystemClock.uptimeMillis() - t0 }
    }
    Canvas(modifier.background(Stage.ground).felt(0.5f, at + 0.04f, 0.7f).clearAndSetSemantics {}) {
        if (play != null) Fx.frame(play, 0, clock, 100, rgb) else rgb.fill(0)
        val hw = minOf(size.width * 0.34f, size.height * fit)
        with(renderer) { draw(rgb, yaw.value, REST_PITCH, 1f, Look.AWAKE, Offset(size.width / 2, size.height * at), hw) }
    }
}

private fun describe(b: Beacon, fmt: Format): String = when (val p = b.presence) {
    Presence.Near, Presence.AwayWifi -> "Awake"
    is Presence.Asleep -> fmt.presence(p, System.currentTimeMillis())
    Presence.Offline -> "Offline"
}

private val Figure = TextStyle(fontFamily = Jost, fontWeight = FontWeight.Light, fontSize = 40.sp, lineHeight = 44.sp, fontFeatureSettings = "tnum")
private val Reading = TextStyle(fontFamily = Jost, fontWeight = FontWeight.Normal, fontSize = 23.sp, lineHeight = 28.sp)

/** Battery, network and state, readable over the cage's left lobe. */
@Composable
private fun Dashboard(b: Beacon, fmt: Format, modifier: Modifier) {
    val now = System.currentTimeMillis()
    val offline = b.presence == Presence.Offline
    Column(
        modifier
            .width(164.dp)
            // A soft shadow on the cloth behind the figures, so they read where rods pass under them.
            .drawBehind {
                // An ellipse that fades out all round: no edge anywhere on the cloth.
                val rx = size.width * 1.3f
                val mid = Offset(0f, size.height / 2)
                scale(1f, size.height * 0.66f / rx, pivot = mid) {
                    drawCircle(
                        Brush.radialGradient(
                            0f to Color(0f, 0f, 0f, 0.74f), 0.5f to Color(0f, 0f, 0f, 0.5f), 1f to Color.Transparent,
                            center = mid, radius = rx,
                        ),
                        radius = rx, center = mid,
                    )
                }
            }
            .padding(start = 20.dp, end = 8.dp, top = 12.dp, bottom = 12.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        Readout("Battery") {
            val pct = b.batteryPct
            if (pct == null) {
                Text("Unknown", style = Reading, color = Stage.text2)
            } else {
                Row(Modifier.semantics(mergeDescendants = true) {}, verticalAlignment = Alignment.Bottom) {
                    Text("$pct", style = Figure, color = if (pct <= 20) Stage.warn else Stage.text)
                    Text("%", style = Reading, color = Stage.label, modifier = Modifier.padding(start = 2.dp, bottom = 5.dp))
                }
                Spacer(Modifier.height(4.dp))
                Pips(pct, stale = offline)
                if (offline) Sub("Last known")
            }
        }
        Readout("Network") {
            when (b.presence) {
                Presence.Near -> {
                    Text("Near", style = Reading, color = Stage.text)
                    Sub("Bluetooth, through this phone")
                }
                Presence.Offline -> {
                    Text("Offline", style = Reading, color = Stage.warn)
                    b.lastSeenEpochMs?.let { Sub("Seen ${fmt.ago(it, now)}") }
                }
                else -> {
                    Row(verticalAlignment = Alignment.Bottom) {
                        Text("Wi-Fi", style = Reading, color = Stage.text)
                        b.rssi?.let {
                            Spacer(Modifier.width(10.dp))
                            Bars(it)
                        }
                    }
                    b.rssi?.let { Sub("$it dBm") }
                }
            }
        }
        Readout("Beacon") {
            when (val p = b.presence) {
                Presence.Near, Presence.AwayWifi -> Text("Awake", style = Reading, color = Stage.text)
                is Presence.Asleep -> {
                    Text("Asleep", style = Reading, color = Stage.text)
                    p.untilEpochMs?.let { Sub("Wakes ${fmt.clock(it)}") }
                }
                Presence.Offline -> Text("Unreachable", style = Reading, color = Stage.text2)
            }
        }
    }
}

@Composable
private fun Readout(label: String, body: @Composable () -> Unit) {
    Column {
        Text(label, style = MaterialTheme.typography.labelMedium, color = Stage.label)
        body()
    }
}

@Composable
private fun Sub(text: String) =
    Text(text, style = MaterialTheme.typography.bodySmall.merge(Tabular), color = Stage.text2, maxLines = 2, overflow = TextOverflow.Ellipsis)

/** The battery as the beacon shows it on a hold: 9 pixels, red through yellow to green, floor(pct * 9 / 100) lit. */
@Composable
private fun Pips(pct: Int, stale: Boolean = false) {
    val lit = Fx.barPixels(pct)
    val out = remember { IntArray(27) }
    Fx.frame(Fx.Kind.BATT, Fx.PIXELS, Fx.PIXELS * Fx.BAR_STEP_MS.toLong(), 100, out)
    Row(Modifier.padding(bottom = 4.dp), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        for (i in 0 until Fx.PIXELS) {
            val c = if (i < lit) lerp(Color(out[3 * i], out[3 * i + 1], out[3 * i + 2]), Stage.pipOff, if (stale) 0.55f else 0f) else Stage.pipOff
            Box(Modifier.size(width = 8.dp, height = 13.dp).clip(RoundedCornerShape(2.5.dp)).background(c))
        }
    }
}

/** Wi-Fi strength as four rising bars (>= -55, -65, -75, else one bar). */
@Composable
private fun Bars(rssi: Int) {
    val n = when { rssi >= -55 -> 4; rssi >= -65 -> 3; rssi >= -75 -> 2; else -> 1 }
    Row(horizontalArrangement = Arrangement.spacedBy(2.5.dp), verticalAlignment = Alignment.Bottom, modifier = Modifier.padding(bottom = 6.dp)) {
        for (i in 0 until 4) {
            Box(
                Modifier.size(width = 4.dp, height = (6 + 3 * i).dp).clip(RoundedCornerShape(1.dp))
                    .background(if (i < n) Stage.text else Stage.pipOff),
            )
        }
    }
}

/** The stage's foot: what the tap is about to send, or the last thing that happened; the gesture key under it. */
@Composable
private fun Foot(
    beacon: Beacon?,
    server: ServerLink,
    events: List<Event>,
    waiting: Int,
    tapCount: Int,
    sent: Mood?,
    fmt: Format,
    onAdd: () -> Unit,
    modifier: Modifier,
) {
    val now = System.currentTimeMillis()
    Column(modifier, verticalArrangement = Arrangement.spacedBy(10.dp)) {
        val line: Pair<String, Color?> = when {
            tapCount > 0 -> "${label(moodOf(tapCount))}…" to moodColor(moodOf(tapCount))
            sent != null -> "${label(sent)} sent" to moodColor(sent)
            beacon == null -> "No beacon yet. A tap still sends a Heart." to null
            server == ServerLink.OFFLINE -> "This phone is offline. Moods wait and go out after." to null
            server == ServerLink.CONNECTING -> "Connecting. Moods wait and go out after." to null
            waiting > 0 -> "$waiting waiting. They play when it wakes." to null
            else -> events.firstOrNull()?.let { e ->
                val mark = e.mark?.let { " · ${fmt.mark(it)}" }.orEmpty()
                "${fmt.event(e.mood, e.from)} · ${fmt.ago(e.atEpochMs, now)}$mark" to (if (e.mark == Mark.SHOWN || e.from != null) moodColor(e.mood) else null)
            } ?: ("Nothing yet" to null)
        }
        AnimatedContent(
            line,
            transitionSpec = { fadeIn(tween(160)) togetherWith fadeOut(tween(120)) },
            label = "foot",
            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
        ) { (text, dot) ->
            // Full width, so a change of line never animates (and clips) the row's size.
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                if (dot != null) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
                    Spacer(Modifier.width(10.dp))
                }
                Text(text, style = MaterialTheme.typography.titleSmall, color = Stage.text, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        if (beacon == null) {
            Button(
                onClick = onAdd,
                colors = ButtonDefaults.buttonColors(containerColor = Stage.brass, contentColor = Stage.onBrass),
                modifier = Modifier.height(48.dp),
            ) { Text("Add beacon") }
        } else {
            GestureKey(tapCount)
        }
    }
}

/** The button's language, drawn: one, two, three taps and a hold. The one being tapped lights in its mood's colour. */
@Composable
private fun GestureKey(tapCount: Int) {
    FlowRow(
        Modifier.clearAndSetSemantics {
            contentDescription = "Tap the heart once for a Heart, twice for Sorry, three times for Annoy. Hold for the battery."
        },
        horizontalArrangement = Arrangement.spacedBy(18.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        for (m in Mood.entries) {
            val n = m.ordinal + 1
            val on by animateFloatAsState(if (tapCount == n) 1f else 0f, tween(140), label = "key")
            val c = lerp(Stage.label, moodColor(m), on)
            Row(verticalAlignment = Alignment.CenterVertically) {
                repeat(n) {
                    Box(Modifier.padding(end = 3.dp).size(6.dp).clip(CircleShape).background(c))
                }
                Spacer(Modifier.width(4.dp))
                Text(label(m), style = MaterialTheme.typography.bodySmall, color = lerp(Stage.text2, Stage.text, on))
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(width = 16.dp, height = 6.dp).clip(CircleShape).background(Stage.label))
            Spacer(Modifier.width(7.dp))
            Text("Battery", style = MaterialTheme.typography.bodySmall, color = Stage.text2)
        }
    }
}

private fun lerp(a: Color, b: Color, t: Float) = androidx.compose.ui.graphics.lerp(a, b, t)

private fun moodOf(n: Int) = when (n) { 1 -> Mood.HEART; 2 -> Mood.SORRY; else -> Mood.ANNOY }

private fun label(m: Mood) = when (m) { Mood.HEART -> "Heart"; Mood.SORRY -> "Sorry"; Mood.ANNOY -> "Annoy" }
