/* Serial sensor stream: reader thread, raw log and module bridges (TCP server, UDP, BLE Nordic UART). */

#include "ucxapp_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_stats.h"
#include "ucxapp_thread.h"
#include "ucxapp_tls.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#include "u_cx_sps.h"
#include "u_port.h"
#include "u_port_uart.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define RING_SIZE     32768u
#define MAX_CLIENTS   4
#define NET_CHUNK     512u
#define BLE_CHUNK     120u
#define BLE_CHUNK_MIN 20u
#define SEND_PERIOD   100u
#define MAX_STREAMS   4
#define EVT_QUEUE     8

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

enum { BR_NONE, BR_TCP, BR_UDP, BR_BLE, BR_UART, BR_SPS };

#define RECONNECT_MS  3000u

struct ucxapp_stream {
    uPortUartHandle_t uart;
    char port[32];
    int32_t baud;
    ucxapp_stream_cfg_t cfg;
    U_CX_MUTEX_HANDLE lock;
    volatile bool stop;
    ucxapp_thread_t *reader;
    uint32_t rx;
    FILE *log;
    char logName[200];

    int bridge;
    ucxapp_t *app;
    ucxapp_thread_t *sender;
    volatile bool senderStop;
    uint8_t *ring;
    size_t head;
    size_t count;
    int32_t listenSock;
    int32_t udpSock;
    uSockIpAddress_t addr;
    int32_t udpPort;
    int32_t tcpPort;
    int32_t clients[MAX_CLIENTS];
    int nClients;
    int32_t accepted[EVT_QUEUE];   /* guarded by gRegLock */
    int nAccepted;
    int32_t closed[EVT_QUEUE];
    int nClosed;
    uint32_t sent;
    uint32_t dropped;
    size_t bleChunk;
    bool bleCustom;                /* BR_BLE: custom service instead of Nordic UART */
    uPortUartHandle_t out;         /* BR_UART: the other serial port */
    char outPort[32];
    int32_t outBaud;
    volatile bool rxPending;       /* the module has data for us (TCP client or UDP socket) */
    uint32_t received;             /* bytes that came back from the far side */
    char connectHost[64];          /* BR_TCP client mode; empty when serving */
    int32_t connectPort;
    uint32_t nextRetry;
    bool udpListen;                /* reply to whoever sent last */
    int32_t spsConn;
    ucxapp_tls_server_t *tlsSrv;   /* BR_TCP server with TLS; NULL for plain TCP */
    ucxapp_tls_conn_t *tls[MAX_CLIENTS];
    bool ntrip;                    /* BR_TCP client speaking NTRIP v1 to a caster */
    bool ntripFailed;              /* caster refused: no reconnect */
    bool ntripStreaming;           /* response header passed */
    bool ntripBle;                 /* also send the receiver stream to a BLE Nordic UART client */
    char ntripMount[64];
    char ntripReq[400];
    char ntripHdr[256];
    size_t ntripHdrLen;
    char nmea[100];                /* NMEA line being assembled from the receiver */
    size_t nmeaLen;
    char gga[100];                 /* latest GGA, sent to the caster */
    uint32_t ggaPeriodMs;
    uint32_t nextGga;
};

static ucxapp_stream_t *gStreams[MAX_STREAMS];

/* Guards gStreams and the URC queues; the events mutex only exists while a module is connected. */
static U_CX_MUTEX_HANDLE gRegLock;
static bool gRegInit;

static void regLock(void)
{
    if (gRegInit) {
        U_CX_MUTEX_LOCK(gRegLock);
    }
}

static void regUnlock(void)
{
    if (gRegInit) {
        U_CX_MUTEX_UNLOCK(gRegLock);
    }
}

/* ---- Reader ---- */

static void ringPut(ucxapp_stream_t *s, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s->count == RING_SIZE) {
            s->head = (s->head + 1) % RING_SIZE;
            s->count--;
            s->dropped++;
        }
        s->ring[(s->head + s->count) % RING_SIZE] = d[i];
        s->count++;
    }
}

static size_t ringGet(ucxapp_stream_t *s, uint8_t *out, size_t max)
{
    size_t n;

    ucxapp_stream_lock(s);
    n = s->count < max ? s->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s->ring[(s->head + i) % RING_SIZE];
    }
    s->head = (s->head + n) % RING_SIZE;
    s->count -= n;
    ucxapp_stream_unlock(s);
    return n;
}

static void ingest(ucxapp_stream_t *s, const uint8_t *buf, size_t n)
{
    ucxapp_stream_lock(s);
    s->rx += (uint32_t)n;
    if (s->log != NULL) {
        fwrite(buf, 1, n, s->log);
        fflush(s->log);
    }
    if (s->cfg.feed != NULL) {
        s->cfg.feed(s->cfg.ctx, buf, n);
    }
    if (s->ring != NULL) {
        ringPut(s, buf, n);
    }
    ucxapp_stream_unlock(s);
}

static void readerLoop(void *arg)
{
    ucxapp_stream_t *s = arg;
    uint8_t buf[512];
    uint32_t lastPoll = ucxapp_platform_tick_ms();

    while (!s->stop) {
        int32_t n = 0;

        if (s->uart == NULL) {
            ucxapp_platform_sleep_ms(20);
        } else {
            n = uPortUartRead(s->uart, buf, sizeof(buf), 20);
        }
        if (n > 0) {
            ingest(s, buf, (size_t)n);
        } else if (n < 0) {
            ucxapp_platform_sleep_ms(20);
        }
        if (s->cfg.poll != NULL && s->cfg.pollMs > 0 && ucxapp_platform_tick_ms() - lastPoll >= s->cfg.pollMs) {
            lastPoll = ucxapp_platform_tick_ms();
            s->cfg.poll(s->cfg.ctx, s);
        }
    }
}

