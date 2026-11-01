/* AES-256-GCM: utils.encrypt/utils.decrypt for files and the sealed Wi-Fi passwords in ucxapp.wifi. Host only. */
#include "ucxapp_crypt.h"
#include "ucxapp_cmd.h"
#include "ucxapp_store.h"

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FILE   "ucxapp.key"
#define KEY_LEN    32
#define IV_LEN     12
#define TAG_LEN    16
#define SALT_LEN   16
#define ITERATIONS 600000u   /* PBKDF2-HMAC-SHA256, OWASP 2023 */
#define MAX_ITERATIONS 10000000u
#define FILE_MAX   (64u * 1024u * 1024u)
#define MAGIC      "UCXE"
#define VERSION    1
#define HDR_LEN    (4 + 1 + 4 + SALT_LEN + IV_LEN)

static const uint8_t kSecretAad[] = "ucxapp-secret-1";

static bool aead(bool encrypt, const uint8_t *key, const uint8_t *iv, const uint8_t *aad, size_t aadLen,
                 const uint8_t *in, size_t inLen, uint8_t *out, size_t outSize, size_t *outLen)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    psa_status_t st;

    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, KEY_LEN * 8);
    psa_set_key_usage_flags(&attr, encrypt ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    if (psa_import_key(&attr, key, KEY_LEN, &id) != PSA_SUCCESS) {
        return false;
    }
    st = encrypt ? psa_aead_encrypt(id, PSA_ALG_GCM, iv, IV_LEN, aad, aadLen, in, inLen, out, outSize, outLen)
                 : psa_aead_decrypt(id, PSA_ALG_GCM, iv, IV_LEN, aad, aadLen, in, inLen, out, outSize, outLen);
    psa_destroy_key(id);
    return st == PSA_SUCCESS;
}

static bool deriveKey(const char *pass, const uint8_t *salt, uint32_t iterations, uint8_t *key)
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    bool ok = psa_crypto_init() == PSA_SUCCESS &&
              psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
              psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, SALT_LEN) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD, (const uint8_t *)pass,
                                             strlen(pass)) == PSA_SUCCESS &&
              psa_key_derivation_output_bytes(&op, key, KEY_LEN) == PSA_SUCCESS;
    psa_key_derivation_abort(&op);
    return ok;
}

/* ---- per-install key for saved secrets ---- */

static bool installKey(uint8_t *key, bool create)
{
    static uint8_t cached[KEY_LEN];
    static bool have;
    size_t len = 0;
    uint8_t *data;

    if (!have) {
        data = ucxapp_store_load(KEY_FILE, &len);
        if (data != NULL) {
            have = len == KEY_LEN;
            if (have) {
                memcpy(cached, data, KEY_LEN);
            }
            mbedtls_platform_zeroize(data, len);
            free(data);
        } else if (create && psa_crypto_init() == PSA_SUCCESS &&
                   psa_generate_random(cached, KEY_LEN) == PSA_SUCCESS && ucxapp_store_save(KEY_FILE, cached, KEY_LEN)) {
            ucxapp_store_make_private(KEY_FILE);
            have = true;
        }
    }
    if (have) {
        memcpy(key, cached, KEY_LEN);
    }
    return have;
}

