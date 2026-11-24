package com.heartbeacon.app.data

import com.heartbeacon.app.link.Tls
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.IOException
import java.net.URL
import java.util.concurrent.ConcurrentHashMap
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLException
import javax.net.ssl.SSLSocketFactory

/** An HTTP error answer from the server: FastAPI's `detail` as [reason], `Retry-After` on a 429. */
class ApiError(val code: Int, val reason: String, val retryAfterS: Long? = null) : Exception("$code $reason")

data class Login(val token: String, val uid: String, val name: String, val mqttPort: Int, val mqttUser: String, val mqttPass: String)

/** `claim` fields for the beacon (sent over BLE as is) and the signed `cfg` envelope to write after it. */
data class Claim(val beacon: JsonObject, val cfg: JsonObject)

/**
 * The public API (server/hb_server/api.py, port 8443) over HttpsURLConnection: TLS pinned to the private
 * CA with [Tls.host] as the hostname check. Blocking I/O runs on [Dispatchers.IO].
 *
 * Settings go up with PUT, which the server takes as PATCH: HttpURLConnection has no PATCH.
 * A 429 is honoured locally: until its `Retry-After` passes, the same path fails at once with the
 * remaining wait and never reaches the server.
 */
class HttpApi(
    host: String,
    private val tls: SSLSocketFactory,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    val base = baseUrl(host)
    @Volatile var token: String? = null
    private val blocked = ConcurrentHashMap<String, Long>() // path -> epoch ms the server said to wait for

    suspend fun login(user: String, pass: String): Login {
        val o = call("POST", "/api/v1/login", buildJsonObject {
            put("user", JsonPrimitive(user))
            put("pass", JsonPrimitive(pass))
        }).jsonObject
        val mqtt = o["mqtt"]!!.jsonObject
        return Login(
            token = o.str("token"),
            uid = o.str("uid"),
            name = o.str("name"),
            mqttPort = mqtt["port"]!!.jsonPrimitive.int,
            mqttUser = mqtt.str("user"),
            mqttPass = mqtt.str("pass"),
        )
    }

    /** `{uid, name, circle, devices: [{did, name, cfg_n, st, wake, last_seen, tele, ...}]}`. */
    suspend fun me(): JsonObject = call("GET", "/api/v1/me").jsonObject

    suspend fun claim(did: String): Claim {
        val o = call("POST", "/api/v1/devices/claim", buildJsonObject { put("did", JsonPrimitive(did)) }).jsonObject
        val beacon = buildJsonObject {
            put("host", o["host"]!!)
            put("mport", o["mport"]!!)
            put("aport", o["aport"]!!)
            put("user", o["user"]!!)
            put("pass", o["pass"]!!)
        }
        return Claim(beacon, o["cfg"]!!.jsonObject)
    }

    suspend fun unclaim(did: String) {
        call("DELETE", "/api/v1/devices/$did/claim")
    }

    /** The user-editable keys and `n`: `{n, name, led, sched, pwr, net}`. */
    suspend fun settings(did: String): JsonObject = call("GET", "/api/v1/devices/$did/settings").jsonObject

    /** Merges [changes] into the config; returns the new signed `cfg` envelope for the BLE write. */
    suspend fun saveSettings(did: String, changes: JsonObject): JsonObject =
        call("PUT", "/api/v1/devices/$did/settings", changes).jsonObject["cfg"]!!.jsonObject

    /** Newest first: `[{id, k, ts, uid, from, src, to: [{did, st}]}]`. */
    suspend fun events(limit: Int = 50): JsonArray = call("GET", "/api/v1/events?limit=$limit").jsonArray

    private suspend fun call(method: String, path: String, body: JsonElement? = null): JsonElement = withContext(Dispatchers.IO) {
        val key = path.substringBefore('?')
        blocked[key]?.let { until ->
            val left = until - clock()
            if (left > 0) throw ApiError(429, "slow_down", (left + 999) / 1000)
            blocked.remove(key)
        }
        val c = URL(base + path).openConnection() as HttpsURLConnection
        try {
            c.sslSocketFactory = tls
            c.hostnameVerifier = Tls.host
            c.connectTimeout = 10_000
            c.readTimeout = 15_000
            c.useCaches = false
            c.requestMethod = method
            c.setRequestProperty("Accept", "application/json")
            token?.let { c.setRequestProperty("Authorization", "Bearer $it") }
            if (body != null) {
                c.doOutput = true
                c.setRequestProperty("Content-Type", "application/json")
                c.outputStream.use { it.write(body.toString().encodeToByteArray()) }
            }
            val code = c.responseCode
            val text = (if (code < 400) c.inputStream else c.errorStream)?.use { it.readBytes().decodeToString() }.orEmpty()
            if (code >= 400) {
                val retry = c.getHeaderField("Retry-After")?.trim()?.toLongOrNull()
                if (code == 429 && retry != null) blocked[key] = clock() + retry * 1000
                throw ApiError(code, detail(text) ?: "http_$code", retry)
            }
            if (text.isBlank()) JsonNull else Json.parseToJsonElement(text)
        } finally {
            c.disconnect()
        }
    }

    companion object {
        const val API_PORT = 8443

        /** `203.0.113.10` -> `https://203.0.113.10:8443`; an explicit `host:port` is kept. */
        fun baseUrl(host: String): String {
            val h = host.trim().removePrefix("https://").trimEnd('/')
            val hasPort = if (h.startsWith("[")) "]:" in h else ':' in h
            return if (hasPort) "https://$h" else "https://$h:$API_PORT"
        }

        private fun detail(text: String): String? = runCatching {
            (Json.parseToJsonElement(text).jsonObject["detail"] as? JsonPrimitive)?.content
        }.getOrNull()

        private fun JsonObject.str(k: String) = this[k]!!.jsonPrimitive.content
    }
}