ucxapp_stream_t *ucxapp_stream_open(const char *port, int32_t baud, const ucxapp_stream_cfg_t *cfg)
{
    ucxapp_stream_t *s = calloc(1, sizeof(*s));
    bool registered = false;

    if (s == NULL) {
        return NULL;
    }
    if (!gRegInit) {
        U_CX_MUTEX_CREATE(gRegLock);
        gRegInit = true;
    }
    s->cfg = *cfg;
    snprintf(s->port, sizeof(s->port), "%s", port);
    s->baud = baud;
    s->listenSock = -1;
    s->udpSock = -1;
    bool virt = strncmp(port, "virt:", 5) == 0;

    s->uart = virt ? NULL : uPortUartOpen(port, baud, false);
    if (s->uart == NULL && !virt) {
        free(s);
        return NULL;
    }
    U_CX_MUTEX_CREATE(s->lock);
    s->reader = ucxapp_thread_start(readerLoop, s);
    if (s->reader == NULL) {
        if (s->uart != NULL) {
            uPortUartClose(s->uart);
        }
        U_CX_MUTEX_DELETE(s->lock);
        free(s);
        return NULL;
    }
    regLock();
    for (int i = 0; i < MAX_STREAMS && !registered; i++) {
        if (gStreams[i] == NULL) {
            gStreams[i] = s;
            registered = true;
        }
    }
    regUnlock();
    return s;
}

void ucxapp_stream_close(ucxapp_stream_t *s)
{
    if (s == NULL) {
        return;
    }
    ucxapp_stream_bridge_stop(s);
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (gStreams[i] == s) {
            gStreams[i] = NULL;
        }
    }
    regUnlock();
    s->stop = true;
    ucxapp_thread_join(s->reader);
    if (s->uart != NULL) {
        uPortUartClose(s->uart);
    }
    if (s->log != NULL) {
        fclose(s->log);
    }
    U_CX_MUTEX_DELETE(s->lock);
    free(s);
}

void ucxapp_stream_lock(ucxapp_stream_t *s)
{
    U_CX_MUTEX_LOCK(s->lock);
}

void ucxapp_stream_unlock(ucxapp_stream_t *s)
{
    U_CX_MUTEX_UNLOCK(s->lock);
}

bool ucxapp_stream_write(ucxapp_stream_t *s, const uint8_t *data, size_t len)
{
    if (s->uart == NULL) {
        return s->cfg.write != NULL && s->cfg.write(s->cfg.ctx, data, len);
    }
    return uPortUartWrite(s->uart, data, len) == (int32_t)len;
}

void ucxapp_stream_push(ucxapp_stream_t *s, const uint8_t *data, size_t len)
{
    ingest(s, data, len);
}

const char *ucxapp_stream_port(const ucxapp_stream_t *s)
{
    return s->port;
}

int32_t ucxapp_stream_baud(const ucxapp_stream_t *s)
{
    return s->baud;
}

uint32_t ucxapp_stream_rx_bytes(const ucxapp_stream_t *s)
{
    return s->rx;
}

bool ucxapp_stream_log(ucxapp_stream_t *s, const char *file)
{
    FILE *f = NULL;

    if (file != NULL) {
        f = fopen(file, "wb");
        if (f == NULL) {
            return false;
        }
    }
    ucxapp_stream_lock(s);
    if (s->log != NULL) {
        fclose(s->log);
    }
    s->log = f;
    snprintf(s->logName, sizeof(s->logName), "%s", file != NULL ? file : "");
    ucxapp_stream_unlock(s);
    return true;
}

const char *ucxapp_stream_log_name(const ucxapp_stream_t *s)
{
    return s->logName;
}

/* ---- Bridge sender ---- */

static void dropClient(ucxapp_stream_t *s, int idx, bool closeSock)
{
    ucxapp_tls_close(s->tls[idx], closeSock);
    if (closeSock) {
        (void)uCxSocketClose(s->app->ucx, s->clients[idx]);
    }
    s->nClients--;
    s->clients[idx] = s->clients[s->nClients];
    s->tls[idx] = s->tls[s->nClients];
    s->tls[s->nClients] = NULL;
}

static bool connectOnce(ucxapp_stream_t *s)
{
    uCxHandle_t *ucx = s->app->ucx;
    int32_t sock = -1;

    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        return false;
    }
    if (uCxSocketConnect(ucx, sock, s->connectHost, s->connectPort) != 0) {
        (void)uCxSocketClose(ucx, sock);
        return false;
    }
    if (s->ntrip) {
        s->ntripStreaming = false;
        s->ntripHdrLen = 0;
        if (uCxSocketWrite(ucx, sock, (const uint8_t *)s->ntripReq, (int32_t)strlen(s->ntripReq)) < 0) {
            (void)uCxSocketClose(ucx, sock);
            return false;
        }
    }
    s->clients[s->nClients++] = sock;
    return true;
}

/* Response header from the caster: "ICY 200 OK" (v1) or "HTTP/1.x 200"; returns bytes of buf used by it, -1 on refusal. */
static int32_t ntripHeader(ucxapp_stream_t *s, const uint8_t *buf, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        char *h = s->ntripHdr;
        size_t len;

        if (s->ntripHdrLen + 1 >= sizeof(s->ntripHdr)) {
            ucxapp_print("ERROR: NTRIP caster sent no valid response\n");
            return -1;
        }
        h[s->ntripHdrLen++] = (char)buf[i];
        h[s->ntripHdrLen] = '\0';
        len = s->ntripHdrLen;
        if (len < 2 || h[len - 2] != '\r' || h[len - 1] != '\n') {
            continue;
        }
        if (strstr(h, "\r\n") == h + len - 2) {   /* status line complete */
            bool ok = strncmp(h, "ICY 200", 7) == 0 || (strncmp(h, "HTTP/1.", 7) == 0 && strncmp(h + 8, " 200", 4) == 0);
            if (!ok) {
                h[len - 2] = '\0';
                ucxapp_print("ERROR: NTRIP caster refused /%s: %s%s\n", s->ntripMount, h,
                             strncmp(h, "SOURCETABLE", 11) == 0 ? " (unknown mountpoint)"
                             : strstr(h, " 401") != NULL    ? " (wrong user name or password)" : "");
                return -1;
            }
            if (h[0] == 'I') {
                s->ntripStreaming = true;
                return i + 1;
            }
        } else if (len >= 4 && h[len - 4] == '\r' && h[len - 3] == '\n') {
            s->ntripStreaming = true;
            return i + 1;
        }
    }
    return n;
}

