package com.heartbeacon.app.data

import com.heartbeacon.app.link.Tls
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.tls.HandshakeCertificates
import okhttp3.tls.HeldCertificate
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import javax.net.ssl.SSLPeerUnverifiedException

/** [HttpApi] against a local TLS server: the wire shapes of server/hb_server/api.py, pinning and error mapping. */
class HttpApiTest {
    private val ca = HeldCertificate.Builder().certificateAuthority(0).commonName("test ca").build()
    private val leaf = HeldCertificate.Builder().commonName("hb").addSubjectAlternativeName("127.0.0.1").signedBy(ca).build()
    private val server = MockWebServer()
    private var now = 1_000_000L

    @After fun stop() = server.shutdown()

    private fun serve(cert: HeldCertificate = leaf, vararg answers: MockResponse) {
        server.useHttps(HandshakeCertificates.Builder().heldCertificate(cert, ca.certificate).build().sslSocketFactory(), false)
        answers.forEach(server::enqueue)
        server.start(InetAddress.getByName("127.0.0.1"), 0)
    }

    private fun api(trust: HeldCertificate = ca) =
        HttpApi("127.0.0.1:${server.port}", Tls.pinned(trust.certificatePem().byteInputStream()), clock = { now })

    private fun json(code: Int, body: String) =
        MockResponse().setResponseCode(code).setHeader("Content-Type", "application/json").setBody(body)

    private fun failure(block: suspend () -> Unit): Throwable {
        try {
            runBlocking { block() }
        } catch (e: Throwable) {
            return e
        }
        fail("expected a failure")
        error("unreachable")
    }

    @Test fun loginSendsUserAndPassAndReadsTheAccount() = runBlocking {
        serve(answers = arrayOf(json(200, """{"token":"t0k","uid":"asha","name":"Asha",
            "mqtt":{"host":"203.0.113.10","port":8883,"user":"asha","pass":"mq-secret"}}""")))
        val l = api().login("asha", "pw")
        assertEquals(Login("t0k", "asha", "Asha", 8883, "asha", "mq-secret"), l)
        val r = server.takeRequest()
        assertEquals("POST /api/v1/login", "${r.method} ${r.path}")
        val body = Json.parseToJsonElement(r.body.readUtf8()).jsonObject
        assertEquals(setOf("user", "pass"), body.keys) // `pass` is the server's alias (LoginIn)
        assertEquals(null, r.getHeader("Authorization"))
    }

    @Test fun authedCallsCarryTheTokenAndSettingsGoUpWithPut() = runBlocking {
        serve(answers = arrayOf(
            json(200, """{"n":3,"name":"Desk"}"""),
            json(200, """{"cfg":{"v":1,"t":"cfg","id":"0011223344556677","ts":5,"n":4,"b":"e30","s":"sig"}}"""),
            json(200, """[{"id":"00000000000000e1","k":"heart","ts":10,"uid":"asha","from":"Asha","src":"asha","to":[]}]"""),
        ))
        val api = api().apply { token = "t0k" }
        assertEquals("Desk", api.settings("HB-3FA2C1")["name"]!!.jsonPrimitive.content)
        val cfg = api.saveSettings("HB-3FA2C1", buildJsonObject { put("name", JsonPrimitive("Desk 2")) })
        assertEquals(4, cfg["n"]!!.jsonPrimitive.content.toInt())
        assertEquals(1, api.events(20).size)
        val get = server.takeRequest()
        assertEquals("GET /api/v1/devices/HB-3FA2C1/settings", "${get.method} ${get.path}")
        assertEquals("Bearer t0k", get.getHeader("Authorization"))
        val put = server.takeRequest()
        assertEquals("PUT /api/v1/devices/HB-3FA2C1/settings", "${put.method} ${put.path}")
        assertEquals("""{"name":"Desk 2"}""", put.body.readUtf8())
        assertEquals("GET /api/v1/events?limit=20", server.takeRequest().let { "${it.method} ${it.path}" })
    }

    @Test fun claimSplitsBeaconCredentialsFromTheSignedCfg() = runBlocking {
        serve(answers = arrayOf(json(200, """{"did":"HB-3FA2C1","host":"203.0.113.10","mport":8883,"aport":8443,
            "user":"HB-3FA2C1","pass":"beacon-pw","cfg":{"v":1,"t":"cfg","id":"0011223344556677","ts":5,"n":1,"b":"e30","s":"sig"}}""")))
        val c = api().apply { token = "t0k" }.claim("HB-3FA2C1")
        assertEquals(setOf("host", "mport", "aport", "user", "pass"), c.beacon.keys) // PROTOCOL.md 3 `claim` body
        assertEquals("beacon-pw", c.beacon["pass"]!!.jsonPrimitive.content)
        assertEquals(mapOf("n" to "1", "b" to "e30", "s" to "sig"), cfgBody(c.cfg).mapValues { it.value.jsonPrimitive.content })
        assertEquals("""{"did":"HB-3FA2C1"}""", server.takeRequest().body.readUtf8())
    }