static int hexVal(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool ucxapp_secret_seal(const char *plain, char *out, size_t outSize)
{
    size_t len = strlen(plain), clen = 0;
    size_t need = sizeof(UCXAPP_SECRET_PREFIX) + (IV_LEN + len + TAG_LEN) * 2;
    uint8_t key[KEY_LEN];
    uint8_t *buf;
    bool ok;

    if (outSize == 0) {
        return false;
    }
    out[0] = '\0';
    if (len == 0) {
        return true;
    }
    if (outSize < need || (buf = (uint8_t *)malloc(IV_LEN + len + TAG_LEN)) == NULL) {
        return false;
    }
    ok = installKey(key, true) && psa_generate_random(buf, IV_LEN) == PSA_SUCCESS &&
         aead(true, key, buf, kSecretAad, sizeof(kSecretAad) - 1, (const uint8_t *)plain, len, buf + IV_LEN,
              len + TAG_LEN, &clen);
    if (ok) {
        size_t n = (size_t)snprintf(out, outSize, "%s", UCXAPP_SECRET_PREFIX);
        for (size_t i = 0; i < IV_LEN + clen; i++) {
            n += (size_t)snprintf(out + n, outSize - n, "%02x", buf[i]);
        }
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    free(buf);
    return ok;
}

bool ucxapp_secret_open(const char *sealed, char *out, size_t outSize)
{
    size_t prefix = strlen(UCXAPP_SECRET_PREFIX), hex, n, plen = 0;
    uint8_t key[KEY_LEN];
    uint8_t *buf;
    bool ok = true;

    if (outSize == 0) {
        return false;
    }
    out[0] = '\0';
    if (strncmp(sealed, UCXAPP_SECRET_PREFIX, prefix) != 0) {
        return false;
    }
    hex = strlen(sealed + prefix);
    n = hex / 2;
    if (hex % 2 != 0 || n < IV_LEN + TAG_LEN || n - IV_LEN - TAG_LEN >= outSize ||
        (buf = (uint8_t *)malloc(n)) == NULL) {
        return false;
    }
    for (size_t i = 0; i < n && ok; i++) {
        int hi = hexVal(sealed[prefix + 2 * i]), lo = hexVal(sealed[prefix + 2 * i + 1]);
        ok = hi >= 0 && lo >= 0;
        buf[i] = (uint8_t)(hi << 4 | lo);
    }
    ok = ok && installKey(key, false) &&
         aead(false, key, buf, kSecretAad, sizeof(kSecretAad) - 1, buf + IV_LEN, n - IV_LEN, (uint8_t *)out,
              outSize - 1, &plen);
    out[ok ? plen : 0] = '\0';
    mbedtls_platform_zeroize(key, sizeof(key));
    free(buf);
    return ok;
}

/* ---- utils.encrypt / utils.decrypt ---- */

static uint8_t *loadFile(const char *name, size_t *len)
{
    uint8_t *data = ucxapp_store_load(name, len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", name);
    } else if (*len > FILE_MAX) {
        ucxapp_print("ERROR: %s is too big (max %u MB)\n", name, (unsigned)(FILE_MAX >> 20));
        free(data);
        data = NULL;
    }
    return data;
}

static bool saveFile(const char *name, const uint8_t *data, size_t len)
{
    if (!ucxapp_store_save(name, data, len)) {
        ucxapp_print("ERROR: cannot write %s\n", name);
        return false;
    }
    ucxapp_print("wrote %s, %u bytes\n", name, (unsigned)len);
    return true;
}

static int cmdEncrypt(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t key[KEY_LEN];
    size_t len = 0, clen = 0;
    uint8_t *in, *out;
    bool ok;
    (void)app;

    if (argc != 4 || argv[3][0] == '\0') {
        return UCXAPP_ERR_USAGE;
    }
    if ((in = loadFile(argv[1], &len)) == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    out = (uint8_t *)malloc(HDR_LEN + len + TAG_LEN);
    ok = out != NULL;
    if (ok) {
        memcpy(out, MAGIC, 4);
        out[4] = VERSION;
        out[5] = (uint8_t)(ITERATIONS >> 24);
        out[6] = (uint8_t)(ITERATIONS >> 16);
        out[7] = (uint8_t)((ITERATIONS >> 8) & 0xFFu);
        out[8] = (uint8_t)(ITERATIONS & 0xFFu);
        ok = psa_crypto_init() == PSA_SUCCESS && psa_generate_random(out + 9, SALT_LEN + IV_LEN) == PSA_SUCCESS &&
             deriveKey(argv[3], out + 9, ITERATIONS, key) &&
             aead(true, key, out + 9 + SALT_LEN, out, HDR_LEN, in, len, out + HDR_LEN, len + TAG_LEN, &clen);
        if (!ok) {
            ucxapp_print("ERROR: encryption failed\n");
        }
    }
    ok = ok && saveFile(argv[2], out, HDR_LEN + clen);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(in, len);
    free(in);
    free(out);
    return ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdDecrypt(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t key[KEY_LEN];
    size_t len = 0, plen = 0;
    uint8_t *in, *out = NULL;
    uint32_t iterations;
    bool ok;
    (void)app;

    if (argc != 4 || argv[3][0] == '\0') {
        return UCXAPP_ERR_USAGE;
    }
    if ((in = loadFile(argv[1], &len)) == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    ok = len >= HDR_LEN + TAG_LEN && memcmp(in, MAGIC, 4) == 0 && in[4] == VERSION;
    iterations = ok ? (uint32_t)in[5] << 24 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 8 | in[8] : 0;
    if (!ok || iterations == 0 || iterations > MAX_ITERATIONS) {
        ucxapp_print("ERROR: %s is not a utils.encrypt file\n", argv[1]);
        free(in);
        return UCXAPP_ERR_FAILED;
    }
    out = (uint8_t *)malloc(len - HDR_LEN + 1);
    ok = out != NULL && deriveKey(argv[3], in + 9, iterations, key) &&
         aead(false, key, in + 9 + SALT_LEN, in, HDR_LEN, in + HDR_LEN, len - HDR_LEN, out, len - HDR_LEN, &plen);
    if (out != NULL && !ok) {
        ucxapp_print("ERROR: wrong passphrase or %s was changed\n", argv[1]);
    }
    ok = ok && saveFile(argv[2], out, plen);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (out != NULL) {
        mbedtls_platform_zeroize(out, plen);
    }
    free(in);
    free(out);
    return ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gCryptCmds[] = {
    { "utils.encrypt", "<in> <out> <passphrase>", "Encrypt a file with AES-256-GCM, key from the passphrase (PBKDF2-SHA256)", 0, cmdEncrypt },
    { "utils.decrypt", "<in> <out> <passphrase>", "Decrypt a utils.encrypt file; fails on a wrong passphrase or a changed file", 0, cmdDecrypt },
};

void ucxapp_register_crypt_cmds(void)
{
    ucxapp_cmd_register(gCryptCmds, sizeof(gCryptCmds) / sizeof(gCryptCmds[0]));
}
