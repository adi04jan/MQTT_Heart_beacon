package com.heartbeacon.app.ui.heart

import com.heartbeacon.app.data.Mood
import com.heartbeacon.core.Fx

/**
 * What the phone heart plays: the desk beacon's effects at the moment the beacon plays them (hb_core on_show).
 * A mood that reaches an awake beacon starts at once and replaces whatever was playing, like hb_fx_start. While
 * the beacon sleeps or is offline the router holds the moods; on wake it flushes them and each replaces the last,
 * so the twin plays the last one. A mood the phone sends never plays here: the router delivers it to the *other*
 * beacons only. Time is injected; pure, host-tested.
 */
class Twin {
    class Play(val kind: Fx.Kind, val arg: Int, val startMs: Long)

    var playing: Play? = null
        private set

    /** Moods held for the sleeping beacon, and the one it will show on wake. */
    var waiting = 0
        private set
    private var next: Fx.Kind? = null

    fun mood(m: Mood, awake: Boolean, now: Long) {
        val k = kindOf(m)
        if (awake) {
            playing = Play(k, 0, now)
        } else {
            next = k
            waiting++
        }
    }

    /** The beacon reported itself up: the router flushes what it held. */
    fun woke(now: Long) {
        next?.let { playing = Play(it, 0, now) }
        next = null
        waiting = 0
    }

    /** Hold on the phone heart: the battery bar, as a hold on the button plays it (replaces any effect). */
    fun battery(pct: Int, now: Long) {
        playing = Play(Fx.Kind.BATT, Fx.barPixels(pct), now)
    }

    /** The frame at [now] into [out] (27 ints); false and dark when nothing plays. */
    fun frame(now: Long, bright: Int, out: IntArray): Boolean {
        val p = playing ?: return false.also { out.fill(0) }
        if (Fx.frame(p.kind, p.arg, now - p.startMs, bright, out)) return true
        playing = null
        return false
    }

    companion object {
        fun kindOf(m: Mood) = when (m) {
            Mood.HEART -> Fx.Kind.HEART
            Mood.SORRY -> Fx.Kind.SORRY
            Mood.ANNOY -> Fx.Kind.ANNOY
        }
    }
}