    @Test fun errorsReadTheWayTheUserNeedsThem() {
        serve(answers = arrayOf(
            json(401, """{"detail":"bad_login"}"""),
            json(403, """{"detail":"not_assigned"}"""),
            json(409, """{"detail":"claimed"}"""),
            json(401, """{"detail":"login"}"""),
            json(500, "Internal Server Error"),
        ))
        val api = api()
        val bad = failure { api.login("asha", "nope") } as ApiError
        assertEquals(401 to "bad_login", bad.code to bad.reason)
        assertEquals("Wrong username or password.", userMessage(bad))
        val notMine = failure { api.claim("HB-3FA2C1") } as ApiError
        assertEquals(403 to "not_assigned", notMine.code to notMine.reason)
        assertEquals("HB-3FA2C1 isn't on your account yet. Ask your admin to assign it to you, then try again.", userMessage(notMine, "HB-3FA2C1"))
        val taken = failure { api.claim("HB-3FA2C1") } as ApiError
        assertTrue(userMessage(taken, "HB-3FA2C1").startsWith("HB-3FA2C1 belongs to another account. Ask your admin to release it"))
        assertEquals("You were signed out. Sign in again.", userMessage(failure { api.me() }))
        val boom = failure { api.me() } as ApiError
        assertEquals(500 to "http_500", boom.code to boom.reason) // no JSON detail: still an ApiError
    }

    @Test fun tooManyAttemptsHonoursRetryAfterWithoutAskingAgain() {
        serve(answers = arrayOf(
            json(429, """{"detail":"slow_down"}""").setHeader("Retry-After", "42"),
            json(200, """{"token":"t","uid":"asha","name":"Asha","mqtt":{"host":"h","port":8883,"user":"asha","pass":"p"}}"""),
        ))
        val api = api()
        val first = failure { api.login("asha", "pw") } as ApiError
        assertEquals(42L, first.retryAfterS)
        assertEquals("Too many attempts. Try again in 42 s.", userMessage(first))
        now += 30_000
        val second = failure { api.login("asha", "pw") } as ApiError
        assertEquals("Too many attempts. Try again in 12 s.", userMessage(second))
        assertEquals(1, server.requestCount) // the wait is honoured here, not by hammering the server
        now += 12_000
        assertEquals("t", runBlocking { api.login("asha", "pw") }.token) // after the wait it goes through again
        assertEquals(2, server.requestCount)
        assertEquals("Too many attempts. Try again in 10 min.", userMessage(ApiError(429, "slow_down", 600)))
    }

    @Test fun aServerSignedByAnotherCaIsRefused() {
        serve(answers = arrayOf(json(200, "{}")))
        val stranger = HeldCertificate.Builder().certificateAuthority(0).commonName("not ours").build()
        val e = failure { api(trust = stranger).me() }
        assertTrue("$e", e is IOException)
        assertEquals("The server's certificate is not the one this app trusts. Check the server address.", userMessage(e))
        assertEquals(0, server.requestCount)
    }

    @Test fun ourCaButALeafForAnotherAddressIsRefused() {
        val elsewhere = HeldCertificate.Builder().commonName("127.0.0.1").addSubjectAlternativeName("198.51.100.7").signedBy(ca).build()
        serve(elsewhere, json(200, "{}"))
        val e = failure { api().me() }
        assertTrue("$e", e is IOException)
        assertEquals("The server's certificate is not the one this app trusts. Check the server address.", userMessage(e))
        assertEquals(0, server.requestCount)
        // What Android's HttpsURLConnection throws for the same mismatch reads the same.
        assertEquals(userMessage(e), userMessage(SSLPeerUnverifiedException("Hostname 127.0.0.1 not verified")))
    }

    @Test fun nothingListeningReadsAsUnreachable() {
        val port = ServerSocket(0).use { it.localPort } // free, then closed
        val api = HttpApi("127.0.0.1:$port", Tls.pinned(ca.certificatePem().byteInputStream()))
        assertEquals("Can't reach the server. Check the address and your connection.", userMessage(failure { api.me() }))
    }

    @Test fun baseUrlAddsTheApiPortUnlessGiven() {
        assertEquals("https://203.0.113.10:8443", HttpApi.baseUrl(" 203.0.113.10 "))
        assertEquals("https://203.0.113.10:9443", HttpApi.baseUrl("203.0.113.10:9443"))
        assertEquals("https://[2001:db8::1]:8443", HttpApi.baseUrl("[2001:db8::1]"))
        assertEquals("https://203.0.113.10:8443", HttpApi.baseUrl("https://203.0.113.10/"))
    }
}
