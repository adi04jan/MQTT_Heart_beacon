package com.heartbeacon.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class RelayTest {
    private var n = 0L
    private val engine = RelayEngine("HB-3FA2C1", "u1", newId = { "%016x".format(n++) }, tzMin = { 330 }, outboxCap = 3)

    private fun beaconMsg(t: String, id: String, body: JsonObject = JsonObject(emptyMap())) =
        Envelope.encode(t, id, 0, body)

    /** Messages the engine sent to the beacon, reassembled and decoded the way the beacon would. */
    private fun toBeacon(out: List<RelayOut>): List<Msg> = out.filterIsInstance<RelayOut.BleSend>().map { s ->
        val r = Reassembler()
        val last = s.frames.map { r.push(it, 0) }.last()
        assertEquals(Reassembler.Out.COMPLETE, last.out)
        Envelope.decode(last.msg!!)
    }

    private fun published(out: List<RelayOut>): List<Msg> =
        out.filterIsInstance<RelayOut.Publish>().map { assertEquals("hb/v1/u/u1/up", it.topic); Envelope.decode(it.payload) }

    private fun framesFromBeacon(msg: ByteArray, seq: Int, now: Long): List<RelayOut> =
        Frames.split(msg, seq, 23).flatMap { engine.bleFrame(it, now) }

    @Test fun moodQueuedWhileOfflineThenFlushedAfterLink() {
        assertTrue(engine.sendMood("heart", 1_000).isEmpty())
        val out = published(engine.serverChanged(true, 2_000))
        assertEquals(listOf("link", "mood"), out.map { it.t })
        assertEquals(setOf("did", "ble"), out[0].body.keys) // exactly, no `up` (PROTOCOL.md 3)
        assertEquals("HB-3FA2C1", out[0].body["did"]!!.jsonPrimitive.content)
        assertEquals(false, out[0].body["ble"]!!.jsonPrimitive.boolean)
        assertEquals("heart", out[1].body["k"]!!.jsonPrimitive.content)
        assertEquals(1L, out[1].ts) // stamped when pressed, not when flushed
    }

    @Test fun outboxIsBoundedOldestDropped() {
        listOf("heart", "sorry", "annoy", "heart", "sorry").forEach { engine.sendMood(it, 0) }
        assertEquals(2, engine.dropped)
        val moods = published(engine.serverChanged(true, 0)).filter { it.t == "mood" }
        assertEquals(listOf("annoy", "heart", "sorry"), moods.map { it.body["k"]!!.jsonPrimitive.content })
    }

    @Test fun bleUpSendsTimeAndLinkAndReportsToRouter() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        val out = engine.bleChanged(true, 1_700_000_000_000, mtu = 247)
        val sent = toBeacon(out)
        assertEquals(listOf("time", "link"), sent.map { it.t })
        assertEquals(1_700_000_000L, sent[0].ts)
        assertEquals(330, sent[0].body["tz"]!!.jsonPrimitive.content.toInt())
        assertEquals(setOf("up"), sent[1].body.keys)
        assertEquals(true, sent[1].body["up"]!!.jsonPrimitive.boolean)
        val toRouter = published(out).single()
        assertEquals("link", toRouter.t)
        assertEquals(true, toRouter.body["ble"]!!.jsonPrimitive.boolean)
    }

    @Test fun serverChangesAreReportedToTheBeacon() {
        engine.bleChanged(true, 0)
        val down = toBeacon(engine.serverChanged(true, 0) + engine.serverChanged(false, 1))
        assertEquals(listOf(true, false), down.map { it.body["up"]!!.jsonPrimitive.boolean })
        assertTrue(engine.serverChanged(false, 2).isEmpty()) // idempotent
    }

    @Test fun beaconUplinkIsWrappedInRelayVerbatim() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val mood = beaconMsg("mood", "00000000000000aa", buildJsonObject { put("k", JsonPrimitive("heart")) })
        val relay = published(framesFromBeacon(mood, 5, 10)).single()
        assertEquals("relay", relay.t)
        assertEquals("HB-3FA2C1", relay.body["did"]!!.jsonPrimitive.content)
        assertEquals(Json.parseToJsonElement(mood.decodeToString()), relay.body["m"])
    }

    @Test fun beaconStateIsRelayedAndShownLocally() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val state = beaconMsg("state", "00000000000000ab", buildJsonObject { put("st", JsonPrimitive("near")); put("path", JsonPrimitive("ble")) })
        val out = framesFromBeacon(state, 1, 0)
        assertEquals("state", (out[0] as RelayOut.Local).msg.t)
        assertEquals("relay", published(out).single().t)
    }

    @Test fun answerToOwnRequestStaysLocal() {
        engine.bleChanged(true, 0)
        val req = toBeacon(engine.request("get", buildJsonObject { put("what", JsonPrimitive("info")) }, 0)).single()
        val resp = beaconMsg("resp", "00000000000000ac", buildJsonObject { put("re", JsonPrimitive(req.id)); put("ok", JsonPrimitive(true)) })
        val out = framesFromBeacon(resp, 2, 0)
        assertEquals("resp", (out.single() as RelayOut.Local).msg.t)
        // A resp to the router's request (unknown id here) is relayed: queued, since the server is down.
        assertTrue(framesFromBeacon(resp, 3, 0).isEmpty())
        assertEquals("relay", published(engine.serverChanged(true, 0)).last().t)
    }

    @Test fun downlinkGoesStraightToBeacon() {
        engine.bleChanged(true, 0)
        val show = beaconMsg("show", "00000000000000ad", buildJsonObject { put("k", JsonPrimitive("sorry")) })
        val relay = Envelope.encode("relay", "00000000000000ae", 0, buildJsonObject {
            put("did", JsonPrimitive("HB-3FA2C1"))
            put("m", Json.parseToJsonElement(show.decodeToString()))
        })
        val got = toBeacon(engine.serverMessage(relay, 0)).single()
        assertEquals("show", got.t)
        assertEquals("00000000000000ad", got.id)
    }

    @Test fun downlinkHeldWhileBleDownThenDeliveredOrExpired() {
        val show = { id: String ->
            Envelope.encode("relay", "00000000000000f0", 0, buildJsonObject {
                put("did", JsonPrimitive("HB-3FA2C1"))
                put("m", Json.parseToJsonElement(beaconMsg("show", id, buildJsonObject { put("k", JsonPrimitive("heart")) }).decodeToString()))
            })
        }
        assertTrue(engine.serverMessage(show("0000000000000001"), 0).isEmpty())
        assertTrue(engine.serverMessage(show("0000000000000002"), 9_000).isEmpty())
        engine.tick(10_500) // the first is past the router's 10 s retry: dropped
        val sent = toBeacon(engine.bleChanged(true, 11_000))
        assertEquals(listOf("time", "link", "show"), sent.map { it.t })
        assertEquals("0000000000000002", sent[2].id)
    }

    @Test fun downlinkForAnotherBeaconOrJunkIsIgnored() {
        engine.bleChanged(true, 0)
        val other = Envelope.encode("relay", "00000000000000f1", 0, buildJsonObject {
            put("did", JsonPrimitive("HB-000000"))
            put("m", Json.parseToJsonElement(beaconMsg("show", "0000000000000003").decodeToString()))
        })
        assertTrue(engine.serverMessage(other, 0).isEmpty())
        assertTrue(engine.serverMessage("{not json".encodeToByteArray(), 0).isEmpty())
        val rcpt = Envelope.encode("rcpt", "00000000000000f2", 0, buildJsonObject { put("to", JsonPrimitive("HB-3FA2C1")); put("st", JsonPrimitive("shown")) })
        assertEquals("rcpt", (engine.serverMessage(rcpt, 0).single() as RelayOut.Local).msg.t)
    }

    @Test fun noPubackIn5sMarksServerDeadAndResendsSameId() {
        engine.bleChanged(true, 0)
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        val first = engine.sendMood("heart", 1_000).filterIsInstance<RelayOut.Publish>().single() // + the own glow
        assertTrue(engine.tick(5_999).isEmpty())
        val out = engine.tick(6_000)
        assertEquals(RelayOut.ReconnectServer, out[0])
        assertEquals(false, toBeacon(out).single().body["up"]!!.jsonPrimitive.boolean) // beacon falls back to WiFi
        assertTrue(!engine.serverUp)
        val again = engine.serverChanged(true, 7_000).filterIsInstance<RelayOut.Publish>().last()
        assertEquals(first.id, again.id)
        assertTrue(first.payload.contentEquals(again.payload))
    }

    @Test fun watchdogBacksOffOnALinkThatStaysSlow() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        engine.sendMood("heart", 0)
        assertEquals(RelayOut.ReconnectServer, engine.tick(5_000).first()) // 5 s
        engine.serverChanged(true, 6_000) // the same mood again, still no PUBACK
        assertTrue(engine.tick(6_000 + 14_999).none { it == RelayOut.ReconnectServer })
        assertEquals(RelayOut.ReconnectServer, engine.tick(6_000 + 15_000).first()) // then 15 s
        val again = engine.serverChanged(true, 30_000).filterIsInstance<RelayOut.Publish>()
        assertTrue(engine.tick(30_000 + 59_999).none { it == RelayOut.ReconnectServer })
        assertEquals(RelayOut.ReconnectServer, engine.tick(30_000 + 60_000).first()) // then 60 s
        engine.serverChanged(true, 100_000).filterIsInstance<RelayOut.Publish>().forEach { engine.published(it.id, 100_100) }
        engine.sendMood("sorry", 200_000)
        assertEquals(RelayOut.ReconnectServer, engine.tick(205_000).first()) // a PUBACK resets it to 5 s
        assertTrue(again.isNotEmpty())
    }

    @Test fun pubackStopsTheTimer() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        val p = engine.sendMood("annoy", 0).single() as RelayOut.Publish
        engine.published(p.id, 0)
        assertTrue(engine.tick(60_000).isEmpty())
    }

    @Test fun messageTooBigToWrapIsDroppedNotCrashed() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val pad = "x".repeat(1024 - 60)
        val big = beaconMsg("tele", "00000000000000b0", buildJsonObject { put("fw", JsonPrimitive(pad)) })
        assertTrue(big.size <= 1024)
        val out = framesFromBeacon(big, 4, 0)
        assertTrue(published(out).isEmpty())
        assertEquals(1, engine.dropped)
    }

    @Test fun reassemblyResetsOnDisconnect() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val mood = beaconMsg("mood", "00000000000000c0", buildJsonObject { put("k", JsonPrimitive("heart")) })
        val frames = Frames.split(mood, 7, 23)
        engine.bleFrame(frames[0], 0)
        engine.bleChanged(false, 1)
        engine.bleChanged(true, 2)
        // The tail of the old message must not complete anything.
        assertTrue(frames.drop(1).flatMap { engine.bleFrame(it, 3) }.none { it is RelayOut.Publish })
    }

    @Test fun relayedBeaconMessageIsAckedOverBleAfterPuback() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        engine.bleChanged(true, 0)
        val mood = beaconMsg("mood", "00000000000000d0", buildJsonObject { put("k", JsonPrimitive("heart")) })
        val relay = framesFromBeacon(mood, 5, 10).filterIsInstance<RelayOut.Publish>().single()
        assertTrue(engine.tick(1_000).isEmpty()) // no ack before the broker has it
        val ack = toBeacon(engine.published(relay.id, 2_000)).single()
        assertEquals("resp", ack.t)
        assertEquals(mapOf("re" to "00000000000000d0", "ok" to "true"), ack.body.mapValues { it.value.jsonPrimitive.content })
        assertEquals(2L, ack.ts)
        assertTrue(engine.published(relay.id, 2_001).isEmpty()) // a duplicate PUBACK acks nothing
    }

    @Test fun noAckWhileTheServerIsDownThenAckAfterTheFlush() {
        engine.bleChanged(true, 0)
        val mood = beaconMsg("mood", "00000000000000d1", buildJsonObject { put("k", JsonPrimitive("sorry")) })
        assertTrue(framesFromBeacon(mood, 6, 0).isEmpty()) // queued: the beacon's 5 s rule takes WiFi
        val relay = engine.serverChanged(true, 9_000).filterIsInstance<RelayOut.Publish>().last()
        assertEquals("00000000000000d1", toBeacon(engine.published(relay.id, 9_500)).single().body["re"]!!.jsonPrimitive.content)
    }

    @Test fun ownMoodAlsoGlowsTheNearBeaconAndItsShownStaysHere() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val out = engine.sendMood("sorry", 2_000, id = "00000000000000e1")
        val show = toBeacon(out).single()
        assertEquals("show", show.t)
        assertEquals("00000000000000e1", show.id) // the mood's id: the beacon plays it once whatever else arrives
        assertEquals("sorry", show.body["k"]!!.jsonPrimitive.content)
        assertEquals("mood", published(out).single().t)
        // its `shown` is our own glow, not a delivery: nothing goes to the router
        assertTrue(framesFromBeacon(beaconMsg("shown", "00000000000000e1"), 3, 2_100).isEmpty())
        // a `shown` for a router delivery is still relayed
        assertEquals("relay", published(framesFromBeacon(beaconMsg("shown", "00000000000000e2"), 4, 2_200)).single().t)

        engine.bleChanged(false, 3_000) // away: the router's path only
        assertTrue(toBeacon(engine.sendMood("heart", 3_100)).isEmpty())
    }

    @Test fun nearBeaconHearsFromThePhoneEveryFourMinutes() {
        engine.bleChanged(true, 0) // `time` on connect
        assertTrue(toBeacon(engine.tick(RelayEngine.TIME_EVERY_MS - 1)).isEmpty())
        val beat = toBeacon(engine.tick(RelayEngine.TIME_EVERY_MS)).single()
        assertEquals("time", beat.t) // the beacon's sign of life (firmware HB_PHONE_SILENT_MS)
        assertTrue(toBeacon(engine.tick(RelayEngine.TIME_EVERY_MS + 1_000)).isEmpty())
        engine.bleChanged(false, RelayEngine.TIME_EVERY_MS + 2_000)
        assertTrue(toBeacon(engine.tick(3 * RelayEngine.TIME_EVERY_MS)).isEmpty()) // no link, nothing to say
    }

    @Test fun appMessagesAndLostBleGetNoAck() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id, 0) }
        val own = engine.sendMood("heart", 0).single() as RelayOut.Publish
        assertTrue(engine.published(own.id, 1).isEmpty()) // the app's own mood: nothing to ack on BLE
        engine.bleChanged(true, 0)
        val tele = beaconMsg("tele", "00000000000000d2", buildJsonObject { put("mv", JsonPrimitive(3900)) })
        val relay = framesFromBeacon(tele, 7, 0).filterIsInstance<RelayOut.Publish>().single()
        engine.bleChanged(false, 1)
        assertTrue(engine.published(relay.id, 2).isEmpty())
    }

    @Test fun maxSizeBeaconMessageStillFitsInARelay() {
        engine.serverChanged(true, 0)
        engine.bleChanged(true, 0)
        val head = beaconMsg("tele", "00000000000000d3", buildJsonObject { put("pad", JsonPrimitive("")) }).size
        val msg = beaconMsg("tele", "00000000000000d3", buildJsonObject { put("pad", JsonPrimitive("x".repeat(896 - head))) })
        assertEquals(896, msg.size)
        val relay = framesFromBeacon(msg, 8, 0).filterIsInstance<RelayOut.Publish>().single()
        assertTrue(relay.payload.size <= Envelope.MAX_BYTES)
        assertEquals(0, engine.dropped)
    }

    @Test fun deviceStatusFromTheRouterStaysOnThePhone() {
        engine.bleChanged(true, 0)
        for ((t, body) in listOf(
            "state" to buildJsonObject { put("st", JsonPrimitive("away")); put("path", JsonPrimitive("wifi")) },
            "tele" to buildJsonObject { put("mv", JsonPrimitive(3912)) },
        )) {
            val relay = Envelope.encode("relay", "00000000000000e0", 0, buildJsonObject {
                put("did", JsonPrimitive("HB-3FA2C1"))
                put("m", Json.parseToJsonElement(beaconMsg(t, "00000000000000e1", body).decodeToString()))
            })
            val out = engine.serverMessage(relay, 0)
            assertEquals(t, (out.single() as RelayOut.Local).msg.t) // shown in the app, not written to the beacon
            assertEquals("00000000000000e1", (out.single() as RelayOut.Local).msg.id)
        }
    }

    @Test fun cfgInsideARelayIsDropped() {
        engine.bleChanged(true, 0)
        val cfg = beaconMsg("cfg", "00000000000000e2", buildJsonObject {
            put("n", JsonPrimitive(2)); put("b", JsonPrimitive("e30")); put("s", JsonPrimitive("x"))
        })
        val relay = Envelope.encode("relay", "00000000000000e3", 0, buildJsonObject {
            put("did", JsonPrimitive("HB-3FA2C1"))
            put("m", Json.parseToJsonElement(cfg.decodeToString()))
        })
        assertTrue(engine.serverMessage(relay, 0).isEmpty())
    }

    @Test fun beaconFramesUseRunningSequence() {
        engine.bleChanged(true, 0, mtu = 23)
        val seqs = engine.serverChanged(true, 0).filterIsInstance<RelayOut.BleSend>().map { it.frames[0][0].toInt() and 0x7F } +
            engine.serverChanged(false, 0).filterIsInstance<RelayOut.BleSend>().map { it.frames[0][0].toInt() and 0x7F }
        assertEquals(listOf(2, 3), seqs) // 0 = time, 1 = link at connect
    }

    @Test fun callerChosenIdsForMoodAndRequest() {
        engine.serverChanged(true, 0)
        val mood = published(engine.sendMood("heart", 0, id = "00000000000000aa")).single()
        assertEquals("00000000000000aa", mood.id) // the app shows it before the router's rcpt names it
        engine.bleChanged(true, 0)
        val req = toBeacon(engine.request("get", buildJsonObject { put("what", JsonPrimitive("info")) }, 0, id = "00000000000000ab")).single()
        assertEquals("00000000000000ab", req.id)
        val resp = beaconMsg("resp", "00000000000000ac", buildJsonObject { put("re", JsonPrimitive("00000000000000ab")); put("ok", JsonPrimitive(true)) })
        assertEquals("resp", (framesFromBeacon(resp, 1, 0).single() as RelayOut.Local).msg.t)
    }

    @Test fun noBeaconYetMeansNoLinkAndNothingRelayed() {
        val e = RelayEngine(null, "u1", newId = { "%016x".format(n++) })
        val up = e.serverChanged(true, 0)
        assertTrue(up.isEmpty()) // no `link{did, ble}` without a did
        assertEquals(listOf("mood"), e.sendMood("heart", 0).map { Envelope.decode((it as RelayOut.Publish).payload).t })
        val ble = e.bleChanged(true, 0)
        assertTrue(ble.none { it is RelayOut.Publish })
        val r = Reassembler()
        assertEquals(listOf("time", "link"), ble.map { o -> Envelope.decode((o as RelayOut.BleSend).frames.map { r.push(it, 0) }.last().msg!!).t })
        // Pairing: the app's own `get info` answer comes back; the beacon's own uplinks are not relayed.
        e.request("get", JsonObject(emptyMap()), 0, id = "00000000000000b0")
        val resp = beaconMsg("resp", "00000000000000b1", buildJsonObject { put("re", JsonPrimitive("00000000000000b0")); put("ok", JsonPrimitive(true)) })
        assertEquals(1, Frames.split(resp, 0, 23).flatMap { e.bleFrame(it, 0) }.size)
        val tele = beaconMsg("tele", "00000000000000b2", buildJsonObject { put("mv", JsonPrimitive(3900)) })
        assertTrue(Frames.split(tele, 1, 23).flatMap { e.bleFrame(it, 0) }.isEmpty())
    }
}
