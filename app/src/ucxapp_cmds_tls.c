/* tls.* - local CA and certificates for the app's own TLS servers (stream bridge, web server). */

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"
#include "ucxapp_tls.h"

#include "mbedtls/build_info.h"
#include "mbedtls/base64.h"
#include "mbedtls/private/aes.h"
#include "mbedtls/private/bignum.h"
#include "mbedtls/private/ccm.h"
#include "mbedtls/private/chacha20.h"
#include "mbedtls/private/chachapoly.h"
#include "mbedtls/private/cmac.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/gcm.h"
#include "mbedtls/private/hmac_drbg.h"
#include "mbedtls/private/md5.h"
#include "mbedtls/private/poly1305.h"
#include "mbedtls/private/rsa.h"
#include "mbedtls/private/sha1.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/sha3.h"
#include "mbedtls/private/sha512.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    int (*run)(int verbose);
} kSelfTests[] = {
#if defined(MBEDTLS_SELF_TEST)
#if defined(MBEDTLS_MD5_C)
    { "md5", mbedtls_md5_self_test },
#endif
#if defined(MBEDTLS_SHA1_C)
    { "sha1", mbedtls_sha1_self_test },
#endif
#if defined(MBEDTLS_SHA224_C)
    { "sha224", mbedtls_sha224_self_test },
#endif
#if defined(MBEDTLS_SHA256_C)
    { "sha256", mbedtls_sha256_self_test },
#endif
#if defined(MBEDTLS_SHA384_C)
    { "sha384", mbedtls_sha384_self_test },
#endif
#if defined(MBEDTLS_SHA512_C)
    { "sha512", mbedtls_sha512_self_test },
#endif
#if defined(MBEDTLS_SHA3_C)
    { "sha3", mbedtls_sha3_self_test },
#endif
#if defined(MBEDTLS_AES_C)
    { "aes", mbedtls_aes_self_test },
#endif
#if defined(MBEDTLS_GCM_C) && defined(MBEDTLS_AES_C)
    { "gcm", mbedtls_gcm_self_test },
#endif
#if defined(MBEDTLS_CCM_C) && defined(MBEDTLS_AES_C)
    { "ccm", mbedtls_ccm_self_test },
#endif
#if defined(MBEDTLS_CMAC_C)
    { "cmac", mbedtls_cmac_self_test },
#endif
#if defined(MBEDTLS_CHACHA20_C)
    { "chacha20", mbedtls_chacha20_self_test },
#endif
#if defined(MBEDTLS_POLY1305_C)
    { "poly1305", mbedtls_poly1305_self_test },
#endif
#if defined(MBEDTLS_CHACHAPOLY_C)
    { "chacha20-poly1305", mbedtls_chachapoly_self_test },
#endif
#if defined(MBEDTLS_BASE64_C)
    { "base64", mbedtls_base64_self_test },
#endif
#if defined(MBEDTLS_BIGNUM_C)
    { "mpi", mbedtls_mpi_self_test },
#endif
#if defined(MBEDTLS_RSA_C)
    { "rsa", mbedtls_rsa_self_test },
#endif
#if defined(MBEDTLS_CTR_DRBG_C)
    { "ctr_drbg", mbedtls_ctr_drbg_self_test },
#endif
#if defined(MBEDTLS_HMAC_DRBG_C)
    { "hmac_drbg", mbedtls_hmac_drbg_self_test },
#endif
#if defined(MBEDTLS_ECP_C)
    { "ecp", mbedtls_ecp_self_test },
#endif
#endif /* MBEDTLS_SELF_TEST */
    { NULL, NULL }
};

