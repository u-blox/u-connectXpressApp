/* Secure section of the store: *.key files sealed with AES-256-GCM under the per-install key ucxapp.key, and the
 * sealed short secrets (Wi-Fi passwords, tokens). The install key is wrapped by the OS where it can (Windows DPAPI)
 * and kept in internal flash on STM32 boards. */
#include "ucxapp_secure.h"
#include "ucxapp_cmd.h"
#include "ucxapp_crypt.h"
#include "ucxapp_platform.h"

#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_FILE     "ucxapp.key"
#define KEY_LEN      UCXAPP_SECURE_KEY_LEN
#define IV_LEN       UCXAPP_SECURE_IV_LEN
#define TAG_LEN      UCXAPP_SECURE_TAG_LEN
#define KEY_MAGIC    "UCXK\x01"   /* followed by the platform-wrapped key */
#define KEY_FILE_MAX 1024u
#define MAGIC        "UCXS"
#define VERSION      1
#define HDR_LEN      (4 + 1 + IV_LEN)
#define MAX_FILES    32

static const uint8_t kSecretAad[] = "ucxapp-secret-1";
static const char kFileAad[] = "ucxapp-secure-1";

static const ucxapp_store_ops_t *gKeyOps;
static const char *gKeyWhere;
static bool gKeyWrapped;

bool ucxapp_secure_aead(bool encrypt, const uint8_t *key, const uint8_t *iv, const uint8_t *aad, size_t aadLen,
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

/* ---- per-install key ---- */

static uint8_t *keyFileLoad(size_t *len)
{
    void *h;
    uint8_t *buf;
    int32_t r = 0;

    if (gKeyOps == NULL) {
        return ucxapp_store_load(KEY_FILE, len);
    }
    if ((h = gKeyOps->open(KEY_FILE, false)) == NULL) {
        return NULL;
    }
    *len = 0;
    buf = (uint8_t *)malloc(KEY_FILE_MAX);
    while (buf != NULL && *len < KEY_FILE_MAX && (r = gKeyOps->read(h, buf + *len, KEY_FILE_MAX - *len)) > 0) {
        *len += (size_t)r;
    }
    gKeyOps->close(h);
    if (buf != NULL && r < 0) {
        mbedtls_platform_zeroize(buf, KEY_FILE_MAX);
        free(buf);
        buf = NULL;
    }
    return buf;
}

static bool keyFileSave(const uint8_t *data, size_t len)
{
    void *h;
    bool ok;

    if (gKeyOps == NULL) {
        ok = ucxapp_store_save(KEY_FILE, data, len);
        if (ok) {
            ucxapp_store_make_private(KEY_FILE);
        }
        return ok;
    }
    if ((h = gKeyOps->open(KEY_FILE, true)) == NULL) {
        return false;
    }
    ok = gKeyOps->write(h, data, len) == (int32_t)len;
    gKeyOps->close(h);
    return ok;
}

/* Wrapped by the platform where it can, raw otherwise (onlyWrapped: nothing to write then). */
static bool keySave(const uint8_t *key, bool onlyWrapped)
{
    uint8_t *file = (uint8_t *)malloc(KEY_FILE_MAX);
    size_t n = 0;
    bool wrapped, ok = true;

    if (file == NULL) {
        return false;
    }
    wrapped = ucxapp_platform_key_protect(true, key, KEY_LEN, file + 5, KEY_FILE_MAX - 5, &n);
    if (wrapped) {
        memcpy(file, KEY_MAGIC, 5);
        n += 5;
    } else {
        memcpy(file, key, KEY_LEN);
        n = KEY_LEN;
    }
    if (wrapped || !onlyWrapped) {
        ok = keyFileSave(file, n);
        gKeyWrapped = ok && wrapped;
    }
    mbedtls_platform_zeroize(file, KEY_FILE_MAX);
    free(file);
    return ok;
}

static bool installKey(uint8_t *key, bool create)
{
    static uint8_t cached[KEY_LEN];
    static bool have;
    size_t len = 0, n = 0;
    uint8_t *data;

    if (!have) {
        data = keyFileLoad(&len);
        if (data != NULL) {
            if (len == KEY_LEN) {
                /* raw key from an older version: wrap it now if the platform can */
                memcpy(cached, data, KEY_LEN);
                have = true;
                (void)keySave(cached, true);
            } else if (len > 5 && memcmp(data, KEY_MAGIC, 5) == 0) {
                have = ucxapp_platform_key_protect(false, data + 5, len - 5, cached, KEY_LEN, &n) && n == KEY_LEN;
                gKeyWrapped = have;
            }
            mbedtls_platform_zeroize(data, len);
            free(data);
        } else if (create && psa_crypto_init() == PSA_SUCCESS &&
                   psa_generate_random(cached, KEY_LEN) == PSA_SUCCESS && keySave(cached, false)) {
            have = true;
        }
    }
    if (have) {
        memcpy(key, cached, KEY_LEN);
    }
    return have;
}

/* ---- short secrets as text (Wi-Fi passwords, tokens) ---- */

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
    ok = psa_crypto_init() == PSA_SUCCESS && installKey(key, true) && psa_generate_random(buf, IV_LEN) == PSA_SUCCESS &&
         ucxapp_secure_aead(true, key, buf, kSecretAad, sizeof(kSecretAad) - 1, (const uint8_t *)plain, len,
                            buf + IV_LEN, len + TAG_LEN, &clen);
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
         ucxapp_secure_aead(false, key, buf, kSecretAad, sizeof(kSecretAad) - 1, buf + IV_LEN, n - IV_LEN,
                            (uint8_t *)out, outSize - 1, &plen);
    out[ok ? plen : 0] = '\0';
    mbedtls_platform_zeroize(key, sizeof(key));
    free(buf);
    return ok;
}

/* ---- sealed files ---- */

static const char *baseName(const char *name)
{
    const char *base = name;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }
    return base;
}

static bool isKeyFile(const char *name)
{
    const char *base = baseName(name);
    size_t len = strlen(base);
    const char *ext = base + len - 4;
    return len > 4 && ext[0] == '.' && (ext[1] | 0x20) == 'k' && (ext[2] | 0x20) == 'e' && (ext[3] | 0x20) == 'y' &&
           strcmp(base, KEY_FILE) != 0;
}

bool ucxapp_secure_name(const char *name)
{
    return isKeyFile(name) && baseName(name) == name;
}

static bool isSealed(const uint8_t *data, size_t len)
{
    return len >= 4 && memcmp(data, MAGIC, 4) == 0;
}

/* AAD: the header and the file name, so a sealed file cannot be passed off as another one */
static size_t fileAad(const uint8_t *hdr, const char *name, uint8_t *aad, size_t size)
{
    const char *base = baseName(name);
    size_t baseLen = strlen(base);
    size_t n = sizeof(kFileAad) - 1 + HDR_LEN + baseLen;

    if (n > size) {
        return 0;
    }
    memcpy(aad, kFileAad, sizeof(kFileAad) - 1);
    memcpy(aad + sizeof(kFileAad) - 1, hdr, HDR_LEN);
    memcpy(aad + sizeof(kFileAad) - 1 + HDR_LEN, base, baseLen);
    return n;
}

static bool hookSecret(const char *name, bool *mayBeSealed)
{
    *mayBeSealed = isKeyFile(name);
    return ucxapp_secure_name(name);
}

static uint8_t *hookSeal(const char *name, const uint8_t *plain, size_t len, size_t *outLen)
{
    uint8_t key[KEY_LEN];
    uint8_t aad[160];
    size_t aadLen, clen = 0;
    uint8_t *out = (uint8_t *)malloc(HDR_LEN + len + TAG_LEN);
    bool ok = out != NULL;

    if (ok) {
        memcpy(out, MAGIC, 4);
        out[4] = VERSION;
        ok = psa_crypto_init() == PSA_SUCCESS && psa_generate_random(out + 5, IV_LEN) == PSA_SUCCESS &&
             (aadLen = fileAad(out, name, aad, sizeof(aad))) > 0 && installKey(key, true) &&
             ucxapp_secure_aead(true, key, out + 5, aad, aadLen, plain, len, out + HDR_LEN, len + TAG_LEN, &clen);
        mbedtls_platform_zeroize(key, sizeof(key));
    }
    if (!ok) {
        free(out);
        return NULL;
    }
    *outLen = HDR_LEN + clen;
    return out;
}

static bool hookOpen(const char *name, uint8_t *data, size_t *len)
{
    uint8_t key[KEY_LEN];
    uint8_t aad[160];
    size_t aadLen, plen = 0;
    uint8_t *plain;
    bool ok;

    if (!isSealed(data, *len)) {
        return true;   /* stored before the secure section existed, or copied in by hand */
    }
    if (*len < HDR_LEN + TAG_LEN || data[4] != VERSION || (plain = (uint8_t *)malloc(*len)) == NULL) {
        return false;
    }
    ok = (aadLen = fileAad(data, name, aad, sizeof(aad))) > 0 && installKey(key, false) &&
         ucxapp_secure_aead(false, key, data + 5, aad, aadLen, data + HDR_LEN, *len - HDR_LEN, plain, *len, &plen);
    mbedtls_platform_zeroize(key, sizeof(key));
    if (ok) {
        memcpy(data, plain, plen);
        mbedtls_platform_zeroize(data + plen, *len - plen);
        *len = plen;
    }
    mbedtls_platform_zeroize(plain, *len);
    free(plain);
    return ok;
}

