package com.heartbeacon.app.ui.heart

import com.heartbeacon.app.data.Mood
import com.heartbeacon.core.Fx
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class CageTest {
    @Test fun geodesicHasTheRealCagesCounts() {
        assertEquals(92, Cage.nodeCount)
        assertEquals(270, Cage.rodCount)
        // every rod joins two distinct nodes, none twice
        val seen = HashSet<Pair<Int, Int>>()
        for (i in 0 until Cage.rodCount) {
            val a = Cage.rods[2 * i]
            val b = Cage.rods[2 * i + 1]
            assertTrue(a != b && a in 0 until 92 && b in 0 until 92)
            assertTrue(seen.add(minOf(a, b) to maxOf(a, b)))
        }
    }

    @Test fun nodesSitOnTheHeartSurfaceAtThePhotosProportions() {
        val min = FloatArray(3) { Float.MAX_VALUE }
        val max = FloatArray(3) { -Float.MAX_VALUE }
        for (i in 0 until Cage.nodeCount) {
            val p = Cage.toImplicit(Cage.nodes[3 * i], Cage.nodes[3 * i + 1], Cage.nodes[3 * i + 2])
            assertTrue("node $i off the surface", abs(Cage.shape(p[0], p[1], p[2])) < 1e-3)
            for (k in 0..2) {
                min[k] = minOf(min[k], Cage.nodes[3 * i + k])
                max[k] = maxOf(max[k], Cage.nodes[3 * i + k])
            }
        }
        assertEquals(2f, max[0] - min[0], 1e-3f)
        assertEquals(1.94f, max[1] - min[1], 1e-3f)
        assertEquals(1f, max[2] - min[2], 1e-3f)
        assertEquals(0f, max[0] + min[0], 1e-3f) // left-right symmetric
    }

    @Test fun ledRingIsMirrorSymmetric() {
        assertEquals(0f, Cage.leds[0], 0f)
        for (i in 1..4) {
            val j = 9 - i
            assertEquals("led $i x", -Cage.leds[3 * i], Cage.leds[3 * j], 1e-6f)
            assertEquals("led $i y", Cage.leds[3 * i + 1], Cage.leds[3 * j + 1], 1e-6f)
            assertEquals("led $i turn", -CageRenderer.LED_TURN[i], CageRenderer.LED_TURN[j], 1e-6f)
        }
    }

    @Test fun ledsAndCoreAreInsideTheCage() {
        fun inside(x: Float, y: Float, z: Float) = Cage.toImplicit(x, y, z).let { Cage.shape(it[0], it[1], it[2]) < 0 }
        for (i in 0 until 9) assertTrue("led $i", inside(Cage.leds[3 * i], Cage.leds[3 * i + 1], Cage.leds[3 * i + 2]))
        for (x in listOf(Cage.coreMin[0], Cage.coreMax[0])) for (y in listOf(Cage.coreMin[1], Cage.coreMax[1])) {
            for (z in listOf(Cage.coreMin[2], Cage.coreMax[2])) assertTrue("core corner $x $y $z", inside(x, y, z))
        }
    }
}

class TwinTest {
    private val out = IntArray(27)

    @Test fun awakeMoodPlaysAtOnceAndReplaces() {
        val t = Twin()
        t.mood(Mood.HEART, awake = true, now = 1000)
        assertTrue(t.frame(1520, 100, out))
        assertEquals(250, out[0]) // peak of the first pulse, firmware red
        t.mood(Mood.ANNOY, awake = true, now = 1600)
        assertEquals(Fx.Kind.ANNOY, t.playing!!.kind)
        assertFalse(t.frame(1600 + Fx.durationMs(Fx.Kind.ANNOY)!!, 100, out))
        assertNull(t.playing)
        assertTrue(out.all { it == 0 })
    }

    @Test fun asleepMoodsWaitAndTheLastPlaysOnWake() {
        val t = Twin()
        t.mood(Mood.HEART, awake = false, now = 0)
        t.mood(Mood.SORRY, awake = false, now = 10)
        assertEquals(2, t.waiting)
        assertFalse(t.frame(20, 100, out))
        t.woke(5000)
        assertEquals(0, t.waiting)
        assertEquals(Fx.Kind.SORRY, t.playing!!.kind)
        t.woke(9000) // a later wake with nothing held plays nothing new
        assertEquals(5000L, t.playing!!.startMs)
    }

    @Test fun customPlaysTheSendersColourAndIdleShowsEmberAndPresence() {
        val t = Twin()
        t.mood(Mood.CUSTOM, awake = true, now = 0, rgb = "#0a14c8")
        assertTrue(t.frame(520, 100, out))
        assertEquals(listOf(10, 20, 200), listOf(out[0], out[1], out[2])) // peak of the first pulse, its colour
        t.echo(10_000)
        assertEquals(Fx.Kind.ECHO, t.playing!!.kind)
        assertFalse(t.frame(10_000 + Fx.durationMs(Fx.Kind.ECHO)!!, 100, out))
        assertTrue(out.all { it == 0 })
        t.ember = true
        t.presence = true
        assertFalse(t.frame(20_000, 100, out)) // no effect, but the overlay is lit
        assertTrue(out[0] > 0 && out[4 * 3] > 0 && out[2 * 3] == 0)
    }

    @Test fun holdShowsTheBatteryBarLikeTheButton() {
        val t = Twin()
        t.battery(82, 0)
        assertEquals(7, t.playing!!.arg)
        assertTrue(t.frame(7 * 80L, 100, out))
        assertEquals(255, out[0]) // pixel 0 red
        assertEquals(0, out[7 * 3] + out[7 * 3 + 1]) // pixel 7 stays dark at 82 %
    }
}
