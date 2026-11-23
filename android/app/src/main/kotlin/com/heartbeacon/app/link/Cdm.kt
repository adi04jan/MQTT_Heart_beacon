package com.heartbeacon.app.link

import android.app.Activity
import android.bluetooth.le.ScanFilter
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.Context
import android.content.IntentSender
import android.os.ParcelUuid
import android.util.Log

/**
 * CompanionDeviceManager: the system chooser instead of our own scan (no scan or location permission),
 * and presence callbacks that start [BeaconService] when the beacon comes into range.
 */
object Cdm {
    private const val TAG = "Cdm"

    private fun cdm(ctx: Context): CompanionDeviceManager? = ctx.getSystemService(CompanionDeviceManager::class.java)

    fun isAssociated(ctx: Context): Boolean = cdm(ctx)?.myAssociations?.isNotEmpty() == true

    /** Only devices advertising the beacon service; never auto-pick (setSingleDevice(false), as in the reference). */
    private fun request(): AssociationRequest = AssociationRequest.Builder()
        .addDeviceFilter(
            BluetoothLeDeviceFilter.Builder()
                .setScanFilter(ScanFilter.Builder().setServiceUuid(ParcelUuid(GattLink.SERVICE)).build())
                .build(),
        )
        .setSingleDevice(false)
        .build()

    /** Shows the chooser via [launch] (an Activity result launcher); [onMac] gets the chosen beacon's address. */
    fun associate(activity: Activity, launch: (IntentSender) -> Unit, onMac: (String) -> Unit, onFail: (String) -> Unit) {
        val m = cdm(activity) ?: return onFail("Companion devices are not supported on this phone.")
        m.associate(request(), activity.mainExecutor, object : CompanionDeviceManager.Callback() {
            override fun onAssociationPending(intentSender: IntentSender) = launch(intentSender)

            override fun onAssociationCreated(info: AssociationInfo) {
                val mac = info.deviceMacAddress?.toString()?.uppercase() ?: return onFail("No address from the chooser.")
                observe(activity, mac)
                onMac(mac)
            }

            override fun onFailure(error: CharSequence?) = onFail(error?.toString() ?: "Pairing was cancelled.")
        })
    }

    /**
     * Idempotent; call on every app start. The String overload is deprecated on API 36 but its
     * replacement needs API 36 and this app runs from 34; both move when minSdk does.
     */
    @Suppress("DEPRECATION")
    fun observe(ctx: Context, mac: String) {
        runCatching { cdm(ctx)?.startObservingDevicePresence(mac) }
            .onFailure { Log.w(TAG, "cannot observe $mac", it) }
    }

    @Suppress("DEPRECATION")
    fun forget(ctx: Context, mac: String) {
        val m = cdm(ctx) ?: return
        runCatching { m.stopObservingDevicePresence(mac) }
        m.myAssociations.filter { it.deviceMacAddress?.toString().equals(mac, ignoreCase = true) }
            .forEach { runCatching { m.disassociate(it.id) } }
    }
}
