/* dtls.* - DTLS 1.2 client and server on the host, over a module UDP socket (or a self-test). Host builds only. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include "ucxapp_dtls.h"
#include "ucxapp_tls.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define DTLS_IO_MAX      UCXAPP_DTLS_MAX_DGRAM
#define DTLS_POLL_MS     10
#define DTLS_HS_POLLS    2000    /* 20 s for the handshake */
#define DTLS_IO_POLLS    1000    /* 10 s for application data */
#define DTLS_PEER_POLLS  6000    /* 60 s for the first client datagram */
#define DTLS_PEER_ID_LEN 19      /* address type, 16 bytes of address, port (big endian) */

/* A module UDP socket as a datagram transport. A server keeps the first datagram it reads (so the
 * handshake can run on it after the cookie peer id is known) and replies to the same peer. */
typedef struct {
    uCxHandle_t *ucx;
    int32_t sock;
    bool server;
    uSockIpAddress_t peerIp;
    int32_t peerPort;
    bool pending;
    int pendingLen;
    uint8_t pendingBuf[DTLS_IO_MAX];
} modSock_t;

static void peerIdOf(const modSock_t *m, uint8_t id[DTLS_PEER_ID_LEN])
{
    id[0] = (uint8_t)m->peerIp.type;
    memset(id + 1, 0, 16);
    memcpy(id + 1, &m->peerIp.address, sizeof(m->peerIp.address));
    id[17] = (uint8_t)(m->peerPort >> 8);
    id[18] = (uint8_t)m->peerPort;
}

/* Reads one datagram from the module; returns the byte count (0 when none queued) or < 0. */
static int modReceive(modSock_t *m, uint8_t *buf, int len)
{
    uCxSocketReceiveFrom_t from;
    memset(&from, 0, sizeof(from));
    int32_t rc = ucxapp_udp_recvfrom(m->ucx, m->sock, len, buf, &from);
    if (rc > 0 && m->server) {
        m->peerIp = from.remote_ip;
        m->peerPort = from.remote_port;
    }
    /* The module answers ERROR when nothing is queued; the handshake loop keeps polling. */
    return rc < 0 ? 0 : (int)rc;
}

static int clientSend(void *ctx, const unsigned char *buf, size_t len)
{
    modSock_t *m = ctx;
    return uCxSocketWrite(m->ucx, m->sock, buf, (int32_t)len) < 0 ? -1 : (int)len;
}

static int clientRecv(void *ctx, unsigned char *buf, size_t len)
{
    return modReceive(ctx, buf, (int)len);
}

static int serverSend(void *ctx, const unsigned char *buf, size_t len)
{
    modSock_t *m = ctx;
    return uCxSocketSendTo(m->ucx, m->sock, &m->peerIp, m->peerPort, buf, (int32_t)len) < 0 ? -1 : (int)len;
}

static int serverRecv(void *ctx, unsigned char *buf, size_t len)
{
    modSock_t *m = ctx;
    if (m->pending) {
        size_t n = (size_t)m->pendingLen < len ? (size_t)m->pendingLen : len;
        memcpy(buf, m->pendingBuf, n);
        m->pending = false;
        return (int)n;
    }
    return modReceive(m, buf, (int)len);
}

static bool createSocket(ucxapp_t *app, modSock_t *m, const char *what)
{
    memset(m, 0, sizeof(*m));
    m->ucx = app->ucx;
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &m->sock) != 0) {
        ucxapp_print("ERROR: failed to create a UDP socket for %s\n", what);
        return false;
    }
    return true;
}

