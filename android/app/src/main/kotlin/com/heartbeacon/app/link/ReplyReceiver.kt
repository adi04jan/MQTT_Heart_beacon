package com.heartbeacon.app.link

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.heartbeacon.app.App
import com.heartbeacon.app.data.Mood

/** A Heart / Sorry / Together reply from a mood notification (review I8): sends it and clears the notification. */
class ReplyReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, i: Intent) {
        val mood = Mood.entries.firstOrNull { it.wire == i.getStringExtra(MOOD) } ?: return
        (ctx.applicationContext as App).repo.send(mood)
        ctx.getSystemService(NotificationManager::class.java).cancel(i.getIntExtra(NOTE, 0))
    }

    companion object {
        const val MOOD = "mood"
        const val NOTE = "note"
    }
}
