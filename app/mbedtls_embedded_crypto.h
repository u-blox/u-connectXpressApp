/* TF-PSA-Crypto user config for the STM32 boards (FreeRTOS and Zephyr), appended to psa/crypto_config.h.
 *
 * Randomness: there is no OS entropy, so MBEDTLS_PSA_DRIVER_GET_ENTROPY takes it from the MCU's hardware
 * true random number generator (mbedtls_platform_get_entropy() in ucxapp_rng_stm32.c / ucxapp_rng_zephyr.c).
 * It seeds CTR_DRBG (NIST SP 800-90A, AES-256), which produces every key, nonce and web token. A board whose
 * TRNG fails its health checks makes psa_crypto_init() fail: there is no fallback to a weak source.
 * See docs/SECURITY.md (Random numbers). */
#pragma once

#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/* No file system: keys live in RAM, certificates in the app store (littlefs). */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
/* Certificate dates come from the build date; peers' certificates are not checked against a clock. */
#undef MBEDTLS_HAVE_TIME_DATE
/* TLS 1.3 session ages use a millisecond clock: the RTOS tick (mbedtls_ms_time() next to the TRNG driver). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_HMAC_DRBG_C

/* What a browser needs: ECDHE (P-256, P-384, X25519) with ECDSA P-256 certificates, AES-GCM or ChaCha20-Poly1305,
 * SHA-256/384. RSA, finite-field DH and the legacy ciphers and hashes are left out. */
#undef PSA_WANT_ALG_CBC_NO_PADDING
#undef PSA_WANT_ALG_CBC_PKCS7
#undef PSA_WANT_ALG_CCM
#undef PSA_WANT_ALG_CCM_STAR_NO_TAG
#undef PSA_WANT_ALG_CMAC
#undef PSA_WANT_ALG_CFB
#undef PSA_WANT_ALG_CTR
#undef PSA_WANT_ALG_FFDH
#undef PSA_WANT_ALG_JPAKE
#undef PSA_WANT_ALG_MD5
#undef PSA_WANT_ALG_OFB
#undef PSA_WANT_ALG_PBKDF2_HMAC
#undef PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128
#undef PSA_WANT_ALG_RIPEMD160
#undef PSA_WANT_ALG_RSA_OAEP
#undef PSA_WANT_ALG_RSA_PKCS1V15_CRYPT
#undef PSA_WANT_ALG_RSA_PKCS1V15_SIGN
#undef PSA_WANT_ALG_RSA_PSS
#undef PSA_WANT_ALG_SHA_224
#undef PSA_WANT_ALG_SHA3_224
#undef PSA_WANT_ALG_SHA3_256
#undef PSA_WANT_ALG_SHA3_384
#undef PSA_WANT_ALG_SHA3_512
#undef PSA_WANT_ALG_SHAKE128
#undef PSA_WANT_ALG_SHAKE256
#undef PSA_WANT_ALG_STREAM_CIPHER
#undef PSA_WANT_ALG_TLS12_PSK_TO_MS
#undef PSA_WANT_ALG_TLS12_ECJPAKE_TO_PMS
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_256
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_384
#undef PSA_WANT_ECC_BRAINPOOL_P_R1_512
#undef PSA_WANT_ECC_MONTGOMERY_448
#undef PSA_WANT_ECC_SECP_K1_256
#undef PSA_WANT_ECC_SECP_R1_521
#undef PSA_WANT_DH_RFC7919_2048
#undef PSA_WANT_DH_RFC7919_3072
#undef PSA_WANT_DH_RFC7919_4096
#undef PSA_WANT_DH_RFC7919_6144
#undef PSA_WANT_DH_RFC7919_8192
#undef PSA_WANT_KEY_TYPE_PASSWORD
#undef PSA_WANT_KEY_TYPE_PASSWORD_HASH
#undef PSA_WANT_KEY_TYPE_ARIA
#undef PSA_WANT_KEY_TYPE_CAMELLIA
#undef PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY
#undef PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT
#undef PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE
