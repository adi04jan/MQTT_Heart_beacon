package com.heartbeacon.app.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Demonstration data so every screen renders before the server and the beacon exist. */
class FakeRepo(
    private val scope: CoroutineScope,
    presence: Presence = Presence.Near,
    loggedIn: Boolean = false,
    now: Long = System.currentTimeMillis(),
) : HeartRepo {
    private val min = 60_000L
    private val hour = 60 * min

    override val session = MutableStateFlow(if (loggedIn) Session("aditya", "203.0.113.10") else null)
    override val server = MutableStateFlow(ServerLink.CONNECTED)
    override val beacon = MutableStateFlow<Beacon?>(
        Beacon("HB-3FA2C1", "Desk", presence, batteryPct = 82, lastSeenEpochMs = now - 2 * min),
    )
    override val events = MutableStateFlow(
        listOf(
            Event("e01", Mood.HEART, "Asha", now - 4 * min, null),
            Event("e02", Mood.HEART, null, now - 12 * min, Mark.SHOWN),
            Event("e03", Mood.ANNOY, null, now - 41 * min, Mark.SHOWN),
            Event("e04", Mood.SORRY, "Asha", now - 70 * min, null),
            Event("e05", Mood.SORRY, null, now - 2 * hour, Mark.QUEUED),
            Event("e06", Mood.HEART, "Asha", now - 22 * hour, null),
            Event("e07", Mood.HEART, null, now - 24 * hour, Mark.SHOWN),
            Event("e08", Mood.ANNOY, "Asha", now - 26 * hour, null),
            Event("e09", Mood.HEART, null, now - 27 * hour, Mark.SHOWN),
            Event("e10", Mood.HEART, "Asha", now - 49 * hour, null),
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

    override suspend fun login(user: String, pass: String, host: String): String? {
        delay(600)
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

    override fun update(s: DeviceSettings) {
        settings.value = s
        beacon.update { it?.copy(name = s.name) }
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

    override fun forgetWifi(ssid: String) {
        settings.update { it?.copy(wifi = it.wifi - ssid) }
    }

    override suspend fun claim(did: String, name: String) {
        delay(500)
        beacon.update { it?.copy(did = did, name = name) }
        settings.update { it?.copy(name = name) }
    }

    override fun remove() {
        beacon.value = null
    }
}
