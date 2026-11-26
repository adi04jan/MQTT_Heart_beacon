package com.heartbeacon.app.link

import android.util.Log
import org.eclipse.paho.mqttv5.client.IMqttToken
import org.eclipse.paho.mqttv5.client.MqttActionListener
import org.eclipse.paho.mqttv5.client.MqttAsyncClient
import org.eclipse.paho.mqttv5.client.MqttCallback
import org.eclipse.paho.mqttv5.client.MqttConnectionOptions
import org.eclipse.paho.mqttv5.client.MqttDisconnectResponse
import org.eclipse.paho.mqttv5.client.persist.MemoryPersistence
import org.eclipse.paho.mqttv5.common.MqttException
import org.eclipse.paho.mqttv5.common.MqttMessage
import org.eclipse.paho.mqttv5.common.packet.MqttProperties
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLSocketFactory

/**
 * The phone's MQTT 5 session (PROTOCOL.md section 5): client id `u-{uid}-{install}`, persistent session
 * with 24 h expiry, keepalive 120 s, LWT `{"on":false}` retained on `u/{uid}/state`, QoS 1 everywhere.
 * TLS trusts only the bundled private CA and checks the leaf with [Tls.host] (IP SAN).
 */
class MqttLink(
    host: String,
    port: Int,
    uid: String,
    install: String,
    private val user: String,
    private val pass: ByteArray,
    private val socketFactory: SSLSocketFactory,
    private val listener: Listener,
) {
    interface Listener {
        fun onServer(up: Boolean)
        fun onMessage(payload: ByteArray)
        /** PUBACK for the publish tagged [id]. */
        fun onPublished(id: String)
    }

    private val stateTopic = "hb/v1/u/$uid/state"
    private val downTopic = "hb/v1/u/$uid/down"
    private val client = MqttAsyncClient("ssl://$host:$port", "u-$uid-$install", MemoryPersistence())
    private val retry = Executors.newSingleThreadScheduledExecutor()
    @Volatile private var backoffS = 1L
    @Volatile private var closed = false

    private val options = MqttConnectionOptions().apply {
        userName = user
        password = pass
        isCleanStart = false
        sessionExpiryInterval = 86_400L
        keepAliveInterval = 120
        isAutomaticReconnect = true
        setAutomaticReconnectDelay(1, 120)
        setWill(stateTopic, MqttMessage("""{"on":false}""".toByteArray()).apply { qos = 1; isRetained = true })
        socketFactory = this@MqttLink.socketFactory
        // One check for API and broker: Tls.host (IP SAN). The JSSE "HTTPS" endpoint check is off so the
        // result does not depend on how the platform's provider treats an IP literal.
        isHttpsHostnameVerificationEnabled = false
        sslHostnameVerifier = Tls.host
    }

    /**
     * Paho's automatic reconnect only covers a session that was up once; a first connect that fails
     * (no network at boot, server down) is retried here, 1 s doubling to 120 s.
     */
    fun connect() {
        if (closed) return
        client.setCallback(callback)
        runCatching { client.connect(options, null, firstConnect) }.onFailure { retryLater() }
    }

    private val firstConnect = object : MqttActionListener {
        override fun onSuccess(token: IMqttToken) {
            backoffS = 1
        }

        override fun onFailure(token: IMqttToken?, e: Throwable?) {
            listener.onServer(false)
            retryLater()
        }
    }

    private fun retryLater() {
        if (closed) return
        runCatching { retry.schedule(::connect, backoffS, TimeUnit.SECONDS) }
        backoffS = (backoffS * 2).coerceAtMost(120)
    }

    fun publish(topic: String, payload: ByteArray, id: String) {
        runCatching { client.publish(topic, payload, 1, false, id, null) }
            .onFailure {
                Log.w(TAG, "publish $id refused: $it")
                listener.onServer(false) // the relay keeps it in its outbox
                reconnectSoon()
            }
    }

    /**
     * A refused publish or a protocol error: the session may look connected while being of no use, and with nothing
     * left in flight the relay's PUBACK watchdog would never fire. Dial again in 2 s unless Paho already has.
     */
    private fun reconnectSoon() {
        if (closed) return
        runCatching { retry.schedule({ if (!closed && !client.isConnected) runCatching { client.reconnect() } }, 2, TimeUnit.SECONDS) }
    }

    /** The relay saw no PUBACK in time: drop the socket and dial again. */
    fun reconnect() {
        runCatching { client.disconnectForcibly(0, 1_000, false) }
        runCatching { client.reconnect() }
    }

    fun close() {
        closed = true
        retry.shutdownNow()
        runCatching { client.disconnectForcibly(0, 1_000, false) }
        runCatching { client.close() }
    }

    private val callback = object : MqttCallback {
        override fun connectComplete(reconnect: Boolean, serverURI: String) {
            runCatching {
                client.subscribe(downTopic, 1)
                client.publish(stateTopic, """{"on":true}""".toByteArray(), 1, true)
            }
            listener.onServer(true)
        }

        override fun disconnected(r: MqttDisconnectResponse) {
            Log.w(TAG, "disconnected: ${r.reasonString ?: r.exception ?: r.returnCode}")
            listener.onServer(false)
        }

        override fun mqttErrorOccurred(e: MqttException) {
            Log.w(TAG, "error: $e")
            listener.onServer(false)
            reconnectSoon()
        }

        override fun messageArrived(topic: String, message: MqttMessage) {
            if (topic == downTopic) listener.onMessage(message.payload)
        }

        override fun deliveryComplete(token: IMqttToken) {
            Log.d(TAG, "puback ${token.userContext} rc ${token.reasonCodes?.joinToString()}")
            (token.userContext as? String)?.let(listener::onPublished)
        }

        override fun authPacketArrived(reasonCode: Int, properties: MqttProperties) = Unit
    }

    private companion object {
        const val TAG = "MqttLink"
    }
}
