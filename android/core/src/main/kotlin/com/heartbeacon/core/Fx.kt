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
    private const val PERIOD_MS = PULSE_MS + PULSE_GAP_MS

    enum class Kind(val wire: String, internal val rgb: IntArray?) {
        NONE("none", null),
        HEART("heart", intArrayOf(250, 0, 0)),
        SORRY("sorry", intArrayOf(235, 161, 23)),
        ANNOY("annoy", intArrayOf(23, 231, 235)),
        PAIR("pair", intArrayOf(0, 0, 250)),
        BATT("batt", null),
        LOW("low", intArrayOf(200, 0, 0)),
    }

    /** How long [kind] plays, in ms; null for PAIR, which repeats until replaced. */
    fun durationMs(kind: Kind, arg: Int = 0): Long? = when (kind) {
        Kind.HEART, Kind.SORRY, Kind.ANNOY -> (MOOD_PULSES * PERIOD_MS).toLong()
        Kind.PAIR -> null
        Kind.BATT -> (arg * BAR_STEP_MS + BAR_HOLD_MS + PIXELS * BAR_WIPE_MS).toLong()
        Kind.LOW -> 6L * LOW_BLINK_MS
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

    private fun set(out: IntArray, i: Int, c: IntArray, level: Long, bright: Int) {
        for (k in 0..2) out[i * 3 + k] = (c[k] * level * bright / (255L * 100L)).toInt()
    }

    // v1 battery_level_show(): red -> yellow over pixels 0..4, yellow -> green over 5..8.
    private fun barColour(i: Int) = intArrayOf(
        if (i <= 4) 255 else 255 - (i - 5) * 255 / 3,
        if (i <= 4) i * 255 / 4 else 255,
        0,
    )

    /**
     * The frame [e] ms after [kind] started, as 9 x RGB (chain order) into [out] (size 27), scaled by [bright]
     * percent. Returns false, with a dark frame, once the effect has ended.
     */
    fun frame(kind: Kind, arg: Int, e: Long, bright: Int, out: IntArray): Boolean {
        out.fill(0)
        when (kind) {
            Kind.HEART, Kind.SORRY, Kind.ANNOY, Kind.PAIR -> {
                if (kind != Kind.PAIR && e >= MOOD_PULSES * PERIOD_MS) return false
                val level = pulse(e % PERIOD_MS)
                for (i in 0 until PIXELS) set(out, i, kind.rgb!!, level, bright)
                return true
            }
            Kind.BATT -> {
                val fill = arg.toLong() * BAR_STEP_MS
                val wipe = fill + BAR_HOLD_MS
                if (e >= wipe + PIXELS * BAR_WIPE_MS) return false
                val on = if (e < fill) e / BAR_STEP_MS + 1 else arg.toLong()
                val off = if (e < wipe) 0 else (e - wipe) / BAR_WIPE_MS + 1
                for (i in off until on) set(out, i.toInt(), barColour(i.toInt()), 255, bright)
                return true
            }
            Kind.LOW -> {
                if (e >= 6L * LOW_BLINK_MS) return false
                if ((e / LOW_BLINK_MS) % 2 == 0L) set(out, 0, Kind.LOW.rgb!!, 255, bright)
                return true
            }
            Kind.NONE -> return false
        }
    }
}
