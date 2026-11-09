#ifndef UCXAPP_CRYPT_H
#define UCXAPP_CRYPT_H

/* Sealed short secrets (ucxapp_secure.c) and utils.encrypt (ucxapp_crypt.c); built where mbedTLS is linked. */

#include <stdbool.h>
#include <stddef.h>

#define UCXAPP_SECRET_PREFIX "a1:"
/** Text size of a sealed secret of plainMax bytes: prefix, hex of IV + cipher text + tag, NUL. */
#define UCXAPP_SECRET_LEN(plainMax) (sizeof(UCXAPP_SECRET_PREFIX) + ((plainMax) + 28) * 2)

/** Seals a short secret (a saved Wi-Fi password) with the per-install key in the store file ucxapp.key,
    created on first use. An empty secret gives an empty string. */
bool ucxapp_secret_seal(const char *plain, char *out, size_t outSize);

/** Opens a text from ucxapp_secret_seal; false if the key is missing or the text was changed. */
bool ucxapp_secret_open(const char *sealed, char *out, size_t outSize);

#endif /* UCXAPP_CRYPT_H */
