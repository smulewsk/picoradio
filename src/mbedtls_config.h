#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* Minimal TLS 1.2 client for HTTPS radio streams and the Gemini WebSocket
 * (ECDHE + AES-GCM / ChaCha20-Poly1305, ECDSA/RSA certificates). */

/* Platform: allocations go through mem_calloc/mem_free (both heaps) */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT          /* RP2350 TRNG via pico_rand */
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_ECP_NIST_OPTIM

/* Crypto */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_MODE_CBC                    /* older radio servers */
#define MBEDTLS_SHA1_C                             /* old CBC suites, legacy certs */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Certificates */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK   /* roots parsed on demand */

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED           /* old radio servers */
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384   /* server records can be 16 KB */
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096

#define MBEDTLS_ERROR_C

#endif
