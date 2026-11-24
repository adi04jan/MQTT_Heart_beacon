package com.heartbeacon.app.ui

import com.heartbeacon.app.data.Mark
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.Presence
import java.time.Instant
import java.time.LocalTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.time.temporal.ChronoUnit
import java.util.Locale

/** Pure text for the UI; time, zone and locale are passed in so it runs (and is tested) on the host. */
class Format(private val zone: ZoneId, private val locale: Locale) {
    private val time = DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT).withLocale(locale)
    private val day = DateTimeFormatter.ofPattern("EEE d MMM", locale)
    private val weekday = DateTimeFormatter.ofPattern("EEE", locale)

    fun clock(ms: Long): String = time.format(Instant.ofEpochMilli(ms).atZone(zone))

    fun clock(t: LocalTime): String = time.format(t)

    /** The schedule's stop: `stop <= start` runs past midnight, and equal is 24 h (PROTOCOL.md 7.2). */
    fun sleeps(start: LocalTime, stop: LocalTime): String = if (stop <= start) "${clock(stop)} next day" else clock(stop)

    private fun daysBetween(ms: Long, now: Long) = ChronoUnit.DAYS.between(
        Instant.ofEpochMilli(ms).atZone(zone).toLocalDate(),
        Instant.ofEpochMilli(now).atZone(zone).toLocalDate(),
    )

    fun presence(p: Presence, now: Long): String = when (p) {
        Presence.Near -> "Near"
        Presence.AwayWifi -> "Away on Wi-Fi"
        is Presence.Asleep -> when {
            p.untilEpochMs == null -> "Asleep"
            daysBetween(now, p.untilEpochMs) == 0L -> "Asleep until ${clock(p.untilEpochMs)}"
            else -> "Asleep until ${weekday.format(Instant.ofEpochMilli(p.untilEpochMs).atZone(zone))} ${clock(p.untilEpochMs)}"
        }
        Presence.Offline -> "Offline"
    }

    fun presenceDetail(p: Presence): String = when (p) {
        Presence.Near -> "Connected through this phone"
        Presence.AwayWifi -> "Reaching the server on its own Wi-Fi"
        is Presence.Asleep -> "Hearts wait and play when it wakes"
        Presence.Offline -> "Not reachable. Hearts wait up to 24 hours"
    }

    fun ago(ms: Long, now: Long): String {
        val s = (now - ms) / 1000
        return when {
            s < 60 -> "just now"
            s < 3600 -> "${s / 60} min ago"
            s < 86_400 && daysBetween(ms, now) == 0L -> "${s / 3600} h ago"
            daysBetween(ms, now) == 1L -> "yesterday"
            else -> "${daysBetween(ms, now)} days ago"
        }
    }

    fun dayHeader(ms: Long, now: Long): String = when (daysBetween(ms, now)) {
        0L -> "Today"
        1L -> "Yesterday"
        else -> day.format(Instant.ofEpochMilli(ms).atZone(zone))
    }

    fun event(mood: Mood, from: String?): String {
        val what = when (mood) {
            Mood.HEART -> "a Heart"
            Mood.SORRY -> "a Sorry"
            Mood.ANNOY -> "an Annoy"
        }
        return "${from ?: "You"} sent $what"
    }

    fun mark(m: Mark): String = when (m) {
        Mark.QUEUED -> "Queued"
        Mark.SHOWN -> "Shown"
        Mark.EXPIRED -> "Expired"
    }
}

/**
 * The awake part of a day as fractions of it (0 = midnight): one span, or two when the schedule runs past
 * midnight; equal times are the whole day (PROTOCOL.md 7.2).
 */
fun awakeSpans(start: LocalTime, stop: LocalTime): List<Pair<Float, Float>> {
    val a = start.toSecondOfDay() / 86_400f
    val b = stop.toSecondOfDay() / 86_400f
    return when {
        stop == start -> listOf(0f to 1f)
        stop > start -> listOf(a to b)
        else -> listOf(0f to b, a to 1f).filter { it.second > it.first }
    }
}