/* Keeps the last GGA sentence of the receiver stream for the caster. */
static void ntripScan(ucxapp_stream_t *s, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)d[i];

        if (c == '$') {
            s->nmeaLen = 0;
        }
        if (s->nmeaLen + 3 >= sizeof(s->nmea)) {
            s->nmeaLen = 0;
            continue;
        }
        s->nmea[s->nmeaLen++] = c;
        if (c == '\n') {
            s->nmea[s->nmeaLen] = '\0';
            if (s->nmeaLen > 7 && s->nmea[0] == '$' && strncmp(s->nmea + 3, "GGA,", 4) == 0) {
                memcpy(s->gga, s->nmea, s->nmeaLen + 1);
            }
            s->nmeaLen = 0;
        }
    }
}

static void tcpService(ucxapp_stream_t *s)
{
    int32_t acc[EVT_QUEUE];
    int32_t cls[EVT_QUEUE];
    int nAcc;
    int nCls;

    regLock();
    nAcc = s->nAccepted;
    nCls = s->nClosed;
    memcpy(acc, s->accepted, sizeof(acc));
    memcpy(cls, s->closed, sizeof(cls));
    s->nAccepted = 0;
    s->nClosed = 0;
    regUnlock();

    for (int i = 0; i < nCls; i++) {
        for (int c = 0; c < s->nClients; c++) {
            if (s->clients[c] == cls[i]) {
                dropClient(s, c, false);
                break;
            }
        }
    }
    for (int i = 0; i < nAcc; i++) {
        if (s->nClients < MAX_CLIENTS) {
            s->tls[s->nClients] = s->tlsSrv != NULL ? ucxapp_tls_accept(s->tlsSrv, s->app->ucx, acc[i]) : NULL;
            if (s->tlsSrv != NULL && s->tls[s->nClients] == NULL) {
                (void)uCxSocketClose(s->app->ucx, acc[i]);
                continue;
            }
            s->clients[s->nClients++] = acc[i];
        } else {
            (void)uCxSocketClose(s->app->ucx, acc[i]);
        }
    }
    if (s->connectHost[0] != '\0' && !s->ntripFailed && s->nClients == 0 && ucxapp_platform_tick_ms() >= s->nextRetry) {
        s->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
        (void)connectOnce(s);
    }
}

static bool spsReady(ucxapp_stream_t *s)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    if (conn < 0) {
        s->spsConn = -1;
        return false;
    }
    if (conn != s->spsConn) {
        if (uCxSpsConnect1(s->app->ucx, conn) != 0) {
            return false;
        }
        s->spsConn = conn;
    }
    return true;
}

static bool bleSend(ucxapp_stream_t *s, const uint8_t *d, size_t n)
{
    if (s->bridge == BR_SPS) {
        return s->spsConn >= 0 && uCxSpsWrite(s->app->ucx, s->spsConn, d, (int32_t)n) >= 0;
    }
    return s->bleCustom ? ucxapp_gatt_notify_custom(s->app, d, n) : ucxapp_gatt_notify_nus(s->app, d, n);
}

static void deliver(ucxapp_stream_t *s, const uint8_t *d, size_t n)
{
    uCxHandle_t *ucx = s->app->ucx;

    if (s->bridge == BR_TCP) {
        for (int c = s->nClients - 1; c >= 0; c--) {
            if (s->tls[c] != NULL && ucxapp_tls_handshake(s->tls[c]) <= 0) {
                continue;   /* not secured yet: nothing in clear text */
            }
            if ((s->tls[c] != NULL ? ucxapp_tls_write(s->tls[c], d, n)
                                   : uCxSocketWrite(ucx, s->clients[c], d, (int32_t)n)) < 0) {
                dropClient(s, c, true);
            } else {
                ucxapp_stats_add(UCXAPP_STATS_TCP, n, 0);
            }
        }
        if (s->nClients > 0) {
            s->sent += (uint32_t)n;
        }
    } else if (s->bridge == BR_UDP) {
        if (s->udpPort <= 0) {
            s->dropped += (uint32_t)n;   /* listening, nobody has written yet */
        } else if (uCxSocketSendTo(ucx, s->udpSock, &s->addr, s->udpPort, d, (int32_t)n) < 0) {
            s->dropped += (uint32_t)n;
        } else {
            s->sent += (uint32_t)n;
            ucxapp_stats_add(UCXAPP_STATS_UDP, n, 0);
        }
    } else if (s->bridge == BR_BLE || s->bridge == BR_SPS) {
        size_t off = 0;

        while (off < n) {
            size_t piece = n - off < s->bleChunk ? n - off : s->bleChunk;
            bool ok = bleSend(s, d + off, piece);
            if (!ok && s->bleChunk > BLE_CHUNK_MIN) {
                s->bleChunk = BLE_CHUNK_MIN;   /* the link has the default 23 byte MTU */
                piece = piece < s->bleChunk ? piece : s->bleChunk;
                ok = bleSend(s, d + off, piece);
            }
            if (!ok) {
                s->dropped += (uint32_t)(n - off);
                return;
            }
            s->sent += (uint32_t)piece;
            ucxapp_stats_add(s->bridge == BR_SPS ? UCXAPP_STATS_SPS : UCXAPP_STATS_GATT, piece, 0);
            off += piece;
        }
    }
}