/* Runs the mbedTLS built-in known-answer tests quietly and prints one line per algorithm. */
static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    int failed = 0;
    int count = 0;
    (void)app;
    (void)argv;

    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; kSelfTests[i].name != NULL; i++) {
        uint32_t start = ucxapp_platform_tick_ms();
        int r = kSelfTests[i].run(0);
        ucxapp_print("  %-18s %s  (%u ms)\n", kSelfTests[i].name, r == 0 ? "passed" : "FAILED",
                     (unsigned)(ucxapp_platform_tick_ms() - start));
        failed += r != 0;
        count++;
    }
    if (count == 0) {
        ucxapp_print("ERROR: mbedTLS was built without MBEDTLS_SELF_TEST\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("mbedTLS self test: %d of %d passed\n", count - failed, count);
    return failed == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdCaCreate(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char err[160] = "";
    bool force = argc > 1 && strcmp(argv[argc - 1], "force") == 0;
    int named = argc - 1 - (force ? 1 : 0);
    if (named > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_tls_create_ca(named == 1 ? argv[1] : NULL, force, err, sizeof(err))) {
        ucxapp_print("ERROR: %s\n", err);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("created local CA tls-ca.crt / tls-ca.key\n");
    ucxapp_print("install tls-ca.crt as a trusted root on clients (utils.tls.pem ca prints it)\n");
    return UCXAPP_OK;
}

static int createCert(int argc, char *argv[], bool client)
{
    char err[160] = "";
    if (argc < 2 || (client && argc != 2)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_tls_create_cert(argv[1], client, (const char *const *)&argv[2], argc - 2, err, sizeof(err))) {
        ucxapp_print("ERROR: %s\n", err);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("created tls-%s.crt / tls-%s.key (signed by the local CA)\n", argv[1], argv[1]);
    if (!client && argc == 2) {
        ucxapp_print("warning: no host names or IP addresses, browsers will reject the name\n");
    }
    return UCXAPP_OK;
}

static int cmdServerCreate(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    return createCert(argc, argv, false);
}

static int cmdClientCreate(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    return createCert(argc, argv, true);
}

static void listOne(const char *name, uint64_t size, void *ctx)
{
    (void)size;
    int *count = ctx;
    size_t n = strlen(name);
    if (strncmp(name, "tls-", 4) != 0 || n < 9 || strcmp(name + n - 4, ".crt") != 0) {
        return;
    }
    char cert[64], info[400];
    snprintf(cert, sizeof(cert), "%.*s", (int)(n - 8), name + 4);
    if (ucxapp_tls_describe(cert, info, sizeof(info))) {
        ucxapp_print("%-12s %s\n", cert, info);
    } else {
        ucxapp_print("%-12s (unreadable)\n", cert);
    }
    (*count)++;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    int count = 0;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (ucxapp_store_list(listOne, &count) < 0) {
        ucxapp_print("ERROR: cannot list the store\n");
        return UCXAPP_ERR_FAILED;
    }
    if (count == 0) {
        ucxapp_print("no certificates, start with utils.tls.ca.create\n");
    }
    return UCXAPP_OK;
}

static int cmdPem(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char file[96];
    size_t len = 0;
    bool key = argc == 3 && strcmp(argv[2], "key") == 0;
    if (argc != 2 && !key) {
        return UCXAPP_ERR_USAGE;
    }
    if (key) {
        ucxapp_tls_key_file(argv[1], file, sizeof(file));
    } else {
        ucxapp_tls_cert_file(argv[1], file, sizeof(file));
    }
    uint8_t *pem = ucxapp_store_load(file, &len);
    if (!pem) {
        ucxapp_print("ERROR: %s not found\n", file);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s", (const char *)pem);
    free(pem);
    return UCXAPP_OK;
}

static int cmdRemove(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char crt[96], key[96];
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_tls_cert_file(argv[1], crt, sizeof(crt));
    ucxapp_tls_key_file(argv[1], key, sizeof(key));
    bool a = ucxapp_store_remove(crt);
    bool b = ucxapp_store_remove(key);
    if (!a && !b) {
        ucxapp_print("ERROR: %s not found\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("removed %s\n", argv[1]);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gTlsCmds[] = {
    { "utils.tls.ca.create",     "[cn] [force]",            "Create the local CA (ECDSA P-256) that signs the app certificates", 0, cmdCaCreate },
    { "utils.tls.server.create", "<name> <host|ip> [...]",  "Create a server certificate for the given host names / IPv4 addresses", 0, cmdServerCreate },
    { "utils.tls.client.create", "<name>",                  "Create a client certificate (for tls clientauth)", 0, cmdClientCreate },
    { "utils.tls.list",          "",                        "List the app certificates", 0, cmdList },
    { "utils.tls.pem",           "<name> [key]",            "Print a certificate (or its key) in PEM", 0, cmdPem },
    { "utils.tls.remove",        "<name>",                  "Remove a certificate and its key", UCXAPP_CMD_DANGER, cmdRemove },
    { "utils.tls.selftest",      "",                        "Run the mbedTLS built-in known-answer tests (hashes, ciphers, DRBGs, bignum, RSA, ECP)", 0, cmdSelftest },
};

void ucxapp_register_tls_cmds(void)
{
    ucxapp_cmd_register(gTlsCmds, sizeof(gTlsCmds) / sizeof(gTlsCmds[0]));
}
