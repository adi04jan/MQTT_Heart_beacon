package com.heartbeacon.app.data

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonObject
import java.util.Base64

/** API JSON (server/hb_server/api.py) to the screens' models and back. Pure, so the host tests cover it. */

fun moodOf(k: String?): Mood? = Mood.entries.firstOrNull { it.wire == k }

private fun JsonObject.str(k: String) = (this[k] as? JsonPrimitive)?.contentOrNull
private fun JsonObject.long(k: String) = (this[k] as? JsonPrimitive)?.longOrNull
private fun JsonObject.obj(k: String) = this[k] as? JsonObject

/** Delivery mark on an event [me] sent, from the router's per-beacon statuses; null when it had no beacon to reach. */
fun markOf(statuses: List<String>): Mark? = when {
    statuses.isEmpty() -> null
    "shown" in statuses -> Mark.SHOWN
    statuses.all { it == "expired" } -> Mark.EXPIRED
    else -> Mark.QUEUED
}

/** `GET /events` rows. The user's own (from the app or their beacon) read "You sent" and carry a mark. */
fun eventsOf(rows: JsonArray, me: String): List<Event> = rows.mapNotNull { r ->
    val o = r as? JsonObject ?: return@mapNotNull null
    val mood = moodOf(o.str("k")) ?: return@mapNotNull null
    val mine = o.str("uid") == me
    val to = (o["to"] as? JsonArray).orEmpty().mapNotNull { (it as? JsonObject)?.str("st") }
    Event(
        id = o.str("id") ?: return@mapNotNull null,
        mood = mood,
        from = if (mine) null else o.str("from") ?: "Someone",
        atEpochMs = (o.long("ts") ?: 0) * 1000,
        mark = if (mine) markOf(to) else null,
    )
}

/** One `GET /me` device: the router's last known `st`, `wake`, `last_seen` and `tele`. */
fun beaconOf(d: JsonObject): Beacon {
    val did = d.str("did").orEmpty()
    return Beacon(
        did = did,
        name = d.str("name").orEmpty().ifBlank { did },
        presence = Presence.of(d.str("st").orEmpty(), d.long("wake") ?: 0) ?: Presence.Offline,
        batteryPct = (d.obj("tele")?.get("pct") as? JsonPrimitive)?.intOrNull,
        lastSeenEpochMs = d.long("last_seen")?.let { it * 1000 },
    )
}

/** `GET /settings` plus what only the beacon knows (its saved networks, its running firmware). */
fun settingsOf(s: JsonObject, fw: String, wifi: List<String>, fwTarget: String?): DeviceSettings {
    val sched = s.obj("sched")
    return DeviceSettings(
        name = s.str("name").orEmpty(),
        wifi = wifi,
        useWifiAway = (s.obj("net")?.get("use_wifi") as? JsonPrimitive)?.booleanOrNull ?: true,
        schedOn = (sched?.get("on") as? JsonPrimitive)?.booleanOrNull ?: false,
        days = sched?.str("days") ?: "1111100",
        start = sched?.str("start") ?: "09:00",
        stop = sched?.str("stop") ?: "19:00",
        bright = (s.obj("led")?.get("bright") as? JsonPrimitive)?.intOrNull ?: 60,
        napWhenIdle = (napMinOf(s) ?: 0) > 0,
        fw = fw,
        fwAvailable = fwTarget?.takeIf { it.isNotEmpty() && it != fw },
    )
}

fun napMinOf(s: JsonObject): Int? = (s.obj("pwr")?.get("idle_nap_min") as? JsonPrimitive)?.intOrNull

/**
 * The PUT body: every user key (PROTOCOL.md 7 "Who edits"), `sched.tz` left to the server. Nap on keeps
 * the beacon's current minutes, else the app's 90.
 */
fun settingsBody(s: DeviceSettings, napMin: Int?): JsonObject = buildJsonObject {
    put("name", s.name)
    putJsonObject("led") { put("bright", s.bright) }
    putJsonObject("sched") {
        put("on", s.schedOn)
        put("days", s.days)
        put("start", s.start)
        put("stop", s.stop)
    }
    putJsonObject("pwr") { put("idle_nap_min", if (s.napWhenIdle) napMin?.takeIf { it > 0 } ?: 90 else 0) }
    putJsonObject("net") { put("use_wifi", s.useWifiAway) }
}

/** `fw.ver` inside a signed `cfg` envelope's config bytes (`b`, base64url): the image the admin targeted. */
fun fwTargetOf(cfg: JsonObject): String? = runCatching {
    val raw = Base64.getUrlDecoder().decode(cfg.str("b")!!)
    Json.parseToJsonElement(raw.decodeToString()).jsonObject.obj("fw")?.str("ver")
}.getOrNull()

/** `{n, b, s}` of a `cfg` envelope: the body the app writes to the beacon (with its own envelope id). */
fun cfgBody(cfg: JsonObject): JsonObject = JsonObject(cfg.filterKeys { it in setOf("n", "b", "s") })

/** `resp.d` lists from the beacon. Shape proposed for the contract: `wifi.scan` d.nets, `wifi.list` d.ssids. */
fun netsOf(d: JsonObject?): List<WifiNet> = (d?.get("nets") as? JsonArray).orEmpty().mapNotNull { n ->
    val o = n as? JsonObject ?: return@mapNotNull null
    WifiNet(o.str("ssid") ?: return@mapNotNull null, (o["rssi"] as? JsonPrimitive)?.intOrNull ?: -100,
        (o["sec"] as? JsonPrimitive)?.booleanOrNull ?: true)
}

fun ssidsOf(d: JsonObject?): List<String> = (d?.get("ssids") as? JsonArray).orEmpty().mapNotNull { it.jsonPrimitive.contentOrNull }
