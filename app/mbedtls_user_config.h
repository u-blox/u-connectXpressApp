/* TF-PSA-Crypto user config for the desktop app, appended to the default psa/crypto_config.h. */
#pragma once

/* Faster P-256 (TLS/DTLS handshakes), as in u-connectMatter; costs a few KB of RAM per curve. */
#undef MBEDTLS_ECP_WINDOW_SIZE
#define MBEDTLS_ECP_WINDOW_SIZE 5
#undef MBEDTLS_ECP_FIXED_POINT_OPTIM
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 1
#define MBEDTLS_ECP_NIST_OPTIM

/* Built-in mbedTLS self tests, run by utils.tls.selftest. */
#define MBEDTLS_SELF_TEST

/* In the browser the Matter node is linked in: its thread and the app's share one PSA key store. */
#if defined(__EMSCRIPTEN__)
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#endif