/* Bytes from the far side go back to the serial device. */
static void pullInbound(ucxapp_stream_t *s)
{
    uCxHandle_t *ucx = s->app->ucx;
    uint8_t buf[NET_CHUNK];

    if (s->bridge == BR_TCP) {
        for (int c = s->nClients - 1; c >= 0; c--) {
            for (int i = 0; i < 8; i++) {
                int32_t n = s->tls[c] != NULL ? ucxapp_tls_read(s->tls[c], buf, sizeof(buf))
                                              : uCxSocketRead(ucx, s->clients[c], (int32_t)sizeof(buf), buf);
                if (n < 0 && s->tls[c] != NULL) {
                    dropClient(s, c, true);   /* failed handshake or peer closed the session */
                    break;
                }
                if (n <= 0) {
                    break;
                }
                ucxapp_stats_add(UCXAPP_STATS_TCP, 0, (size_t)n);
                int32_t skip = 0;
                if (s->ntrip && !s->ntripStreaming) {
                    skip = ntripHeader(s, buf, n);
                    if (skip < 0) {
                        s->ntripFailed = true;
                        dropClient(s, c, true);
                        break;
                    }
                    if (s->ntripStreaming) {
                        ucxapp_print("NTRIP: streaming corrections from /%s\n", s->ntripMount);
                        s->nextGga = 0;
                    }
                }
                if (n > skip) {
                    (void)ucxapp_stream_write(s, buf + skip, (size_t)(n - skip));
                    s->received += (uint32_t)(n - skip);
                }
            }
        }
    } else if (s->bridge == BR_UDP) {
        uCxSocketReceiveFrom_t from;
        for (int i = 0; i < 8; i++) {
            int32_t n = ucxapp_udp_recvfrom(ucx, s->udpSock, (int32_t)sizeof(buf), buf, &from);
            if (n <= 0) {
                break;
            }
            if (s->udpListen) {
                s->addr = from.remote_ip;
                s->udpPort = from.remote_port;
            }
            (void)ucxapp_stream_write(s, buf, (size_t)n);
            s->received += (uint32_t)n;
            ucxapp_stats_add(UCXAPP_STATS_UDP, 0, (size_t)n);
        }
    } else if (s->bridge == BR_SPS && s->spsConn >= 0) {
        for (int i = 0; i < 8; i++) {
            int32_t n = uCxSpsRead(ucx, s->spsConn, (int32_t)sizeof(buf), buf);
            if (n <= 0) {
                break;
            }
            ucxapp_stats_add(UCXAPP_STATS_SPS, 0, (size_t)n);
            (void)ucxapp_stream_write(s, buf, (size_t)n);
            s->received += (uint32_t)n;
        }
    }
}

static void senderLoop(void *arg)
{
    ucxapp_stream_t *s = arg;
    uint8_t chunk[NET_CHUNK];

    while (!s->senderStop) {
        size_t n;

        ucxapp_platform_sleep_ms(SEND_PERIOD);
        if (s->app->ucx == NULL) {
            continue;
        }
        if (s->bridge == BR_TCP) {
            tcpService(s);
        }
        if (s->rxPending || (s->tlsSrv != NULL && s->nClients > 0)) {
            s->rxPending = false;
            pullInbound(s);   /* TLS polls: records can wait in the module without a new URC */
        }
        if (s->ntrip) {
            while ((n = ringGet(s, chunk, sizeof(chunk))) > 0) {
                ntripScan(s, chunk, n);
                if (s->ntripBle && ucxapp_handle_current(UCXAPP_HANDLE_CONN) >= 0) {
                    for (size_t off = 0; off < n; off += BLE_CHUNK_MIN) {
                        size_t piece = n - off < BLE_CHUNK_MIN ? n - off : BLE_CHUNK_MIN;
                        if (ucxapp_gatt_notify_nus(s->app, chunk + off, piece)) {
                            ucxapp_stats_add(UCXAPP_STATS_GATT, piece, 0);
                        }
                    }
                }
            }
            if (s->ntripStreaming && s->nClients > 0 && s->gga[0] != '\0' &&
                (int32_t)(ucxapp_platform_tick_ms() - s->nextGga) >= 0) {
                s->nextGga = ucxapp_platform_tick_ms() + s->ggaPeriodMs;
                deliver(s, (const uint8_t *)s->gga, strlen(s->gga));
            }
            continue;
        }
        while (!s->senderStop && (n = ringGet(s, chunk, sizeof(chunk))) > 0) {
            if (s->bridge == BR_BLE && ucxapp_handle_current(UCXAPP_HANDLE_CONN) < 0) {
                continue;   /* nobody connected: discard */
            }
            if (s->bridge == BR_SPS && !spsReady(s)) {
                continue;
            }
            deliver(s, chunk, n);
        }
    }
}

/* Serial to serial: no module needed, both directions. */
static void uartBridgeLoop(void *arg)
{
    ucxapp_stream_t *s = arg;
    uint8_t chunk[NET_CHUNK];

    while (!s->senderStop) {
        size_t n = ringGet(s, chunk, sizeof(chunk));

        if (n > 0 && uPortUartWrite(s->out, chunk, n) == (int32_t)n) {
            s->sent += (uint32_t)n;
        } else if (n > 0) {
            s->dropped += (uint32_t)n;
        }
        int32_t r = uPortUartRead(s->out, chunk, sizeof(chunk), 20);
        if (r > 0) {
            (void)ucxapp_stream_write(s, chunk, (size_t)r);
            s->received += (uint32_t)r;
        } else if (r < 0) {
            ucxapp_platform_sleep_ms(20);
        }
    }
}

