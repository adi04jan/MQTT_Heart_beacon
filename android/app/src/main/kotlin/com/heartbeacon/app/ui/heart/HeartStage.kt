@file:OptIn(ExperimentalLayoutApi::class)

package com.heartbeacon.app.ui.heart

import android.content.Context
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
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.offset
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.ReadOnlyComposable
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
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.platform.ViewConfiguration
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
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
import androidx.core.content.edit
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

/**
 * The cloth the beacon sits on, following the theme: aubergine felt under a lamp in the dark, pale lilac cloth by
 * daylight in the light. Violet is brass's complement, so the cage stands off it; the LEDs stay the only bright colour.
 */
object Stage {
    val light: Boolean @Composable @ReadOnlyComposable get() = MaterialTheme.colorScheme.surface.luminance() > 0.5f
    private fun pick(light: Boolean, day: Long, night: Long) = Color(if (light) day else night)

    /** The cloth at its edge, away from the light (the felt shader's `lo` times its tint). */
    val ground @Composable @ReadOnlyComposable get() = pick(light, 0xFFD5CADB, 0xFF110C16)
    /** What sits behind figures that cross the rods: a shade on felt, a veil of light on the pale cloth. */
    val veil @Composable @ReadOnlyComposable get() = pick(light, 0xFFF8F2FA, 0xFF07050A)
    val text @Composable @ReadOnlyComposable get() = pick(light, 0xFF1E1722, 0xFFF4EEF3)
    val text2 @Composable @ReadOnlyComposable get() = pick(light, 0xFF4C4352, 0xFFCABDCC)
    val label @Composable @ReadOnlyComposable get() = pick(light, 0xFF5D5363, 0xFFA898AE)
    val pipOff @Composable @ReadOnlyComposable get() = pick(light, 0xFFBFB3C6, 0xFF3D3145)
    val warn @Composable @ReadOnlyComposable get() = pick(light, 0xFFB3261E, 0xFFF2B8B5)
    val brass @Composable @ReadOnlyComposable get() = pick(light, 0xFFA57C1F, 0xFFE2BC6A)
    /** Brass as text (text buttons on the cloth): darker by day, so it reads at 5:1 on the lilac. */
    val brassInk @Composable @ReadOnlyComposable get() = pick(light, 0xFF684C0D, 0xFFE2BC6A)
    val onBrass @Composable @ReadOnlyComposable get() = pick(light, 0xFF231700, 0xFF2E2100)
}

private const val REST_YAW = -0.26f
private const val REST_PITCH = 0.06f

/** How long a mood waits before it goes out, so a stray poke can be taken back. */
private const val TAKE_BACK_MS = 1_500L
/** A mood that came while the app was closed plays once on opening if it is this recent (the router's window). */
private const val REPLAY_MS = 24 * 3_600_000L
private const val PLAYED = "played"
private const val COACHED = "coached"  // the first send happened: no more "tap the heart" tag

// Cloth under a soft overhead light: a pool where the beacon stands, the cloth's grain, falling off beyond.
private const val FELT = """
uniform float2 spot;
uniform float reach;
uniform float lo;
uniform float hi;
uniform float grain;
uniform float3 tint;
half4 main(float2 p) {
    float2 d = (p - spot) / reach;
    float pool = exp(-dot(d, d) * 1.45);
    float n = fract(sin(dot(floor(p / 2.0), float2(12.9898, 78.233))) * 43758.5453);
    float m = fract(sin(dot(floor(p / float2(9.0, 3.0)), float2(39.346, 11.135))) * 24634.6345);
    float v = mix(lo, hi, pool);
    v += grain * ((n - 0.5) * 0.050 * (0.35 + pool) + (m - 0.5) * 0.026 * (0.2 + pool));
    return half4(v * tint.r, v * tint.g, v * tint.b, 1.0);
}
"""

/**
 * The cloth ground, its light pooled at ([x], [y]) as fractions of the size, reaching [reach] widths out: dark
 * felt, or the pale daylight cloth when [light].
 */
