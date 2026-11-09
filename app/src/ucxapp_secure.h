#ifndef UCXAPP_SECURE_H
#define UCXAPP_SECURE_H

/* Secure section of the store: private keys (*.key) are sealed with AES-256-GCM under a per-install key
 * (ucxapp.key). Windows wraps that key with DPAPI; STM32 boards keep it in internal flash behind readout
 * protection. Built where mbedTLS is linked (hosts and the STM32 web boards, UCXAPP_SECURE). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ucxapp_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCXAPP_SECURE_KEY_LEN 32
#define UCXAPP_SECURE_IV_LEN  12
#define UCXAPP_SECURE_TAG_LEN 16

/** Installs the store hook, then seals the private keys still stored in plain text. Call after the store is set up. */
void ucxapp_secure_init(void);

/** Keeps the install key in other storage than the files (STM32: internal flash while files are on the SD card);
    where describes it for utils.secure. Call before ucxapp_secure_init. */
void ucxapp_secure_set_key_store(const ucxapp_store_ops_t *ops, const char *where);

/** True for names the secure section seals: store-relative *.key files except ucxapp.key. */
bool ucxapp_secure_name(const char *name);

/** AES-256-GCM with a 12-byte IV; the output holds the 16-byte tag after the cipher text when encrypting. */
bool ucxapp_secure_aead(bool encrypt, const uint8_t *key, const uint8_t *iv, const uint8_t *aad, size_t aadLen,
                        const uint8_t *in, size_t inLen, uint8_t *out, size_t outSize, size_t *outLen);

void ucxapp_register_secure_cmds(void);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_SECURE_H */
