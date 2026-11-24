package com.heartbeacon.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class TapsTest {
    @Test fun singleDoubleTripleAndCap() {
        val t = Taps()
        assertNull(t.deadline())
        assertEquals(1, t.tap(1000))
        assertNull(t.due(1399))
        assertEquals(1, t.due(1400))
        assertNull(t.due(5000))

        t.tap(2000); assertEquals(2, t.tap(2399))
        assertNull(t.due(2798))
        assertEquals(2, t.due(2799))

        t.tap(3000); t.tap(3100); t.tap(3200)
        assertEquals(3, t.tap(3300)) // four taps are still annoy, like the button's "3 or more"
        assertEquals(3700L, t.deadline())
        assertEquals(3, t.due(3700))
    }

    @Test fun gapRestartsFromTheLastTap() {
        val t = Taps()
        t.tap(0); t.tap(390); t.tap(780)
        assertNull(t.due(1179))
        assertEquals(3, t.due(1180))
    }
}