static bool bridgeBegin(ucxapp_stream_t *s, ucxapp_t *app, int kind)
{
    if (kind != BR_UART && app->ucx == NULL) {
        ucxapp_print("ERROR: no module connected\n");
        return false;
    }
    if (s->bridge != BR_NONE) {
        ucxapp_print("ERROR: a bridge is already running (stop it first)\n");
        return false;
    }
    s->ring = malloc(RING_SIZE);
    if (s->ring == NULL) {
        return false;
    }
    s->app = app;
    s->bridge = kind;
    s->head = 0;
    s->count = 0;
    s->sent = 0;
    s->dropped = 0;
    s->nClients = 0;
    s->nAccepted = 0;
    s->nClosed = 0;
    s->bleChunk = BLE_CHUNK;
    s->received = 0;
    s->rxPending = false;
    s->connectHost[0] = '\0';
    s->ntrip = false;
    s->ntripFailed = false;
    s->ntripStreaming = false;
    s->ntripBle = false;
    s->gga[0] = '\0';
    s->nmeaLen = 0;
    s->udpListen = false;
    s->udpPort = 0;
    s->spsConn = -1;
    return true;
}

static bool bridgeRun(ucxapp_stream_t *s)
{
    s->senderStop = false;
    s->sender = ucxapp_thread_start(s->bridge == BR_UART ? uartBridgeLoop : senderLoop, s);
    if (s->sender == NULL) {
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    return true;
}

bool ucxapp_stream_bridge_tcp(ucxapp_stream_t *s, ucxapp_t *app, int32_t port)
{
    char ssid[64];
    char ip[48];

    if (!bridgeBegin(s, app, BR_TCP)) {
        return false;
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_TCP, &s->listenSock) != 0 ||
        uCxSocketListen2(app->ucx, s->listenSock, port) != 0) {
        ucxapp_print("ERROR: cannot listen on port %d (is Wi-Fi connected?)\n", (int)port);
        if (s->listenSock >= 0) {
            (void)uCxSocketClose(app->ucx, s->listenSock);
        }
        s->listenSock = -1;
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->tcpPort = port;
    if (!bridgeRun(s)) {
        return false;
    }
    if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("%s bridge: connect a TCP client to %s:%d\n", s->cfg.kind, ip, (int)port);
    } else {
        ucxapp_print("%s bridge: listening on TCP port %d\n", s->cfg.kind, (int)port);
    }
    return true;
}

bool ucxapp_stream_bridge_tls(ucxapp_stream_t *s, ucxapp_t *app, int32_t port, const char *certName, bool clientAuth)
{
    char err[160] = "";
    ucxapp_tls_server_t *srv;

    if (s->bridge != BR_NONE) {
        ucxapp_print("ERROR: a bridge is already running (stop it first)\n");
        return false;
    }
    srv = ucxapp_tls_server_new(certName, clientAuth, err, sizeof(err));
    if (srv == NULL) {
        ucxapp_print("ERROR: TLS certificate '%s': %s (see utils.tls.ca.create / utils.tls.server.create)\n", certName, err);
        return false;
    }
    s->tlsSrv = srv;   /* set before the sender starts so no client is ever served in clear text */
    if (!ucxapp_stream_bridge_tcp(s, app, port)) {
        ucxapp_tls_server_free(s->tlsSrv);   /* bridge_stop may already have freed it */
        s->tlsSrv = NULL;
        return false;
    }
    ucxapp_print("TLS with certificate '%s'%s\n", certName, clientAuth ? ", client certificate required" : "");
    return true;
}

bool ucxapp_stream_bridge_tcp_connect(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port)
{
    if (strlen(host) >= sizeof(s->connectHost)) {
        ucxapp_print("ERROR: host name too long\n");
        return false;
    }
    if (!bridgeBegin(s, app, BR_TCP)) {
        return false;
    }
    snprintf(s->connectHost, sizeof(s->connectHost), "%s", host);
    s->connectPort = port;
    s->tcpPort = port;
    if (!connectOnce(s)) {
        ucxapp_print("ERROR: cannot connect to %s:%d (is Wi-Fi connected?)\n", host, (int)port);
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: connected to %s:%d, reconnects if the link drops\n", s->cfg.kind, host, (int)port);
    return true;
}

static void base64(const char *in, char *out, size_t size)
{
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in);
    size_t o = 0;

    for (size_t i = 0; i < len && o + 5 < size; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        v |= i + 1 < len ? (uint32_t)(uint8_t)in[i + 1] << 8 : 0;
        v |= i + 2 < len ? (uint32_t)(uint8_t)in[i + 2] : 0;
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? kB64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? kB64[v & 63] : '=';
    }
    out[o] = '\0';
}

bool ucxapp_stream_bridge_ntrip(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, const char *mount,
                                const char *user, const char *pass, int32_t ggaSeconds, bool ble)
{
    char cred[200];
    char auth[280] = "";

    if (strlen(host) >= sizeof(s->connectHost) || strlen(mount) >= sizeof(s->ntripMount) ||
        strlen(user) + strlen(pass) + 2 > sizeof(cred)) {
        ucxapp_print("ERROR: host, mountpoint or credentials too long\n");
        return false;
    }
    if (!bridgeBegin(s, app, BR_TCP)) {
        return false;
    }
    if (ble && ucxapp_gatt_server_ensure(app, "nus") != UCXAPP_OK) {
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->ntripBle = ble;
    if (ble) {
        ucxapp_gatt_ensure_advertising(app);
    }
    if (user[0] != '\0') {
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);
        base64(cred, auth, sizeof(auth));
        memset(cred, 0, sizeof(cred));
    }
    snprintf(s->ntripMount, sizeof(s->ntripMount), "%s", mount[0] == '/' ? mount + 1 : mount);
    snprintf(s->ntripReq, sizeof(s->ntripReq),
             "GET /%s HTTP/1.0\r\nUser-Agent: NTRIP ucxapp\r\nAccept: */*\r\n%s%s%sConnection: close\r\n\r\n",
             s->ntripMount, auth[0] ? "Authorization: Basic " : "", auth, auth[0] ? "\r\n" : "");
    memset(auth, 0, sizeof(auth));
    s->ntrip = true;
    s->ggaPeriodMs = (uint32_t)ggaSeconds * 1000u;
    snprintf(s->connectHost, sizeof(s->connectHost), "%s", host);
    s->connectPort = port;
    s->tcpPort = port;
    if (!connectOnce(s)) {
        ucxapp_print("ERROR: cannot connect to %s:%d (is Wi-Fi connected?)\n", host, (int)port);
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: NTRIP client to %s:%d/%s, GGA every %d s, corrections go to the receiver%s\n", s->cfg.kind,
                 host, (int)port, s->ntripMount, (int)ggaSeconds,
                 ble ? "; corrected NMEA to BLE Nordic UART clients" : "");
    return true;
}

