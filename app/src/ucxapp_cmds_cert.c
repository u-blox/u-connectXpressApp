#include "ucxapp_cmd.h"
#include "ucxapp_hash.h"
#include "ucxapp_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_at_client.h"
#include "u_cx_security.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define CERT_MAX_FILE 8192
#define CERT_MAX_NAME 127

static const char *typeName(uSecCertType_t type)
{
    switch (type) {
        case U_SEC_CERT_TYPE_ROOT:   return "ca";
        case U_SEC_CERT_TYPE_CLIENT: return "client";
        case U_SEC_CERT_TYPE_KEY:    return "key";
        default:                     return "?";
    }
}

static bool parseType(const char *s, uSecCertType_t *type)
{
    if (strcmp(s, "ca") == 0) {
        *type = U_SEC_CERT_TYPE_ROOT;
    } else if (strcmp(s, "client") == 0) {
        *type = U_SEC_CERT_TYPE_CLIENT;
    } else if (strcmp(s, "key") == 0) {
        *type = U_SEC_CERT_TYPE_KEY;
    } else {
        return false;
    }
    return true;
}

static int b64Value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* DER of the first CERTIFICATE block in a PEM file (malloc'ed), NULL if there is none. */
static uint8_t *pemToDer(const char *pem, size_t *derLen)
{
    const char *begin = strstr(pem, "-----BEGIN CERTIFICATE-----");
    if (begin == NULL) {
        return NULL;
    }
    begin += strlen("-----BEGIN CERTIFICATE-----");
    const char *end = strstr(begin, "-----END CERTIFICATE-----");
    if (end == NULL) {
        return NULL;
    }
    uint8_t *der = (uint8_t *)malloc((size_t)(end - begin) * 3 / 4 + 3);
    if (der == NULL) {
        return NULL;
    }
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = begin; p < end; p++) {
        int v = b64Value((unsigned char)*p);
        if (v < 0) {
            continue;   /* whitespace and '=' padding */
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der[n++] = (uint8_t)(acc >> bits);
        }
    }
    *derLen = n;
    return der;
}

static void toHexUpper(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02X", data[i]);
    }
}

/* Looks up the type of an installed certificate; false if the module does not list it. */
static bool findType(ucxapp_t *app, const char *name, uSecCertType_t *type)
{
    uCxSecListCertificates_t info;
    bool found = false;

    uCxSecurityListCertificatesBegin(app->ucx);
    while (uCxSecurityListCertificatesGetNext(app->ucx, &info)) {
        if (!found && strcmp(info.name, name) == 0) {
            *type = info.cert_type;
            found = true;
        }
    }
    uCxEnd(app->ucx);
    return found;
}

/* Module fingerprint as upper-case hex; false if unavailable. */
static bool moduleFingerprint(ucxapp_t *app, const char *name, char *hex, size_t hexSize)
{
    uCxSecReadCertificatesDetails_t d;
    bool found = false;

    /* One request per detail: each Begin sends AT+USECD again and reads a single response line. */
    if (uCxSecurityReadCertificatesDetailsBegin(app->ucx, name, U_SEC_CERT_DETAIL_ID_FINGERPRINT, &d) &&
        d.type == U_CX_SECURITY_READ_CERTIFICATES_DETAILS_RSP_TYPE_CERT_DETAIL_ID_BYTES) {
        size_t n = d.rsp.CertDetailIdBytes.hex_value.length;
        if (n * 2 < hexSize) {
            toHexUpper(d.rsp.CertDetailIdBytes.hex_value.pData, n, hex);
            found = true;
        }
    }
    uCxEnd(app->ucx);
    return found;
}

static int cmdCertList(ucxapp_t *app, int argc, char *argv[])
{
    (void)argv;
    uCxSecListCertificates_t info;
    int count = 0;

    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxSecurityListCertificatesBegin(app->ucx);
    while (uCxSecurityListCertificatesGetNext(app->ucx, &info)) {
        ucxapp_print("%-8s %s\n", typeName(info.cert_type), info.name);
        count++;
    }
    uCxEnd(app->ucx);
    if (count == 0) {
        ucxapp_print("no certificates installed\n");
    }
    return UCXAPP_OK;
}

static int cmdCertInfo(ucxapp_t *app, int argc, char *argv[])
{
    uCxSecReadCertificatesDetails_t d;
    char hex[UCXAPP_SHA256_HEX_LEN];

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!moduleFingerprint(app, argv[1], hex, sizeof(hex))) {
        ucxapp_print("ERROR: certificate %s not found\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sha256:       %s\n", hex);

    if (uCxSecurityReadCertificatesDetailsBegin(app->ucx, argv[1], U_SEC_CERT_DETAIL_ID_CERTIFICATE_SIZE, &d) &&
        d.type == U_CX_SECURITY_READ_CERTIFICATES_DETAILS_RSP_TYPE_CERT_DETAIL_ID_INT) {
        ucxapp_print("size:         %d bytes\n", (int)d.rsp.CertDetailIdInt.int_value);
    }
    uCxEnd(app->ucx);
    return UCXAPP_OK;
}

