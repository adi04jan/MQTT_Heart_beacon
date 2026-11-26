package com.heartbeacon.core

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class FxTest {
    /** vectors.json "fx" is generated from hb_fx.c: the phone plays the firmware's frames, byte for byte. */
    @Test fun framesMatchFirmware() {
        val vec = Json.parseToJsonElement(javaClass.classLoader.getResourceAsStream("vectors.json")!!.reader().readText())
        val cases = vec.jsonObject["fx"]!!.jsonObject["cases"]!!.jsonArray.map { it.jsonObject }
        assertTrue(cases.size > 90)
        val out = IntArray(Fx.PIXELS * 3)
        for (c in cases) {
            val kind = Fx.Kind.entries.first { it.wire == c["kind"]!!.jsonPrimitive.content }
            val t = c["t"]!!.jsonPrimitive.int.toLong()
            val level = c["level"]?.jsonPrimitive?.int ?: 100
            val color = c["color"]?.jsonArray?.map { it.jsonPrimitive.int }?.toIntArray()
            val more = Fx.frame(kind, c["arg"]!!.jsonPrimitive.int, t, c["bright"]!!.jsonPrimitive.int, out, level, color)
            val want = c["rgb"]!!.jsonArray.flatMap { px -> px.jsonArray.map { it.jsonPrimitive.int } }.toIntArray()
            assertEquals("$kind t=$t more", c["more"]!!.jsonPrimitive.boolean, more)
            assertArrayEquals("$kind t=$t", want, out)
        }
    }

    @Test fun durationsEndWhereFramesEnd() {
        val out = IntArray(27)
        for (k in listOf(Fx.Kind.HEART, Fx.Kind.SORRY, Fx.Kind.ANNOY, Fx.Kind.LOW, Fx.Kind.RESET, Fx.Kind.ECHO,
            Fx.Kind.BREATH, Fx.Kind.CELEBRATE, Fx.Kind.CUSTOM)) {
            val d = Fx.durationMs(k)!!
            assertTrue(Fx.frame(k, 0, d - 1, 100, out))
            assertEquals(false, Fx.frame(k, 0, d, 100, out))
        }
        val d = Fx.durationMs(Fx.Kind.BATT, 6)!!
        assertTrue(Fx.frame(Fx.Kind.BATT, 6, d - 1, 100, out))
        assertEquals(false, Fx.frame(Fx.Kind.BATT, 6, d, 100, out))
        assertNull(Fx.durationMs(Fx.Kind.PAIR))
    }

    @Test fun barPixelsFloorsLikeTheFirmware() {
        assertEquals(listOf(0, 0, 0, 1, 7, 8, 9), listOf(-5, 0, 11, 12, 82, 99, 100).map(Fx::barPixels))
    }
}
