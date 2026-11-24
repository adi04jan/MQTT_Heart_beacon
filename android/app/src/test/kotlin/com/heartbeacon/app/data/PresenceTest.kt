package com.heartbeacon.app.data

import com.heartbeacon.core.Envelope
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class PresenceTest {
    @Test fun everyStateNameMapsAndNothingElseDoes() {
        (Envelope.STATES + Envelope.LWT_STATE).forEach { assertNotNull(it, Presence.of(it, 0)) }
        listOf("NEAR", "Away", "awake", "").forEach { assertNull(it, Presence.of(it, 0)) } // exact lowercase only
    }

    @Test fun cardLines() {
        assertEquals(Presence.Near, Presence.of("near", 0))
        assertEquals(Presence.AwayWifi, Presence.of("away", 0))
        assertEquals(Presence.Asleep(1_795_000_000_000), Presence.of("sleep", 1_795_000_000))
        assertEquals(Presence.Asleep(null), Presence.of("lowbatt", 0)) // button wake only
        assertEquals(Presence.Offline, Presence.of("lost", 0))
    }
}
