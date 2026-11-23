package com.heartbeacon.core

import com.heartbeacon.core.Handover.Phase
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class HandoverTest {
    private val h = Handover()

    @Test fun nearOnlyAfterLinkUpHeld30s() {
        assertEquals(Phase.AWAY, h.phase(0))
        h.update(ble = true, server = true, now = 1_000)
        assertEquals(Phase.ENTERING, h.phase(30_999))
        assertEquals(31_000L, h.nextChange(30_999))
        assertEquals(Phase.NEAR, h.phase(31_000))
        assertNull(h.nextChange(31_000))
    }

    @Test fun serverFlapRestartsTheHold() {
        h.update(true, true, 0)
        h.update(true, false, 20_000)
        h.update(true, true, 21_000)
        assertEquals(Phase.ENTERING, h.phase(50_000))
        assertEquals(Phase.NEAR, h.phase(51_000))
    }

    @Test fun linkLostWaitsGraceThenAway() {
        h.update(true, true, 0)
        h.update(false, true, 100_000)
        assertEquals(Phase.LEAVING, h.phase(159_999))
        assertEquals(160_000L, h.nextChange(159_999))
        assertEquals(Phase.AWAY, h.phase(160_000))
        // A server change while BLE is down does not extend the grace.
        h.update(false, false, 150_000)
        assertEquals(Phase.AWAY, h.phase(160_000))
    }

    @Test fun upstreamDown60sMeansBeaconStartsWifi() {
        h.update(true, true, 0)
        h.update(true, false, 40_000)
        assertEquals(Phase.LEAVING, h.phase(99_999))
        assertEquals(100_000L, h.nextChange(40_000))
        assertEquals(Phase.AWAY, h.phase(100_000))
    }

    @Test fun startGateFlapsGiveOneStart() {
        val gate = StartGate()
        // Four presence callbacks in 11 s (the reference app's measured flap): one start.
        assertEquals(listOf(true, false, false, false), List(4) { gate.claim() })
        gate.stopped()
        assertEquals(true, gate.claim())
    }

    @Test fun startGateIsSingleFlightAcrossThreads() {
        val gate = StartGate()
        val wins = AtomicInteger()
        val go = CountDownLatch(1)
        val pool = Executors.newFixedThreadPool(16)
        repeat(64) { pool.execute { go.await(); if (gate.claim()) wins.incrementAndGet() } }
        go.countDown()
        pool.shutdown()
        pool.awaitTermination(10, TimeUnit.SECONDS)
        assertEquals(1, wins.get())
    }
}
