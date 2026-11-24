package com.heartbeacon.app.link

import okhttp3.tls.HeldCertificate
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate

class TlsTest {
    private val ca = HeldCertificate.Builder().certificateAuthority(0).build()
    private fun leaf(vararg sans: String, cn: String = "hb") =
        HeldCertificate.Builder().commonName(cn).apply { sans.forEach { addSubjectAlternativeName(it) } }.signedBy(ca).build().certificate

    @Test fun ipSanMustNameTheDialledAddress() {
        val c = leaf("203.0.113.10")
        assertTrue(Tls.names("203.0.113.10", c))
        assertFalse(Tls.names("203.0.113.11", c))
        assertFalse(Tls.names("203.0.113.1", c))
    }

    @Test fun ipv6SpellingsAreOneAddress() {
        val c = leaf("2001:db8::1")
        assertTrue(Tls.names("2001:db8:0:0:0:0:0:1", c))
        assertTrue(Tls.names("[2001:db8::1]", c))
        assertFalse(Tls.names("2001:db8::2", c))
    }

    @Test fun noCnFallbackAndNoCrossingIpAndDns() {
        assertFalse(Tls.names("203.0.113.10", leaf(cn = "203.0.113.10"))) // CN is not a SAN
        assertFalse(Tls.names("203.0.113.10", leaf("hb.example")))
        val dns = leaf("hb.example")
        assertTrue(Tls.names("HB.example", dns))
        assertFalse(Tls.names("x.hb.example", dns))
    }

    @Test fun bundledCaIsACertificateAuthority() {
        val pem = File("src/main/assets/hb_ca.pem")
        Tls.pinned(pem.inputStream()) // parses into a trust store
        val ca = pem.inputStream().use { CertificateFactory.getInstance("X.509").generateCertificate(it) } as X509Certificate
        assertTrue(ca.basicConstraints >= 0)
        assertEquals(ca.subjectX500Principal, ca.issuerX500Principal) // a root, as deploy/mkca.sh makes
    }
}