bool ucxapp_stream_bridge_udp(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, int32_t local)
{
    int32_t bindPort = local > 0 ? local : port;

    if (!bridgeBegin(s, app, BR_UDP)) {
        return false;
    }
    s->udpListen = (host == NULL);
    if (host != NULL && uCxStringToIpAddress(host, &s->addr) != 0 &&
        uCxSocketGetHostByName(app->ucx, host, &s->addr) != 0) {
        ucxapp_print("ERROR: cannot resolve %s\n", host);
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &s->udpSock) != 0) {
        ucxapp_print("ERROR: cannot create UDP socket (is Wi-Fi connected?)\n");
        s->udpSock = -1;
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    if (uCxSocketBind(app->ucx, s->udpSock, bindPort) != 0) {
        if (local > 0 || host == NULL) {
            ucxapp_print("ERROR: cannot bind UDP port %d\n", (int)bindPort);
            ucxapp_stream_bridge_stop(s);
            return false;
        }
        ucxapp_print("note: UDP port %d is busy, receiving is off\n", (int)bindPort);
    }
    s->udpPort = host != NULL ? port : 0;
    if (!bridgeRun(s)) {
        return false;
    }
    if (host != NULL) {
        ucxapp_print("%s bridge: UDP to %s:%d, listening on %d\n", s->cfg.kind, host, (int)port, (int)bindPort);
    } else {
        ucxapp_print("%s bridge: UDP listening on %d, replies go to the last sender\n", s->cfg.kind, (int)bindPort);
    }
    return true;
}

