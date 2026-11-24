package com.heartbeacon.app.ui

import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.ZoneId
import java.time.ZonedDateTime
import java.util.Locale

class FormatTest {
    private val ist = ZoneId.of("Asia/Kolkata")
    private val f = Format(ist, Locale.UK)
    private fun at(d: Int, h: Int, m: Int = 0) = ZonedDateTime.of(2026, 11, d, h, m, 0, 0, ist).toInstant().toEpochMilli()
    private val now = at(23, 14, 30) // Monday 23 Nov 2026, 14:30 IST

    @Test fun beaconCardStates() {
        assertEquals("Near", f.presence(Presence.Near, now))
        assertEquals("Away on Wi-Fi", f.presence(Presence.AwayWifi, now))
        assertEquals("Offline", f.presence(Presence.Offline, now))
        assertEquals("Asleep until 19:00", f.presence(Presence.Asleep(at(23, 19)), now))
        assertEquals("Asleep until Tue 09:00", f.presence(Presence.Asleep(at(24, 9)), now))
        assertEquals("Asleep", f.presence(Presence.Asleep(null), now))
    }

    @Test fun relativeTimesRespectCalendarDays() {
        assertEquals("just now", f.ago(now - 30_000, now))
        assertEquals("12 min ago", f.ago(now - 12 * 60_000, now))
        assertEquals("3 h ago", f.ago(at(23, 11, 0), now))
        // 23:50 yesterday is under 24 h ago but on another day.
        assertEquals("yesterday", f.ago(at(22, 23, 50), now))
        assertEquals("2 days ago", f.ago(at(21, 16), now))
    }

    @Test fun dayHeadersAndEventText() {
        assertEquals("Today", f.dayHeader(at(23, 0, 1), now))
        assertEquals("Yesterday", f.dayHeader(at(22, 23, 59), now))
        assertEquals("Sat 21 Nov", f.dayHeader(at(21, 10), now))
        assertEquals("Asha sent a Heart", f.event(Mood.HEART, "Asha"))
        assertEquals("You sent an Annoy", f.event(Mood.ANNOY, null))
    }

    @Test fun scheduleStopPastMidnightSaysNextDay() {
        val t = { s: String -> java.time.LocalTime.parse(s) }
        assertEquals("19:00", f.sleeps(t("09:00"), t("19:00")))
        assertEquals("06:00 next day", f.sleeps(t("22:00"), t("06:00")))
        assertEquals("09:00 next day", f.sleeps(t("09:00"), t("09:00"))) // equal = 24 h on
    }
}
