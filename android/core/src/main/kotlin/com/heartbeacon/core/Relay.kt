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

    /** MQTT publish, QoS 1. Report the PUBACK with [RelayEngine.published] using [id]. */
    class Publish(val topic: String, val payload: ByteArray, val id: String) : RelayOut

    /** A message for the app itself: notifications, receipts, beacon status, answers to its own requests. */
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
 * - Downlink (router -> beacon): written at once when BLE is up, else held for [DOWN_HOLD_MS]. After that
 *   the router has already retried on the other path, so a late copy is dropped, not replayed.
 */
class RelayEngine(
    private val did: String,
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

    private class Queued(val id: String, val payload: ByteArray)
    private class Held(val msg: ByteArray, val at: Long)

    private val outbox = ArrayDeque<Queued>()
    private val inflight = LinkedHashMap<String, Pair<Queued, Long>>()
    private val held = ArrayDeque<Held>()
    private val ownRequests = LinkedHashSet<String>()
    private val rx = Reassembler()
    private var mtu = Frames.MIN_MTU
    private var seq = 0

    fun serverChanged(up: Boolean, now: Long): List<RelayOut> {
        if (up == serverUp) return emptyList()
        serverUp = up
        val out = mutableListOf<RelayOut>()
        if (up) {
            out += publish(env("link", now, "did" to JsonPrimitive(did), "ble" to JsonPrimitive(bleUp)), now)
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
            out += toBeacon(env("link", now, "up" to JsonPrimitive(serverUp)))
            held.filter { now - it.at <= DOWN_HOLD_MS }.forEach { out += toBeacon(it.msg) }
            held.clear()
        }
        if (serverUp) out += publish(env("link", now, "did" to JsonPrimitive(did), "ble" to JsonPrimitive(up)), now)
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
        if (msg.t == "resp" && ownRequests.remove(msg.body["re"]?.jsonPrimitive?.content)) {
            return listOf(RelayOut.Local(msg))
        }
        val wrapped = try {
            env("relay", now, "did" to JsonPrimitive(did), "m" to Json.parseToJsonElement(raw.decodeToString()).jsonObject)
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
        try {
            Envelope.decode(inner) // never forward something the beacon would drop anyway
        } catch (e: ProtocolException) {
            return emptyList()
        }
        if (bleUp) return listOf(toBeacon(inner))
        held.addLast(Held(inner, now))
        while (held.size > HELD_CAP) held.removeFirst()
        return emptyList()
    }

    /** The app's own Heart / Sorry / Annoy. Works with no beacon at all. */
    fun sendMood(k: String, now: Long): List<RelayOut> = uplink(env("mood", now, "k" to JsonPrimitive(k)), now)

    /** A request from the app to the beacon (`get`, `wifi.*`, `claim`, ...); its `resp` comes back as [RelayOut.Local]. */
    fun request(t: String, body: JsonObject, now: Long): List<RelayOut> {
        if (!bleUp) return emptyList()
        val id = newId()
        ownRequests += id
        if (ownRequests.size > 16) ownRequests.remove(ownRequests.first())
        return listOf(toBeacon(Envelope.encode(t, id, now / 1000, body)))
    }

    fun published(id: String) {
        inflight.remove(id)
    }

    /** Call at least once a second while anything is in flight or held. */
    fun tick(now: Long): List<RelayOut> {
        held.removeAll { now - it.at > DOWN_HOLD_MS }
        if (inflight.values.none { now - it.second >= ACK_MS }) return emptyList()
        val out = mutableListOf<RelayOut>(RelayOut.ReconnectServer)
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

    private fun env(t: String, now: Long, vararg body: Pair<String, kotlinx.serialization.json.JsonElement>): Queued {
        val id = newId()
        return Queued(id, Envelope.encode(t, id, now / 1000, buildJsonObject { body.forEach { (k, v) -> put(k, v) } }))
    }

    companion object {
        /** PROTOCOL.md 4: no ack in 5 s -> the other path. */
        const val ACK_MS = 5_000L
        /** PROTOCOL.md 4: the router tries the other path after 10 s without `shown`. */
        const val DOWN_HOLD_MS = 10_000L
        const val HELD_CAP = 16
    }
}

object Ids {
    private val rng = SecureRandom()

    /** A random 16-hex envelope id. */
    fun random(): String = "%016x".format(rng.nextLong())
}
