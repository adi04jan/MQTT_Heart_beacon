package com.heartbeacon.app.link

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings

/** Doze exemption, as in the reference app's BatteryExemption: asked for at onboarding, shown in phone details. */
object BatteryExemption {
    fun isExempt(context: Context): Boolean =
        context.getSystemService(PowerManager::class.java)?.isIgnoringBatteryOptimizations(context.packageName) == true

    @SuppressLint("BatteryLife") // a companion device link is the documented use case
    fun requestIntent(context: Context): Intent =
        Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
            .setData(Uri.parse("package:${context.packageName}"))
            .takeIf { it.resolveActivity(context.packageManager) != null }
            ?: Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
}