static const char *baseName(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    if (back != NULL && (slash == NULL || back > slash)) {
        slash = back;
    }
    return slash != NULL ? slash + 1 : path;
}

int ucxapp_cert_upload(ucxapp_t *app, int certType, const char *name, const uint8_t *data, size_t len,
                       const char *pw)
{
    uSecCertType_t type = (uSecCertType_t)certType;

    if (name[0] == '\0' || strlen(name) > CERT_MAX_NAME) {
        ucxapp_print("ERROR: invalid certificate name\n");
        return UCXAPP_ERR_FAILED;
    }
    if (len == 0 || len > CERT_MAX_FILE) {
        ucxapp_print("ERROR: file size %u is outside 1-%d bytes\n", (unsigned)len, CERT_MAX_FILE);
        return UCXAPP_ERR_FAILED;
    }

    char localFp[UCXAPP_SHA256_HEX_LEN] = "";
    if (type != U_SEC_CERT_TYPE_KEY) {
        size_t derLen = 0;
        uint8_t *der = pemToDer((const char *)data, &derLen);
        if (der == NULL) {
            ucxapp_print("warning: no PEM certificate block found, fingerprint check skipped\n");
        } else {
            ucxapp_sha256_hex(der, derLen, localFp);
            free(der);
        }
    }

    int32_t err;
    if (pw != NULL) {
        err = uCxSecurityCertificateUploadPW(app->ucx, type, name, pw, data, (int32_t)len);
    } else {
        err = uCxSecurityCertificateUpload(app->ucx, type, name, data, (int32_t)len);
    }
    if (err != 0) {
        ucxapp_print("ERROR: upload failed (%d%s); check PEM format, size and that the name is unused\n", (int)err, ucxapp_errname(err));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("uploaded %s %s (%u bytes)\n", typeName(type), name, (unsigned)len);

    if (localFp[0] != '\0') {
        char modFp[UCXAPP_SHA256_HEX_LEN];
        if (!moduleFingerprint(app, name, modFp, sizeof(modFp))) {
            ucxapp_print("warning: could not read the fingerprint back from the module\n");
        } else if (ucxapp_hex_equal(localFp, modFp)) {
            ucxapp_print("fingerprint verified: %s\n", modFp);
        } else {
            ucxapp_print("ERROR: fingerprint mismatch (local %s, module %s)\n", localFp, modFp);
            return UCXAPP_ERR_FAILED;
        }
    }
    return UCXAPP_OK;
}

static int cmdCertUpload(ucxapp_t *app, int argc, char *argv[])
{
    uSecCertType_t type;
    size_t len = 0;

    if (argc < 3 || argc > 5 || !parseType(argv[1], &type) ||
        (argc > 4 && type != U_SEC_CERT_TYPE_KEY)) {
        return UCXAPP_ERR_USAGE;
    }
    const char *name = argc > 3 ? argv[3] : baseName(argv[2]);
    if (name[0] == '\0' || strlen(name) > CERT_MAX_NAME) {
        ucxapp_print("ERROR: invalid certificate name\n");
        return UCXAPP_ERR_FAILED;
    }

    uint8_t *data = ucxapp_store_load(argv[2], &len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    int rc = ucxapp_cert_upload(app, (int)type, name, data, len, argc > 4 ? argv[4] : NULL);
    free(data);
    return rc;
}

static int cmdCertRemove(ucxapp_t *app, int argc, char *argv[])
{
    uSecCertType_t type = (uSecCertType_t)0;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!findType(app, argv[1], &type)) {
        ucxapp_print("ERROR: certificate %s not found\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    int32_t err = uCxSecurityCertificateRemove(app->ucx, type, argv[1]);
    if (err != 0) {
        ucxapp_print("ERROR: remove failed (%d%s); the certificate may be in use\n", (int)err, ucxapp_errname(err));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("removed %s %s\n", typeName(type), argv[1]);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gCertCmds[] = {
    { "cert.list",   "",                                  "List installed certificates and keys",             UCXAPP_CMD_NEEDS_DEVICE, cmdCertList },
    { "cert.info",   "<name>",                            "Show size and SHA-256 fingerprint", UCXAPP_CMD_NEEDS_DEVICE, cmdCertInfo },
    { "cert.upload", "<ca|client|key> <file> [name] [pw]", "Upload a PEM file (max 8192 bytes); pw is for encrypted keys", UCXAPP_CMD_NEEDS_DEVICE, cmdCertUpload },
    { "cert.remove", "<name>",                            "Remove a certificate or key",                      UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdCertRemove },
};

void ucxapp_register_cert_cmds(void)
{
    ucxapp_cmd_register(gCertCmds, sizeof(gCertCmds) / sizeof(gCertCmds[0]));
}
