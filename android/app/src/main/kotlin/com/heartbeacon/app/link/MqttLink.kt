package com.heartbeacon.app.link

import org.eclipse.paho.mqttv5.client.IMqttToken
import org.eclipse.paho.mqttv5.client.MqttAsyncClient
import org.eclipse.paho.mqttv5.client.MqttCallback
import org.eclipse.paho.mqttv5.client.MqttConnectionOptions
import org.eclipse.paho.mqttv5.client.MqttDisconnectResponse
import org.eclipse.paho.mqttv5.client.persist.MemoryPersistence
import org.eclipse.paho.mqttv5.common.MqttException
import org.eclipse.paho.mqttv5.common.MqttMessage
import org.eclipse.paho.mqttv5.common.packet.MqttProperties
import java.io.InputStream
import java.security.KeyStore
import java.security.cert.CertificateFactory
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory

/**
 * The phone's MQTT 5 session (PROTOCOL.md section 5): client id `u-{uid}-{install}`, persistent session
 * with 24 h expiry, keepalive 120 s, LWT `{"on":false}` retained on `u/{uid}/state`, QoS 1 everywhere.
 * TLS trusts only the bundled private CA ([pinnedCa]); Paho's hostname check matches the leaf's IP SAN.
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

    fun connect() {
        val o = MqttConnectionOptions().apply {
            userName = user
            password = pass
            isCleanStart = false
            sessionExpiryInterval = 86_400L
            keepAliveInterval = 120
            isAutomaticReconnect = true
            setAutomaticReconnectDelay(1, 120)
            setWill(stateTopic, MqttMessage("""{"on":false}""".toByteArray()).apply { qos = 1; isRetained = true })
            socketFactory = this@MqttLink.socketFactory
        }
        client.setCallback(callback)
        client.connect(o)
    }

    fun publish(topic: String, payload: ByteArray, id: String) {
        runCatching { client.publish(topic, payload, 1, false, id, null) }
            .onFailure { listener.onServer(false) } // not connected: the relay keeps it in its outbox
    }

    /** The relay saw no PUBACK in time: drop the socket and dial again. */
    fun reconnect() {
        runCatching { client.disconnectForcibly(0, 1_000, false) }
        runCatching { client.reconnect() }
    }

    fun close() {
        runCatching { client.disconnectForcibly(0, 1_000, false) }
        runCatching { client.close() }
    }

    private val callback = object : MqttCallback {
        override fun connectComplete(reconnect: Boolean, serverURI: String) {
            client.subscribe(downTopic, 1)
            client.publish(stateTopic, """{"on":true}""".toByteArray(), 1, true)
            listener.onServer(true)
        }

        override fun disconnected(r: MqttDisconnectResponse) = listener.onServer(false)

        override fun mqttErrorOccurred(e: MqttException) = listener.onServer(false)

        override fun messageArrived(topic: String, message: MqttMessage) {
            if (topic == downTopic) listener.onMessage(message.payload)
        }

        override fun deliveryComplete(token: IMqttToken) {
            (token.userContext as? String)?.let(listener::onPublished)
        }

        override fun authPacketArrived(reasonCode: Int, properties: MqttProperties) = Unit
    }

    companion object {
        /** Trust anchored only on the private CA (plan decision 5), never the system store. */
        fun pinnedCa(caPem: InputStream): SSLSocketFactory {
            val ca = caPem.use { CertificateFactory.getInstance("X.509").generateCertificate(it) }
            val ks = KeyStore.getInstance(KeyStore.getDefaultType()).apply {
                load(null)
                setCertificateEntry("hb-ca", ca)
            }
            val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(ks) }
            return SSLContext.getInstance("TLS").apply { init(null, tmf.trustManagers, null) }.socketFactory
        }
    }
}
