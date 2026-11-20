#include "ucxapp_dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cookie.h"
#include "mbedtls/timing.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "ucxapp_store.h"
#include "ucxapp_tls.h"

#define PEM_MAX_SIZE 4096
#define PIPE_SLOTS   8
#define PEER_MAX     32

struct ucxapp_dtls_conn {
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt own;
    mbedtls_x509_crt ca;
    mbedtls_pk_context key;
    mbedtls_ssl_cookie_ctx cookie;
    mbedtls_timing_delay_context timer;
    ucxapp_dtls_xport_t *x;
    bool server;
    unsigned char peerId[PEER_MAX];
    size_t peerLen;
};

static bool cryptoReady(void)
{
    return psa_crypto_init() == PSA_SUCCESS;
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

/* Same store layout as the TLS server (tls-<name>.crt / tls-<name>.key). */
static bool loadCert(const char *name, mbedtls_x509_crt *crt, mbedtls_pk_context *key, char *err, size_t errLen)
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

static ucxapp_dtls_conn_t *connNew(ucxapp_dtls_xport_t *x, bool server)
{
    ucxapp_dtls_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->x = x;
    c->server = server;
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_x509_crt_init(&c->own);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_pk_init(&c->key);
    mbedtls_ssl_cookie_init(&c->cookie);
    return c;
}

static void connFree(ucxapp_dtls_conn_t *c)
{
    if (!c) {
        return;
    }
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->own);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_pk_free(&c->key);
    mbedtls_ssl_cookie_free(&c->cookie);
    free(c);
}