fun Modifier.felt(x: Float, y: Float, reach: Float = 0.78f, light: Boolean = false): Modifier = drawWithCache {
    val shader = RuntimeShader(FELT)
    shader.setFloatUniform("spot", size.width * x, size.height * y)
    shader.setFloatUniform("reach", size.width * reach)
    // Keep Stage.ground at lo * tint.
    if (light) {
        shader.setFloatUniform("lo", 0.835f)
        shader.setFloatUniform("hi", 0.965f)
        shader.setFloatUniform("grain", 0.55f)
        shader.setFloatUniform("tint", 1f, 0.95f, 1.03f)
    } else {
        shader.setFloatUniform("lo", 0.066f)
        shader.setFloatUniform("hi", 0.245f)
        shader.setFloatUniform("grain", 1f)
        shader.setFloatUniform("tint", 1.02f, 0.74f, 1.32f)
    }
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
    /** Review U1: the brightness slider is being dragged; the cage lights steadily at that brightness. */
    previewing: Boolean = false,
    top: @Composable () -> Unit,
) {
    val renderer = remember { CageRenderer() }
    val twin = remember { Twin() }
    val taps = remember { Taps() }
    val scope = rememberCoroutineScope()
    val haptics = LocalHapticFeedback.current
    val still = animationsOff()

    val yaw = remember { Animatable(if (still) REST_YAW else REST_YAW - 1.1f) }
    val squash = remember { Animatable(1f) }
    val wave = remember { Animatable(0f) }
    var waveMood by remember { mutableStateOf(Mood.HEART) }
    var clock by remember { mutableLongStateOf(SystemClock.uptimeMillis()) }
    var effectUntil by remember { mutableLongStateOf(0L) }
    var tapCount by remember { mutableIntStateOf(0) }
    var tapSerial by remember { mutableIntStateOf(0) }
    var sentFlash by remember { mutableStateOf<Mood?>(null) }
    var pending by remember { mutableStateOf<Mood?>(null) }
    var pendingSerial by remember { mutableIntStateOf(0) }
    var waiting by remember { mutableIntStateOf(0) }
    var lastTouch by remember { mutableLongStateOf(0L) }
    val ctx = LocalContext.current
    val prefs = remember { ctx.getSharedPreferences("stage", Context.MODE_PRIVATE) }

    val present = beacon != null
    val awake = beacon?.presence == Presence.Near || beacon?.presence == Presence.AwayWifi
    val look = when {
        beacon == null -> Look.OFFLINE
        awake -> Look.AWAKE
        beacon.presence is Presence.Asleep -> Look.ASLEEP
        else -> Look.OFFLINE
    }

    fun play(now: Long) {
        // PAIR has no end: it pulses until the bond is back (the re-pair plate)
        twin.playing?.let { effectUntil = it.startMs + (Fx.durationMs(it.kind, it.arg) ?: (24 * 3600_000L)) }
        clock = now
    }

    fun showBattery(pct: Int) {
        twin.battery(pct, SystemClock.uptimeMillis())
        play(SystemClock.uptimeMillis())
    }

    // Intro: a turn into the resting three-quarter view, once.
    LaunchedEffect(Unit) {
        if (!still) yaw.animateTo(REST_YAW, tween(1500, easing = FastOutSlowInEasing))
    }

    // An incoming mood on the twin: at once when the beacon is up, else held for its wake. Remembered, so the same
    // mood never plays twice on opening.
    fun arrive(e: Event, up: Boolean) {
        val now = SystemClock.uptimeMillis()
        if (up && !still) heartbeat(ctx, e.mood)  // review U6: felt as well as seen
        twin.mood(e.mood, up, now, e.rgb)
        waiting = twin.waiting
        play(now)
        prefs.edit { putString(PLAYED, e.id) }
    }

    // Live moods reach the twin as they reach the beacon. History fetched later does not, except the newest mood
    // from the other side: it usually came while the app was closed, so it plays once on opening, if the beacon is up
    // (it played it too). The foot line says when it came.
    val seen = remember { HashSet<String>() }
    var primed by remember { mutableStateOf(false) }
    var caughtUp by remember { mutableStateOf(false) }
    LaunchedEffect(events) {
        val now = System.currentTimeMillis()
        for (e in events) {
            if (!seen.add(e.id) || !primed) continue
            if (e.from != null && now - e.atEpochMs < 30_000) arrive(e, awake)
        }
        primed = true
        if (!caughtUp && events.isNotEmpty()) {
            caughtUp = true
            val last = events.firstOrNull { it.from != null }
            if (awake && last != null && now - last.atEpochMs < REPLAY_MS && last.id != prefs.getString(PLAYED, null)) arrive(last, true)
        }
    }
    // The beacon came up: the router flushes what it held, the twin plays it.
    LaunchedEffect(awake) {
        if (awake && twin.waiting > 0) {
            twin.woke(SystemClock.uptimeMillis())
            waiting = twin.waiting
            play(SystemClock.uptimeMillis())
        }
    }

    // The beacon refused this phone's bond (review U3): the twin pulses blue like the real pairing window.
    val needsPair = beacon?.needsPair == true
    LaunchedEffect(needsPair) {
        if (needsPair) {
            twin.pair(SystemClock.uptimeMillis())
            play(SystemClock.uptimeMillis())
        }
    }

    // The idle overlay the beacon shows: the streak's ember, the partner's presence (review I5, I7). Redrawn once
    // when either changes, as the frame clock runs only while an effect plays.
    val ember = (beacon?.streak ?: 0) > 0
    val partner = beacon?.partnerNear == true
    LaunchedEffect(ember, partner) {
        twin.ember = ember
        twin.presence = partner
        play(SystemClock.uptimeMillis())
    }
    // One of your hearts just played on the other desk: the soft brass echo, as your beacon plays it (review I2).
    // Marks already shown when the app opens are history, not news.
    val echoed = remember { HashSet<String>() }
    var echoPrimed by remember { mutableStateOf(false) }
    LaunchedEffect(events) {
        for (e in events) {
            if (e.from == null && e.mark == Mark.SHOWN && echoed.add(e.id) && echoPrimed && awake) {
                twin.echo(SystemClock.uptimeMillis())
                play(SystemClock.uptimeMillis())
            }
        }
        echoPrimed = true
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

    var coached by remember { mutableStateOf(prefs.getBoolean(COACHED, false)) }
    if (!coached && events.any { it.from == null }) coached = true  // has sent before (history): no tip

    fun fire(m: Mood) {
        onSend(m)
        if (!coached) {
            coached = true
            prefs.edit { putBoolean(COACHED, true) }
        }
        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
        sentFlash = m
        waveMood = m
        if (!still) scope.launch {
            wave.snapTo(0f)
            wave.animateTo(1f, tween(1300, easing = LinearEasing))
        }
    }
    // A mood waits TAKE_BACK_MS with a Cancel at the foot; a newer one sends the waiting one at once.
    fun commit(m: Mood) {
        pending?.let(::fire)
        pending = m
        pendingSerial++
        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
    }
    LaunchedEffect(pendingSerial, pending) {
        val m = pending ?: return@LaunchedEffect
        delay(TAKE_BACK_MS)
        fire(m)
        pending = null
    }
    LaunchedEffect(sentFlash) {
        if (sentFlash == null) return@LaunchedEffect
        delay(1_200)
        sentFlash = null
    }

    // Multi-tap: commit the count once the button's gap passes with no new tap.
    LaunchedEffect(tapSerial) {
        if (tapSerial == 0) return@LaunchedEffect
        delay(Taps.GAP_MS)
        val n = taps.due(SystemClock.uptimeMillis()) ?: return@LaunchedEffect
        tapCount = 0
        commit(moodOf(n))
    }

    // Back to the resting pose a while after the last touch.
    LaunchedEffect(lastTouch) {
        if (lastTouch == 0L) return@LaunchedEffect
        delay(3500)
        val target = REST_YAW + Math.round((yaw.value - REST_YAW) / (2 * Math.PI)).toFloat() * (2 * Math.PI).toFloat()
        yaw.animateTo(target, spring(stiffness = 40f, dampingRatio = 0.9f))
    }

    val rgb = remember { IntArray(27) }
    val stateText = beacon?.let { describe(it, fmt) } ?: "No beacon yet"
    val light = Stage.light
    val ground = Stage.ground

    Box(
        modifier
            .background(ground)
            .felt(0.60f, 0.50f, light = light)
            // Top and foot fall back to the cloth's edge, so the name and the last event read off the lit pool.
            .drawBehind {
                drawRect(Brush.verticalGradient(0f to ground.copy(alpha = 0.5f), 0.2f to Color.Transparent, 0.68f to Color.Transparent, 1f to ground.copy(alpha = 0.62f)))
            }
            .semantics {
                contentDescription = "Your beacon, ${stateText.lowercase()}"
                customActions = listOf(
                    CustomAccessibilityAction("Send a Heart") { fire(Mood.HEART); true },
                    CustomAccessibilityAction("Send a Sorry") { fire(Mood.SORRY); true },
                    CustomAccessibilityAction("Send an Annoy") { fire(Mood.ANNOY); true },
                )
            },
    ) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val wPx = constraints.maxWidth.toFloat()
            val hPx = constraints.maxHeight.toFloat()
            val halfWidth = minOf(wPx * 0.35f, hPx * 0.27f)
            val center = Offset(wPx * 0.60f, hPx * 0.455f)
            fun inside(p: Offset) = hypot(p.x - center.x, p.y - center.y) < halfWidth * 1.15f

            // Review U8: a brass tag tied under the cage until the first send, then never again.
            if (!coached && present) {
                val d = LocalDensity.current
                Text(
                    "Tap the heart",
                    style = MaterialTheme.typography.labelLarge,
                    color = Stage.text,
                    modifier = Modifier
                        .offset { IntOffset((center.x - with(d) { 52.dp.toPx() }).toInt(), (center.y + halfWidth * 1.18f).toInt()) }
                        .clip(RoundedCornerShape(4.dp))
                        .border(1.dp, Stage.label, RoundedCornerShape(4.dp))
                        .padding(horizontal = 10.dp, vertical = 4.dp)
                        .clearAndSetSemantics { contentDescription = "Tip: tap the heart to send a Heart" },
                )
            }

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
                            // Sideways turns the heart; up and down belongs to the page, so it scrolls on to the list.
                            val tracker = VelocityTracker()
                            detectHorizontalDragGestures(
                                onDragStart = { tracker.resetTracking(); lastTouch = 0L },
                                onDragEnd = {
                                    val v = tracker.calculateVelocity()
                                    scope.launch { yaw.animateDecay(v.x * 0.0075f, exponentialDecay(1.6f)) }
                                    lastTouch = SystemClock.uptimeMillis()
                                },
                            ) { change, dx ->
                                change.consume()
                                tracker.addPosition(change.uptimeMillis, change.position)
                                scope.launch { yaw.snapTo(yaw.value + dx * 0.0075f) }
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
                                    showBattery(pct)
                                },
                            )
                        },
                ) {
                    val now = clock
                    twin.frame(now, 100, rgb)
                    if (previewing) for (i in 0 until Fx.PIXELS) { rgb[i * 3] = 250; rgb[i * 3 + 1] = 0; rgb[i * 3 + 2] = 0 }
                    // The desk LEDs dim with the brightness setting; a screen needs a floor to stay readable.
                    val k = 0.62f + 0.38f * bright.coerceIn(5, 100) / 100f
                    for (i in rgb.indices) rgb[i] = (rgb[i] * k).toInt()
                    with(renderer) {
                        paper = light
                        if (wave.value in 0.001f..0.999f) wave(wave.value, moodColor(waveMood, onLight = light), yaw.value, REST_PITCH, center, halfWidth)
                        draw(rgb, yaw.value, REST_PITCH, squash.value, look, center, halfWidth)
                    }
                }
            }
        }

        // Name, dashboard, foot in one column: the dashboard takes the space between, so at any font size it never
        // runs into the foot.
        Column(Modifier.fillMaxSize()) {
            top()
            Box(Modifier.weight(1f).fillMaxWidth()) {
                // Unbounded: if it ever outgrows the space it spills over the edges rather than losing its last line.
                if (beacon != null) Dashboard(beacon, fmt, Modifier.align(Alignment.CenterStart).wrapContentHeight(unbounded = true))
            }
            val newest = events.firstOrNull()
            Foot(
                beacon = beacon,
                server = server,
                events = events,
                waiting = waiting,
                tapCount = tapCount,
                pending = pending,
                sent = sentFlash,
                fmt = fmt,
                onAdd = onAdd,
                onSend = ::commit,
                onCancel = { pending = null },
                onBattery = beacon?.batteryPct?.let { pct -> { showBattery(pct) } },
                onReplay = newest?.takeIf { awake && it.from != null }?.let { e -> { arrive(e, true) } },
                modifier = Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, bottom = 24.dp),
            )
        }
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
    val light = Stage.light
    Canvas(modifier.background(Stage.ground).felt(0.5f, at + 0.04f, 0.7f, light).clearAndSetSemantics {}) {
        if (play != null) Fx.frame(play, 0, clock, 100, rgb) else rgb.fill(0)
        val hw = minOf(size.width * 0.34f, size.height * fit)
        with(renderer) {
            paper = light
            draw(rgb, yaw.value, REST_PITCH, 1f, Look.AWAKE, Offset(size.width / 2, size.height * at), hw)
        }
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
    val veil = Stage.veil
    Column(
        modifier
            .width(164.dp)
            // A soft veil on the cloth behind the figures, so they read where rods pass under them.
            .drawBehind {
                // An ellipse that fades out all round: no edge anywhere on the cloth.
                val rx = size.width * 1.3f
                val mid = Offset(0f, size.height / 2)
                scale(1f, size.height * 0.66f / rx, pivot = mid) {
                    drawCircle(
                        Brush.radialGradient(
                            0f to veil.copy(alpha = 0.74f), 0.5f to veil.copy(alpha = 0.5f), 1f to Color.Transparent,
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
            if (b.needsPair) {
                Text("Pair again", style = Reading, color = Stage.warn)
                Sub("Hold its button 5 s, then keep this phone close")
                return@Readout
            }
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
                        b.rssi?.takeIf { it < 0 }?.let {
                            Spacer(Modifier.width(10.dp))
                            Bars(it)
                        }
                    }
                    b.rssi?.takeIf { it < 0 }?.let { Sub(signalWord(it)) }  // review U4: words here, the dBm in the beacon's settings
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
        if (b.streak > 0) {  // review I5: the ember on the desk, in words
            Readout("Streak") {
                Text("${b.streak} ${if (b.streak == 1) "day" else "days"}", style = Reading, color = Stage.text)
                Sub("A heart from each of you, every day")
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
private fun beads(rssi: Int) = when { rssi >= -62 -> 3; rssi >= -75 -> 2; else -> 1 }

private fun signalWord(rssi: Int) = when (beads(rssi)) { 3 -> "Strong signal"; 2 -> "Fair signal"; else -> "Weak signal" }

/** Review U4: Wi-Fi strength as three solder beads, the cage's own joints, not a phone's bar graph. */
@Composable
private fun Bars(rssi: Int) {
    val n = beads(rssi)
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(bottom = 8.dp)) {
        for (i in 0 until 3) {
            Box(
                Modifier.size(7.dp).clip(CircleShape)
                    .background(if (i < n) Stage.brass else Stage.pipOff)
                    .border(0.5.dp, Stage.label.copy(alpha = 0.6f), CircleShape),
            )
        }
    }
}

/** The stage's foot: what the tap is about to send, or the last thing that happened; the gesture key under it. */
private class FootLine(val text: String, val dot: Color?, val action: String? = null, val act: (() -> Unit)? = null)

@Composable
private fun Foot(
    beacon: Beacon?,
    server: ServerLink,
    events: List<Event>,
    waiting: Int,
    tapCount: Int,
    pending: Mood?,
    sent: Mood?,
    fmt: Format,
    onAdd: () -> Unit,
    onSend: (Mood) -> Unit,
    onCancel: () -> Unit,
    onBattery: (() -> Unit)?,
    onReplay: (() -> Unit)?,
    modifier: Modifier,
) {
    val now = System.currentTimeMillis()
    val light = Stage.light
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        val line: FootLine = when {
            tapCount > 0 -> FootLine("${label(moodOf(tapCount))}…", moodColor(moodOf(tapCount), light))
            pending != null -> FootLine("Sending your ${label(pending)}", moodColor(pending, light), "Cancel", onCancel)
            sent != null -> FootLine("${label(sent)} sent", moodColor(sent, light))
            beacon == null -> FootLine("No beacon yet. A tap still sends a Heart.", null)
            server == ServerLink.OFFLINE -> FootLine("This phone is offline. Moods wait and go out when it is back.", null)
            server == ServerLink.CONNECTING -> FootLine("Connecting. Moods wait and go out when it connects.", null)
            waiting > 0 -> FootLine("$waiting waiting. They play when it wakes.", null)
            else -> events.firstOrNull()?.let { e ->
                val mark = e.mark?.let { " · ${fmt.mark(it)}" }.orEmpty()
                FootLine(
                    "${fmt.event(e.mood, e.from)} · ${fmt.ago(e.atEpochMs, now)}$mark",
                    if (e.mark == Mark.SHOWN || e.from != null) moodColor(e.mood, light) else null,
                    onReplay?.let { "Play again" }, onReplay,
                )
            } ?: FootLine("Nothing yet", null)
        }
        AnimatedContent(
            line,
            contentKey = { it.text to it.action },
            transitionSpec = { fadeIn(tween(160)) togetherWith fadeOut(tween(120)) },
            label = "foot",
            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
        ) { l ->
            // Full width and one height, so a change of line never animates (and clips) the row's size.
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically) {
                if (l.dot != null) {
                    Box(Modifier.size(8.dp).clip(CircleShape).background(l.dot))
                    Spacer(Modifier.width(10.dp))
                }
                Text(l.text, style = MaterialTheme.typography.titleSmall, color = Stage.text, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                if (l.action != null && l.act != null) {
                    TextButton(onClick = l.act, colors = ButtonDefaults.textButtonColors(contentColor = Stage.brassInk)) { Text(l.action) }
                }
            }
        }
        if (beacon == null) {
            Button(
                onClick = onAdd,
                colors = ButtonDefaults.buttonColors(containerColor = Stage.brass, contentColor = Stage.onBrass),
                modifier = Modifier.height(48.dp),
            ) { Text("Add beacon") }
        } else {
            GestureKey(tapCount, pending, onSend, onBattery)
        }
    }
}

/**
 * The button's language as buttons: one, two, three dots send a Heart, a Sorry, an Annoy, as that many taps on the
 * heart do; the bar shows the battery, as a hold does. The one being tapped or sent lights in its mood's colour.
 */
@Composable
private fun GestureKey(tapCount: Int, pending: Mood?, send: (Mood) -> Unit, battery: (() -> Unit)?) {
    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        for (m in Mood.entries) {
            val n = m.ordinal + 1
            val on by animateFloatAsState(if (tapCount == n || pending == m) 1f else 0f, tween(140), label = "key")
            val c = lerp(Stage.label, moodColor(m, Stage.light), on)
            Key("Send ${article(m)}", lerp(Stage.pipOff, c, on), onClick = { send(m) }) {
                when (m) {  // the button's gesture, drawn: pips are taps, a bar is a hold
                    Mood.CUSTOM -> {
                        Box(Modifier.padding(end = 3.dp).size(6.dp).clip(CircleShape).background(c))
                        Box(Modifier.padding(end = 3.dp).size(width = 12.dp, height = 6.dp).clip(CircleShape).background(c))
                    }
                    Mood.TOGETHER -> Box(Modifier.padding(end = 3.dp).size(width = 30.dp, height = 6.dp).clip(CircleShape).background(c))
                    else -> repeat(n) {
                        Box(Modifier.padding(end = 3.dp).size(6.dp).clip(CircleShape).background(c))
                    }
                }
                Spacer(Modifier.width(4.dp))
                Text(label(m), style = MaterialTheme.typography.bodySmall, color = lerp(Stage.text2, Stage.text, on))
            }
        }
        if (battery != null) {
            Key("Show the battery", Stage.pipOff, onClick = battery) {
                Box(Modifier.size(width = 11.dp, height = 6.dp).clip(CircleShape).background(Stage.label))  // a short hold
                Spacer(Modifier.width(7.dp))
                Text("Battery", style = MaterialTheme.typography.bodySmall, color = Stage.text2)
            }
        }
    }
}

/** One key: a thin ring on the cloth, a full 48dp to touch. */
@Composable
private fun Key(action: String, ring: Color, onClick: () -> Unit, content: @Composable RowScope.() -> Unit) {
    Row(
        Modifier
            .minimumInteractiveComponentSize()
            .clip(CircleShape)
            .border(1.dp, ring, CircleShape)
            .clickable(role = Role.Button, onClickLabel = action, onClick = onClick)
            .clearAndSetSemantics { contentDescription = action; role = Role.Button }
            .padding(horizontal = 10.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        content = content,
    )
}

private fun lerp(a: Color, b: Color, t: Float) = androidx.compose.ui.graphics.lerp(a, b, t)

private fun moodOf(n: Int) = when (n) { 1 -> Mood.HEART; 2 -> Mood.SORRY; else -> Mood.ANNOY }

private fun label(m: Mood) = when (m) {
    Mood.HEART -> "Heart"; Mood.SORRY -> "Sorry"; Mood.ANNOY -> "Annoy"; Mood.CUSTOM -> "Colour"; Mood.TOGETHER -> "Together"
}

private fun article(m: Mood) = when (m) {
    Mood.HEART -> "a Heart"; Mood.SORRY -> "a Sorry"; Mood.ANNOY -> "an Annoy"; Mood.CUSTOM -> "your colour"
    Mood.TOGETHER -> "a Together"
}

/**
 * A haptic heartbeat timed to the LED effect (review U6): a short tick at each pulse's peak (520 ms in, then every
 * 1140 ms), a slow swell for a together. Touch-feedback usage, so the system's own haptics switch silences it.
 */
private fun heartbeat(ctx: Context, mood: Mood) {
    val v = ctx.getSystemService(android.os.VibratorManager::class.java)?.defaultVibrator ?: return
    if (!v.hasVibrator()) return
    val effect = if (mood == Mood.TOGETHER) {
        android.os.VibrationEffect.createWaveform(longArrayOf(0, 2000), intArrayOf(0, 60), -1)
    } else {
        android.os.VibrationEffect.createWaveform(longArrayOf(520, 30, 1110, 30, 1110, 30), intArrayOf(0, 180, 0, 180, 0, 180), -1)
    }
    v.vibrate(effect, android.os.VibrationAttributes.createForUsage(android.os.VibrationAttributes.USAGE_TOUCH))
}
