#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// --- platform ---
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY      // no /dev/urandom on the pico; we seed manually

// --- TLS client, TLS1.2 stream only (keeps flash/RAM down) ---
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  // required for mbedtls_ssl_set_hostname() to actually
                                             // add the SNI extension - without it, it's silently a no-op

// --- record buffer sizes. IN must be big enough to hold one full TLS record;
// this broker sends its whole 3-cert Let's Encrypt chain (~4.3KB) as a single
// certificate record, so 4096 was too small. OUT stays small since our own
// writes (ClientHello, small MQTT/WS frames) are tiny.
#define MBEDTLS_SSL_IN_CONTENT_LEN       8192
#define MBEDTLS_SSL_OUT_CONTENT_LEN      2048

// --- cert parsing (needed even with verify=NONE, handshake still parses the cert msg) ---
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_OID_C

// --- crypto primitives for common cipher suites (ECDHE-RSA-AES-GCM etc, 128 and 256 bit) ---
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

// --- key exchange methods: mbedtls 3.x does NOT auto-enable these just because
// RSA/ECDH/ECDSA are on above - they must be switched on explicitly, or the
// handshake has no cipher suites to offer at all.
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED

#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C   // needed for the *-GCM-SHA384 (AES-256) suites specifically -
                           // SHA512_C alone doesn't imply this in newer mbedtls
#define MBEDTLS_SHA512_C

// --- RNG ---
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// NOTE: do NOT #include "mbedtls/check_config.h" here. mbedtls 3.x includes it
// automatically at the correct point (after it derives things like available
// key-exchange methods from the primitives above). Including it manually runs
// validation too early and produces false "prerequisites missing" errors.

#endif /* MBEDTLS_CONFIG_H */