/**
 * What to tell the user about a failed call. [did] names the beacon in claim answers. Wording is shared
 * with [FakeRepo]'s debug scenarios so the screenshots show the real copy.
 */
fun userMessage(e: Throwable, did: String? = null): String = when (e) {
    is ApiError -> when {
        e.code == 401 && e.reason == "bad_login" -> "Wrong username or password."
        e.code == 401 -> "You were signed out. Sign in again."
        e.code == 403 && e.reason == "not_assigned" ->
            "${did ?: "This beacon"} isn't on your account yet. Ask your admin to assign it to you, then try again."
        e.code == 403 && e.reason.startsWith("not_editable") -> "Only your admin can change that setting."
        e.code == 409 -> "${did ?: "This beacon"} belongs to another account. Ask your admin to release it first."
        e.code == 429 -> "Too many attempts. Try again in ${wait(e.retryAfterS)}."
        e.code == 404 -> "This beacon is no longer on your account."
        e.code == 422 && e.reason == "bad_did" -> "That is not a beacon name. They look like HB-3FA2C1."
        e.code == 422 -> "The server did not accept that value."
        e.code >= 500 -> "The server had a problem. Try again in a minute."
        else -> "Something went wrong (${e.code})."
    }
    is SSLException -> NOT_TRUSTED
    // A plain JVM HttpsURLConnection reports a hostname mismatch as IOException("HTTPS hostname wrong ...").
    is IOException -> if (e.message.orEmpty().contains("hostname", ignoreCase = true)) NOT_TRUSTED
        else "Can't reach the server. Check the address and your connection."
    else -> "Something went wrong."
}

private const val NOT_TRUSTED = "The server's certificate is not the one this app trusts. Check the server address."

/** `Retry-After` for people: seconds up to 90, then whole minutes. */
private fun wait(s: Long?): String = when {
    s == null -> "a few minutes"
    s <= 90 -> "$s s"
    else -> "${(s + 59) / 60} min"
}
