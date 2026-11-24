package com.heartbeacon.app.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.emptyPreferences
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import javax.crypto.AEADBadTagException
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

/**
 * [Creds] with an in-memory key standing in for the Android Keystore, over an in-memory DataStore.
 * (A file DataStore cannot run here: with SDK_INT 0 under host tests it renames with File.renameTo,
 * which cannot replace a file on a Windows host. The phone's DataStore does an atomic replace.)
 */
class CredsTest {
    private class MemStore : DataStore<Preferences> {
        val state = MutableStateFlow(emptyPreferences())
        override val data = state
        override suspend fun updateData(transform: suspend (Preferences) -> Preferences) = transform(state.value).also { state.value = it }
    }

    private fun newKey(): SecretKey = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
    private val key = newKey()
    private val store = MemStore()
    private val acct = Account("203.0.113.10", "asha", "Asha", "api-token-123", 8883, "asha", "mqtt-pass-456")

    /** A new Creds over the same stored bytes, as after an app restart. */
    private fun open(k: SecretKey = key) = Creds(store, Sealer { k })

    private fun stored() = store.state.value.asMap().values.joinToString()

    @Test fun accountSurvivesARestartAndNoSecretIsStoredInTheClear() = runBlocking {
        open().save(acct)
        assertEquals(acct, open().load())
        assertTrue(stored().isNotEmpty())
        listOf("api-token-123", "mqtt-pass-456", "asha", "203.0.113.10").forEach { assertFalse(it, it in stored()) }
    }

    @Test fun logoutWipesItForGood() = runBlocking {
        open().save(acct)
        open().wipe()
        assertNull(open().load())
        assertEquals("", stored())
    }

    @Test fun aLostKeystoreKeyMeansSignInAgainNotACrash() = runBlocking {
        open().save(acct)
        assertNull(open(newKey()).load())
    }

    @Test fun sealedBytesAreAuthenticated() {
        val s = Sealer { key }
        val sealed = s.seal("token".encodeToByteArray())
        assertEquals("token", s.open(sealed).decodeToString())
        assertFalse(sealed.copyOfRange(0, 12).contentEquals(s.seal("token".encodeToByteArray()).copyOfRange(0, 12))) // fresh IV
        sealed[sealed.size - 1] = (sealed[sealed.size - 1].toInt() xor 1).toByte()
        try {
            s.open(sealed)
            error("tampered blob opened")
        } catch (_: AEADBadTagException) {
        }
    }
}