static const ucxapp_store_secure_t gHook = { hookSecret, hookSeal, hookOpen };

/* ---- startup and utils.secure ---- */

typedef struct {
    char *names[MAX_FILES];
    int count;
} fileList_t;

static void collect(const char *name, uint64_t size, void *ctx)
{
    fileList_t *list = (fileList_t *)ctx;
    size_t len = strlen(name) + 1;
    (void)size;
    if (list->count < MAX_FILES && ucxapp_secure_name(name) && (list->names[list->count] = (char *)malloc(len)) != NULL) {
        memcpy(list->names[list->count++], name, len);
    }
}

static void listFree(fileList_t *list)
{
    for (int i = 0; i < list->count; i++) {
        free(list->names[i]);
    }
    list->count = 0;
}

/* 1 sealed, 0 plain, -1 unreadable */
static int fileState(const char *name)
{
    size_t len = 0;
    uint8_t *data = ucxapp_store_load_raw(name, &len);
    int state = data == NULL ? -1 : isSealed(data, len) ? 1 : 0;
    if (data != NULL) {
        mbedtls_platform_zeroize(data, len);
        free(data);
    }
    return state;
}

void ucxapp_secure_set_key_store(const ucxapp_store_ops_t *ops, const char *where)
{
    gKeyOps = ops;
    gKeyWhere = where;
}

void ucxapp_secure_init(void)
{
    fileList_t list = { { NULL }, 0 };

    ucxapp_store_set_secure(&gHook);
    (void)ucxapp_store_list(collect, &list);
    for (int i = 0; i < list.count; i++) {
        size_t len = 0;
        uint8_t *data = fileState(list.names[i]) == 0 ? ucxapp_store_load_raw(list.names[i], &len) : NULL;
        if (data != NULL) {
            (void)ucxapp_store_save(list.names[i], data, len);   /* seals it */
            mbedtls_platform_zeroize(data, len);
            free(data);
        }
    }
    listFree(&list);
}

static void printKeyProtection(void)
{
    int rdp = ucxapp_platform_readout_protection();
    uint8_t key[KEY_LEN];

    if (!installKey(key, false)) {
        ucxapp_print("install key: none yet (made when the first secret is saved)\n");
        return;
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    if (gKeyWrapped) {
        ucxapp_print("install key: %s, wrapped by the OS for this user on this computer (DPAPI)\n", KEY_FILE);
    } else if (gKeyWhere != NULL) {
        ucxapp_print("install key: %s in %s\n", KEY_FILE, gKeyWhere);
    } else {
#ifdef _WIN32
        ucxapp_print("install key: %s, not wrapped (DPAPI unavailable)\n", KEY_FILE);
#else
        ucxapp_print("install key: %s, readable only by this user\n", KEY_FILE);
#endif
    }
    if (rdp >= 0) {
        ucxapp_print("readout protection: level %d%s\n", rdp,
                     rdp == 0 ? " (flash readable over SWD; set level 1 with STM32CubeProgrammer)" : "");
    }
}

static int cmdSecure(ucxapp_t *app, int argc, char *argv[])
{
    fileList_t list = { { NULL }, 0 };
    int sealed = 0, plain = 0;
    (void)app;
    (void)argv;

    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    printKeyProtection();
    (void)ucxapp_store_list(collect, &list);
    for (int i = 0; i < list.count; i++) {
        int state = fileState(list.names[i]);
        ucxapp_print("  %-32s %s\n", list.names[i], state > 0 ? "sealed" : state == 0 ? "PLAIN" : "unreadable");
        sealed += state > 0;
        plain += state == 0;
    }
    listFree(&list);
    ucxapp_print("%d private key file(s) sealed with AES-256-GCM, %d plain\n", sealed, plain);
    return plain == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gSecureCmds[] = {
    { "utils.secure", "", "Secure store: how the install key is kept and which private keys are sealed", 0, cmdSecure },
};

void ucxapp_register_secure_cmds(void)
{
    ucxapp_cmd_register(gSecureCmds, sizeof(gSecureCmds) / sizeof(gSecureCmds[0]));
}
