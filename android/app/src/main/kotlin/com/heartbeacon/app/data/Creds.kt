package com.heartbeacon.app.data

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import kotlinx.coroutines.flow.first
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.security.KeyStore
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** What a login leaves on the phone. [token], [mqttUser] and [mqttPass] are the secrets. */
data class Account(
    val host: String,
    val uid: String,
    val name: String,
    val token: String,
    val mqttPort: Int,
    val mqttUser: String,
    val mqttPass: String,
)

/**
 * AES-256-GCM with a key from [key]: on the phone a non-exportable Android Keystore key ([keystore]);
 * in tests an in-memory key. Output is the 12-byte IV followed by ciphertext and tag.
 */
class Sealer(private val key: () -> SecretKey) {
    fun seal(plain: ByteArray): ByteArray {
        val c = Cipher.getInstance(GCM)
        c.init(Cipher.ENCRYPT_MODE, key())
        return c.iv + c.doFinal(plain)
    }

    fun open(sealed: ByteArray): ByteArray {
        val c = Cipher.getInstance(GCM)
        c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, sealed, 0, 12))
        return c.doFinal(sealed, 12, sealed.size - 12)
    }

    companion object {
        private const val GCM = "AES/GCM/NoPadding"

        fun keystore(alias: String = "hb_creds") = Sealer {
            val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
            (ks.getKey(alias, null) as? SecretKey) ?: KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
                init(
                    KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setKeySize(256)
                        .build(),
                )
                generateKey()
            }
        }
    }
}

/**
 * The signed-in account, sealed as one blob in its own DataStore file (`creds`, separate from the app
 * settings). Nothing of it is readable without the Keystore key; [wipe] on logout removes the blob.
 */
class Creds(private val store: DataStore<Preferences>, private val sealer: Sealer) {
    private val blob = stringPreferencesKey("account")

    suspend fun save(a: Account) {
        val json = buildJsonObject {
            put("host", JsonPrimitive(a.host))
            put("uid", JsonPrimitive(a.uid))
            put("name", JsonPrimitive(a.name))
            put("token", JsonPrimitive(a.token))
            put("mqtt_port", JsonPrimitive(a.mqttPort))
            put("mqtt_user", JsonPrimitive(a.mqttUser))
            put("mqtt_pass", JsonPrimitive(a.mqttPass))
        }.toString()
        val sealed = Base64.getEncoder().encodeToString(sealer.seal(json.encodeToByteArray()))
        store.edit { it[blob] = sealed }
    }

    /** Null when signed out, or when the blob no longer opens (Keystore key lost after a restore): sign in again. */
    suspend fun load(): Account? {
        val sealed = store.data.first()[blob] ?: return null
        return runCatching {
            val o = Json.parseToJsonElement(sealer.open(Base64.getDecoder().decode(sealed)).decodeToString()).jsonObject
            fun s(k: String) = o[k]!!.jsonPrimitive.content
            Account(s("host"), s("uid"), s("name"), s("token"), o["mqtt_port"]!!.jsonPrimitive.int, s("mqtt_user"), s("mqtt_pass"))
        }.getOrNull()
    }

    suspend fun wipe() {
        store.edit { it.remove(blob) }
    }
}
