package com.heartbeacon.app.link

import android.companion.AssociationInfo
import android.companion.CompanionDeviceService
import android.companion.DevicePresenceEvent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.annotation.RequiresApi

/**
 * The OS binds this when the associated beacon comes into range, even with the app closed. Both
 * callbacks route to the one single-flight [BeaconService.start]; presence flaps are absorbed there.
 * The service is started directly, not through WorkManager (the reference's presence job sat queued
 * for minutes when out of quota).
 */
class PresenceService : CompanionDeviceService() {
    @RequiresApi(36)
    override fun onDevicePresenceEvent(event: DevicePresenceEvent) {
        if (event.event == DevicePresenceEvent.EVENT_BLE_APPEARED) BeaconService.start(this)
    }

    @Deprecated("API 34-35 path; API 36 calls onDevicePresenceEvent")
    override fun onDeviceAppeared(associationInfo: AssociationInfo) {
        if (Build.VERSION.SDK_INT < 36) BeaconService.start(this)
    }

    // Disappeared: nothing to do. The service stays up and the autoConnect GATT waits for the beacon.
}

/** BOOT_COMPLETED and MY_PACKAGE_REPLACED: bring the link back without waiting for a presence event. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) = BeaconService.start(context)
}
