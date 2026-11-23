package com.heartbeacon.core

import java.util.concurrent.atomic.AtomicBoolean

/**
 * The beacon's transport phase as the phone can predict it from PROTOCOL.md section 4, so the beacon
 * card says "Near" without waiting for the router. Time is injected; the router's `state` stays the
 * authority whenever the phone is not the gateway.
 */
class Handover(
    private val nearHoldMs: Long = 30_000,
    private val awayGraceMs: Long = 60_000,
    private val upstreamDownMs: Long = 60_000,
) {
    enum class Phase { NEAR, ENTERING, LEAVING, AWAY }

    private var ble = false
    private var server = false
    private var since = 0L // when the current (ble, server) pair began
    private var bleLostAt: Long? = null

    fun update(ble: Boolean, server: Boolean, now: Long) {
        if (this.ble && !ble) bleLostAt = now
        if (ble) bleLostAt = null
        if (ble != this.ble || server != this.server) since = now
        this.ble = ble
        this.server = server
    }

    fun phase(now: Long): Phase = when {
        // Enter NEAR: bonded link + link up held near_hold_s, then the beacon stops WiFi.
        ble && server -> if (now - since >= nearHoldMs) Phase.NEAR else Phase.ENTERING
        // Phone upstream down 60 s: the beacon starts WiFi while keeping BLE.
        ble -> if (now - since >= upstreamDownMs) Phase.AWAY else Phase.LEAVING
        // Leave NEAR: link lost, the beacon waits away_grace_s before starting WiFi.
        bleLostAt.let { it != null && now - it < awayGraceMs } -> Phase.LEAVING
        else -> Phase.AWAY
    }

    /** When [phase] flips next if nothing else happens, or null when it is settled. */
    fun nextChange(now: Long): Long? = when (phase(now)) {
        Phase.ENTERING -> since + nearHoldMs
        Phase.LEAVING -> if (ble) since + upstreamDownMs else bleLostAt!! + awayGraceMs
        else -> null
    }
}

/**
 * Single-flight start for the link service. CDM presence flaps several times in seconds (four in 11 s
 * in the reference app), and BOOT_COMPLETED, app open and presence can race: exactly one caller gets
 * true until the service reports [stopped].
 */
class StartGate {
    private val running = AtomicBoolean(false)

    fun claim(): Boolean = running.compareAndSet(false, true)

    fun stopped() = running.set(false)
}