bool ucxapp_stream_bridge_sps(ucxapp_stream_t *s, ucxapp_t *app)
{
    if (!bridgeBegin(s, app, BR_SPS)) {
        return false;
    }
    if (uCxSpsSetServiceEnable(app->ucx, U_SPS_SERVICE_OPTION_ENABLE) != 0 ||
        uCxSpsSetDataMode(app->ucx, U_READ_MODE_BUFFERED) != 0) {
        ucxapp_print("ERROR: cannot enable the SPS service\n");
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the u-blox Serial Port Service\n", s->cfg.kind);
    return true;
}

bool ucxapp_stream_bridge_ble(ucxapp_stream_t *s, ucxapp_t *app)
{
    if (!bridgeBegin(s, app, BR_BLE)) {
        return false;
    }
    if (ucxapp_gatt_server_ensure(app, "nus") != UCXAPP_OK) {
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->bleCustom = false;
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the Nordic UART service and enable notifications\n", s->cfg.kind);
    return true;
}

bool ucxapp_stream_bridge_ble_custom(ucxapp_stream_t *s, ucxapp_t *app, const uint8_t svc[16], const uint8_t rx[16],
                                     const uint8_t tx[16])
{
    if (!bridgeBegin(s, app, BR_BLE)) {
        return false;
    }
    if (ucxapp_gatt_server_custom(app, svc, rx, tx) != UCXAPP_OK) {
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    s->bleCustom = true;
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the custom service, write to the RX and enable TX notifications\n",
                 s->cfg.kind);
    return true;
}

bool ucxapp_stream_bridge_uart(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud)
{
    if (strcmp(port, s->port) == 0) {
        ucxapp_print("ERROR: that is the port of the %s itself\n", s->cfg.kind);
        return false;
    }
    if (!bridgeBegin(s, app, BR_UART)) {
        return false;
    }
    s->out = uPortUartOpen(port, baud, false);
    if (s->out == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", port);
        ucxapp_stream_bridge_stop(s);
        return false;
    }
    snprintf(s->outPort, sizeof(s->outPort), "%s", port);
    s->outBaud = baud;
    if (!bridgeRun(s)) {
        return false;
    }
    ucxapp_print("%s bridge: %s <-> %s at %d baud\n", s->cfg.kind, s->port, port, (int)baud);
    return true;
}

void ucxapp_stream_bridge_stop(ucxapp_stream_t *s)
{
    uCxHandle_t *ucx = s->app != NULL ? s->app->ucx : NULL;

    if (s->bridge == BR_NONE) {
        return;
    }
    if (s->sender != NULL) {
        s->senderStop = true;
        ucxapp_thread_join(s->sender);
        s->sender = NULL;
    }
    if (s->out != NULL) {
        uPortUartClose(s->out);
        s->out = NULL;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ucxapp_tls_close(s->tls[i], ucx != NULL && i < s->nClients);
        s->tls[i] = NULL;
    }
    ucxapp_tls_server_free(s->tlsSrv);
    s->tlsSrv = NULL;
    if (ucx != NULL && s->bridge != BR_UART) {
        for (int i = 0; i < s->nClients; i++) {
            (void)uCxSocketClose(ucx, s->clients[i]);
        }
        if (s->listenSock >= 0) {
            (void)uCxSocketClose(ucx, s->listenSock);
        }
        if (s->udpSock >= 0) {
            (void)uCxSocketClose(ucx, s->udpSock);
        }
    }
    s->nClients = 0;
    s->listenSock = -1;
    s->udpSock = -1;
    memset(s->ntripReq, 0, sizeof(s->ntripReq));
    ucxapp_stream_lock(s);
    free(s->ring);
    s->ring = NULL;
    s->bridge = BR_NONE;
    ucxapp_stream_unlock(s);
}

typedef struct {
    const char *port;
    bool (*probe)(const char *port, void *result);
    void *result;
    bool hit;
    volatile bool done;
} probeJob_t;

static void probeEntry(void *arg)
{
    probeJob_t *j = (probeJob_t *)arg;

    j->hit = j->probe(j->port, j->result);
    j->done = true;
}

void ucxapp_stream_probe_ports(const char (*ports)[32], int count, bool (*probe)(const char *port, void *result),
                               void *results, size_t resultSize, bool *hits)
{
    static probeJob_t jobs[32];
    ucxapp_thread_t *threads[32];

    count = count > 32 ? 32 : count;
    for (int i = 0; i < count; i++) {
        jobs[i] = (probeJob_t){ ports[i], probe, (uint8_t *)results + (size_t)i * resultSize, false, false };
        threads[i] = ucxapp_thread_start(probeEntry, &jobs[i]);
        if (threads[i] == NULL) {
            probeEntry(&jobs[i]);
        }
    }
    for (int i = 0; i < count; i++) {
        /* The Windows join gives up after 5 s; a probe over every baud can take longer. */
        while (!jobs[i].done) {
            ucxapp_platform_sleep_ms(20);
        }
        ucxapp_thread_join(threads[i]);
        hits[i] = jobs[i].hit;
    }
}

bool ucxapp_stream_bridge_active(const ucxapp_stream_t *s)
{
    return s->bridge != BR_NONE;
}

static int openStreams(ucxapp_stream_t *out[MAX_STREAMS])
{
    int n = 0;

    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (gStreams[i] != NULL) {
            out[n++] = gStreams[i];
        }
    }
    regUnlock();
    return n;
}

static int cmdBridgeList(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_t *s[MAX_STREAMS];
    int n = openStreams(s);

    (void)app;
    (void)argc;
    (void)argv;
    if (n == 0) {
        ucxapp_print("no sources open (gnss.open, imu.open, cell.open, can.bridge, hw.bridge)\n");
    }
    for (int i = 0; i < n; i++) {
        ucxapp_print("%-5s %-12s ", s[i]->cfg.kind != NULL ? s[i]->cfg.kind : "?", s[i]->port);
        ucxapp_stream_bridge_status(s[i]);
    }
    return UCXAPP_OK;
}

static int cmdBridgeOff(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_t *s[MAX_STREAMS];
    int n = openStreams(s);
    int stopped = 0;

    (void)app;
    (void)argc;
    (void)argv;
    for (int i = 0; i < n; i++) {
        if (ucxapp_stream_bridge_active(s[i])) {
            ucxapp_stream_bridge_stop(s[i]);
            stopped++;
        }
    }
    ucxapp_print("%d bridge(s) stopped\n", stopped);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kBridgeCmds[] = {
    { "bridge.list", "", "Every open source (gnss, imu, cell, can, hw) and where its bridge goes", 0, cmdBridgeList },
    { "bridge.off", "", "Stop every running bridge", 0, cmdBridgeOff },
};

void ucxapp_register_bridge_cmds(void)
{
    (void)ucxapp_cmd_register(kBridgeCmds, sizeof(kBridgeCmds) / sizeof(kBridgeCmds[0]));
}

void ucxapp_stream_bridge_status(ucxapp_stream_t *s)
{
    static const char *const kName[] = { "off", "TCP server", "UDP", "BLE Nordic UART", "UART", "BLE SPS" };

    if (s->bridge == BR_NONE) {
        ucxapp_print("bridge: off\n");
        return;
    }
    if (s->ntrip) {
        ucxapp_print("bridge: NTRIP %s:%d/%s, %s, %u bytes of corrections\n", s->connectHost, (int)s->connectPort,
                     s->ntripMount, s->ntripFailed ? "refused by the caster"
                                    : s->nClients == 0 ? "reconnecting"
                                    : !s->ntripStreaming ? "waiting for the caster"
                                    : s->gga[0] == '\0' ? "streaming, no GGA from the receiver yet" : "streaming",
                     (unsigned)s->received);
        return;
    }
    ucxapp_print("bridge: %s", s->bridge == BR_TCP && s->connectHost[0] ? "TCP client"
                               : s->bridge == BR_BLE && s->bleCustom ? "BLE custom service" : kName[s->bridge]);
    if (s->bridge == BR_TCP && s->connectHost[0]) {
        ucxapp_print(" %s:%d, %s", s->connectHost, (int)s->connectPort, s->nClients > 0 ? "connected" : "reconnecting");
    } else if (s->bridge == BR_TCP) {
        ucxapp_print("%s port %d, %d client(s)", s->tlsSrv != NULL ? " (TLS)" : "", (int)s->tcpPort, s->nClients);
        for (int c = 0; c < s->nClients; c++) {
            if (s->tls[c] != NULL && ucxapp_tls_peer(s->tls[c])[0] != '\0') {
                ucxapp_print(" [%s]", ucxapp_tls_peer(s->tls[c]));
            }
        }
    } else if (s->bridge == BR_UDP) {
        ucxapp_print(" port %d", (int)s->udpPort);
    } else if (s->bridge == BR_UART) {
        ucxapp_print(" %s at %d baud", s->outPort, (int)s->outBaud);
    } else {
        ucxapp_print(", %s", ucxapp_handle_current(UCXAPP_HANDLE_CONN) < 0 ? "no client connected" : "client connected");
    }
    ucxapp_print(", %u bytes out, %u in, %u dropped\n", (unsigned)s->sent, (unsigned)s->received,
                 (unsigned)s->dropped);
}

/* 128-bit UUID as 32 hex digits, dashes allowed; false if malformed. */
static bool parseUuid128(const char *s, uint8_t out[16])
{
    int n = 0;

    for (; *s != '\0'; s++) {
        int d;
        char lc = (char)(*s | 0x20);

        if (*s == '-') {
            continue;
        }
        d = (*s >= '0' && *s <= '9') ? *s - '0' : (lc >= 'a' && lc <= 'f') ? lc - 'a' + 10 : -1;
        if (d < 0 || n >= 32) {
            return false;
        }
        out[n / 2] = (uint8_t)(n % 2 == 0 ? d << 4 : out[n / 2] | d);
        n++;
    }
    return n == 32;
}

/* bridge subcommand shared by gnss, imu and cell: argv[1] is tcp|udp|ble|uart|off|status. */
int ucxapp_stream_bridge_cmd(ucxapp_stream_t *s, ucxapp_t *app, int argc, char *argv[], int32_t defaultPort)
{
    int32_t port = defaultPort;
    int32_t baud = 115200;

    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "status") == 0) {
        ucxapp_stream_bridge_status(s);
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        ucxapp_stream_bridge_stop(s);
        ucxapp_print("bridge stopped\n");
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "tcp") == 0 && argc <= 3) {
        if (argc == 3 && (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535)) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_tcp(s, app, port) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "tls") == 0 && argc <= 5) {
        const char *cert = "server";
        bool auth = argc > 2 && strcasecmp(argv[argc - 1], "clientauth") == 0;
        int rest = argc - (auth ? 1 : 0);
        if (rest >= 3 && (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535)) {
            return UCXAPP_ERR_USAGE;
        }
        if (rest == 4) {
            cert = argv[3];
        } else if (rest > 4) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_tls(s, app, port, cert, auth) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "tcpc") == 0 && argc == 4) {
        if (!ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_tcp_connect(s, app, argv[2], port) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "udp") == 0 && (argc == 4 || argc == 5)) {
        int32_t local = 0;
        if (!ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535 ||
            (argc == 5 && (!ucxapp_parse_int(argv[4], &local) || local < 1 || local > 65535))) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_udp(s, app, argv[2], port, local) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "udps") == 0 && argc == 3) {
        if (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_udp(s, app, NULL, 0, port) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "ble") == 0 && argc >= 3 && strcasecmp(argv[2], "custom") == 0) {
        uint8_t svc[16], rx[16], tx[16];

        if ((argc != 4 && argc != 6) || !parseUuid128(argv[3], svc)) {
            return UCXAPP_ERR_USAGE;
        }
        if (argc == 6) {
            if (!parseUuid128(argv[4], rx) || !parseUuid128(argv[5], tx)) {
                return UCXAPP_ERR_USAGE;
            }
        } else {   /* Nordic style: rx and tx differ from the service in the fourth byte */
            memcpy(rx, svc, 16);
            memcpy(tx, svc, 16);
            rx[3] = (uint8_t)(svc[3] + 1);
            tx[3] = (uint8_t)(svc[3] + 2);
        }
        return ucxapp_stream_bridge_ble_custom(s, app, svc, rx, tx) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "ble") == 0 && argc <= 3) {
        if (argc == 3 && strcasecmp(argv[2], "sps") == 0) {
            return ucxapp_stream_bridge_sps(s, app) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
        }
        if (argc == 3 && strcasecmp(argv[2], "nus") != 0) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_ble(s, app) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "cell") == 0 && argc == 2) {
        char cport[32];
        int32_t cbaud;

        if (!ucxapp_cell_data_link(cport, sizeof(cport), &cbaud)) {
            ucxapp_print("ERROR: no cellular data link (use cell.connect first)\n");
            return UCXAPP_ERR_FAILED;
        }
        return ucxapp_stream_bridge_uart(s, app, cport, cbaud) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "uart") == 0 && argc >= 3) {
        if (argc == 4 && (!ucxapp_parse_int(argv[3], &baud) || baud <= 0)) {
            return UCXAPP_ERR_USAGE;
        }
        return ucxapp_stream_bridge_uart(s, app, argv[2], baud) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    return UCXAPP_ERR_USAGE;
}

