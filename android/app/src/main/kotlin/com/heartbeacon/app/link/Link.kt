package com.heartbeacon.app.link

import android.annotation.SuppressLint
import android.content.Context
import android.provider.Settings
import android.util.Log
import com.heartbeacon.app.data.Account
import com.heartbeacon.app.data.HttpApi
import com.heartbeacon.core.Handover
import com.heartbeacon.core.Ids
import com.heartbeacon.core.Msg
import com.heartbeacon.core.RelayEngine
import com.heartbeacon.core.RelayOut
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.net.URL
import java.util.TimeZone
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLSocketFactory

/**
 * One MQTT session, at most one GATT to the beacon, and :core's [RelayEngine] between them, for one
 * account and one claimed beacon ([did], null when there is none yet). Every engine call runs on one
 * thread ([loop]) in arrival order: the engine is single-threaded logic and BLE frame order matters.
 * The repo makes a new Link when the account or the beacon changes.
 */
class Link(
    private val ctx: Context,
    account: Account,
    did: String?,
    tls: SSLSocketFactory,
    private val out: Out,
) {
    interface Out {
        fun server(up: Boolean)
        /** Moods, receipts and the beacon's `state` / `tele` for the app. Answers to [request] go to the caller. */
        fun local(m: Msg)
        /** The predicted phase (Handover) says the beacon is NEAR this phone. */
        fun near(near: Boolean)
        /** The beacon refused this phone's bond: it needs pairing again (hold the button 5 s). */
        fun bondLost() {}
    }

    private val loop = Executors.newSingleThreadScheduledExecutor()
    private val engine = RelayEngine(did, account.uid, tzMin = { TimeZone.getDefault().getOffset(now()) / 60_000 })
    private val handover = Handover()
    private val answers = ConcurrentHashMap<String, CompletableDeferred<Msg>>()
    private var gatt: GattLink? = null // loop thread only
    private var mac: String? = null // loop thread only: the beacon to reopen after a lost bond
    private val _ble = MutableStateFlow(false)
    val ble: StateFlow<Boolean> = _ble

    private val mqtt = MqttLink(
        // The address the API answered on (and whose IP SAN the TLS check accepted), not the server's own idea of it.
        URL(HttpApi.baseUrl(account.host)).host,
        account.mqttPort, account.uid, installId(ctx), account.mqttUser, account.mqttPass.encodeToByteArray(), tls,
        object : MqttLink.Listener {
            override fun onServer(up: Boolean) = step { serverChanged(up, now()) }
            override fun onMessage(payload: ByteArray) = step { serverMessage(payload, now()) }
            override fun onPublished(id: String) = step { published(id, now()) }
        },
    )

    private val gattListener = object : GattLink.Listener {
        override fun onLink(up: Boolean, mtu: Int) = step { bleChanged(up, now(), mtu) }
        override fun onBondLost() = onLoop {
            out.bondLost()
            gatt?.close()
            gatt = null
            step { bleChanged(false, now()) }
            // Try again in 30 s: by then the owner may have held the button (the beacon's pairing window).
            runCatching { loop.schedule({ onLoop { reopen() } }, BOND_RETRY_S, TimeUnit.SECONDS) }
        }

        override fun onFrame(frame: ByteArray) = step {
            bleFrame(frame, now()).also { Log.d(TAG, "ble frame ${frame.size} B, seq ${frame.firstOrNull()}: ${it.size} out") }
        }
    }

    init {
        mqtt.connect()
        loop.scheduleWithFixedDelay({ step { tick(now()) } }, 1, 1, TimeUnit.SECONDS) // the engine's 5 s ack and 10 s hold timers
    }

    private fun now() = System.currentTimeMillis()

    /** RejectedExecutionException after [close]: nothing left to do. */
    private fun onLoop(block: () -> Unit) {
        // A scheduled executor keeps a task's exception to itself: log it, or a bug just stops work silently.
        runCatching { loop.execute { runCatching(block).onFailure { Log.e(TAG, "link loop", it) } } }
    }

    private var lastServer: Boolean? = null // loop thread only

    private fun step(block: RelayEngine.() -> List<RelayOut>) = onLoop {
        engine.block().forEach(::run)
        if (engine.bleUp != _ble.value) Log.d(TAG, "ble ${engine.bleUp}: ${engine.debug(now())}")
        _ble.value = engine.bleUp
        if (engine.serverUp != lastServer) {
            Log.d(TAG, "server ${engine.serverUp}: ${engine.debug(now())}")
            lastServer = engine.serverUp
            out.server(engine.serverUp)
        }
        handover.update(engine.bleUp, engine.serverUp, now())
        out.near(handover.phase(now()) == Handover.Phase.NEAR)
    }

    private fun run(o: RelayOut) {
        when (o) {
            is RelayOut.BleSend -> gatt?.send(o.frames)
            is RelayOut.Publish -> {
                Log.d(TAG, "publish ${o.id} ${o.payload.size} B")
                mqtt.publish(o.topic, o.payload, o.id)
            }
            is RelayOut.Local -> {
                // ConcurrentHashMap.remove(null) throws, and an exception here silently skips the rest of
                // the step's outputs (the relay publish after a beacon's state / tele): only look up a resp.
                val re = if (o.msg.t == "resp") o.msg.body["re"]?.jsonPrimitive?.content else null
                re?.let(answers::remove)?.complete(o.msg) ?: out.local(o.msg)
            }
            RelayOut.ReconnectServer -> {
                Log.w(TAG, "no PUBACK in 5 s: reconnecting (${engine.debug(now())})")
                mqtt.reconnect()
            }
        }
    }

    /** Open the GATT: [autoConnect] false for the first connect after the chooser (plan), true after. */
    fun openBle(mac: String, autoConnect: Boolean) = onLoop {
        this.mac = mac
        gatt?.close()
        gatt = GattLink(ctx, mac, gattListener).also { it.open(autoConnect) }
    }

    private fun reopen() {
        val m = mac ?: return
        if (gatt != null) return
        gatt = GattLink(ctx, m, gattListener).also { it.open(autoConnect = true) }
    }

    /** Closing a GATT reports nothing, so the engine is told here. */
    fun closeBle() {
        onLoop {
            mac = null
            gatt?.close()
            gatt = null
        }
        step { bleChanged(false, now()) }
    }

    suspend fun awaitBle(timeoutMs: Long): Boolean = withTimeoutOrNull(timeoutMs) { ble.first { it } } ?: false

    fun sendMood(k: String, id: String, extra: JsonObject = JsonObject(emptyMap())) = step { sendMood(k, now(), id, extra) }

    /** A request to the beacon over BLE; its `resp`, or null when BLE is down or it does not answer in time. */
    suspend fun request(t: String, body: JsonObject, timeoutMs: Long = 10_000): Msg? {
        if (!ble.value) return null
        val id = Ids.random()
        val answer = CompletableDeferred<Msg>().also { answers[id] = it }
        step { request(t, body, now(), id) }
        return withTimeoutOrNull(timeoutMs) { answer.await() }.also { answers.remove(id) }
    }

    fun close() {
        onLoop {
            gatt?.close()
            mqtt.close()
        }
        loop.shutdown()
    }

    companion object {
        private const val TAG = "Link"
        private const val BOND_RETRY_S = 30L

        /** The `{install}` in the MQTT client id: stable per app install and user (Android scopes it so). */
        @SuppressLint("HardwareIds")
        fun installId(ctx: Context): String = Settings.Secure.getString(ctx.contentResolver, Settings.Secure.ANDROID_ID) ?: "0"
    }
}
