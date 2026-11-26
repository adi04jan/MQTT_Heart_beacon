package com.heartbeacon.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.security.SecureRandom

/** What [RelayEngine] asks the Android glue to do. The glue executes these in order. */
sealed interface RelayOut {
    /** Write these frames, in order, to the beacon's RX characteristic. */
    class BleSend(val frames: List<ByteArray>) : RelayOut

    /** MQTT publish, QoS 1. Report the PUBACK with [RelayEngine.published] using [id]; run what it returns. */
    class Publish(val topic: String, val payload: ByteArray, val id: String) : RelayOut

    /** A message for the app itself: notifications, receipts, beacon `state` / `tele`, answers to its own requests. */
    class Local(val msg: Msg) : RelayOut

    /** The server link looks dead (no PUBACK in time): drop the socket and reconnect. */
    data object ReconnectServer : RelayOut
}

/**
 * The phone's half of PROTOCOL.md section 4: relays between the beacon (BLE frames) and the router
 * (MQTT), reports `link` both ways, and holds what cannot be delivered yet. Pure logic: every input
 * carries `now` (epoch ms), every effect is a returned [RelayOut].
 *
 * - Uplink (beacon or app -> router): published at once when the server link is up, else queued in a
 *   bounded outbox (oldest dropped) and flushed on the next server-up. A publish without a PUBACK in
 *   [ACK_MS] marks the server link dead: the beacon is told `link up:false`, so its own 5 s rule resends
 *   on WiFi, and the unacked messages go back to the outbox (the router dedupes on `id`).
 * - Relay ack: once the broker PUBACKs the `relay` of a beacon message, the beacon gets
 *   `resp{re: <its id>, ok: true}` over BLE. That is the ack its 5 s resend rule waits for.
 * - Downlink (router -> beacon): written at once when BLE is up, else held for [DOWN_HOLD_MS]. After that
 *   the router has already retried on the other path, so a late copy is dropped, not replayed. A `relay`
 *   holding the beacon's own `state` / `tele` is device status for the app and never goes to the beacon;
 *   one holding a `cfg` is dropped (the app writes the signed `cfg` it got from the API itself).
 * - No beacon yet ([did] null: none claimed, or pairing before the claim): moods and the app's own BLE
 *   requests work; there is no `link` to report and nothing of the beacon's to relay.
 */
