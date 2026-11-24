package com.heartbeacon.app.link

import java.io.InputStream
import java.net.InetAddress
import java.security.KeyStore
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSession
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.TrustManagerFactory

/**
 * TLS for the API (8443) and the broker (8883): trust anchored only on the private CA bundled as
 * `assets/hb_ca.pem` (plan decision 5), never the system store, and the leaf must name the address
 * dialled. The server is a bare public IP, so that is an IP SAN (`deploy/mkca.sh` issues `IP:<ip>`).
 */
object Tls {
    fun pinned(caPem: InputStream): SSLSocketFactory {
        val ca = caPem.use { CertificateFactory.getInstance("X.509").generateCertificate(it) }
        val ks = KeyStore.getInstance(KeyStore.getDefaultType()).apply {
            load(null)
            setCertificateEntry("hb-ca", ca)
        }
        val tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(ks) }
        return SSLContext.getInstance("TLS").apply { init(null, tmf.trustManagers, null) }.socketFactory
    }

    /** The one hostname check for both HTTPS and MQTT, so the device runs what the tests run. */
    val host = HostnameVerifier { host: String, session: SSLSession ->
        val leaf = runCatching { session.peerCertificates.firstOrNull() as? X509Certificate }.getOrNull()
        leaf != null && names(host, leaf)
    }

    /**
     * True when [cert] has a SAN for [host]: an IP SAN equal to an IP literal (any spelling of the same
     * address), or a DNS SAN equal to a name, ignoring case. No wildcards and no CN fallback.
     */
    fun names(host: String, cert: X509Certificate): Boolean {
        val h = host.removeSurrounding("[", "]")
        val literal = h.isNotEmpty() && (':' in h || h.all { it.isDigit() || it == '.' })
        val want = if (literal) runCatching { InetAddress.getByName(h) }.getOrNull() ?: return false else null
        return cert.subjectAlternativeNames.orEmpty().any { san ->
            val value = san.getOrNull(1) as? String ?: return@any false
            when (san[0] as? Int) {
                IP_SAN -> want != null && runCatching { InetAddress.getByName(value) == want }.getOrDefault(false)
                DNS_SAN -> want == null && value.equals(h, ignoreCase = true)
                else -> false
            }
        }
    }

    private const val DNS_SAN = 2
    private const val IP_SAN = 7
}
