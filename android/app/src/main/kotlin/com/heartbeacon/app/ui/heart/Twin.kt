package com.heartbeacon.app.ui.heart

import com.heartbeacon.app.data.Mood
import com.heartbeacon.core.Fx

/**
 * What the phone heart plays: the desk beacon's effects at the moment the beacon plays them (hb_core on_show).
 * A mood that reaches an awake beacon starts at once and replaces whatever was playing, like hb_fx_start. While
 * the beacon sleeps or is offline the router holds the moods; on wake it flushes them and each replaces the last,
 * so the twin plays the last one. A mood the phone sends never plays here: the router delivers it to the *other*
 * beacons only. With nothing playing, the idle overlay shows (a streak's ember, the partner's presence), as
 * hb_fx_idle does. Time is injected; pure, host-tested.
 */
class Twin {
    class Play(val kind: Fx.Kind, val arg: Int, val startMs: Long, val rgb: IntArray? = null)

    var playing: Play? = null
        private set

    /** Moods held for the sleeping beacon, and the one it will show on wake. */
    var waiting = 0
        private set
    private var next: Play? = null

    /** The idle overlay (`streak` > 0, and `presence` when the beacon shows it). */
    var ember = false
    var presence = false

    fun mood(m: Mood, awake: Boolean, now: Long, rgb: String? = null) {
        val p = playOf(m, now, rgb)
        if (awake) {
            playing = p
        } else {
            next = p
            waiting++
        }
    }

    /** The beacon reported itself up: the router flushes what it held. */
    fun woke(now: Long) {
        next?.let { playing = Play(it.kind, it.arg, now, it.rgb) }
        next = null
        waiting = 0
    }

    /** Your heart just played on the other desk (`rcpt shown`): the soft brass echo the beacon plays too. */
    fun echo(now: Long) {
        playing = Play(Fx.Kind.ECHO, 0, now)
    }

    /** The beacon no longer knows this phone (review U3): its pairing pulse, until it is paired again. */
    fun pair(now: Long) {
        playing = Play(Fx.Kind.PAIR, 0, now)
    }

    /** Hold on the phone heart: the battery bar, as a hold on the button plays it (replaces any effect). */
    fun battery(pct: Int, now: Long) {
        playing = Play(Fx.Kind.BATT, Fx.barPixels(pct), now)
    }

    /** The frame at [now] into [out] (27 ints); false when no effect plays (the idle overlay may still be lit). */
    fun frame(now: Long, bright: Int, out: IntArray): Boolean {
        val p = playing
        if (p != null && Fx.frame(p.kind, p.arg, now - p.startMs, bright, out, color = p.rgb)) return true
        playing = null
        out.fill(0)
        Fx.idle(ember, presence, bright, out)
        return false
    }

    companion object {
        fun kindOf(m: Mood) = when (m) {
            Mood.HEART -> Fx.Kind.HEART
            Mood.SORRY -> Fx.Kind.SORRY
            Mood.ANNOY -> Fx.Kind.ANNOY
            Mood.CUSTOM -> Fx.Kind.CUSTOM
            Mood.TOGETHER -> Fx.Kind.BREATH
        }

        /** "#rrggbb" -> rgb, or null. */
        fun rgbOf(hex: String?): IntArray? =
            hex?.takeIf { it.length == 7 && it[0] == '#' }?.let { h ->
                runCatching { IntArray(3) { h.substring(1 + 2 * it, 3 + 2 * it).toInt(16) } }.getOrNull()
            }

        private fun playOf(m: Mood, now: Long, rgb: String?) = when (m) {
            Mood.CUSTOM -> Play(Fx.Kind.CUSTOM, 0, now, rgbOf(rgb) ?: intArrayOf(255, 79, 163))
            Mood.TOGETHER -> Play(Fx.Kind.BREATH, 3, now) // an invitation: 3 breaths
            else -> Play(kindOf(m), 0, now)
        }
    }
}
