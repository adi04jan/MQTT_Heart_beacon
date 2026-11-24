package com.heartbeacon.app

import android.app.Application
import android.content.Context
import androidx.datastore.preferences.preferencesDataStore
import com.heartbeacon.app.data.Creds
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.RealRepo
import com.heartbeacon.app.data.Sealer
import com.heartbeacon.app.data.SettingsStore
import com.heartbeacon.app.link.BeaconService
import com.heartbeacon.app.link.Tls
import kotlinx.coroutines.MainScope

private val Context.credStore by preferencesDataStore("creds")

/** Manual DI, as in the reference app: the graph is these fields. */
class App : Application() {
    val scope = MainScope()
    val settings by lazy { SettingsStore(this) }

    /** The real repo; debug builds may swap in a FakeRepo for screenshots (MainActivity). */
    lateinit var repo: HeartRepo

    override fun onCreate() {
        super.onCreate()
        BeaconService.channels(this)
        repo = RealRepo(this, scope, settings, Creds(credStore, Sealer.keystore()), Tls.pinned(assets.open("hb_ca.pem")))
    }
}
