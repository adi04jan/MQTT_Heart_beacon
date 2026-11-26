package com.heartbeacon.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.longOrNull

/** [code] is the PROTOCOL.md error name, e.g. `bad_id`. */
class ProtocolException(val code: String) : Exception(code)

/** A decoded envelope; [body] is every key except v/t/id/ts. */
data class Msg(val t: String, val id: String, val ts: Long, val body: JsonObject = JsonObject(emptyMap()))

/** The v1 JSON envelope, protocol/PROTOCOL.md section 2. */
object Envelope {
    const val MAX_BYTES = 1024

    val TYPES = setOf(
        "mood", "show", "shown", "rcpt", "tele", "state", "cfg", "time", "sleep", "get", "resp",
        "claim", "wifi.scan", "wifi.list", "wifi.set", "wifi.forget", "wifi.use", "link", "relay", "reset",
        "streak", "presence", "batt",
    )

    /** `state.st` values, exact and lowercase (PROTOCOL.md 3); [LWT_STATE] only as the broker's will. */
    val STATES = listOf("pairing", "near", "away", "offline", "nap", "sleep", "lowbatt")
    const val LWT_STATE = "lost"

    private val HEADER = setOf("v", "t", "id", "ts")
    private val ID = Regex("[0-9a-f]{16}")

    fun encode(t: String, id: String, ts: Long, body: JsonObject = JsonObject(emptyMap())): ByteArray {
        val raw = buildJsonObject {
            put("v", JsonPrimitive(1))
            put("t", JsonPrimitive(t))
            put("id", JsonPrimitive(id))
            put("ts", JsonPrimitive(ts))
            body.forEach { (k, v) -> put(k, v) }
        }.toString().encodeToByteArray()
        if (raw.size > MAX_BYTES) throw ProtocolException("too_big")
        return raw
    }

    fun decode(raw: ByteArray): Msg {
        if (raw.size > MAX_BYTES) throw ProtocolException("too_big")
        val obj = try {
            Json.parseToJsonElement(raw.decodeToString())
        } catch (e: IllegalArgumentException) { // SerializationException
            null
        } as? JsonObject ?: throw ProtocolException("bad_json")
        if (!strict(obj)) throw ProtocolException("bad_json")

        val v = obj["v"] as? JsonPrimitive
        if (v == null || v.isString || v.content != "1") throw ProtocolException("bad_v")
        val t = obj["t"] as? JsonPrimitive
        if (t == null || !t.isString || t.content.isEmpty()) throw ProtocolException("bad_t")
        val id = obj["id"] as? JsonPrimitive
        if (id == null || !id.isString || !ID.matches(id.content)) throw ProtocolException("bad_id")
        val ts = (obj["ts"] as? JsonPrimitive)?.takeIf { !it.isString }?.longOrNull
        if (ts == null || ts !in 0L..0xFFFFFFFFL) throw ProtocolException("bad_ts")
        return Msg(t.content, id.content, ts, JsonObject(obj.filterKeys { it !in HEADER }))
    }

    /** The `resp ok:false` owed for an unknown `t`, or null when [req]'s type is known. */
    fun unknownTypeResponse(req: Msg, respId: String, now: Long): ByteArray? =
        if (req.t in TYPES) null
        else encode("resp", respId, now, buildJsonObject {
            put("re", JsonPrimitive(req.id))
            put("ok", JsonPrimitive(false))
            put("err", JsonPrimitive("unknown_t"))
        })

    // kotlinx's parser accepts unquoted tokens (NaN, heart) as literals; RFC 8259 does not.
    private val NUMBER = Regex("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?")

    private fun strict(e: JsonElement): Boolean = when (e) {
        is JsonObject -> e.values.all(::strict)
        is JsonArray -> e.all(::strict)
        is JsonPrimitive -> e.isString || e.content in setOf("true", "false", "null") || NUMBER.matches(e.content)
    }

    fun didFromMac(mac: ByteArray): String =
        "HB-" + (3..5).joinToString("") { "%02X".format(mac[it].toInt() and 0xFF) }
}
