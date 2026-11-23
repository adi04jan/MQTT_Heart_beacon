package com.heartbeacon.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.int
import kotlinx.serialization.json.long
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.fail
import org.junit.Test

/** protocol/vectors.json (on the test classpath, see core/build.gradle.kts) against the codec. */
class VectorsTest {
    private val vec: JsonObject = Json.parseToJsonElement(
        javaClass.classLoader.getResourceAsStream("vectors.json")!!.reader(Charsets.UTF_8).readText(),
    ).jsonObject

    private fun list(a: String, b: String): List<JsonObject> = vec[a]!!.jsonObject[b]!!.jsonArray.map { it.jsonObject }
    private fun JsonObject.s(k: String) = this[k]!!.jsonPrimitive.content
    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it.toInt() and 0xFF) }
    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test fun knownTypes() {
        assertEquals(vec["envelope"]!!.jsonObject["types"]!!.jsonArray.map { it.jsonPrimitive.content }.toSet(), Envelope.TYPES)
    }

    @Test fun encodeAndRoundTrip() = list("envelope", "encode").forEach { v ->
        val name = v.s("name")
        val body = v["body"]!!.jsonObject
        val raw = Envelope.encode(v.s("t"), v.s("id"), v["ts"]!!.jsonPrimitive.long, body)
        assertEquals(name, v.s("json"), raw.decodeToString())
        assertEquals(name, Msg(v.s("t"), v.s("id"), v["ts"]!!.jsonPrimitive.long, body), Envelope.decode(raw))
    }

    @Test fun decode() = list("envelope", "decode").forEach { v ->
        val name = v.s("name")
        val raw = v.s("json").encodeToByteArray()
        if ("err" in v) {
            try {
                Envelope.decode(raw)
                fail("$name: expected ${v.s("err")}")
            } catch (e: ProtocolException) {
                assertEquals(name, v.s("err"), e.code)
            }
        } else {
            val want = Msg(v.s("t"), v.s("id"), v["ts"]!!.jsonPrimitive.long, v["body"]!!.jsonObject)
            assertEquals(name, want, Envelope.decode(raw))
        }
    }

    @Test fun unknownTypeGetsErrorResponse() = list("envelope", "unknown_t").forEach { v ->
        val req = Envelope.decode(v.s("json").encodeToByteArray())
        val got = Envelope.unknownTypeResponse(req, v.s("resp_id"), v["resp_ts"]!!.jsonPrimitive.long)
        if (v["resp_json"] is JsonNull) assertNull(v.s("name"), got)
        else assertEquals(v.s("name"), v.s("resp_json"), got!!.decodeToString())
    }

    @Test fun didFromMac() = list("envelope", "did").forEach { v ->
        assertEquals(v.s("did"), Envelope.didFromMac(unhex(v.s("mac"))))
    }

    @Test(expected = ProtocolException::class) fun encodeRefusesOversize() {
        Envelope.encode("mood", "0123456789abcdef", 0, JsonObject(mapOf("pad" to Json.parseToJsonElement("\"" + "x".repeat(1024) + "\""))))
    }

    @Test fun splitAndReassemble() = list("frame", "split").forEach { v ->
        val name = v.s("name")
        val msg = v.s("msg").encodeToByteArray()
        val frames = Frames.split(msg, v["seq"]!!.jsonPrimitive.int, v["mtu"]!!.jsonPrimitive.int)
        assertEquals(name, (v["frames"] as JsonArray).map { it.jsonPrimitive.content }, frames.map(::hex))
        val r = Reassembler()
        val outs = frames.mapIndexed { i, f -> r.push(f, 10L * i) }
        outs.dropLast(1).forEach { assertEquals(name, Reassembler.Out.PARTIAL, it.out) }
        assertEquals(name, Reassembler.Out.COMPLETE, outs.last().out)
        assertArrayEquals(name, msg, outs.last().msg)
    }

    @Test fun splitRejects() = list("frame", "split_reject").forEach { v ->
        try {
            Frames.split(v.s("msg").encodeToByteArray(), v["seq"]!!.jsonPrimitive.int, v["mtu"]!!.jsonPrimitive.int)
            fail("${v.s("name")}: expected rejection")
        } catch (e: IllegalArgumentException) {
            // expected
        }
    }

    @Test fun reassembleSteps() = list("frame", "reassemble").forEach { v ->
        val r = Reassembler()
        v["steps"]!!.jsonArray.map { it.jsonObject }.forEachIndexed { i, s ->
            val name = "${v.s("name")} step $i"
            val res = r.push(unhex(s.s("frame")), s["at"]!!.jsonPrimitive.long)
            assertEquals(name, s.s("out"), res.out.name.lowercase())
            if (res.out == Reassembler.Out.COMPLETE) assertArrayEquals(name, s.s("msg").encodeToByteArray(), res.msg)
            else assertNull(name, res.msg)
        }
    }
}
