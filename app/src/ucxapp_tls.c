#include "ucxapp_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "ucxapp_store.h"
#include "u_cx_socket.h"
#include "ucxapp_cmd.h"

#define IO_CHUNK   1460   /* AT+USORB/USOWB maximum; ucxapp_sock_read/write drop to 1000 on FW 3.2 */
#define PEM_MAX    4096
#define MAX_SANS   8

struct ucxapp_tls_server {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt own;
    mbedtls_x509_crt ca;
    mbedtls_pk_context key;
};

struct ucxapp_tls_conn {
    mbedtls_ssl_context ssl;
    uCxHandle_t *ucx;
    int32_t sock;
    bool established;
    char peer[96];
};

static bool cryptoReady(void)
{
    static bool ok;
    if (!ok) {
        ok = psa_crypto_init() == PSA_SUCCESS;
    }
    return ok;
}

bool ucxapp_tls_random(void *buf, size_t len)
{
    return cryptoReady() && psa_generate_random(buf, len) == PSA_SUCCESS;
}

static void setErr(char *err, size_t errLen, const char *what, int rc)
{
    if (err && errLen) {
        if (rc) {
            snprintf(err, errLen, "%s (-0x%04X)", what, (unsigned)(rc < 0 ? -rc : rc));
        } else {
            snprintf(err, errLen, "%s", what);
        }
    }
}

void ucxapp_tls_cert_file(const char *name, char *out, size_t outLen)
{
    snprintf(out, outLen, "tls-%s.crt", name);
}

void ucxapp_tls_key_file(const char *name, char *out, size_t outLen)
{
    snprintf(out, outLen, "tls-%s.key", name);
}

/* Names become file names and DN values, so keep them to a safe character set. */
static bool safeName(const char *s, bool allowDot)
{
    if (!s || !*s || strlen(s) > 63) {
        return false;
    }
    for (; *s; s++) {
        char ch = *s;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '-' || ch == '_' || (allowDot && (ch == '.' || ch == '*'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool parseIpv4(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
    return true;
}

static int generateKey(mbedtls_pk_context *pk)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_ANY_HASH));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_status_t st = psa_generate_key(&attr, &id);
    if (st != PSA_SUCCESS) {
        return (int)st;
    }
    int rc = mbedtls_pk_copy_from_psa(id, pk);
    psa_destroy_key(id);
    return rc;
}

/* Validity starts at the build date: no target needs a real-time clock. */
static void validity(char notBefore[32], char notAfter[32], int years)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__; /* "Mmm dd yyyy" */
    int month = 1;
    for (int m = 0; m < 12; m++) {
        if (strncmp(months + m * 3, date, 3) == 0) {
            month = m + 1;
        }
    }
    int day = atoi(date + 4);
    int year = atoi(date + 7);
    snprintf(notBefore, 32, "%04d%02d%02d000000", year, month, day);
    snprintf(notAfter, 32, "%04d%02d%02d000000", year + years, month, day);
}

static bool loadPair(const char *name, mbedtls_x509_crt *crt, mbedtls_pk_context *key, char *err, size_t errLen)
{
    char file[96];
    size_t len = 0;
    int rc;
    ucxapp_tls_cert_file(name, file, sizeof(file));
    uint8_t *pem = ucxapp_store_load(file, &len);
    if (!pem) {
        setErr(err, errLen, "certificate not found", 0);
        return false;
    }
    rc = mbedtls_x509_crt_parse(crt, pem, len + 1);
    free(pem);
    if (rc != 0) {
        setErr(err, errLen, "bad certificate", rc);
        return false;
    }
    if (!key) {
        return true;
    }
    ucxapp_tls_key_file(name, file, sizeof(file));
    pem = ucxapp_store_load(file, &len);
    if (!pem) {
        setErr(err, errLen, "key not found", 0);
        return false;
    }
    rc = mbedtls_pk_parse_key(key, pem, len + 1, NULL, 0);
    mbedtls_platform_zeroize(pem, len);
    free(pem);
    if (rc != 0) {
        setErr(err, errLen, "bad key", rc);
        return false;
    }
    return true;
}

static bool savePem(const char *file, const unsigned char *pem)
{
    return ucxapp_store_save(file, pem, strlen((const char *)pem));
}

