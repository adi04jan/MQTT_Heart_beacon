package com.heartbeacon.app.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Demonstration data for previews, tests and debug screenshots. [fail] makes login and claim answer the
 * way [RealRepo] does for that error ([userMessage]), so error states render with the real copy.
 */
class FakeRepo(
    private val scope: CoroutineScope,
    presence: Presence = Presence.Near,
    loggedIn: Boolean = false,
    now: Long = System.currentTimeMillis(),
    link: ServerLink = ServerLink.CONNECTED,
    private val fail: Throwable? = null,
) : HeartRepo {
    private val min = 60_000L
    private val hour = 60 * min

    override val session = MutableStateFlow(if (loggedIn) Session("aditya", "203.0.113.10") else null)
    override val server = MutableStateFlow(link)
    override val beacon = MutableStateFlow<Beacon?>(
        Beacon("HB-756268", "Desk", presence, batteryPct = 82, lastSeenEpochMs = now - 2 * min, rssi = -58),
    )
    override val events = MutableStateFlow(
        listOf(
            Event("e01", Mood.HEART, "BulBul", now - 4 * min, null),
            Event("e02", Mood.HEART, null, now - 12 * min, Mark.SHOWN),
            Event("e03", Mood.ANNOY, null, now - 41 * min, Mark.SHOWN),
            Event("e04", Mood.SORRY, "BulBul", now - 70 * min, null),
            Event("e05", Mood.SORRY, null, now - 2 * hour, Mark.QUEUED),
            Event("e06", Mood.HEART, "BulBul", now - 22 * hour, null),
            Event("e07", Mood.HEART, null, now - 24 * hour, Mark.SHOWN),
            Event("e08", Mood.ANNOY, "BulBul", now - 26 * hour, null),
            Event("e09", Mood.HEART, null, now - 27 * hour, Mark.SHOWN),
            Event("e10", Mood.HEART, "BulBul", now - 49 * hour, null),
            Event("e11", Mood.SORRY, null, now - 52 * hour, Mark.EXPIRED),
        ),
    )
    override val settings = MutableStateFlow<DeviceSettings?>(
        DeviceSettings(
            name = "Desk",
            wifi = listOf("Office-5G", "Home"),
            useWifiAway = true,
            schedOn = true,
            days = "1111110",
            start = "09:00",
            stop = "19:00",
            bright = 60,
            napWhenIdle = false,
            fw = "2.0.0",
            fwAvailable = null,
        ),
    )

    /** Debug screenshots: a mood from BulBul arrives now, as RealRepo reports a live one. */
    fun incoming(m: Mood) {
        events.update { (listOf(Event("in" + System.nanoTime(), m, "BulBul", System.currentTimeMillis(), null)) + it).take(20) }
    }

    override suspend fun login(user: String, pass: String, host: String): String? {
        delay(600)
        fail?.let { return userMessage(it) }
        if (pass.isEmpty()) return "Enter your password."
        session.value = Session(user.trim(), host.trim())
        return null
    }

    override fun logout() {
        session.value = null
    }

    override fun send(mood: Mood) {
        val id = "s" + System.nanoTime()
        events.update { (listOf(Event(id, mood, null, System.currentTimeMillis(), Mark.QUEUED)) + it).take(20) }
        scope.launch {
            delay(1_500)
            events.update { list -> list.map { if (it.id == id) it.copy(mark = Mark.SHOWN) else it } }
        }
    }

    override suspend fun update(s: DeviceSettings): String? {
        val old = settings.value
        settings.value = s
        beacon.update { it?.copy(name = s.name) }
        fail ?: return null
        delay(700)
        settings.value = old
        return userMessage(fail)
    }

    override suspend fun pair(mac: String?): String? {
        delay(1_800)
        return "HB-3FA2C1"
    }

    override suspend fun scanWifi(): List<WifiNet> {
        delay(900)
        return listOf(
            WifiNet("Office-5G", -48, true),
            WifiNet("Home", -61, true),
            WifiNet("Office-Guest", -67, false),
            WifiNet("JioFiber-2.4", -78, true),
        )
    }

    override suspend fun joinWifi(ssid: String, psk: String): String? {
        delay(1_200)
        if (psk.isNotEmpty() && psk.length < 8) return "Wrong password: the network refused it."
        settings.update { it?.copy(wifi = (it.wifi + ssid).distinct().take(4)) }
        return null
    }

    override suspend fun forgetWifi(ssid: String): String? {
        delay(500)
        if (beacon.value?.presence != Presence.Near) return RealRepo.NO_BEACON
        settings.update { it?.copy(wifi = it.wifi - ssid) }
        return null
    }

    override suspend fun claim(did: String, name: String): String? {
        delay(500)
        fail?.let { return userMessage(it, did) }
        beacon.update { it?.copy(did = did, name = name) }
        settings.update { it?.copy(name = name) }
        return null
    }

    override fun remove() {
        beacon.value = null
    }
}
