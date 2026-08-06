/**
 * File: mbedtls_config.h
 * Description: Minimal mbedTLS configuration for the firmware-update
 * version check (sidetnfs_update_check.c) -- a single, short-lived TLS 1.2
 * client connection to raw.githubusercontent.com to fetch version.txt.
 * TLS client only (no server, no client-certificate auth), certificate
 * verification intentionally disabled (see sidetnfs_update_check.c's own
 * doc comment for the reasoning) -- so X.509 chain-building/verification
 * code is not needed, only enough X.509 parsing for mbedTLS's handshake
 * state machine to process the server's Certificate message structurally.
 * Buffer sizes trimmed well below mbedTLS's 16KB defaults -- this device
 * has 264KB of RAM total, shared with the WiFi stack, lwIP, and the rest
 * of the GEMDOS emulation firmware.
 */
#include <limits.h> // some mbedtls source files use INT_MAX without including this themselves

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

// Trimmed record buffers -- a version.txt response is a handful of bytes,
// and the handshake messages (ServerHello/Certificate/etc.) comfortably
// fit well under mbedTLS's 16KB default.
#define MBEDTLS_SSL_IN_CONTENT_LEN 4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN 2048

// TLS 1.2 client, modern ECDHE key exchange (small certs, no RSA
// key-exchange fallback needed since we're not verifying the chain
// anyway) plus the AES-GCM/SHA-256 primitives it needs.
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C

#define MBEDTLS_GCM_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_CIPHER_C

#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_MD_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_PLATFORM_C
