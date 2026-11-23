package com.heartbeacon.app

import android.app.Application
import com.heartbeacon.app.data.FakeRepo
import com.heartbeacon.app.data.HeartRepo
import com.heartbeacon.app.data.SettingsStore
import com.heartbeacon.app.link.BeaconService
import kotlinx.coroutines.MainScope

/** Manual DI, as in the reference app: the graph is these fields. */
class App : Application() {
    val scope = MainScope()
    val settings by lazy { SettingsStore(this) }

    /** Fakes until the server (M2) and the beacon (M4) exist; the real repo sits on BeaconService. */
    var repo: HeartRepo = FakeRepo(scope)

    override fun onCreate() {
        super.onCreate()
        BeaconService.channels(this)
    }
}