class RelayEngine(
    private val did: String?,
    uid: String,
    private val newId: () -> String = Ids::random,
    private val tzMin: () -> Int = { 0 },
    private val outboxCap: Int = 32,
) {
    val upTopic = "hb/v1/u/$uid/up"

    var serverUp = false
        private set
    var bleUp = false
        private set
    /** Uplinks dropped because the outbox was full, or too big to wrap. For diagnostics. */
    var dropped = 0
        private set

    /** For the log: link flags and what waits for a PUBACK (id and age) or for the server. */
    fun debug(now: Long): String =
        "server=$serverUp ble=$bleUp inflight=${inflight.entries.joinToString { "${it.key}:${now - it.value.second}ms" }} " +
            "outbox=${outbox.size} held=${held.size} dropped=$dropped"

    /** [ackRe]: the beacon message this `relay` carries, acked to the beacon on PUBACK. */
    private class Queued(val id: String, val payload: ByteArray, val ackRe: String? = null)
    private class Held(val msg: ByteArray, val at: Long)

    private val outbox = ArrayDeque<Queued>()
    private val inflight = LinkedHashMap<String, Pair<Queued, Long>>()
    private val held = ArrayDeque<Held>()
    private val ownRequests = LinkedHashSet<String>()
    private var timeAt = 0L // last `time` written to the beacon
    private var stuck = 0 // forced reconnects in a row with no PUBACK in between: the watchdog backs off
    private val ownShows = LinkedHashSet<String>() // moods this phone played on its own beacon: their `shown` stays here
    private val rx = Reassembler()
    private var mtu = Frames.MIN_MTU
    private var seq = 0

    fun serverChanged(up: Boolean, now: Long): List<RelayOut> {
        if (up == serverUp) return emptyList()
        serverUp = up
        val out = mutableListOf<RelayOut>()
        if (up) {
            if (did != null) out += publish(env("link", now, "did" to JsonPrimitive(did), "ble" to JsonPrimitive(bleUp)), now)
            while (outbox.isNotEmpty()) out += publish(outbox.removeFirst(), now)
        } else {
            requeueInflight()
        }
        if (bleUp) out += toBeacon(env("link", now, "up" to JsonPrimitive(up)))
        return out
    }

    fun bleChanged(up: Boolean, now: Long, mtu: Int = Frames.MIN_MTU): List<RelayOut> {
        rx.reset()
        if (up == bleUp) return emptyList()
        bleUp = up
        this.mtu = mtu.coerceAtLeast(Frames.MIN_MTU)
        val out = mutableListOf<RelayOut>()
        if (up) {
            // Time comes from the phone on every BLE connect (plan, decision 8).
            out += toBeacon(env("time", now, "tz" to JsonPrimitive(tzMin())))
            timeAt = now
            out += toBeacon(env("link", now, "up" to JsonPrimitive(serverUp)))
            held.filter { now - it.at <= DOWN_HOLD_MS }.forEach { out += toBeacon(it.msg) }
            held.clear()
        }
        if (serverUp && did != null) out += publish(env("link", now, "did" to JsonPrimitive(did), "ble" to JsonPrimitive(up)), now)
        return out
    }

    /** One RX notification from the beacon. */
    fun bleFrame(frame: ByteArray, now: Long): List<RelayOut> {
        val r = rx.push(frame, now)
        if (r.out != Reassembler.Out.COMPLETE) return emptyList()
        val raw = r.msg!!
        val msg = try {
            Envelope.decode(raw)
        } catch (e: ProtocolException) {
            return emptyList() // a beacon never answers junk; neither do we
        }
        if (msg.t == "shown" && ownShows.remove(msg.id)) return emptyList() // our own glow, not a delivery
        if (msg.t == "resp" && ownRequests.remove(msg.body["re"]?.jsonPrimitive?.content)) {
            return listOf(RelayOut.Local(msg))
        }
        val did = did ?: return emptyList() // not claimed yet: the router would not take it
        val wrapped = try {
            env("relay", now, "did" to JsonPrimitive(did), "m" to Json.parseToJsonElement(raw.decodeToString()).jsonObject)
                .let { Queued(it.id, it.payload, ackRe = msg.id) }
        } catch (e: ProtocolException) {
            dropped++ // inner message too close to 1024 B to fit in a relay; see report
            return emptyList()
        }
        val out = mutableListOf<RelayOut>()
        if (msg.t == "state" || msg.t == "tele") out += RelayOut.Local(msg)
        out += uplink(wrapped, now)
        return out
    }

    /** One message from `hb/v1/u/{uid}/down`. */
    fun serverMessage(payload: ByteArray, now: Long): List<RelayOut> {
        val msg = try {
            Envelope.decode(payload)
        } catch (e: ProtocolException) {
            return emptyList()
        }
        if (msg.t != "relay") return listOf(RelayOut.Local(msg))
        if (msg.body["did"]?.jsonPrimitive?.content != did) return emptyList()
        val inner = (msg.body["m"] as? JsonObject)?.toString()?.encodeToByteArray() ?: return emptyList()
        val m = try {
            Envelope.decode(inner) // never forward something the beacon would drop anyway
        } catch (e: ProtocolException) {
            return emptyList()
        }
        when (m.t) {
            "state", "tele" -> return listOf(RelayOut.Local(m)) // device status, from the beacon itself
            "cfg" -> return emptyList() // never in a relay (PROTOCOL.md 7)
        }
        if (bleUp) return listOf(toBeacon(inner))
        held.addLast(Held(inner, now))
        while (held.size > HELD_CAP) held.removeFirst()
        return emptyList()
    }

    /** The app's own Heart / Sorry / Annoy. Works with no beacon at all. [id] is the event id `rcpt` will carry. */
    fun sendMood(k: String, now: Long, id: String = newId(), extra: JsonObject = JsonObject(emptyMap())): List<RelayOut> {
        val out = mutableListOf<RelayOut>()
        // The router delivers only to the *other* members, so the sender's own beacon glows from here, as it does
        // for its own button: a `show` with the mood's id (the beacon's dedupe ring stops any second play).
        if (bleUp && did != null) {
            ownShows += id
            if (ownShows.size > 16) ownShows.remove(ownShows.first())
            out += toBeacon(Envelope.encode("show", id, now / 1000, buildJsonObject {
                put("k", JsonPrimitive(k))
                extra.forEach { (kk, v) -> put(kk, v) }
            }))
        }
        out += uplink(env("mood", now, "k" to JsonPrimitive(k), *extra.map { it.key to it.value }.toTypedArray(), id = id), now)
        return out
    }

    /** A request from the app to the beacon (`get`, `wifi.*`, `claim`, ...); its `resp` comes back as [RelayOut.Local]. */
    fun request(t: String, body: JsonObject, now: Long, id: String = newId()): List<RelayOut> {
        if (!bleUp) return emptyList()
        ownRequests += id
        if (ownRequests.size > 16) ownRequests.remove(ownRequests.first())
        return listOf(toBeacon(Envelope.encode(t, id, now / 1000, body)))
    }

    /** The broker PUBACKed publish [id]. For a relayed beacon message, the beacon gets its ack. */
    fun published(id: String, now: Long): List<RelayOut> {
        stuck = 0
        val re = inflight.remove(id)?.first?.ackRe ?: return emptyList()
        if (!bleUp) return emptyList() // the beacon has moved on to WiFi; the router dedupes
        return listOf(toBeacon(env("resp", now, "re" to JsonPrimitive(re), "ok" to JsonPrimitive(true))))
    }

    /** Call at least once a second while anything is in flight or held. */
    fun tick(now: Long): List<RelayOut> {
        held.removeAll { now - it.at > DOWN_HOLD_MS }
        val out = mutableListOf<RelayOut>()
        // The beacon counts a NEAR phone as alive only while it hears from it (firmware HB_PHONE_SILENT_MS, 10 min):
        // a `time` every 4 min. In Doze this stops, and the beacon's own WiFi takes over.
        if (bleUp && now - timeAt >= TIME_EVERY_MS) {
            out += toBeacon(env("time", now, "tz" to JsonPrimitive(tzMin())))
            timeAt = now
        }
        val wait = ACK_WAIT[minOf(stuck, ACK_WAIT.size - 1)] // 5 s, then 15 s, then 60 s: no TLS thrash on a slow link
        if (inflight.values.none { now - it.second >= wait }) return out
        stuck++
        out += RelayOut.ReconnectServer
        out += serverChanged(false, now)
        return out
    }

    private fun uplink(q: Queued, now: Long): List<RelayOut> {
        if (serverUp) return listOf(publish(q, now))
        outbox.addLast(q)
        while (outbox.size > outboxCap) {
            outbox.removeFirst()
            dropped++
        }
        return emptyList()
    }

    private fun publish(q: Queued, now: Long): RelayOut {
        inflight[q.id] = q to now
        return RelayOut.Publish(upTopic, q.payload, q.id)
    }

    private fun requeueInflight() {
        inflight.values.map { it.first }.asReversed().forEach { outbox.addFirst(it) }
        inflight.clear()
        while (outbox.size > outboxCap) {
            outbox.removeFirst()
            dropped++
        }
    }

    private fun toBeacon(msg: ByteArray): RelayOut {
        val frames = Frames.split(msg, seq, mtu)
        seq = (seq + 1) and 0x7F
        return RelayOut.BleSend(frames)
    }

    private fun toBeacon(q: Queued) = toBeacon(q.payload)

    private fun env(t: String, now: Long, vararg body: Pair<String, kotlinx.serialization.json.JsonElement>, id: String = newId()): Queued {
        return Queued(id, Envelope.encode(t, id, now / 1000, buildJsonObject { body.forEach { (k, v) -> put(k, v) } }))
    }

    companion object {
        /** PROTOCOL.md 4: no ack in 5 s -> the other path. */
        const val ACK_MS = 5_000L
        /** No-PUBACK watchdog per forced reconnect in a row: [ACK_MS] first, then longer on a link that stays slow. */
        val ACK_WAIT = longArrayOf(ACK_MS, 15_000L, 60_000L)
        /** PROTOCOL.md 4: the router tries the other path after 10 s without `shown`. */
        const val DOWN_HOLD_MS = 10_000L
        const val HELD_CAP = 16
        /** A `time` to a NEAR beacon this often: its sign of life (the beacon gives up after 10 min). */
        const val TIME_EVERY_MS = 4 * 60_000L
    }
}

object Ids {
    private val rng = SecureRandom()

    /** A random 16-hex envelope id. */
    fun random(): String = "%016x".format(rng.nextLong())
}