/* Signs subject with issuer (self-signed when issuerKey == NULL) and stores cert + key. */
static bool issue(const char *name, const char *subject, const char *issuerName, mbedtls_pk_context *issuerKey,
                  bool ca, bool client, const char *const *sans, int sanCount, char *err, size_t errLen)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_x509_san_list san[MAX_SANS];
    uint8_t ips[MAX_SANS][4];
    unsigned char serial[16];
    char notBefore[32], notAfter[32], file[96];
    unsigned char *pem = calloc(1, PEM_MAX);
    bool ok = false;
    int rc;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    if (!pem) {
        setErr(err, errLen, "out of memory", 0);
        goto out;
    }
    if ((rc = generateKey(&key)) != 0) {
        setErr(err, errLen, "key generation failed", rc);
        goto out;
    }
    if (psa_generate_random(serial, sizeof(serial)) != PSA_SUCCESS) {
        setErr(err, errLen, "no random", 0);
        goto out;
    }
    serial[0] &= 0x7F;
    serial[0] |= 0x01;
    validity(notBefore, notAfter, ca ? 20 : 10);

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, issuerKey ? issuerKey : &key);
    if ((rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0 ||
        (rc = mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter)) != 0 ||
        (rc = mbedtls_x509write_crt_set_subject_name(&crt, subject)) != 0 ||
        (rc = mbedtls_x509write_crt_set_issuer_name(&crt, issuerName ? issuerName : subject)) != 0 ||
        (rc = mbedtls_x509write_crt_set_basic_constraints(&crt, ca ? 1 : 0, ca ? 0 : -1)) != 0 ||
        (rc = mbedtls_x509write_crt_set_key_usage(&crt, ca ? (MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN)
                                                            : MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0) {
        setErr(err, errLen, "certificate fields", rc);
        goto out;
    }
#if defined(PSA_WANT_ALG_SHA_1)
    mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    mbedtls_x509write_crt_set_authority_key_identifier(&crt);
#endif
    if (!ca && !client && sanCount > 0) {
        memset(san, 0, sizeof(san));
        for (int i = 0; i < sanCount && i < MAX_SANS; i++) {
            mbedtls_x509_buf *b = &san[i].node.san.unstructured_name;
            if (parseIpv4(sans[i], ips[i])) {
                san[i].node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
                b->p = ips[i];
                b->len = 4;
            } else {
                san[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
                b->p = (unsigned char *)(uintptr_t)sans[i];
                b->len = strlen(sans[i]);
            }
            san[i].next = (i + 1 < sanCount && i + 1 < MAX_SANS) ? &san[i + 1] : NULL;
        }
        if ((rc = mbedtls_x509write_crt_set_subject_alternative_name(&crt, san)) != 0) {
            setErr(err, errLen, "subject alternative name", rc);
            goto out;
        }
    }
    if ((rc = mbedtls_x509write_crt_pem(&crt, pem, PEM_MAX)) != 0) {
        setErr(err, errLen, "signing failed", rc);
        goto out;
    }
    ucxapp_tls_cert_file(name, file, sizeof(file));
    if (!savePem(file, pem)) {
        setErr(err, errLen, "cannot store certificate", 0);
        goto out;
    }
    memset(pem, 0, PEM_MAX);
    if ((rc = mbedtls_pk_write_key_pem(&key, pem, PEM_MAX)) != 0) {
        setErr(err, errLen, "key export failed", rc);
        goto out;
    }
    ucxapp_tls_key_file(name, file, sizeof(file));
    if (!savePem(file, pem)) {
        setErr(err, errLen, "cannot store key", 0);
        goto out;
    }
    ok = true;
out:
    if (pem) {
        mbedtls_platform_zeroize(pem, PEM_MAX);
        free(pem);
    }
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return ok;
}

bool ucxapp_tls_create_ca(const char *commonName, bool force, char *err, size_t errLen)
{
    char file[96], subject[160];
    if (!cryptoReady()) {
        setErr(err, errLen, "crypto init failed", 0);
        return false;
    }
    if (!commonName || !*commonName) {
        commonName = "ucxapp-local-ca";
    }
    if (!safeName(commonName, true)) {
        setErr(err, errLen, "use letters, digits, '-', '_' and '.' only", 0);
        return false;
    }
    ucxapp_tls_cert_file(UCXAPP_TLS_CA_NAME, file, sizeof(file));
    if (!force && ucxapp_store_exists(file)) {
        setErr(err, errLen, "CA already exists (use force to replace it; issued certificates stop working)", 0);
        return false;
    }
    snprintf(subject, sizeof(subject), "CN=%s,O=u-blox ucxapp", commonName);
    return issue(UCXAPP_TLS_CA_NAME, subject, NULL, NULL, true, false, NULL, 0, err, errLen);
}

bool ucxapp_tls_create_cert(const char *name, bool client, const char *const *sans, int sanCount,
                            char *err, size_t errLen)
{
    mbedtls_x509_crt caCrt;
    mbedtls_pk_context caKey;
    char subject[160], issuerName[256];
    bool ok = false;

    if (!cryptoReady()) {
        setErr(err, errLen, "crypto init failed", 0);
        return false;
    }
    if (!safeName(name, false) || strcmp(name, UCXAPP_TLS_CA_NAME) == 0) {
        setErr(err, errLen, "name: letters, digits, '-' and '_' only (not 'ca')", 0);
        return false;
    }
    if (sanCount > MAX_SANS) {
        setErr(err, errLen, "too many host names", 0);
        return false;
    }
    for (int i = 0; i < sanCount; i++) {
        if (!safeName(sans[i], true)) {
            setErr(err, errLen, "bad host name", 0);
            return false;
        }
    }
    mbedtls_x509_crt_init(&caCrt);
    mbedtls_pk_init(&caKey);
    if (!loadPair(UCXAPP_TLS_CA_NAME, &caCrt, &caKey, err, errLen)) {
        if (err && errLen) {
            snprintf(err, errLen, "no local CA, run utils.tls.ca.create first");
        }
        goto out;
    }
    if (mbedtls_x509_dn_gets(issuerName, sizeof(issuerName), &caCrt.subject) < 0) {
        setErr(err, errLen, "bad CA subject", 0);
        goto out;
    }
    snprintf(subject, sizeof(subject), "CN=%s,O=u-blox ucxapp", (!client && sanCount > 0) ? sans[0] : name);
    ok = issue(name, subject, issuerName, &caKey, false, client, sans, sanCount, err, errLen);
out:
    mbedtls_pk_free(&caKey);
    mbedtls_x509_crt_free(&caCrt);
    return ok;
}

bool ucxapp_tls_web_cert(const char *ip, bool *created, char *err, size_t errLen)
{
    mbedtls_x509_crt crt;
    char subject[128] = "", want[80], file[96];
    const char *sans[1] = { ip };

    *created = false;
    mbedtls_x509_crt_init(&crt);
    if (loadPair(UCXAPP_TLS_WEB_NAME, &crt, NULL, NULL, 0)) {
        (void)mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    }
    mbedtls_x509_crt_free(&crt);
    snprintf(want, sizeof(want), "CN=%s,", ip);
    if (strncmp(subject, want, strlen(want)) == 0) {
        return true;
    }
    ucxapp_tls_cert_file(UCXAPP_TLS_CA_NAME, file, sizeof(file));
    if (!ucxapp_store_exists(file) && !ucxapp_tls_create_ca(NULL, false, err, errLen)) {
        return false;
    }
    *created = ucxapp_tls_create_cert(UCXAPP_TLS_WEB_NAME, false, sans, 1, err, errLen);
    return *created;
}

bool ucxapp_tls_describe(const char *name, char *out, size_t outLen)
{
    mbedtls_x509_crt crt;
    char subject[128] = "", issuer[128] = "";
    bool ok;
    mbedtls_x509_crt_init(&crt);
    ok = loadPair(name, &crt, NULL, NULL, 0);
    if (ok) {
        mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
        mbedtls_x509_dn_gets(issuer, sizeof(issuer), &crt.issuer);
        snprintf(out, outLen, "%s | issuer %s | valid %04d-%02d-%02d .. %04d-%02d-%02d%s",
                 subject, issuer, crt.valid_from.year, crt.valid_from.mon, crt.valid_from.day,
                 crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day,
                 mbedtls_x509_crt_has_ext_type(&crt, MBEDTLS_X509_EXT_SUBJECT_ALT_NAME) ? " | SAN" : "");
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
}

ucxapp_tls_server_t *ucxapp_tls_server_new(const char *certName, bool requireClientCert, char *err, size_t errLen)
{
    int rc;
    if (!cryptoReady()) {
        setErr(err, errLen, "crypto init failed", 0);
        return NULL;
    }
    ucxapp_tls_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        setErr(err, errLen, "out of memory", 0);
        return NULL;
    }
    mbedtls_ssl_config_init(&srv->conf);
    mbedtls_x509_crt_init(&srv->own);
    mbedtls_x509_crt_init(&srv->ca);
    mbedtls_pk_init(&srv->key);

    if (!loadPair(certName, &srv->own, &srv->key, err, errLen)) {
        goto fail;
    }
    if ((rc = mbedtls_ssl_config_defaults(&srv->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0 ||
        (rc = mbedtls_ssl_conf_own_cert(&srv->conf, &srv->own, &srv->key)) != 0) {
        setErr(err, errLen, "TLS config failed", rc);
        goto fail;
    }
    if (requireClientCert) {
        if (!loadPair(UCXAPP_TLS_CA_NAME, &srv->ca, NULL, err, errLen)) {
            goto fail;
        }
        mbedtls_ssl_conf_ca_chain(&srv->conf, &srv->ca, NULL);
        mbedtls_ssl_conf_authmode(&srv->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&srv->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    return srv;
fail:
    ucxapp_tls_server_free(srv);
    return NULL;
}

void ucxapp_tls_server_free(ucxapp_tls_server_t *srv)
{
    if (!srv) {
        return;
    }
    mbedtls_ssl_config_free(&srv->conf);
    mbedtls_x509_crt_free(&srv->own);
    mbedtls_x509_crt_free(&srv->ca);
    mbedtls_pk_free(&srv->key);
    free(srv);
}

static int bioSend(void *ctx, const unsigned char *buf, size_t len)
{
    ucxapp_tls_conn_t *c = ctx;
    int32_t n = ucxapp_sock_write(c->ucx, c->sock, buf, (int32_t)(len > IO_CHUNK ? IO_CHUNK : len));
    if (n < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : n;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len)
{
    ucxapp_tls_conn_t *c = ctx;
    int32_t n = ucxapp_sock_read(c->ucx, c->sock, buf, (int32_t)(len > IO_CHUNK ? IO_CHUNK : len));
    if (n < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return n == 0 ? MBEDTLS_ERR_SSL_WANT_READ : n;
}

ucxapp_tls_conn_t *ucxapp_tls_accept(ucxapp_tls_server_t *srv, uCxHandle_t *ucx, int32_t sock)
{
    ucxapp_tls_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ucx = ucx;
    c->sock = sock;
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &srv->conf) != 0) {
        mbedtls_ssl_free(&c->ssl);
        free(c);
        return NULL;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bioSend, bioRecv, NULL);
    return c;
}

int ucxapp_tls_handshake(ucxapp_tls_conn_t *c)
{
    if (c->established) {
        return 1;
    }
    int rc = mbedtls_ssl_handshake(&c->ssl);
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (rc != 0) {
        return rc < 0 ? rc : -1;
    }
    c->established = true;
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
    if (peer) {
        mbedtls_x509_dn_gets(c->peer, sizeof(c->peer), &peer->subject);
    }
    return 1;
}

int32_t ucxapp_tls_read(ucxapp_tls_conn_t *c, uint8_t *buf, size_t len)
{
    int rc = ucxapp_tls_handshake(c);
    if (rc <= 0) {
        return rc;
    }
    for (;;) {
        rc = mbedtls_ssl_read(&c->ssl, buf, len);
        if (rc > 0) {
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        return rc == 0 ? -1 : rc;
    }
}

int32_t ucxapp_tls_write(ucxapp_tls_conn_t *c, const uint8_t *data, size_t len)
{
    size_t done = 0;
    int stalls = 0;
    if (!c->established) {
        return -1;
    }
    while (done < len) {
        int rc = mbedtls_ssl_write(&c->ssl, data + done, len - done);
        if (rc > 0) {
            done += (size_t)rc;
            stalls = 0;
        } else if ((rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) && ++stalls < 50) {
            continue;
        } else {
            return rc < 0 ? rc : -1;
        }
    }
    return (int32_t)done;
}

const char *ucxapp_tls_peer(const ucxapp_tls_conn_t *c)
{
    return c ? c->peer : "";
}

void ucxapp_tls_close(ucxapp_tls_conn_t *c, bool sendClose)
{
    if (!c) {
        return;
    }
    if (sendClose && c->established) {
        mbedtls_ssl_close_notify(&c->ssl);
    }
    mbedtls_ssl_free(&c->ssl);
    free(c);
}
