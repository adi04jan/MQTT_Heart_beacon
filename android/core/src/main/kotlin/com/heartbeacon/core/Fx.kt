package com.heartbeacon.core

/**
 * The beacon's LED effects (fw/components/hb_core/hb_fx.c), frame for frame, so the phone heart plays exactly what
 * the desk heart plays. Same integer math as the firmware; checked against protocol/vectors.json "fx", which is
 * generated from the C code.
 */
object Fx {
    const val PIXELS = 9
    const val PULSE_MS = 1040
    const val PULSE_GAP_MS = 100
    const val MOOD_PULSES = 3
    const val BAR_STEP_MS = 80
    const val BAR_HOLD_MS = 500
    const val BAR_WIPE_MS = 100
    const val LOW_BLINK_MS = 500
    const val FLASH_MS = 150
    const val BREATH_MS = 4000
    const val BREATHS = 15
    const val CELEBRATE_MS = 12000
    const val CELEBRATE_STEP = 10
    private const val PERIOD_MS = PULSE_MS + PULSE_GAP_MS

    enum class Kind(val wire: String, internal val rgb: IntArray?) {
        NONE("none", null),
        HEART("heart", intArrayOf(250, 0, 0)),
        SORRY("sorry", intArrayOf(235, 161, 23)),
        ANNOY("annoy", intArrayOf(23, 231, 235)),
        PAIR("pair", intArrayOf(0, 0, 250)),
        BATT("batt", null),
        LOW("low", intArrayOf(200, 0, 0)),
        /** White, 3 quick flashes: a factory reset. */
        RESET("reset", intArrayOf(255, 255, 255)),
        /** One soft brass pulse: your heart just glowed on the other desk. */
        ECHO("echo", intArrayOf(230, 140, 50)),
        /** Rose, slow breaths (arg = breaths, 0 = [BREATHS]): holding together. */
        BREATH("breath", intArrayOf(255, 40, 90)),
        /** Rainbow chase round the ring. */
        CELEBRATE("celebrate", null),
        /** The sender's own colour; arg 0 = 3 pulses, 1 = 3 breaths. */
        CUSTOM("custom", null),
    }

    private val EMBER = intArrayOf(60, 18, 0)
    private val PRESENCE = intArrayOf(40, 30, 20)

    /** How long [kind] plays, in ms; null for PAIR, which repeats until replaced. */
    fun durationMs(kind: Kind, arg: Int = 0): Long? = when (kind) {
        Kind.HEART, Kind.SORRY, Kind.ANNOY -> (MOOD_PULSES * PERIOD_MS).toLong()
        Kind.PAIR -> null
        Kind.BATT -> (arg * BAR_STEP_MS + BAR_HOLD_MS + PIXELS * BAR_WIPE_MS).toLong()
        Kind.LOW -> 6L * LOW_BLINK_MS
        Kind.RESET -> 6L * FLASH_MS
        Kind.ECHO -> PERIOD_MS.toLong()
        Kind.BREATH -> (if (arg != 0) arg else BREATHS).toLong() * BREATH_MS
        Kind.CELEBRATE -> CELEBRATE_MS.toLong()
        Kind.CUSTOM -> if (arg == 1) 3L * BREATH_MS else (MOOD_PULSES * PERIOD_MS).toLong()
        Kind.NONE -> 0
    }

    /** Lit pixels for a battery percentage, as hb_core's battery_bar() computes them (floor). */
    fun barPixels(pct: Int): Int = pct.coerceIn(0, 100) * PIXELS / 100

    // Heartbeat level 0..255, e ms into one period: linear up, linear down, dark gap.
    private fun pulse(e: Long): Long {
        val half = PULSE_MS / 2
        return when {
            e < half -> e * 255 / half
            e < PULSE_MS -> (PULSE_MS - e) * 255 / half
            else -> 0
        }
    }

    // Breath level 0..255, e ms into one breath: in over half, out over the other half.
    private fun breath(e: Long): Long {
        val half = BREATH_MS / 2
        val x = e % BREATH_MS
        return if (x < half) x * 255 / half else (BREATH_MS - x) * 255 / half
    }

    private fun set(out: IntArray, i: Int, c: IntArray, level: Long, bright: Int) {
        for (k in 0..2) out[i * 3 + k] = (c[k] * level * bright / (255L * 100L)).toInt()
    }

