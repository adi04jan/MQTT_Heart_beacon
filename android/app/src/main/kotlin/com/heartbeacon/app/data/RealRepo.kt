package com.heartbeacon.app.data

import android.content.Context
import android.content.Intent
import android.util.Log
import com.heartbeacon.app.link.BeaconService
import com.heartbeacon.app.link.Cdm
import com.heartbeacon.app.link.Link
import com.heartbeacon.core.Ids
import com.heartbeacon.core.Msg
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import javax.net.ssl.SSLSocketFactory

/**
 * The real [HeartRepo]: the API ([HttpApi]) for account, claim, settings and history; one [Link] (MQTT +
 * :core relay, plus the GATT while [BeaconService] hosts it) for live hearts, receipts and beacon status;
 * the sealed [Creds] for the session. Any 401 from the API signs the user out.
 */
class RealRepo(
    private val ctx: Context,
    private val scope: CoroutineScope,
    private val store: SettingsStore,
    private val creds: Creds,
    private val tls: SSLSocketFactory,
) : HeartRepo {
    override val session = MutableStateFlow<Session?>(null)
    override val server = MutableStateFlow(ServerLink.CONNECTING)
    override val beacon = MutableStateFlow<Beacon?>(null)
    override val events = MutableStateFlow<List<Event>>(emptyList())
    override val settings = MutableStateFlow<DeviceSettings?>(null)

    @Volatile private var account: Account? = null
    @Volatile private var api: HttpApi? = null
    @Volatile private var link: Link? = null
    @Volatile private var did: String? = null
    @Volatile private var mac: String? = null
    @Volatile private var hosted = false // BeaconService is up and wants the GATT open
    @Volatile private var near = false
    private var raw: JsonObject? = null // last GET settings, for keys the screens do not model
    private var fw = ""
    private var fwTarget: String? = null
    private var wifi = emptyList<String>()

    init {
        // ponytail: two DataStore reads + a Keystore decrypt on the main thread at startup, so a signed-in
        // user never sees the login screen flash and BeaconService (boot, presence) finds the beacon at once.
        // Move behind a splash state if it ever shows up in traces.
        val (a, s) = runBlocking { creds.load() to store.flow.first() }
        mac = s.beaconMac
        did = s.did
        a?.let(::start)
    }

    private fun apiFor(host: String): HttpApi =
        api?.takeIf { it.base == HttpApi.baseUrl(host) } ?: HttpApi(host, tls).also { api = it }

    private fun start(a: Account) {
        account = a
        apiFor(a.host).token = a.token
        session.value = Session(a.uid, a.host)
        relink()
        scope.launch { refresh() }
    }

    /** A new [Link] for the current account and beacon; the GATT reopens if the service hosts it. */
    private fun relink() {
        link?.close()
        val a = account ?: return
        server.value = ServerLink.CONNECTING
        link = Link(ctx, a, did, tls, linkOut).also { l ->
            val m = mac
            if (hosted && m != null && did != null) l.openBle(m, autoConnect = true)
        }
    }

    private val linkOut = object : Link.Out {
        override fun server(up: Boolean) {
            val was = server.value
            server.value = if (up) ServerLink.CONNECTED else ServerLink.OFFLINE
            if (up && was != ServerLink.CONNECTED) scope.launch { refresh() } // pick up what happened while away
        }

        override fun local(m: Msg) = onLocal(m)

        override fun near(near: Boolean) {
            if (near == this@RealRepo.near) return
            this@RealRepo.near = near
            if (near) beacon.update { it?.copy(presence = Presence.Near, lastSeenEpochMs = System.currentTimeMillis()) }
        }
    }

    private fun onLocal(m: Msg) {
        val now = System.currentTimeMillis()
        val body = m.body
        fun str(k: String) = (body[k] as? JsonPrimitive)?.contentOrNull
        when (m.t) {
            "mood" -> {
                val mood = moodOf(str("k")) ?: return
                val from = str("from") ?: "Someone"
                val e = Event(m.id, mood, from, if (m.ts > 0) m.ts * 1000 else now, null)
                events.update { list -> if (list.any { it.id == e.id }) list else (listOf(e) + list).take(MAX_EVENTS) }
                scope.launch { notifyMood(m.id, from, mood) }
            }
            "rcpt" -> {
                val mark = when (str("st")) {
                    "shown" -> Mark.SHOWN
                    "expired" -> Mark.EXPIRED
                    "queued" -> Mark.QUEUED
                    else -> return
                }
                events.update { list ->
                    list.map { e -> if (e.id != m.id || e.mark == Mark.SHOWN || (mark == Mark.QUEUED && e.mark != null)) e else e.copy(mark = mark) }
                }
            }
            "state" -> {
                val p = Presence.of(str("st") ?: return, (body["wake"] as? JsonPrimitive)?.longOrNull ?: 0) ?: return
                beacon.update { it?.copy(presence = if (near) Presence.Near else p, lastSeenEpochMs = now) }
            }
            "tele" -> {
                (body["fw"] as? JsonPrimitive)?.contentOrNull?.let { v -> fw = v; settings.update { it?.copy(fw = v) } }
                val pct = (body["pct"] as? JsonPrimitive)?.intOrNull
                val rssi = (body["rssi"] as? JsonPrimitive)?.intOrNull
                beacon.update { it?.copy(batteryPct = pct ?: it.batteryPct, rssi = rssi ?: it.rssi, lastSeenEpochMs = now) }
            }
        }
    }

    private suspend fun notifyMood(id: String, from: String, mood: Mood) {
        if (store.flow.first().notify[mood] != true) return
        BeaconService.notifyMood(ctx, id, from, mood)
    }

    /** Pull account, beacon, history and settings. Offline keeps what is shown; a 401 signs out. */
    private suspend fun refresh() {
        val a = account ?: return
        val api = api ?: return
        try {
            val me = api.me()
            val devices = me["devices"] as? JsonArray
            val mine = devices.orEmpty().mapNotNull { it as? JsonObject }
            val dev = mine.firstOrNull { (it["did"] as? JsonPrimitive)?.contentOrNull == did } ?: mine.firstOrNull()
            val newDid = dev?.let { (it["did"] as JsonPrimitive).content }
            if (newDid != did) {
                did = newDid
                store.setBeacon(mac, newDid)
                relink()
            }
            beacon.value = dev?.let { d ->
                fw = (d["tele"] as? JsonObject)?.get("fw")?.jsonPrimitive?.contentOrNull ?: fw
                beaconOf(d).let { if (near) it.copy(presence = Presence.Near) else it }
            }
            val fetched = eventsOf(api.events(MAX_EVENTS), a.uid)
            events.update { live ->
                val ids = fetched.map { it.id }.toSet()
                (live.filter { it.id !in ids && it.mark == Mark.QUEUED } + fetched).sortedByDescending { it.atEpochMs }.take(MAX_EVENTS)
            }
            settings.value = newDid?.let { d -> api.settings(d).also { raw = it }.let { settingsOf(it, fw, wifi, fwTarget) } }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            failed(e)
        }
    }

    /** A 401 means the token is gone server-side: back to the login screen. Everything else is logged. */
    private suspend fun failed(e: Exception) {
        Log.w(TAG, "api: ${e.message}")
        if (e is ApiError && e.code == 401) signOut()
    }

    override suspend fun login(user: String, pass: String, host: String): String? {
        val h = host.trim()
        return try {
            val api = apiFor(h)
            val l = api.login(user.trim(), pass)
            val a = Account(h, l.uid, l.name, l.token, l.mqttPort, l.mqttUser, l.mqttPass)
            creds.save(a)
            start(a)
            null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            userMessage(e)
        }
    }

    override fun logout() {
        scope.launch { signOut() }
    }

    private suspend fun signOut() {
        link?.close()
        link = null
        account = null
        api?.token = null
        creds.wipe()
        session.value = null
        beacon.value = null
        events.value = emptyList()
        settings.value = null
        ctx.stopService(Intent(ctx, BeaconService::class.java))
    }

    override fun send(mood: Mood) {
        val id = Ids.random()
        events.update { (listOf(Event(id, mood, null, System.currentTimeMillis(), Mark.QUEUED)) + it).take(MAX_EVENTS) }
        link?.sendMood(mood.wire, id)
    }

    override fun update(s: DeviceSettings) {
        val d = did ?: return
        val api = api ?: return
        val old = settings.value
        settings.value = s
        beacon.update { it?.copy(name = s.name.ifBlank { d }) }
        scope.launch {
            try {
                val cfg = api.saveSettings(d, settingsBody(s, raw?.let(::napMinOf)))
                fwTarget = fwTargetOf(cfg) ?: fwTarget
                link?.request("cfg", cfgBody(cfg)) // over BLE when near; the router's retained copy covers WiFi
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                settings.value = old
                beacon.update { it?.copy(name = old?.name?.ifBlank { d } ?: it.name) }
                failed(e)
            }
        }
    }

    // ---- beacon over BLE ----------------------------------------------------------------

    /** Called by [BeaconService]: open (or close) the always-on GATT. False when there is nothing to host. */
    fun host(on: Boolean): Boolean {
        hosted = on
        val l = link ?: return false
        val m = mac ?: return false
        if (did == null) return false
        if (on) l.openBle(m, autoConnect = true) else l.closeBle()
        return true
    }

    override suspend fun pair(mac: String?): String? {
        val m = mac ?: return null
        val l = link ?: return null
        this.mac = m
        store.setBeacon(m, did)
        l.openBle(m, autoConnect = false) // first connect after the chooser (plan, Connection row)
        if (!l.awaitBle(PAIR_MS)) return null
        val r = l.request("get", buildJsonObject { put("what", "info") }) ?: return null
        val d = r.body["d"] as? JsonObject ?: return null
        val minPv = (d["min_pv"] as? JsonPrimitive)?.intOrNull ?: 1
        if (minPv > PROTOCOL_VERSION) return null // PROTOCOL.md 6: refuse to operate below min_pv
        return (d["did"] as? JsonPrimitive)?.contentOrNull
    }

    override suspend fun scanWifi(): List<WifiNet> =
        netsOf(link?.request("wifi.scan", JsonObject(emptyMap()), SCAN_MS)?.body?.get("d") as? JsonObject)

    override suspend fun joinWifi(ssid: String, psk: String): String? {
        val r = link?.request("wifi.set", buildJsonObject { put("ssid", ssid); put("psk", psk) }, JOIN_MS)
            ?: return NO_BEACON
        if ((r.body["ok"] as? JsonPrimitive)?.booleanOrNull != true) {
            return "It could not join ${ssid}: ${(r.body["err"] as? JsonPrimitive)?.contentOrNull ?: "no reason given"}."
        }
        wifi = (wifi + ssid).distinct()
        settings.update { it?.copy(wifi = wifi) }
        return null
    }

    override fun forgetWifi(ssid: String) {
        wifi = wifi - ssid
        settings.update { it?.copy(wifi = wifi) }
        scope.launch { link?.request("wifi.forget", buildJsonObject { put("ssid", ssid) }) }
    }

    /**
     * Plan claim flow 3-5: the server claim (beacon credentials + signed cfg), then `claim` and `cfg` to
     * the beacon over BLE, then the name, then the always-on service. Null on success.
     */
    override suspend fun claim(did: String, name: String): String? {
        val api = api ?: return userMessage(ApiError(401, "login"))
        val l = link ?: return NO_BEACON
        return try {
            val c = api.claim(did)
            val ok = l.request("claim", c.beacon)
            if (ok == null || (ok.body["ok"] as? JsonPrimitive)?.booleanOrNull != true) return NO_BEACON
            l.request("cfg", cfgBody(c.cfg))
            val cfg = api.saveSettings(did, buildJsonObject { put("name", name) })
            fwTarget = fwTargetOf(cfg)
            l.request("cfg", cfgBody(cfg))
            this.did = did
            store.setBeacon(mac, did)
            relink()
            refresh()
            BeaconService.start(ctx)
            null
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            if (e is ApiError && e.code == 401) signOut()
            userMessage(e, did)
        }
    }

    /** Plan "Remove": server unclaim, factory reset over BLE (best effort), forget the association. */
    override fun remove() {
        val d = did ?: return
        scope.launch {
            try {
                api?.unclaim(d)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                failed(e)
            }
            link?.request("reset", buildJsonObject { put("what", "factory") })
            mac?.let { Cdm.forget(ctx, it) }
            ctx.stopService(Intent(ctx, BeaconService::class.java))
            mac = null
            did = null
            store.setBeacon(null, null)
            beacon.value = null
            settings.value = null
            relink()
        }
    }

    /** For the debug switch to [FakeRepo]: drop the live link. */
    fun close() {
        link?.close()
        link = null
    }

    companion object {
        private const val TAG = "RealRepo"
        private const val MAX_EVENTS = 50
        private const val PROTOCOL_VERSION = 1
        private const val PAIR_MS = 60_000L // bonding includes the system "Pair?" prompt
        private const val SCAN_MS = 20_000L
        private const val JOIN_MS = 30_000L
        private const val NO_BEACON = "Your beacon did not answer over Bluetooth. Keep it close and try again."
    }
}