/* Datagram BIO: mbedTLS calls these with the transport context as ctx. */
static int bioSend(void *ctx, const unsigned char *buf, size_t len)
{
    ucxapp_dtls_xport_t *x = ctx;
    int rc = x->send(x->ctx, buf, len);
    return rc < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : rc;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len)
{
    ucxapp_dtls_xport_t *x = ctx;
    int rc = x->recv(x->ctx, buf, len);
    if (rc < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return rc == 0 ? MBEDTLS_ERR_SSL_WANT_READ : rc;
}

static int setupCommon(ucxapp_dtls_conn_t *c, int endpoint, char *err, size_t errLen)
{
    int rc = mbedtls_ssl_config_defaults(&c->conf, endpoint, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        setErr(err, errLen, "ssl config failed", rc);
        return -1;
    }
    if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0) {
        setErr(err, errLen, "ssl setup failed", rc);
        return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, c->x, bioSend, bioRecv, NULL);
    /* Fragment handshake flights so a peer that reads at most 1000 bytes per datagram (FW 3.2) gets them whole. */
    mbedtls_ssl_set_mtu(&c->ssl, UCXAPP_DTLS_MTU);
    mbedtls_ssl_set_timer_cb(&c->ssl, &c->timer, mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    return 0;
}

ucxapp_dtls_conn_t *ucxapp_dtls_client_new(ucxapp_dtls_xport_t *x, const char *caName, const char *hostname,
                                           char *err, size_t errLen)
{
    if (!x || !cryptoReady()) {
        setErr(err, errLen, "no crypto", 0);
        return NULL;
    }
    ucxapp_dtls_conn_t *c = connNew(x, false);
    if (!c) {
        setErr(err, errLen, "out of memory", 0);
        return NULL;
    }
    if (!loadCert(caName, &c->ca, NULL, err, errLen) || setupCommon(c, MBEDTLS_SSL_IS_CLIENT, err, errLen) != 0) {
        connFree(c);
        return NULL;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    int rc = mbedtls_ssl_set_hostname(&c->ssl, hostname);
    if (rc != 0) {
        setErr(err, errLen, "bad host name", rc);
        connFree(c);
        return NULL;
    }
    return c;
}

static bool setupServerPeer(ucxapp_dtls_conn_t *c, const unsigned char *peerId, size_t peerLen, char *err,
                            size_t errLen)
{
    int rc = mbedtls_ssl_cookie_setup(&c->cookie);
    if (rc != 0) {
        setErr(err, errLen, "server setup failed", rc);
        return false;
    }
    /* HelloVerifyRequest: the client must echo a cookie bound to its transport address first. */
    mbedtls_ssl_conf_dtls_cookies(&c->conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check, &c->cookie);
    memcpy(c->peerId, peerId, peerLen);
    c->peerLen = peerLen;
    if (mbedtls_ssl_set_client_transport_id(&c->ssl, c->peerId, c->peerLen) != 0) {
        setErr(err, errLen, "transport id failed", 0);
        return false;
    }
    return true;
}

ucxapp_dtls_conn_t *ucxapp_dtls_server_new(ucxapp_dtls_xport_t *x, const char *certName,
                                           const unsigned char *peerId, size_t peerLen, char *err, size_t errLen)
{
    if (!x || !peerId || peerLen == 0 || peerLen > PEER_MAX || !cryptoReady()) {
        setErr(err, errLen, "bad server arguments", 0);
        return NULL;
    }
    ucxapp_dtls_conn_t *c = connNew(x, true);
    if (!c) {
        setErr(err, errLen, "out of memory", 0);
        return NULL;
    }
    if (!loadCert(certName, &c->own, &c->key, err, errLen) ||
        setupCommon(c, MBEDTLS_SSL_IS_SERVER, err, errLen) != 0) {
        connFree(c);
        return NULL;
    }
    int rc = mbedtls_ssl_conf_own_cert(&c->conf, &c->own, &c->key);
    if (rc != 0) {
        setErr(err, errLen, "server setup failed", rc);
        connFree(c);
        return NULL;
    }
    if (!setupServerPeer(c, peerId, peerLen, err, errLen)) {
        connFree(c);
        return NULL;
    }
    return c;
}

static bool setPsk(ucxapp_dtls_conn_t *c, const unsigned char *id, size_t idLen, const unsigned char *key,
                   size_t keyLen, char *err, size_t errLen)
{
    int rc = mbedtls_ssl_conf_psk(&c->conf, key, keyLen, id, idLen);
    if (rc != 0) {
        setErr(err, errLen, "psk setup failed", rc);
        return false;
    }
    return true;
}

static bool pskArgsOk(const unsigned char *id, size_t idLen, const unsigned char *key, size_t keyLen)
{
    return id && key && idLen > 0 && keyLen > 0 && cryptoReady();
}

ucxapp_dtls_conn_t *ucxapp_dtls_client_psk_new(ucxapp_dtls_xport_t *x, const unsigned char *id, size_t idLen,
                                               const unsigned char *key, size_t keyLen, char *err, size_t errLen)
{
    if (!x || !pskArgsOk(id, idLen, key, keyLen)) {
        setErr(err, errLen, "bad psk arguments", 0);
        return NULL;
    }
    ucxapp_dtls_conn_t *c = connNew(x, false);
    if (!c) {
        setErr(err, errLen, "out of memory", 0);
        return NULL;
    }
    if (setupCommon(c, MBEDTLS_SSL_IS_CLIENT, err, errLen) != 0 || !setPsk(c, id, idLen, key, keyLen, err, errLen)) {
        connFree(c);
        return NULL;
    }
    return c;
}

ucxapp_dtls_conn_t *ucxapp_dtls_server_psk_new(ucxapp_dtls_xport_t *x, const unsigned char *id, size_t idLen,
                                               const unsigned char *key, size_t keyLen, const unsigned char *peerId,
                                               size_t peerLen, char *err, size_t errLen)
{
    if (!x || !peerId || peerLen == 0 || peerLen > PEER_MAX || !pskArgsOk(id, idLen, key, keyLen)) {
        setErr(err, errLen, "bad psk arguments", 0);
        return NULL;
    }
    ucxapp_dtls_conn_t *c = connNew(x, true);
    if (!c) {
        setErr(err, errLen, "out of memory", 0);
        return NULL;
    }
    if (setupCommon(c, MBEDTLS_SSL_IS_SERVER, err, errLen) != 0 || !setPsk(c, id, idLen, key, keyLen, err, errLen) ||
        !setupServerPeer(c, peerId, peerLen, err, errLen)) {
        connFree(c);
        return NULL;
    }
    return c;
}

int ucxapp_dtls_handshake(ucxapp_dtls_conn_t *c)
{
    int rc = mbedtls_ssl_handshake(&c->ssl);
    if (rc == 0) {
        return 1;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (rc == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
        /* Cookie sent: restart the session and wait for the cookie-bearing ClientHello. */
        mbedtls_ssl_session_reset(&c->ssl);
        mbedtls_ssl_set_client_transport_id(&c->ssl, c->peerId, c->peerLen);
        return 0;
    }
    return -1;
}

int32_t ucxapp_dtls_read(ucxapp_dtls_conn_t *c, uint8_t *buf, size_t len)
{
    int rc = mbedtls_ssl_read(&c->ssl, buf, len);
    if (rc > 0) {
        return rc;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    return -1;
}

int32_t ucxapp_dtls_write(ucxapp_dtls_conn_t *c, const uint8_t *data, size_t len)
{
    int rc = mbedtls_ssl_write(&c->ssl, data, len);
    return rc >= 0 ? rc : -1;
}

const char *ucxapp_dtls_cipher(const ucxapp_dtls_conn_t *c)
{
    const char *name = c ? mbedtls_ssl_get_ciphersuite(&c->ssl) : NULL;
    return name ? name : "";
}

void ucxapp_dtls_free(ucxapp_dtls_conn_t *c)
{
    connFree(c);
}

/* In-process datagram pair for the selftest: a small FIFO per direction. */
typedef struct {
    unsigned char data[PIPE_SLOTS][UCXAPP_DTLS_MAX_DGRAM];
    size_t len[PIPE_SLOTS];
    unsigned head;
    unsigned count;
} memPipe_t;

typedef struct {
    memPipe_t *out;
    memPipe_t *in;
} memEnd_t;

static int pipePush(memPipe_t *p, const unsigned char *buf, size_t len)
{
    if (len > UCXAPP_DTLS_MAX_DGRAM) {
        return -1;
    }
    if (p->count < PIPE_SLOTS) {
        unsigned idx = (p->head + p->count) % PIPE_SLOTS;
        memcpy(p->data[idx], buf, len);
        p->len[idx] = len;
        p->count++;
    }
    return (int)len;   /* a full queue drops the datagram, as a lossy network would */
}

static int pipePop(memPipe_t *p, unsigned char *buf, size_t len)
{
    if (p->count == 0) {
        return 0;
    }
    size_t n = p->len[p->head];
    if (n > len) {
        n = len;
    }
    memcpy(buf, p->data[p->head], n);
    p->head = (p->head + 1) % PIPE_SLOTS;
    p->count--;
    return (int)n;
}

static int memSend(void *ctx, const unsigned char *buf, size_t len)
{
    memEnd_t *e = ctx;
    return pipePush(e->out, buf, len);
}

static int memRecv(void *ctx, unsigned char *buf, size_t len)
{
    memEnd_t *e = ctx;
    return pipePop(e->in, buf, len);
}

static bool fail(char *out, size_t outLen, const char *msg)
{
    snprintf(out, outLen, "%s", msg);
    return false;
}

/* Handshake and a ping/pong echo between a client and a server over the memory pipes; frees both. */
static bool pairEcho(ucxapp_dtls_conn_t *client, ucxapp_dtls_conn_t *server, const char *what, char *out,
                     size_t outLen)
{
    static const char ping[] = "ping";
    static const char pong[] = "pong";
    uint8_t buf[32];
    int32_t n = 0;
    bool ok = false;
    int cs = 0, ss = 0;

    for (int i = 0; i < 200 && (cs == 0 || ss == 0); i++) {
        if (cs == 0) {
            cs = ucxapp_dtls_handshake(client);
        }
        if (ss == 0) {
            ss = ucxapp_dtls_handshake(server);
        }
        if (cs < 0 || ss < 0) {
            break;
        }
    }
    if (cs != 1 || ss != 1) {
        fail(out, outLen, "handshake failed");
        goto done;
    }
    if (ucxapp_dtls_write(client, (const uint8_t *)ping, sizeof(ping) - 1) != (int32_t)(sizeof(ping) - 1)) {
        goto echo;
    }
    for (int i = 0; i < 200 && n == 0; i++) {
        n = ucxapp_dtls_read(server, buf, sizeof(buf));
    }
    if (n != (int32_t)(sizeof(ping) - 1) || memcmp(buf, ping, sizeof(ping) - 1) != 0) {
        goto echo;
    }
    if (ucxapp_dtls_write(server, (const uint8_t *)pong, sizeof(pong) - 1) != (int32_t)(sizeof(pong) - 1)) {
        goto echo;
    }
    n = 0;
    for (int i = 0; i < 200 && n == 0; i++) {
        n = ucxapp_dtls_read(client, buf, sizeof(buf));
    }
    if (n != (int32_t)(sizeof(pong) - 1) || memcmp(buf, pong, sizeof(pong) - 1) != 0) {
        goto echo;
    }
    snprintf(out, outLen, "DTLS 1.2 %shandshake ok (%s), echo ok", what, ucxapp_dtls_cipher(client));
    ok = true;
echo:
    if (!ok) {
        fail(out, outLen, "echo failed");
    }
done:
    ucxapp_dtls_free(client);
    ucxapp_dtls_free(server);
    return ok;
}

static memPipe_t gC2s, gS2c;
static memEnd_t gCe = { &gC2s, &gS2c };
static memEnd_t gSe = { &gS2c, &gC2s };
static ucxapp_dtls_xport_t gCx = { memSend, memRecv, &gCe };
static ucxapp_dtls_xport_t gSx = { memSend, memRecv, &gSe };
static const unsigned char kPeer[] = "selftest";

bool ucxapp_dtls_selftest(const char *serverCert, char *out, size_t outLen)
{
    char err[160] = "";

    memset(&gC2s, 0, sizeof(gC2s));
    memset(&gS2c, 0, sizeof(gS2c));
    ucxapp_dtls_conn_t *client = ucxapp_dtls_client_new(&gCx, UCXAPP_TLS_CA_NAME, "localhost", err, sizeof(err));
    if (!client) {
        return fail(out, outLen, err);
    }
    ucxapp_dtls_conn_t *server = ucxapp_dtls_server_new(&gSx, serverCert, kPeer, sizeof(kPeer) - 1, err, sizeof(err));
    if (!server) {
        ucxapp_dtls_free(client);
        return fail(out, outLen, err);
    }
    return pairEcho(client, server, "", out, outLen);
}

bool ucxapp_dtls_psk_selftest(char *out, size_t outLen)
{
    static const unsigned char id[] = "ucxapp-selftest";
    static const unsigned char key[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                                           0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10 };
    char err[160] = "";

    memset(&gC2s, 0, sizeof(gC2s));
    memset(&gS2c, 0, sizeof(gS2c));
    ucxapp_dtls_conn_t *client = ucxapp_dtls_client_psk_new(&gCx, id, sizeof(id) - 1, key, sizeof(key), err,
                                                            sizeof(err));
    if (!client) {
        return fail(out, outLen, err);
    }
    ucxapp_dtls_conn_t *server = ucxapp_dtls_server_psk_new(&gSx, id, sizeof(id) - 1, key, sizeof(key), kPeer,
                                                            sizeof(kPeer) - 1, err, sizeof(err));
    if (!server) {
        ucxapp_dtls_free(client);
        return fail(out, outLen, err);
    }
    return pairEcho(client, server, "PSK ", out, outLen);
}