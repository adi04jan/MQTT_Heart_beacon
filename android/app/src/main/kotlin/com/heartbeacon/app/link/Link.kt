package com.heartbeacon.app.link

import android.annotation.SuppressLint
import android.content.Context
import android.provider.Settings
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
    }

    private val loop = Executors.newSingleThreadScheduledExecutor()
    private val engine = RelayEngine(did, account.uid, tzMin = { TimeZone.getDefault().getOffset(now()) / 60_000 })
    private val handover = Handover()
    private val answers = ConcurrentHashMap<String, CompletableDeferred<Msg>>()
    private var gatt: GattLink? = null // loop thread only
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
        override fun onFrame(frame: ByteArray) = step { bleFrame(frame, now()) }
    }

    init {
        mqtt.connect()
        loop.scheduleWithFixedDelay({ step { tick(now()) } }, 1, 1, TimeUnit.SECONDS) // the engine's 5 s ack and 10 s hold timers
    }

    private fun now() = System.currentTimeMillis()

    /** RejectedExecutionException after [close]: nothing left to do. */
    private fun onLoop(block: () -> Unit) {
        runCatching { loop.execute(block) }
    }

    private var lastServer: Boolean? = null // loop thread only

    private fun step(block: RelayEngine.() -> List<RelayOut>) = onLoop {
        engine.block().forEach(::run)
        _ble.value = engine.bleUp
        if (engine.serverUp != lastServer) {
            lastServer = engine.serverUp
            out.server(engine.serverUp)
        }
        handover.update(engine.bleUp, engine.serverUp, now())
        out.near(handover.phase(now()) == Handover.Phase.NEAR)
    }

    private fun run(o: RelayOut) {
        when (o) {
            is RelayOut.BleSend -> gatt?.send(o.frames)
            is RelayOut.Publish -> mqtt.publish(o.topic, o.payload, o.id)
            is RelayOut.Local -> {
                val re = if (o.msg.t == "resp") o.msg.body["re"]?.jsonPrimitive?.content else null
                answers.remove(re)?.complete(o.msg) ?: out.local(o.msg)
            }
            RelayOut.ReconnectServer -> mqtt.reconnect()
        }
    }

    /** Open the GATT: [autoConnect] false for the first connect after the chooser (plan), true after. */
    fun openBle(mac: String, autoConnect: Boolean) = onLoop {
        gatt?.close()
        gatt = GattLink(ctx, mac, gattListener).also { it.open(autoConnect) }
    }

    /** Closing a GATT reports nothing, so the engine is told here. */
    fun closeBle() {
        onLoop {
            gatt?.close()
            gatt = null
        }
        step { bleChanged(false, now()) }
    }

    suspend fun awaitBle(timeoutMs: Long): Boolean = withTimeoutOrNull(timeoutMs) { ble.first { it } } ?: false

    fun sendMood(k: String, id: String) = step { sendMood(k, now(), id) }

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
        /** The `{install}` in the MQTT client id: stable per app install and user (Android scopes it so). */
        @SuppressLint("HardwareIds")
        fun installId(ctx: Context): String = Settings.Secure.getString(ctx.contentResolver, Settings.Secure.ANDROID_ID) ?: "0"
    }
}
