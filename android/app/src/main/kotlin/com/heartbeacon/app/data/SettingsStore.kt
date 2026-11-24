package com.heartbeacon.app.data

import android.content.Context
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.store by preferencesDataStore("settings")

/** Light or dark for this app only; applied through UiModeManager, which keeps it across restarts. */
enum class Appearance(val wire: String, val nightMode: Int) {
    SYSTEM("system", android.app.UiModeManager.MODE_NIGHT_AUTO),
    LIGHT("light", android.app.UiModeManager.MODE_NIGHT_NO),
    DARK("dark", android.app.UiModeManager.MODE_NIGHT_YES),
}

/** App-local settings. Plan: DataStore only. The account and its secrets live in [Creds], sealed. */
data class LocalSettings(
    /** MAC of the CDM-associated beacon; the presence callbacks and GATT connect key off it. */
    val beaconMac: String?,
    val did: String?,
    val notify: Map<Mood, Boolean>,
    val appearance: Appearance = Appearance.SYSTEM,
)

class SettingsStore(private val context: Context) {
    private val mac = stringPreferencesKey("beacon_mac")
    private val did = stringPreferencesKey("did")
    private fun notifyKey(m: Mood) = booleanPreferencesKey("notify_${m.wire}")
    private val appearance = stringPreferencesKey("appearance")

    val flow: Flow<LocalSettings> = context.store.data.map { p ->
        LocalSettings(
            p[mac], p[did], Mood.entries.associateWith { p[notifyKey(it)] ?: true },
            Appearance.entries.firstOrNull { it.wire == p[appearance] } ?: Appearance.SYSTEM,
        )
    }

    suspend fun setNotify(m: Mood, on: Boolean) = edit { it[notifyKey(m)] = on }

    /** Stores the choice for the settings screen and hands it to the system, which re-themes the app at once. */
    suspend fun setAppearance(a: Appearance) {
        edit { it[appearance] = a.wire }
        context.getSystemService(android.app.UiModeManager::class.java).setApplicationNightMode(a.nightMode)
    }
    suspend fun setBeacon(m: String?, d: String?) = edit {
        if (m == null) it.remove(mac) else it[mac] = m
        if (d == null) it.remove(did) else it[did] = d
    }

    private suspend fun edit(block: (androidx.datastore.preferences.core.MutablePreferences) -> Unit): Preferences =
        context.store.edit(block)
}