static void closeSocket(modSock_t *m)
{
    uCxSocketClose(m->ucx, m->sock);
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char out[200];
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    const char *cert = argc == 2 ? argv[1] : "server";
    if (!ucxapp_dtls_selftest(cert, out, sizeof(out))) {
        ucxapp_print("ERROR: dtls selftest: %s\n", out);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("dtls selftest: %s\n", out);
    return UCXAPP_OK;
}

static int cmdClient(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port;
    char err[160] = "";
    uint8_t buf[DTLS_IO_MAX];
    modSock_t m;

    if (argc < 3 || argc > 4 || !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    const char *text = argc == 4 ? argv[3] : "ping";
    if (!createSocket(app, &m, "dtls.client")) {
        return UCXAPP_ERR_FAILED;
    }
    if (uCxSocketConnect(app->ucx, m.sock, argv[1], port) != 0) {
        ucxapp_print("ERROR: connect to %s:%d failed\n", argv[1], (int)port);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_dtls_xport_t x = { clientSend, clientRecv, &m };
    ucxapp_dtls_conn_t *c = ucxapp_dtls_client_new(&x, UCXAPP_TLS_CA_NAME, argv[1], err, sizeof(err));
    if (!c) {
        ucxapp_print("ERROR: %s\n", err);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    int hs = 0;
    for (int i = 0; i < DTLS_HS_POLLS && hs == 0; i++) {
        hs = ucxapp_dtls_handshake(c);
        if (hs == 0) {
            ucxapp_platform_sleep_ms(DTLS_POLL_MS);
        }
    }
    if (hs != 1) {
        ucxapp_print("ERROR: DTLS handshake with %s:%d failed\n", argv[1], (int)port);
        ucxapp_dtls_free(c);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("DTLS established (%s)\n", ucxapp_dtls_cipher(c));
    size_t tlen = strlen(text);
    int32_t n = ucxapp_dtls_write(c, (const uint8_t *)text, tlen);
    int32_t got = 0;
    for (int i = 0; i < DTLS_IO_POLLS && got == 0 && n >= 0; i++) {
        got = ucxapp_dtls_read(c, buf, sizeof(buf));
        if (got == 0) {
            ucxapp_platform_sleep_ms(DTLS_POLL_MS);
        }
    }
    if (n < 0 || got <= 0) {
        ucxapp_print("ERROR: no reply from %s:%d\n", argv[1], (int)port);
        ucxapp_dtls_free(c);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("reply: %.*s\n", (int)got, (const char *)buf);
    ucxapp_dtls_free(c);
    closeSocket(&m);
    return UCXAPP_OK;
}

static int cmdServer(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port;
    char err[160] = "";
    uint8_t buf[DTLS_IO_MAX];
    modSock_t m;

    if (argc < 2 || argc > 3 || !ucxapp_parse_int(argv[1], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    const char *cert = argc == 3 ? argv[2] : "server";
    if (!createSocket(app, &m, "dtls.server")) {
        return UCXAPP_ERR_FAILED;
    }
    m.server = true;
    if (uCxSocketBind(app->ucx, m.sock, (int32_t)port) != 0) {
        ucxapp_print("ERROR: bind to port %d failed\n", (int)port);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("DTLS server on port %d, waiting for a client\n", (int)port);

    /* The first datagram identifies the client for the cookie exchange; it is replayed to the handshake. */
    int got = 0;
    for (int i = 0; i < DTLS_PEER_POLLS && got == 0; i++) {
        got = modReceive(&m, m.pendingBuf, DTLS_IO_MAX);
        if (got == 0) {
            ucxapp_platform_sleep_ms(DTLS_POLL_MS);
        }
    }
    if (got <= 0) {
        ucxapp_print("ERROR: no client\n");
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    m.pending = true;
    m.pendingLen = got;

    uint8_t peerId[DTLS_PEER_ID_LEN];
    peerIdOf(&m, peerId);
    m.server = true;
    ucxapp_dtls_xport_t x = { serverSend, serverRecv, &m };
    ucxapp_dtls_conn_t *c = ucxapp_dtls_server_new(&x, cert, peerId, sizeof(peerId), err, sizeof(err));
    if (!c) {
        ucxapp_print("ERROR: %s\n", err);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    int hs = 0;
    for (int i = 0; i < DTLS_HS_POLLS && hs == 0; i++) {
        hs = ucxapp_dtls_handshake(c);
        if (hs == 0) {
            ucxapp_platform_sleep_ms(DTLS_POLL_MS);
        }
    }
    if (hs != 1) {
        ucxapp_print("ERROR: DTLS handshake failed\n");
        ucxapp_dtls_free(c);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("DTLS established (%s), echoing one message\n", ucxapp_dtls_cipher(c));
    int32_t n = 0;
    for (int i = 0; i < DTLS_IO_POLLS && n == 0; i++) {
        n = ucxapp_dtls_read(c, buf, sizeof(buf));
        if (n == 0) {
            ucxapp_platform_sleep_ms(DTLS_POLL_MS);
        }
    }
    if (n <= 0 || ucxapp_dtls_write(c, buf, (size_t)n) < 0) {
        ucxapp_print("ERROR: no application data\n");
        ucxapp_dtls_free(c);
        closeSocket(&m);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("echoed %d bytes\n", (int)n);
    ucxapp_dtls_free(c);
    closeSocket(&m);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gDtlsCmds[] = {
    { "dtls.selftest", "[server-cert]",               "Run a DTLS client and server in-process (offline) and check an echo", 0, cmdSelftest },
    { "dtls.client",   "<host> <port> [text]",        "DTLS client over a module UDP socket: handshake, send text, print the reply", UCXAPP_CMD_NEEDS_DEVICE, cmdClient },
    { "dtls.server",   "<port> [cert]",               "DTLS server over a module UDP socket: one client, echo one message", UCXAPP_CMD_NEEDS_DEVICE, cmdServer },
};

void ucxapp_register_dtls_cmds(void)
{
    ucxapp_cmd_register(gDtlsCmds, sizeof(gDtlsCmds) / sizeof(gDtlsCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_dtls_cmds(void)
{
}

#endif