/* ---- URC hooks ---- */

/* Module has data on a socket: flag the bridge that owns it. */
void ucxapp_stream_on_data(int32_t sock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        ucxapp_stream_t *s = gStreams[i];
        if (s != NULL && (s->bridge == BR_TCP || (s->bridge == BR_UDP && s->udpSock == sock))) {
            s->rxPending = true;
        }
    }
    (void)sock;
    regUnlock();
}

/* A BLE client wrote to the Nordic UART RX characteristic. */
void ucxapp_stream_on_ble_rx(const uint8_t *data, size_t len)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        ucxapp_stream_t *s = gStreams[i];
        if (s != NULL && s->bridge == BR_BLE) {
            (void)ucxapp_stream_write(s, data, len);
            s->received += (uint32_t)len;
        }
    }
    regUnlock();
}

/* ---- URC hooks ---- */

void ucxapp_stream_on_sps_data(void)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (gStreams[i] != NULL && gStreams[i]->bridge == BR_SPS) {
            gStreams[i]->rxPending = true;
        }
    }
    regUnlock();
}

void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        ucxapp_stream_t *s = gStreams[i];
        if (s != NULL && s->bridge == BR_TCP && s->listenSock == listenSock && s->nAccepted < EVT_QUEUE) {
            s->accepted[s->nAccepted++] = sock;
        }
    }
    regUnlock();
}

void ucxapp_stream_on_closed(int32_t sock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        ucxapp_stream_t *s = gStreams[i];
        if (s != NULL && s->bridge == BR_TCP && s->nClosed < EVT_QUEUE) {
            s->closed[s->nClosed++] = sock;
        }
    }
    regUnlock();
}
