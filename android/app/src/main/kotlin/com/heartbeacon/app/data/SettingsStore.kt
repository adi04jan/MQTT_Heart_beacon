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

/** App-local settings. Plan: DataStore only. The broker password does not live here (Keystore, M5). */
data class LocalSettings(
    val host: String?,
    val user: String?,
    /** MAC of the CDM-associated beacon; the presence callbacks and GATT connect key off it. */
    val beaconMac: String?,
    val did: String?,
    val notify: Map<Mood, Boolean>,
)

class SettingsStore(private val context: Context) {
    private val host = stringPreferencesKey("host")
    private val user = stringPreferencesKey("user")
    private val mac = stringPreferencesKey("beacon_mac")
    private val did = stringPreferencesKey("did")
    private fun notifyKey(m: Mood) = booleanPreferencesKey("notify_${m.wire}")

    val flow: Flow<LocalSettings> = context.store.data.map { p ->
        LocalSettings(p[host], p[user], p[mac], p[did], Mood.entries.associateWith { p[notifyKey(it)] ?: true })
    }

    suspend fun setNotify(m: Mood, on: Boolean) = edit { it[notifyKey(m)] = on }
    suspend fun setAccount(h: String, u: String) = edit { it[host] = h; it[user] = u }
    suspend fun setBeacon(m: String?, d: String?) = edit {
        if (m == null) it.remove(mac) else it[mac] = m
        if (d == null) it.remove(did) else it[did] = d
    }

    private suspend fun edit(block: (androidx.datastore.preferences.core.MutablePreferences) -> Unit): Preferences =
        context.store.edit(block)
}
