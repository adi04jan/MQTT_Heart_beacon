package com.heartbeacon.app.data

import com.heartbeacon.core.Envelope
import kotlinx.coroutines.flow.StateFlow

enum class Mood(val wire: String) { HEART("heart"), SORRY("sorry"), ANNOY("annoy") }

/** The beacon card's one line of state. */
sealed interface Presence {
    data object Near : Presence
    data object AwayWifi : Presence
    data class Asleep(val untilEpochMs: Long?) : Presence
    data object Offline : Presence

    companion object {
        /** The card line for a beacon `state` (exact lowercase `st`, PROTOCOL.md 3); null for anything else. */
        fun of(st: String, wakeEpochS: Long): Presence? = when (st) {
            "near" -> Near
            "away" -> AwayWifi
            "nap", "sleep", "lowbatt" -> Asleep(if (wakeEpochS > 0) wakeEpochS * 1000 else null)
            "offline", "pairing", Envelope.LWT_STATE -> Offline
            else -> null
        }
    }
}

data class Beacon(
    val did: String,
    val name: String,
    val presence: Presence,
    val batteryPct: Int?,
    val lastSeenEpochMs: Long?,
    /** Wi-Fi signal from the last `tele`, dBm. */
    val rssi: Int? = null,
)

/** Delivery mark on an event the user sent (PROTOCOL `rcpt.st`). Null for events received. */
enum class Mark { QUEUED, SHOWN, EXPIRED }

data class Event(val id: String, val mood: Mood, val from: String?, val atEpochMs: Long, val mark: Mark?)

enum class ServerLink { CONNECTED, CONNECTING, OFFLINE }

data class Session(val user: String, val host: String)

/** The user-editable part of config schema v1 (PROTOCOL.md section 7). */
data class DeviceSettings(
    val name: String,
    val wifi: List<String>,
    val useWifiAway: Boolean,
    val schedOn: Boolean,
    val days: String,
    val start: String,
    val stop: String,
    val bright: Int,
    val napWhenIdle: Boolean,
    val fw: String,
    val fwAvailable: String?,
)

data class WifiNet(val ssid: String, val rssi: Int, val secure: Boolean)

/**
 * Everything the screens need. [RealRepo] runs the app; [FakeRepo] renders previews, tests and the
 * debug screenshot scenarios.
 */
interface HeartRepo {
    val session: StateFlow<Session?>
    val server: StateFlow<ServerLink>
    val beacon: StateFlow<Beacon?>
    val events: StateFlow<List<Event>>
    val settings: StateFlow<DeviceSettings?>

    /** Null on success, else what to tell the user. */
    suspend fun login(user: String, pass: String, host: String): String?
    fun logout()
    fun send(mood: Mood)
    /** Shows [s] at once; null once the server has it, else what to tell the user (the old settings are back). */
    suspend fun update(s: DeviceSettings): String?

    /** After the CDM chooser: bond, `get info`; returns the beacon's did, or null when pairing failed. */
    suspend fun pair(mac: String?): String?
    suspend fun scanWifi(): List<WifiNet>
    /** Null when the beacon joined, else the beacon's reason. */
    suspend fun joinWifi(ssid: String, psk: String): String?
    /** Null once the beacon has dropped [ssid] (over BLE), else what to tell the user. */
    suspend fun forgetWifi(ssid: String): String?
    /** Null when the beacon is on the account and has its credentials, else what to tell the user. */
    suspend fun claim(did: String, name: String): String?
    fun remove()
}
