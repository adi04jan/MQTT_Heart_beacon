package com.heartbeacon.app.link

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.provider.Settings
import android.util.Log
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.heartbeacon.app.App
import com.heartbeacon.app.MainActivity
import com.heartbeacon.app.R
import com.heartbeacon.app.data.Mood
import com.heartbeacon.core.Handover
import com.heartbeacon.core.Msg
import com.heartbeacon.core.RelayEngine
import com.heartbeacon.core.RelayOut
import com.heartbeacon.core.StartGate
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.serialization.json.jsonPrimitive
import java.util.TimeZone
import java.util.concurrent.Executors

/**
 * The always-on `connectedDevice` foreground service: one GATT to the beacon, one MQTT session, and
 * :core's [RelayEngine] between them. Every engine call runs on one thread ([loop]) in arrival order,
 * because the engine is plain single-threaded logic and BLE frame order matters.
 */
class BeaconService : LifecycleService() {
    private val loop = Executors.newSingleThreadExecutor()
    private var engine: RelayEngine? = null
    private var gatt: GattLink? = null
    private var mqtt: MqttLink? = null
    private val handover = Handover()

    private fun now() = System.currentTimeMillis()

    private fun onLoop(block: RelayEngine.() -> List<RelayOut>) = loop.execute {
        val e = engine ?: return@execute
        run(e.block())
        handover.update(e.bleUp, e.serverUp, now())
        phase.value = handover.phase(now())
    }

    override fun onCreate() {
        super.onCreate()
        startForeground(NOTE_ID, linkNote("Connecting to your beacon"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        registerReceiver(btState, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
        lifecycleScope.launch {
            val s = (application as App).settings.flow.first()
            val mac = s.beaconMac
            val did = s.did
            val uid = s.user
            if (mac == null || did == null || uid == null) {
                Log.i(TAG, "no beacon or account yet; stopping")
                stopSelf()
                return@launch
            }
            engine = RelayEngine(did, uid, tzMin = { TimeZone.getDefault().getOffset(now()) / 60_000 })
            gatt = GattLink(this@BeaconService, mac, gattListener).also { it.open(autoConnect = true) }
            mqtt = openMqtt(s.host, uid)
            while (isActive) { // the engine's 5 s ack and 10 s hold timers
                delay(1_000)
                onLoop { tick(now()) }
            }
        }
    }

    /** Null (BLE-only) until there is a server, a broker password and the bundled CA (`assets/hb_ca.pem`, M2). */
    private fun openMqtt(host: String?, uid: String): MqttLink? {
        val pass = brokerPassword() ?: return null
        val ca = runCatching { assets.open("hb_ca.pem") }.getOrNull() ?: return null.also { Log.w(TAG, "no bundled CA") }
        return MqttLink(
            host ?: return null, 8883, uid,
            Settings.Secure.getString(contentResolver, Settings.Secure.ANDROID_ID),
            uid, pass, MqttLink.pinnedCa(ca), mqttListener,
        ).also { it.connect() }
    }

    // ponytail: the login API (M2) returns the broker password; it goes in a Keystore-backed store, not DataStore.
    private fun brokerPassword(): ByteArray? = null

    private val gattListener = object : GattLink.Listener {
        override fun onLink(up: Boolean, mtu: Int) = onLoop { bleChanged(up, now(), mtu) }
        override fun onFrame(frame: ByteArray) = onLoop { bleFrame(frame, now()) }
    }

    private val mqttListener = object : MqttLink.Listener {
        override fun onServer(up: Boolean) = onLoop { serverChanged(up, now()) }
        override fun onMessage(payload: ByteArray) = onLoop { serverMessage(payload, now()) }
        override fun onPublished(id: String) = onLoop { published(id); emptyList() }
    }

    private fun run(outs: List<RelayOut>) = outs.forEach { o ->
        when (o) {
            is RelayOut.BleSend -> gatt?.send(o.frames)
            is RelayOut.Publish -> mqtt?.publish(o.topic, o.payload, o.id)
            is RelayOut.Local -> local(o.msg)
            RelayOut.ReconnectServer -> mqtt?.reconnect()
        }
    }

    /** Messages for the app: notify on someone else's mood; receipts and beacon status go to the UI (M5). */
    private fun local(m: Msg) {
        if (m.t != "mood") return
        val from = m.body["from"]?.jsonPrimitive?.content ?: return
        val mood = Mood.entries.firstOrNull { it.wire == m.body["k"]?.jsonPrimitive?.content } ?: return
        lifecycleScope.launch {
            if ((application as App).settings.flow.first().notify[mood] != true) return@launch
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return@launch
            val word = mood.name.lowercase().replaceFirstChar { it.uppercase() }
            getSystemService(NotificationManager::class.java).notify(
                m.id.hashCode(),
                Notification.Builder(this@BeaconService, CH_MOODS)
                    .setSmallIcon(R.drawable.ic_heart)
                    .setContentTitle("$from sent a $word")
                    .setContentIntent(openApp())
                    .setAutoCancel(true)
                    .build(),
            )
        }
    }

    /** Bluetooth off kills the GATT object; recreate it when the adapter is back (plan, Connection row). */
    private val btState = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_OFF -> gatt?.close()
                BluetoothAdapter.STATE_ON -> gatt?.open(autoConnect = true)
            }
        }
    }

    override fun onDestroy() {
        unregisterReceiver(btState)
        gatt?.close()
        mqtt?.close()
        loop.shutdownNow()
        gate.stopped()
        super.onDestroy()
    }

    private fun openApp() = PendingIntent.getActivity(
        this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
    )

    private fun linkNote(text: String): Notification = Notification.Builder(this, CH_LINK)
        .setSmallIcon(R.drawable.ic_heart)
        .setContentTitle("Heart Beacon")
        .setContentText(text)
        .setContentIntent(openApp())
        .setOngoing(true)
        .build()

    companion object {
        private const val TAG = "BeaconService"
        private const val NOTE_ID = 1
        const val CH_LINK = "link"
        const val CH_MOODS = "moods"

        /** The beacon's predicted phase for the UI. */
        val phase = MutableStateFlow(Handover.Phase.AWAY)

        private val gate = StartGate()

        fun channels(ctx: Context) {
            ctx.getSystemService(NotificationManager::class.java).createNotificationChannels(
                listOf(
                    NotificationChannel(CH_LINK, "Beacon link", NotificationManager.IMPORTANCE_LOW).apply {
                        description = "Keeps the connection to your beacon open"
                    },
                    NotificationChannel(CH_MOODS, "Hearts", NotificationManager.IMPORTANCE_HIGH),
                ),
            )
        }

        /**
         * Single-flight start from app open, boot, package update and presence callbacks. Needs a beacon
         * association and BLUETOOTH_CONNECT (the connectedDevice type requires it).
         */
        fun start(ctx: Context) {
            if (!Cdm.isAssociated(ctx)) return
            if (ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) return
            if (!gate.claim()) return
            runCatching { ctx.startForegroundService(Intent(ctx, BeaconService::class.java)) }
                .onFailure {
                    Log.w(TAG, "start refused", it)
                    gate.stopped()
                }
        }
    }
}
