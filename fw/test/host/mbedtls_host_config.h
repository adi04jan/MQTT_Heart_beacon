// mbedTLS features the host build needs: hb_config_verify (ECDSA P-256, SHA-256, SPKI in DER or PEM) and the
// pinned-file check (X.509 CA). The firmware uses IDF's full mbedTLS config instead.
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C  // mbedTLS requires it on Windows
#define MBEDTLS_SHA256_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
