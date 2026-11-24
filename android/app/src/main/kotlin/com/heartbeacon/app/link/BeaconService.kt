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
import android.util.Log
import androidx.lifecycle.LifecycleService
import com.heartbeacon.app.App
import com.heartbeacon.app.MainActivity
import com.heartbeacon.app.R
import com.heartbeacon.app.data.Mood
import com.heartbeacon.app.data.RealRepo
import com.heartbeacon.core.StartGate

/**
 * The always-on `connectedDevice` foreground service. It keeps the process alive and asks the repo to
 * hold the one autoConnect GATT open ([RealRepo.host]); the MQTT session and :core's relay live in the
 * repo's [com.heartbeacon.app.link.Link], so hearts also work with no beacon and no service.
 */
class BeaconService : LifecycleService() {
    private val repo get() = (application as App).repo as? RealRepo

    override fun onCreate() {
        super.onCreate()
        startForeground(NOTE_ID, linkNote("Connected to your beacon"), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        registerReceiver(btState, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
        if (repo?.host(true) != true) {
            Log.i(TAG, "no account or beacon yet; stopping")
            stopSelf()
        }
    }

    /** Bluetooth off kills the GATT object; recreate it when the adapter is back (plan, Connection row). */
    private val btState = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_OFF -> repo?.host(false)
                BluetoothAdapter.STATE_ON -> repo?.host(true)
            }
        }
    }

    override fun onDestroy() {
        unregisterReceiver(btState)
        repo?.host(false)
        gate.stopped()
        super.onDestroy()
    }

    private fun linkNote(text: String): Notification = Notification.Builder(this, CH_LINK)
        .setSmallIcon(R.drawable.ic_heart)
        .setContentTitle("Heart Beacon")
        .setContentText(text)
        .setContentIntent(openApp(this))
        .setOngoing(true)
        .build()

    companion object {
        private const val TAG = "BeaconService"
        private const val NOTE_ID = 1
        const val CH_LINK = "link"
        const val CH_MOODS = "moods"

        private val gate = StartGate()

        /** "Asha sent a Heart". The caller has checked the user's per-kind switch. */
        fun notifyMood(ctx: Context, id: String, from: String, mood: Mood) {
            if (ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
            val word = mood.name.lowercase().replaceFirstChar { it.uppercase() }
            ctx.getSystemService(NotificationManager::class.java).notify(
                id.hashCode(),
                Notification.Builder(ctx, CH_MOODS)
                    .setSmallIcon(R.drawable.ic_heart)
                    .setContentTitle("$from sent a${if (mood == Mood.ANNOY) "n" else ""} $word")
                    .setContentIntent(openApp(ctx))
                    .setAutoCancel(true)
                    .build(),
            )
        }

        private fun openApp(ctx: Context) = PendingIntent.getActivity(
            ctx, 0, Intent(ctx, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )

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
