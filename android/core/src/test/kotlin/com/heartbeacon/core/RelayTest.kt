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
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id) }
        val out = engine.bleChanged(true, 1_700_000_000_000, mtu = 247)
        val sent = toBeacon(out)
        assertEquals(listOf("time", "link"), sent.map { it.t })
        assertEquals(1_700_000_000L, sent[0].ts)
        assertEquals(330, sent[0].body["tz"]!!.jsonPrimitive.content.toInt())
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
        val state = beaconMsg("state", "00000000000000ab", buildJsonObject { put("st", JsonPrimitive("NEAR")); put("path", JsonPrimitive("ble")) })
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
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id) }
        val first = engine.sendMood("heart", 1_000).single() as RelayOut.Publish
        assertTrue(engine.tick(5_999).isEmpty())
        val out = engine.tick(6_000)
        assertEquals(RelayOut.ReconnectServer, out[0])
        assertEquals(false, toBeacon(out).single().body["up"]!!.jsonPrimitive.boolean) // beacon falls back to WiFi
        assertTrue(!engine.serverUp)
        val again = engine.serverChanged(true, 7_000).filterIsInstance<RelayOut.Publish>().last()
        assertEquals(first.id, again.id)
        assertTrue(first.payload.contentEquals(again.payload))
    }

    @Test fun pubackStopsTheTimer() {
        engine.serverChanged(true, 0).forEach { if (it is RelayOut.Publish) engine.published(it.id) }
        val p = engine.sendMood("annoy", 0).single() as RelayOut.Publish
        engine.published(p.id)
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

    @Test fun beaconFramesUseRunningSequence() {
        engine.bleChanged(true, 0, mtu = 23)
        val seqs = engine.serverChanged(true, 0).filterIsInstance<RelayOut.BleSend>().map { it.frames[0][0].toInt() and 0x7F } +
            engine.serverChanged(false, 0).filterIsInstance<RelayOut.BleSend>().map { it.frames[0][0].toInt() and 0x7F }
        assertEquals(listOf(2, 3), seqs) // 0 = time, 1 = link at connect
    }
}
