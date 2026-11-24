package com.heartbeacon.core

/**
 * The beacon button's gesture rule (fw/components/hb_core/hb_button.h) for a touch target: taps count, and the
 * count is reported once no new tap follows within [GAP_MS] of the last one. 1 = heart, 2 = sorry, 3 or more =
 * annoy. A press of [HOLD_MS] or more is a hold (battery bar), never a tap. Time is injected.
 */
class Taps {
    private var count = 0
    private var last = 0L

    /** A tap at [now]; returns the count so far in this gesture (for live feedback). */
    fun tap(now: Long): Int {
        count = minOf(count + 1, 3)
        last = now
        return count
    }

    /** The finished gesture's count (1..3) once [GAP_MS] passed since the last tap, else null. */
    fun due(now: Long): Int? {
        if (count == 0 || now - last < GAP_MS) return null
        return count.also { count = 0 }
    }

    /** When to call [due] next, or null when no gesture is open. */
    fun deadline(): Long? = if (count == 0) null else last + GAP_MS

    companion object {
        const val GAP_MS = 400L
        const val HOLD_MS = 600L
    }
}