    private fun fill(out: IntArray, c: IntArray, level: Long, bright: Int) {
        for (i in 0 until PIXELS) set(out, i, c, level, bright)
    }

    // v1 battery_level_show(): red -> yellow over pixels 0..4, yellow -> green over 5..8.
    private fun barColour(i: Int) = intArrayOf(
        if (i <= 4) 255 else 255 - (i - 5) * 255 / 3,
        if (i <= 4) i * 255 / 4 else 255,
        0,
    )

    // The classic NeoPixel colour wheel.
    private fun wheel(p: Long): IntArray {
        var pos = 255 - (p and 255).toInt()
        return when {
            pos < 85 -> intArrayOf(255 - pos * 3, 0, pos * 3)
            pos < 170 -> { pos -= 85; intArrayOf(0, pos * 3, 255 - pos * 3) }
            else -> { pos -= 170; intArrayOf(pos * 3, 255 - pos * 3, 0) }
        }
    }

    /**
     * The frame [e] ms after [kind] started, as 9 x RGB (chain order) into [out] (size 27), scaled by [bright]
     * percent and the effect's [level] percent (a dimmed replay); [color] is CUSTOM's. Returns false, with a dark
     * frame, once the effect has ended.
     */
    fun frame(kind: Kind, arg: Int, e: Long, bright: Int, out: IntArray, level: Int = 100, color: IntArray? = null): Boolean {
        out.fill(0)
        val b = bright * (if (level in 1..99) level else 100) / 100
        when (kind) {
            Kind.HEART, Kind.SORRY, Kind.ANNOY, Kind.PAIR -> {
                if (kind != Kind.PAIR && e >= MOOD_PULSES * PERIOD_MS) return false
                fill(out, kind.rgb!!, pulse(e % PERIOD_MS), b)
                return true
            }
            Kind.BATT -> {
                val fillMs = arg.toLong() * BAR_STEP_MS
                val wipe = fillMs + BAR_HOLD_MS
                if (e >= wipe + PIXELS * BAR_WIPE_MS) return false
                val on = if (e < fillMs) e / BAR_STEP_MS + 1 else arg.toLong()
                val off = if (e < wipe) 0 else (e - wipe) / BAR_WIPE_MS + 1
                for (i in off until on) set(out, i.toInt(), barColour(i.toInt()), 255, b)
                return true
            }
            Kind.LOW -> {
                if (e >= 6L * LOW_BLINK_MS) return false
                if ((e / LOW_BLINK_MS) % 2 == 0L) set(out, 0, Kind.LOW.rgb!!, 255, b)
                return true
            }
            Kind.RESET -> {
                if (e >= 6L * FLASH_MS) return false
                if ((e / FLASH_MS) % 2 == 0L) fill(out, Kind.RESET.rgb!!, 255, b)
                return true
            }
            Kind.ECHO -> {
                if (e >= PERIOD_MS) return false
                fill(out, Kind.ECHO.rgb!!, pulse(e) * 2 / 5, b)
                return true
            }
            Kind.BREATH -> {
                if (e >= (if (arg != 0) arg else BREATHS).toLong() * BREATH_MS) return false
                fill(out, Kind.BREATH.rgb!!, breath(e), b)
                return true
            }
            Kind.CELEBRATE -> {
                if (e >= CELEBRATE_MS) return false
                for (i in 0 until PIXELS) set(out, i, wheel(e / CELEBRATE_STEP + i * 28L), 255, b)
                return true
            }
            Kind.CUSTOM -> {
                val c = color ?: intArrayOf(255, 255, 255)
                if (arg == 1) {
                    if (e >= 3L * BREATH_MS) return false
                    fill(out, c, breath(e), b)
                } else {
                    if (e >= MOOD_PULSES * PERIOD_MS) return false
                    fill(out, c, pulse(e % PERIOD_MS), b)
                }
                return true
            }
            Kind.NONE -> return false
        }
    }

    /** The idle overlay (hb_fx_idle): a streak ember on pixel 0, the partner's presence on pixel 4. */
    fun idle(ember: Boolean, presence: Boolean, bright: Int, out: IntArray) {
        if (ember) set(out, 0, EMBER, 255, bright)
        if (presence) set(out, 4, PRESENCE, 255, bright)
    }
}
