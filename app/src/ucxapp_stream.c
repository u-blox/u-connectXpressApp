/* Serial sensor stream: reader thread, raw log and module bridges (TCP server, UDP, BLE Nordic UART). */

#include "ucxapp_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_cmd.h"
#include "ucxapp_corr.h"
#include "ucxapp_platform.h"
#include "ucxapp_stats.h"
#include "ucxapp_thread.h"
#include "ucxapp_tls.h"
#include "ucxapp_vport.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_mqtt.h"
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
#define MAX_BRIDGES   UCXAPP_STREAM_MAX_BRIDGES
#define EVT_QUEUE     8

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

enum { BR_NONE, BR_TCP, BR_UDP, BR_BLE, BR_UART, BR_SPS, BR_MQTT };

#define RECONNECT_MS  3000u
#define MQTT_ID       0        /* NORA-W36 has one MQTT connection, shared with mqtt.* */
#define MQTT_BUF      5000u    /* largest binary MQTT message the module hands over */
#define MQTT_RETRY_MS 10000u
#define MQTT_POLL_MS  5000u
#define SPS_POLL_MS   1000u    /* SPS: fallback read without +UESPSDA */
#define MQTT_TOPICS   2

typedef struct bridge bridge_t;

struct ucxapp_stream {
    uPortUartHandle_t uart;
    char port[32];
    int32_t baud;
    ucxapp_stream_cfg_t cfg;
    U_CX_MUTEX_HANDLE lock;
    U_CX_MUTEX_HANDLE writeLock;   /* several bridges write to the same UART */
    volatile bool stop;
    ucxapp_thread_t *reader;
    uint32_t rx;
    FILE *log;
    char logName[200];
    bridge_t *br[MAX_BRIDGES];     /* changed under gRegLock and the stream lock */
};

struct bridge {
    ucxapp_stream_t *s;
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
    bool cellLink;                 /* BR_UART: the port is a cellular data link that reconnects when it drops */
    uint8_t linkLost[2];           /* bytes matched of each "link dropped" line */
    uint32_t relinks;
    volatile bool rxPending;       /* the module has data for us (TCP client or UDP socket) */
    uint32_t received;             /* bytes that came back from the far side */
    char connectHost[64];          /* BR_TCP client mode; empty when serving */
    int32_t connectPort;
    uint32_t nextRetry;
    bool udpListen;                /* reply to whoever sent last */
    int32_t spsConn;
    uint32_t spsLastRead;          /* BR_SPS: tick of the last AT+USPSRB */
    ucxapp_tls_server_t *tlsSrv;   /* BR_TCP server with TLS; NULL for plain TCP */
    ucxapp_tls_conn_t *tls[MAX_CLIENTS];
    bool ntrip;                    /* BR_TCP client (or BR_UART data link) speaking NTRIP v1 to a caster */
    volatile bool ntripFailed;     /* caster refused: no reconnect */
    volatile bool ntripStreaming;  /* response header passed */
    bool ntripBle;                 /* also send the receiver stream to a BLE Nordic UART client */
    char ntripMount[64];
    char args[96];                 /* the <kind>.bridge arguments that started it, to spot a repeat */
    char ntripReq[480];
    char ntripHdr[256];
    size_t ntripHdrLen;
    char nmea[100];                /* NMEA line being assembled from the receiver */
    size_t nmeaLen;
    char gga[100];                 /* latest GGA, sent to the caster */
    uint32_t ggaPeriodMs;
    uint32_t nextGga;
    char mqttClient[64];           /* BR_MQTT: subscriber whose messages go to the device (PointPerfect) */
    char mqttCert[32];
    char mqttTopic[MQTT_TOPICS][64];
    int nTopics;
    bool mqttUp;
    uint32_t mqttMsgs;
    uint32_t mqttNextPoll;
    uint8_t *mqttBuf;
    ucxapp_corr_t *corr;           /* NTRIP and MQTT: RTCM/SPARTN/UBX frame check of what goes to the receiver */
};

static ucxapp_stream_t *gStreams[MAX_STREAMS];

/* Guards gStreams and the URC queues; the events mutex only exists while a module is connected. */
static U_CX_MUTEX_HANDLE gRegLock;
/* Guards a TCP bridge's client list against bridge.list while the sender drops a client (and frees its TLS). */
static U_CX_MUTEX_HANDLE gClientLock;
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

static void ringPut(bridge_t *b, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b->count == RING_SIZE) {
            b->head = (b->head + 1) % RING_SIZE;
            b->count--;
            b->dropped++;
        }
        b->ring[(b->head + b->count) % RING_SIZE] = d[i];
        b->count++;
    }
}

static size_t ringGet(bridge_t *b, uint8_t *out, size_t max)
{
    size_t n;

    ucxapp_stream_lock(b->s);
    n = b->count < max ? b->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = b->ring[(b->head + i) % RING_SIZE];
    }
    b->head = (b->head + n) % RING_SIZE;
    b->count -= n;
    ucxapp_stream_unlock(b->s);
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
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] != NULL) {
            ringPut(s->br[i], buf, n);
        }
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
            n = ucxapp_uart_read(s->uart, buf, sizeof(buf), 20);
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
        U_CX_MUTEX_CREATE(gClientLock);
        gRegInit = true;
    }
    s->cfg = *cfg;
    snprintf(s->port, sizeof(s->port), "%s", port);
    s->baud = baud;
    bool virt = strncmp(port, "virt:", 5) == 0 && !ucxapp_vport_is(port);

    s->uart = virt ? NULL : ucxapp_uart_open(port, baud);
    if (s->uart == NULL && !virt) {
        free(s);
        return NULL;
    }
    U_CX_MUTEX_CREATE(s->lock);
    U_CX_MUTEX_CREATE(s->writeLock);
    s->reader = ucxapp_thread_start(readerLoop, s);
    if (s->reader == NULL) {
        if (s->uart != NULL) {
            ucxapp_uart_close(s->uart);
        }
        U_CX_MUTEX_DELETE(s->writeLock);
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
        ucxapp_uart_close(s->uart);
    }
    if (s->log != NULL) {
        fclose(s->log);
    }
    U_CX_MUTEX_DELETE(s->writeLock);
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
    bool ok;

    U_CX_MUTEX_LOCK(s->writeLock);   /* several bridges may write to one stream, virtual ones too */
    if (s->uart == NULL) {
        ok = s->cfg.write != NULL && s->cfg.write(s->cfg.ctx, data, len);
    } else {
        ok = ucxapp_uart_write(s->uart, data, len) == (int32_t)len;
    }
    U_CX_MUTEX_UNLOCK(s->writeLock);
    return ok;
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

static void dropClient(bridge_t *b, int idx, bool closeSock)
{
    U_CX_MUTEX_LOCK(gClientLock);
    ucxapp_tls_close(b->tls[idx], closeSock);
    if (closeSock) {
        (void)uCxSocketClose(b->app->ucx, b->clients[idx]);
    }
    b->nClients--;
    b->clients[idx] = b->clients[b->nClients];
    b->tls[idx] = b->tls[b->nClients];
    b->tls[b->nClients] = NULL;
    U_CX_MUTEX_UNLOCK(gClientLock);
}

static bool connectOnce(bridge_t *b)
{
    uCxHandle_t *ucx = b->app->ucx;
    int32_t sock = -1;

    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        return false;
    }
    if (uCxSocketConnect(ucx, sock, b->connectHost, b->connectPort) != 0) {
        (void)uCxSocketClose(ucx, sock);
        return false;
    }
    if (b->ntrip) {
        b->ntripStreaming = false;
        b->ntripHdrLen = 0;
        if (uCxSocketWrite(ucx, sock, (const uint8_t *)b->ntripReq, (int32_t)strlen(b->ntripReq)) < 0) {
            (void)uCxSocketClose(ucx, sock);
            return false;
        }
    }
    b->clients[b->nClients++] = sock;
    return true;
}

/* Response header from the caster: "ICY 200 OK" (v1) or "HTTP/1.x 200"; returns bytes of buf used by it, -1 on refusal. */
static int32_t ntripHeader(bridge_t *b, const uint8_t *buf, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        char *h = b->ntripHdr;
        size_t len;

        if (b->ntripHdrLen + 1 >= sizeof(b->ntripHdr)) {
            ucxapp_print("ERROR: NTRIP caster sent no valid response\n");
            return -1;
        }
        h[b->ntripHdrLen++] = (char)buf[i];
        h[b->ntripHdrLen] = '\0';
        len = b->ntripHdrLen;
        if (len < 2 || h[len - 2] != '\r' || h[len - 1] != '\n') {
            continue;
        }
        if (strstr(h, "\r\n") == h + len - 2) {   /* status line complete */
            bool ok = strncmp(h, "ICY 200", 7) == 0 || (strncmp(h, "HTTP/1.", 7) == 0 && strncmp(h + 8, " 200", 4) == 0);
            if (!ok) {
                h[len - 2] = '\0';
                ucxapp_print("ERROR: NTRIP caster refused /%s: %s%s\n", b->ntripMount, h,
                             strncmp(h, "SOURCETABLE", 11) == 0 ? " (unknown mountpoint)"
                             : strstr(h, " 401") != NULL    ? " (wrong user name or password)" : "");
                return -1;
            }
            if (h[0] == 'I') {
                b->ntripStreaming = true;
                return i + 1;
            }
        } else if (len >= 4 && h[len - 4] == '\r' && h[len - 3] == '\n') {
            b->ntripStreaming = true;
            return i + 1;
        }
    }
    return n;
}

/* The receiver stream to a BLE Nordic UART client next to the NTRIP link. */
static void ntripBle(bridge_t *b, const uint8_t *d, size_t n)
{
    if (!b->ntripBle || b->app->ucx == NULL || ucxapp_handle_current(UCXAPP_HANDLE_CONN) < 0) {
        return;
    }
    for (size_t off = 0; off < n; off += BLE_CHUNK_MIN) {
        size_t piece = n - off < BLE_CHUNK_MIN ? n - off : BLE_CHUNK_MIN;
        if (ucxapp_gatt_notify_nus(b->app, d + off, piece)) {
            ucxapp_stats_add(UCXAPP_STATS_GATT, piece, 0);
        }
    }
}

/* Keeps the last GGA sentence of the receiver stream for the caster. */
static void ntripScan(bridge_t *b, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)d[i];

        if (c == '$') {
            b->nmeaLen = 0;
        }
        if (b->nmeaLen + 3 >= sizeof(b->nmea)) {
            b->nmeaLen = 0;
            continue;
        }
        b->nmea[b->nmeaLen++] = c;
        if (c == '\n') {
            b->nmea[b->nmeaLen] = '\0';
            if (b->nmeaLen > 7 && b->nmea[0] == '$' && strncmp(b->nmea + 3, "GGA,", 4) == 0) {
                memcpy(b->gga, b->nmea, b->nmeaLen + 1);
            }
            b->nmeaLen = 0;
        }
    }
}

static void tcpService(bridge_t *b)
{
    int32_t acc[EVT_QUEUE];
    int32_t cls[EVT_QUEUE];
    int nAcc;
    int nCls;

    regLock();
    nAcc = b->nAccepted;
    nCls = b->nClosed;
    memcpy(acc, b->accepted, sizeof(acc));
    memcpy(cls, b->closed, sizeof(cls));
    b->nAccepted = 0;
    b->nClosed = 0;
    regUnlock();

    for (int i = 0; i < nCls; i++) {
        for (int c = 0; c < b->nClients; c++) {
            if (b->clients[c] == cls[i]) {
                dropClient(b, c, false);
                break;
            }
        }
    }
    for (int i = 0; i < nAcc; i++) {
        if (b->nClients < MAX_CLIENTS) {
            b->tls[b->nClients] = b->tlsSrv != NULL ? ucxapp_tls_accept(b->tlsSrv, b->app->ucx, acc[i]) : NULL;
            if (b->tlsSrv != NULL && b->tls[b->nClients] == NULL) {
                (void)uCxSocketClose(b->app->ucx, acc[i]);
                continue;
            }
            b->clients[b->nClients++] = acc[i];
        } else {
            (void)uCxSocketClose(b->app->ucx, acc[i]);
        }
    }
    if (b->connectHost[0] != '\0' && !b->ntripFailed && b->nClients == 0 && ucxapp_platform_tick_ms() >= b->nextRetry) {
        b->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
        (void)connectOnce(b);
    }
}

static bool spsReady(bridge_t *b)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    if (conn < 0) {
        b->spsConn = -1;
        return false;
    }
    if (conn != b->spsConn) {
        if (uCxSpsConnect1(b->app->ucx, conn) != 0) {
            return false;
        }
        b->spsConn = conn;
    }
    return true;
}

static bool bleSend(bridge_t *b, const uint8_t *d, size_t n)
{
    if (b->bridge == BR_SPS) {
        return b->spsConn >= 0 && uCxSpsWrite(b->app->ucx, b->spsConn, d, (int32_t)n) >= 0;
    }
    return b->bleCustom ? ucxapp_gatt_notify_custom(b->app, d, n) : ucxapp_gatt_notify_nus(b->app, d, n);
}

static void deliver(bridge_t *b, const uint8_t *d, size_t n)
{
    uCxHandle_t *ucx = b->app->ucx;

    if (b->bridge == BR_TCP) {
        for (int c = b->nClients - 1; c >= 0; c--) {
            if (b->tls[c] != NULL && ucxapp_tls_handshake(b->tls[c]) <= 0) {
                continue;   /* not secured yet: nothing in clear text */
            }
            if ((b->tls[c] != NULL ? ucxapp_tls_write(b->tls[c], d, n)
                                   : ucxapp_sock_write_all(ucx, b->clients[c], d, n)) < 0) {
                dropClient(b, c, true);
            } else {
                ucxapp_stats_add(UCXAPP_STATS_TCP, n, 0);
            }
        }
        if (b->nClients > 0) {
            b->sent += (uint32_t)n;
        }
    } else if (b->bridge == BR_UDP) {
        if (b->udpPort <= 0) {
            b->dropped += (uint32_t)n;   /* listening, nobody has written yet */
        } else if (uCxSocketSendTo(ucx, b->udpSock, &b->addr, b->udpPort, d, (int32_t)n) < 0) {
            b->dropped += (uint32_t)n;
        } else {
            b->sent += (uint32_t)n;
            ucxapp_stats_add(UCXAPP_STATS_UDP, n, 0);
        }
    } else if (b->bridge == BR_BLE || b->bridge == BR_SPS) {
        size_t off = 0;

        while (off < n) {
            size_t piece = n - off < b->bleChunk ? n - off : b->bleChunk;
            bool ok = bleSend(b, d + off, piece);
            if (!ok && b->bleChunk > BLE_CHUNK_MIN) {
                b->bleChunk = BLE_CHUNK_MIN;   /* the link has the default 23 byte MTU */
                piece = piece < b->bleChunk ? piece : b->bleChunk;
                ok = bleSend(b, d + off, piece);
            }
            if (!ok) {
                b->dropped += (uint32_t)(n - off);
                return;
            }
            b->sent += (uint32_t)piece;
            ucxapp_stats_add(b->bridge == BR_SPS ? UCXAPP_STATS_SPS : UCXAPP_STATS_GATT, piece, 0);
            off += piece;
        }
    }
}

/* Corrections (NTRIP, MQTT) to the serial device, checked frame by frame. */
static void toReceiver(bridge_t *b, const uint8_t *d, size_t n)
{
    (void)ucxapp_stream_write(b->s, d, n);
    b->received += (uint32_t)n;
    if (b->corr != NULL) {
        ucxapp_corr_feed(b->corr, d, n);
    }
}

/* Bytes from the far side go back to the serial device. */
static void pullInbound(bridge_t *b)
{
    uCxHandle_t *ucx = b->app->ucx;
    uint8_t buf[NET_CHUNK];

    if (b->bridge == BR_TCP) {
        for (int c = b->nClients - 1; c >= 0; c--) {
            for (int i = 0; i < 8; i++) {
                int32_t n = b->tls[c] != NULL ? ucxapp_tls_read(b->tls[c], buf, sizeof(buf))
                                              : uCxSocketRead(ucx, b->clients[c], (int32_t)sizeof(buf), buf);
                if (n < 0 && b->tls[c] != NULL) {
                    dropClient(b, c, true);   /* failed handshake or peer closed the session */
                    break;
                }
                if (n <= 0) {
                    break;
                }
                ucxapp_stats_add(UCXAPP_STATS_TCP, 0, (size_t)n);
                int32_t skip = 0;
                if (b->ntrip && !b->ntripStreaming) {
                    skip = ntripHeader(b, buf, n);
                    if (skip < 0) {
                        b->ntripFailed = true;
                        dropClient(b, c, true);
                        break;
                    }
                    if (b->ntripStreaming) {
                        ucxapp_print("NTRIP: streaming corrections from /%s\n", b->ntripMount);
                        b->nextGga = 0;
                    }
                }
                if (n > skip) {
                    toReceiver(b, buf + skip, (size_t)(n - skip));
                }
            }
        }
    } else if (b->bridge == BR_UDP) {
        uCxSocketReceiveFrom_t from;
        for (int i = 0; i < 8; i++) {
            int32_t n = ucxapp_udp_recvfrom(ucx, b->udpSock, (int32_t)sizeof(buf), buf, &from);
            if (n <= 0) {
                break;
            }
            if (b->udpListen) {
                b->addr = from.remote_ip;
                b->udpPort = from.remote_port;
            }
            (void)ucxapp_stream_write(b->s, buf, (size_t)n);
            b->received += (uint32_t)n;
            ucxapp_stats_add(UCXAPP_STATS_UDP, 0, (size_t)n);
        }
    } else if (b->bridge == BR_SPS && b->spsConn >= 0) {
        /* One read per +UESPSDA: the module raises it again after an AT+USPSRB that leaves data behind, and a
         * read that overlaps the module raising it asserts NORA-W36 3.5.0 (u_buffer.c line 267). */
        b->spsLastRead = ucxapp_platform_tick_ms();
        int32_t n = uCxSpsRead(ucx, b->spsConn, (int32_t)sizeof(buf), buf);
        if (n > 0) {
            ucxapp_stats_add(UCXAPP_STATS_SPS, 0, (size_t)n);
            (void)ucxapp_stream_write(b->s, buf, (size_t)n);
            b->received += (uint32_t)n;
        }
    }
}

/* ---- MQTT subscriber: every message on the topics goes to the device unchanged (PointPerfect SPARTN, keys) ---- */

static int32_t mqttRetry(int32_t (*op)(bridge_t *, int), bridge_t *b, int arg)
{
    int32_t rc = -15;

    for (int i = 0; rc == -15 && i < 10; i++) {   /* ERROR:15 while the previous MQTT operation is in flight */
        if (i > 0) {
            ucxapp_platform_sleep_ms(100);
        }
        rc = op(b, arg);
    }
    return rc;
}

static int32_t mqttSubscribeOne(bridge_t *b, int i)
{
    return uCxMqttSubscribe4(b->app->ucx, MQTT_ID, U_MQTT_SUBSCRIBE_ACTION_SUBSCRIBE, b->mqttTopic[i],
                             U_MQTT_QOS_AT_LEAST_ONCE);
}

/* TLS with the client certificate <cert>, its key <cert>-key and the broker CA <cert>-ca (cert.upload names),
   then the subscriptions. verbose explains failures; the sender retries quietly. */
static bool mqttConnect(bridge_t *b, bool verbose)
{
    uCxHandle_t *ucx = b->app->ucx;
    char ca[48];
    char key[48];

    snprintf(ca, sizeof(ca), "%s-ca", b->mqttCert);
    snprintf(key, sizeof(key), "%s-key", b->mqttCert);
    b->mqttUp = false;
    ucxapp_events_clear(URC_FLAG_MQTT_CONNECTED | URC_FLAG_MQTT_DISCONNECTED | URC_FLAG_MQTT_SUBSCRIBED);
    if (uCxMqttSetConnectionParams4(ucx, MQTT_ID, b->connectHost, b->connectPort, b->mqttClient) != 0 ||
        uCxMqttSetTLS5(ucx, MQTT_ID, U_WIFI_TLS_VERSION_TLS1_2, ca, b->mqttCert, key) != 0) {
        if (verbose) {
            ucxapp_print("ERROR: cannot set up MQTT over TLS (upload %s, %s and %s with cert.upload)\n", b->mqttCert, key,
                         ca);
        }
        return false;
    }
    if (uCxMqttConnect(ucx, MQTT_ID) != 0 || !ucxapp_events_wait(URC_FLAG_MQTT_CONNECTED, 15)) {
        if (verbose) {
            ucxapp_print("ERROR: no MQTT connection to %s:%d (Wi-Fi up? client ID and certificates of the same "
                         "PointPerfect thing?)\n", b->connectHost, (int)b->connectPort);
        }
        (void)uCxMqttDisconnect(ucx, MQTT_ID);
        return false;
    }
    for (int i = 0; i < b->nTopics; i++) {
        if (mqttRetry(mqttSubscribeOne, b, i) != 0 || !ucxapp_events_wait(URC_FLAG_MQTT_SUBSCRIBED, 5)) {
            if (verbose) {
                ucxapp_print("ERROR: cannot subscribe to %s (is the thing allowed this plan and region?)\n",
                             b->mqttTopic[i]);
            }
            (void)uCxMqttDisconnect(ucx, MQTT_ID);
            return false;
        }
    }
    b->mqttUp = true;
    b->mqttNextPoll = 0;
    return true;
}

/* Reconnects after a drop and moves the queued messages to the device. */
static void mqttService(bridge_t *b)
{
    uint32_t now = ucxapp_platform_tick_ms();

    if (b->mqttUp && ucxapp_events_take(URC_FLAG_MQTT_DISCONNECTED)) {
        b->mqttUp = false;
        b->nextRetry = now + MQTT_RETRY_MS;
        ucxapp_print("%s bridge: MQTT connection to %s lost, reconnecting\n", b->s->cfg.kind, b->connectHost);
    }
    if (!b->mqttUp) {
        if ((int32_t)(now - b->nextRetry) >= 0) {
            b->nextRetry = now + MQTT_RETRY_MS;
            if (mqttConnect(b, false)) {
                ucxapp_print("%s bridge: MQTT reconnected to %s\n", b->s->cfg.kind, b->connectHost);
            }
        }
        return;
    }
    if (!ucxapp_events_take(URC_FLAG_MQTT_DATA) && (int32_t)(now - b->mqttNextPoll) < 0) {
        return;
    }
    b->mqttNextPoll = now + MQTT_POLL_MS;
    for (int i = 0; i < 16 && !b->senderStop; i++) {
        const char *topic = NULL;
        int32_t n = uCxMqttReadBegin(b->app->ucx, MQTT_ID, b->mqttBuf, (uint16_t)MQTT_BUF, &topic);
        if (n > 0) {
            toReceiver(b, b->mqttBuf, (size_t)n);
            b->mqttMsgs++;
            ucxapp_stats_add(UCXAPP_STATS_MQTT, 0, (size_t)n);
        }
        uCxEnd(b->app->ucx);
        if (n <= 0) {
            break;   /* ERROR:11: nothing queued */
        }
    }
}

static void senderLoop(void *arg)
{
    bridge_t *b = arg;
    uint8_t chunk[NET_CHUNK];

    while (!b->senderStop) {
        size_t n;

        if (b->bridge == BR_SPS) {
            /* Wake on +UESPSDA: SPS reads once per URC (see pullInbound) and must not wait a full period. */
            for (uint32_t t = 0; t < SEND_PERIOD && !b->rxPending && !b->senderStop; t++) {
                ucxapp_platform_sleep_ms(1);
            }
            if (b->spsConn >= 0 && ucxapp_platform_tick_ms() - b->spsLastRead >= SPS_POLL_MS) {
                b->rxPending = true;   /* fallback in case a URC was missed */
            }
        } else {
            ucxapp_platform_sleep_ms(SEND_PERIOD);
        }
        if (b->app->ucx == NULL) {
            continue;
        }
        if (b->bridge == BR_MQTT) {
            mqttService(b);
            while (ringGet(b, chunk, sizeof(chunk)) > 0) {
                /* subscriber only: the receiver's own output is not sent */
            }
            continue;
        }
        if (b->bridge == BR_TCP) {
            tcpService(b);
        }
        if (b->rxPending || (b->tlsSrv != NULL && b->nClients > 0)) {
            b->rxPending = false;
            pullInbound(b);   /* TLS polls: records can wait in the module without a new URC */
        }
        if (b->ntrip) {
            while ((n = ringGet(b, chunk, sizeof(chunk))) > 0) {
                ntripScan(b, chunk, n);
                ntripBle(b, chunk, n);
            }
            if (b->ntripStreaming && b->nClients > 0 && b->gga[0] != '\0' &&
                (int32_t)(ucxapp_platform_tick_ms() - b->nextGga) >= 0) {
                b->nextGga = ucxapp_platform_tick_ms() + b->ggaPeriodMs;
                deliver(b, (const uint8_t *)b->gga, strlen(b->gga));
            }
            continue;
        }
        while (!b->senderStop && (n = ringGet(b, chunk, sizeof(chunk))) > 0) {
            if (b->bridge == BR_BLE && ucxapp_handle_current(UCXAPP_HANDLE_CONN) < 0) {
                continue;   /* nobody connected: discard */
            }
            if (b->bridge == BR_SPS && !spsReady(b)) {
                continue;
            }
            deliver(b, chunk, n);
        }
    }
}

/* Serial to serial: no module needed, both directions. With NTRIP the other port is a data link to the caster:
   only GGA goes up, the corrections come down after the response header. */
static void ntripStart(bridge_t *b)
{
    b->ntripStreaming = false;
    b->ntripHdrLen = 0;
    (void)ucxapp_uart_write(b->out, (const uint8_t *)b->ntripReq, strlen(b->ntripReq));
}

/* A modem in transparent mode leaves it with one of these lines when the far side or the network drops the socket. */
static bool cellLinkLost(bridge_t *b, const uint8_t *d, size_t n)
{
    static const char *const kLost[2] = { "\r\nNO CARRIER\r\n", "\r\nCLOSED\r\n" };

    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            if ((char)d[i] == kLost[k][b->linkLost[k]]) {
                b->linkLost[k]++;
            } else {
                b->linkLost[k] = (char)d[i] == kLost[k][0] ? 1 : 0;
            }
            if (kLost[k][b->linkLost[k]] == '\0') {
                b->linkLost[0] = b->linkLost[1] = 0;
                return true;
            }
        }
    }
    return false;
}

/* Connects the modem again (backing off up to 30 s) and takes its port back; false only when the bridge stops. */
static bool cellRelink(bridge_t *b)
{
    uint32_t waitMs = 1000;

    ucxapp_print("%s bridge: cellular link on %s lost, reconnecting\n", b->s->cfg.kind, b->outPort);
    ucxapp_uart_close(b->out);
    b->out = NULL;
    while (!b->senderStop) {
        if (ucxapp_cell_relink()) {
            b->out = ucxapp_uart_open(b->outPort, b->outBaud);
            if (b->out != NULL) {
                b->relinks++;
                ucxapp_print("%s bridge: cellular link on %s reconnected\n", b->s->cfg.kind, b->outPort);
                if (b->ntrip) {
                    ntripStart(b);
                }
                return true;
            }
        }
        for (uint32_t t = 0; t < waitMs && !b->senderStop; t += 100) {
            ucxapp_platform_sleep_ms(100);
        }
        waitMs = waitMs < 15000 ? waitMs * 2 : 30000;
    }
    return false;
}

static void uartBridgeLoop(void *arg)
{
    bridge_t *b = arg;
    uint8_t chunk[NET_CHUNK];

    if (b->ntrip) {
        ntripStart(b);
    }
    while (!b->senderStop) {
        size_t n = ringGet(b, chunk, sizeof(chunk));

        if (b->ntrip) {
            ntripScan(b, chunk, n);
            ntripBle(b, chunk, n);
            if (b->ntripStreaming && b->gga[0] != '\0' && (int32_t)(ucxapp_platform_tick_ms() - b->nextGga) >= 0) {
                b->nextGga = ucxapp_platform_tick_ms() + b->ggaPeriodMs;
                n = strlen(b->gga);
                if (ucxapp_uart_write(b->out, (const uint8_t *)b->gga, n) == (int32_t)n) {
                    b->sent += (uint32_t)n;
                }
            }
        } else if (n > 0 && ucxapp_uart_write(b->out, chunk, n) == (int32_t)n) {
            b->sent += (uint32_t)n;
        } else if (n > 0) {
            b->dropped += (uint32_t)n;
        }
        int32_t r = ucxapp_uart_read(b->out, chunk, sizeof(chunk), 20);
        int32_t skip = 0;
        if (r > 0 && b->cellLink && cellLinkLost(b, chunk, (size_t)r)) {
            if (!cellRelink(b)) {
                break;
            }
            continue;
        }
        if (r > 0 && b->ntrip && !b->ntripStreaming) {
            skip = b->ntripFailed ? r : ntripHeader(b, chunk, r);
            if (skip < 0) {
                b->ntripFailed = true;
                skip = r;
            } else if (b->ntripStreaming) {
                ucxapp_print("NTRIP: streaming corrections from /%s\n", b->ntripMount);
                b->nextGga = 0;
            }
        }
        if (r > skip) {
            toReceiver(b, chunk + skip, (size_t)(r - skip));
        } else if (r < 0) {
            ucxapp_platform_sleep_ms(20);
        }
    }
}

static bool hasBle(const ucxapp_stream_t *s)
{
    for (int i = 0; i < MAX_BRIDGES; i++) {
        const bridge_t *b = s->br[i];
        if (b != NULL && (b->bridge == BR_BLE || b->bridge == BR_SPS || b->ntripBle)) {
            return true;
        }
    }
    return false;
}

static bool hasNtrip(const ucxapp_stream_t *s)
{
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] != NULL && s->br[i]->ntrip) {
            ucxapp_print("ERROR: an NTRIP bridge is already running on the %s (bridge %d; %s.bridge off %d first)\n",
                         s->cfg.kind, i + 1, s->cfg.kind, i + 1);
            return true;
        }
    }
    return false;
}

/* A new bridge in the first free slot of the stream; the reader fills its ring from now on. */
static bridge_t *bridgeBegin(ucxapp_stream_t *s, ucxapp_t *app, int kind)
{
    bridge_t *b;
    int slot = -1;

    if (kind != BR_UART && app->ucx == NULL) {
        ucxapp_print("ERROR: no module connected\n");
        return NULL;
    }
    for (int i = MAX_BRIDGES - 1; i >= 0; i--) {
        if (s->br[i] == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        ucxapp_print("ERROR: %d bridges are already running on the %s (%s.bridge off <n> first)\n", MAX_BRIDGES,
                     s->cfg.kind, s->cfg.kind);
        return NULL;
    }
    if ((kind == BR_BLE || kind == BR_SPS) && hasBle(s)) {
        ucxapp_print("ERROR: a BLE bridge is already running on the %s (%s.bridge status, then %s.bridge off <n>)\n", s->cfg.kind,
                     s->cfg.kind, s->cfg.kind);
        return NULL;
    }
    b = calloc(1, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->ring = malloc(RING_SIZE);
    if (b->ring == NULL) {
        free(b);
        return NULL;
    }
    b->s = s;
    b->app = app;
    b->bridge = kind;
    b->bleChunk = BLE_CHUNK;
    b->listenSock = -1;
    b->udpSock = -1;
    b->spsConn = -1;
    regLock();
    ucxapp_stream_lock(s);
    s->br[slot] = b;
    ucxapp_stream_unlock(s);
    regUnlock();
    return b;
}

/* Stops the sender, closes everything the bridge owns, then frees its slot. */
static void bridgeStop(bridge_t *b)
{
    ucxapp_stream_t *s = b->s;
    uCxHandle_t *ucx = b->app != NULL ? b->app->ucx : NULL;

    if (b->sender != NULL) {
        b->senderStop = true;
        ucxapp_thread_join(b->sender);
        b->sender = NULL;
    }
    if (b->out != NULL) {
        ucxapp_uart_close(b->out);
        b->out = NULL;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ucxapp_tls_close(b->tls[i], ucx != NULL && i < b->nClients);
        b->tls[i] = NULL;
    }
    ucxapp_tls_server_free(b->tlsSrv);
    b->tlsSrv = NULL;
    if (ucx != NULL && b->bridge != BR_UART) {
        for (int i = 0; i < b->nClients; i++) {
            (void)uCxSocketClose(ucx, b->clients[i]);
        }
        if (b->listenSock >= 0) {
            (void)uCxSocketClose(ucx, b->listenSock);
        }
        if (b->udpSock >= 0) {
            (void)uCxSocketClose(ucx, b->udpSock);
        }
    }
    if (ucx != NULL && b->bridge == BR_MQTT && b->mqttUp) {
        (void)uCxMqttDisconnect(ucx, MQTT_ID);
    }
    free(b->mqttBuf);
    b->mqttBuf = NULL;
    free(b->corr);
    b->corr = NULL;
    memset(b->ntripReq, 0, sizeof(b->ntripReq));
    regLock();
    ucxapp_stream_lock(s);
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] == b) {
            s->br[i] = NULL;
        }
    }
    ucxapp_stream_unlock(s);
    regUnlock();
    free(b->ring);
    free(b);
}

static bool bridgeRun(bridge_t *b)
{
    b->senderStop = false;
    b->sender = ucxapp_thread_start(b->bridge == BR_UART ? uartBridgeLoop : senderLoop, b);
    if (b->sender == NULL) {
        bridgeStop(b);
        return false;
    }
    return true;
}

/* Owns tlsSrv: it is freed when the bridge does not start. */
static bool bridgeTcp(ucxapp_stream_t *s, ucxapp_t *app, int32_t port, ucxapp_tls_server_t *tlsSrv)
{
    char ssid[64];
    char ip[48];
    bridge_t *b = bridgeBegin(s, app, BR_TCP);

    if (b == NULL) {
        ucxapp_tls_server_free(tlsSrv);
        return false;
    }
    b->tlsSrv = tlsSrv;   /* set before the sender starts so no client is ever served in clear text */
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_TCP, &b->listenSock) != 0 ||
        uCxSocketListen2(app->ucx, b->listenSock, port) != 0) {
        ucxapp_print("ERROR: cannot listen on port %d (is Wi-Fi connected?)\n", (int)port);
        if (b->listenSock >= 0) {
            (void)uCxSocketClose(app->ucx, b->listenSock);
        }
        b->listenSock = -1;
        bridgeStop(b);
        return false;
    }
    b->tcpPort = port;
    if (!bridgeRun(b)) {
        return false;
    }
    if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("%s bridge: connect a TCP client to %s:%d\n", s->cfg.kind, ip, (int)port);
    } else {
        ucxapp_print("%s bridge: listening on TCP port %d\n", s->cfg.kind, (int)port);
    }
    return true;
}

bool ucxapp_stream_bridge_tcp(ucxapp_stream_t *s, ucxapp_t *app, int32_t port)
{
    return bridgeTcp(s, app, port, NULL);
}

bool ucxapp_stream_bridge_tls(ucxapp_stream_t *s, ucxapp_t *app, int32_t port, const char *certName, bool clientAuth)
{
    char err[160] = "";
    ucxapp_tls_server_t *srv;

    srv = ucxapp_tls_server_new(certName, clientAuth, err, sizeof(err));
    if (srv == NULL) {
        ucxapp_print("ERROR: TLS certificate '%s': %s (see utils.tls.ca.create / utils.tls.server.create)\n", certName, err);
        return false;
    }
    if (!bridgeTcp(s, app, port, srv)) {
        return false;
    }
    ucxapp_print("TLS with certificate '%s'%s\n", certName, clientAuth ? ", client certificate required" : "");
    return true;
}

bool ucxapp_stream_bridge_tcp_connect(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port)
{
    bridge_t *b;

    if (strlen(host) >= sizeof(b->connectHost)) {
        ucxapp_print("ERROR: host name too long\n");
        return false;
    }
    b = bridgeBegin(s, app, BR_TCP);
    if (b == NULL) {
        return false;
    }
    snprintf(b->connectHost, sizeof(b->connectHost), "%s", host);
    b->connectPort = port;
    b->tcpPort = port;
    if (!connectOnce(b)) {
        ucxapp_print("ERROR: cannot connect to %s:%d (is Wi-Fi connected?)\n", host, (int)port);
        bridgeStop(b);
        return false;
    }
    b->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
    if (!bridgeRun(b)) {
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

/* Request and BLE side of an NTRIP client, after bridgeBegin; stops the bridge on failure. */
static bool ntripSetup(bridge_t *b, ucxapp_t *app, const char *mount, const char *user, const char *pass,
                       int32_t ggaSeconds, bool ble)
{
    char cred[200];
    char auth[280] = "";

    if (ble && app->ucx == NULL) {
        ucxapp_print("ERROR: ble needs a connected module\n");
    } else if (ble && hasBle(b->s)) {
        ucxapp_print("ERROR: a BLE bridge is already running on the %s (%s.bridge status, then %s.bridge off <n>)\n",
                     b->s->cfg.kind, b->s->cfg.kind, b->s->cfg.kind);
        bridgeStop(b);
        return false;
    }
    if (ble && (app->ucx == NULL || ucxapp_gatt_server_ensure(app, "nus") != UCXAPP_OK)) {
        bridgeStop(b);
        return false;
    }
    b->ntripBle = ble;
    if (ble) {
        ucxapp_gatt_ensure_advertising(app);
    }
    if (user[0] != '\0') {
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);
        base64(cred, auth, sizeof(auth));
        memset(cred, 0, sizeof(cred));
    }
    snprintf(b->ntripMount, sizeof(b->ntripMount), "%s", mount[0] == '/' ? mount + 1 : mount);
    snprintf(b->ntripReq, sizeof(b->ntripReq),
             "GET /%s HTTP/1.0\r\nUser-Agent: NTRIP ucxapp\r\nAccept: */*\r\n%s%s%sConnection: close\r\n\r\n",
             b->ntripMount, auth[0] ? "Authorization: Basic " : "", auth, auth[0] ? "\r\n" : "");
    memset(auth, 0, sizeof(auth));
    b->ntrip = true;
    b->ggaPeriodMs = (uint32_t)ggaSeconds * 1000u;
    b->corr = malloc(sizeof(*b->corr));
    if (b->corr == NULL) {
        bridgeStop(b);
        return false;
    }
    ucxapp_corr_reset(b->corr);
    return true;
}

static bool ntripArgsOk(const ucxapp_stream_t *s, const char *host, const char *mount, const char *user,
                        const char *pass)
{
    if (strlen(host) >= sizeof(((bridge_t *)0)->connectHost) ||
        strlen(mount) >= sizeof(((bridge_t *)0)->ntripMount) || strlen(user) + strlen(pass) + 2 > 200) {
        ucxapp_print("ERROR: host, mountpoint or credentials too long\n");
        return false;
    }
    return !hasNtrip(s);
}

bool ucxapp_stream_bridge_ntrip(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, const char *mount,
                                const char *user, const char *pass, int32_t ggaSeconds, bool ble)
{
    bridge_t *b;

    if (!ntripArgsOk(s, host, mount, user, pass) || (b = bridgeBegin(s, app, BR_TCP)) == NULL ||
        !ntripSetup(b, app, mount, user, pass, ggaSeconds, ble)) {
        return false;
    }
    snprintf(b->connectHost, sizeof(b->connectHost), "%s", host);
    b->connectPort = port;
    b->tcpPort = port;
    if (!connectOnce(b)) {
        ucxapp_print("ERROR: cannot connect to %s:%d (is Wi-Fi connected?)\n", host, (int)port);
        bridgeStop(b);
        return false;
    }
    b->nextRetry = ucxapp_platform_tick_ms() + RECONNECT_MS;
    if (!bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: NTRIP client to %s:%d/%s, GGA every %d s, corrections go to the receiver%s\n", s->cfg.kind,
                 host, (int)port, b->ntripMount, (int)ggaSeconds,
                 ble ? "; corrected NMEA to BLE Nordic UART clients" : "");
    return true;
}

/* True when any open stream already runs an MQTT bridge (the module has one MQTT connection). */
static bool hasMqtt(void)
{
    bool found = false;

    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            found = found || (gStreams[i]->br[j] != NULL && gStreams[i]->br[j]->bridge == BR_MQTT);
        }
    }
    regUnlock();
    return found;
}

bool ucxapp_stream_bridge_mqtt(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, const char *clientId,
                               const char *certName, const char *const *topics, int nTopics)
{
    bridge_t *b;

    if (strlen(host) >= sizeof(b->connectHost) || strlen(clientId) >= sizeof(b->mqttClient) ||
        strlen(certName) >= sizeof(b->mqttCert) || nTopics < 1 || nTopics > MQTT_TOPICS) {
        ucxapp_print("ERROR: host, client ID or certificate name too long\n");
        return false;
    }
    for (int i = 0; i < nTopics; i++) {
        if (strlen(topics[i]) >= sizeof(b->mqttTopic[0])) {
            ucxapp_print("ERROR: topic too long\n");
            return false;
        }
    }
    if (hasMqtt()) {
        ucxapp_print("ERROR: an MQTT bridge is already running (the module has one MQTT connection)\n");
        return false;
    }
    b = bridgeBegin(s, app, BR_MQTT);
    if (b == NULL) {
        return false;
    }
    b->mqttBuf = malloc(MQTT_BUF);
    b->corr = malloc(sizeof(*b->corr));
    if (b->mqttBuf == NULL || b->corr == NULL) {
        bridgeStop(b);
        return false;
    }
    ucxapp_corr_reset(b->corr);
    snprintf(b->connectHost, sizeof(b->connectHost), "%s", host);
    b->connectPort = port;
    snprintf(b->mqttClient, sizeof(b->mqttClient), "%s", clientId);
    snprintf(b->mqttCert, sizeof(b->mqttCert), "%s", certName);
    for (int i = 0; i < nTopics; i++) {
        snprintf(b->mqttTopic[i], sizeof(b->mqttTopic[i]), "%s", topics[i]);
    }
    b->nTopics = nTopics;
    if (!mqttConnect(b, true)) {
        bridgeStop(b);
        return false;
    }
    if (!bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: MQTT %s:%d as %s, messages on", s->cfg.kind, host, (int)port, clientId);
    for (int i = 0; i < nTopics; i++) {
        ucxapp_print(" %s", b->mqttTopic[i]);
    }
    ucxapp_print(" go to the receiver\n");
    return true;
}

bool ucxapp_stream_bridge_udp(ucxapp_stream_t *s, ucxapp_t *app, const char *host, int32_t port, int32_t local)
{
    int32_t bindPort = local > 0 ? local : port;
    bridge_t *b = bridgeBegin(s, app, BR_UDP);

    if (b == NULL) {
        return false;
    }
    b->udpListen = (host == NULL);
    if (host != NULL && uCxStringToIpAddress(host, &b->addr) != 0 &&
        uCxSocketGetHostByName(app->ucx, host, &b->addr) != 0) {
        ucxapp_print("ERROR: cannot resolve %s\n", host);
        bridgeStop(b);
        return false;
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &b->udpSock) != 0) {
        ucxapp_print("ERROR: cannot create UDP socket (is Wi-Fi connected?)\n");
        b->udpSock = -1;
        bridgeStop(b);
        return false;
    }
    if (uCxSocketBind(app->ucx, b->udpSock, bindPort) != 0) {
        if (local > 0 || host == NULL) {
            ucxapp_print("ERROR: cannot bind UDP port %d\n", (int)bindPort);
            bridgeStop(b);
            return false;
        }
        ucxapp_print("note: UDP port %d is busy, receiving is off\n", (int)bindPort);
    }
    b->udpPort = host != NULL ? port : 0;
    if (!bridgeRun(b)) {
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
    bridge_t *b = bridgeBegin(s, app, BR_SPS);

    if (b == NULL) {
        return false;
    }
    if (uCxSpsSetServiceEnable(app->ucx, U_SPS_SERVICE_OPTION_ENABLE) != 0 ||
        uCxSpsSetDataMode(app->ucx, U_READ_MODE_BUFFERED) != 0) {
        ucxapp_print("ERROR: cannot enable the SPS service\n");
        bridgeStop(b);
        return false;
    }
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the u-blox Serial Port Service\n", s->cfg.kind);
    return true;
}

bool ucxapp_stream_bridge_ble(ucxapp_stream_t *s, ucxapp_t *app)
{
    bridge_t *b = bridgeBegin(s, app, BR_BLE);

    if (b == NULL) {
        return false;
    }
    if (ucxapp_gatt_server_ensure(app, "nus") != UCXAPP_OK) {
        bridgeStop(b);
        return false;
    }
    b->bleCustom = false;
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the Nordic UART service and enable notifications\n", s->cfg.kind);
    return true;
}

bool ucxapp_stream_bridge_ble_custom(ucxapp_stream_t *s, ucxapp_t *app, const uint8_t svc[16], const uint8_t rx[16],
                                     const uint8_t tx[16])
{
    bridge_t *b = bridgeBegin(s, app, BR_BLE);

    if (b == NULL) {
        return false;
    }
    if (ucxapp_gatt_server_custom(app, svc, rx, tx) != UCXAPP_OK) {
        bridgeStop(b);
        return false;
    }
    b->bleCustom = true;
    ucxapp_gatt_ensure_advertising(app);
    if (!bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: connect a BLE client to the custom service, write to the RX and enable TX notifications\n",
                 s->cfg.kind);
    return true;
}

static bridge_t *uartOpen(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud)
{
    bridge_t *b;

    if (strcmp(port, s->port) == 0) {
        ucxapp_print("ERROR: that is the port of the %s itself\n", s->cfg.kind);
        return NULL;
    }
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] != NULL && s->br[i]->bridge == BR_UART && strcmp(s->br[i]->outPort, port) == 0) {
            ucxapp_print("ERROR: bridge %d already uses %s\n", i + 1, port);
            return NULL;
        }
    }
    b = bridgeBegin(s, app, BR_UART);
    if (b == NULL) {
        return NULL;
    }
    b->out = ucxapp_uart_open(port, baud);
    if (b->out == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", port);
        bridgeStop(b);
        return NULL;
    }
    snprintf(b->outPort, sizeof(b->outPort), "%s", port);
    b->outBaud = baud;
    {
        char cport[32];
        int32_t cbaud;
        b->cellLink = ucxapp_cell_data_link(cport, sizeof(cport), &cbaud) && strcmp(cport, port) == 0;
    }
    return b;
}

bool ucxapp_stream_bridge_uart(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud)
{
    bridge_t *b = uartOpen(s, app, port, baud);

    if (b == NULL || !bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: %s <-> %s at %d baud\n", s->cfg.kind, s->port, port, (int)baud);
    return true;
}

bool ucxapp_stream_bridge_ntrip_uart(ucxapp_stream_t *s, ucxapp_t *app, const char *port, int32_t baud,
                                     const char *mount, const char *user, const char *pass, int32_t ggaSeconds,
                                     bool ble)
{
    bridge_t *b;

    if (!ntripArgsOk(s, "", mount, user, pass) || (b = uartOpen(s, app, port, baud)) == NULL ||
        !ntripSetup(b, app, mount, user, pass, ggaSeconds, ble) || !bridgeRun(b)) {
        return false;
    }
    ucxapp_print("%s bridge: NTRIP client over %s to /%s, GGA every %d s, corrections go to the receiver%s\n",
                 s->cfg.kind, port, b->ntripMount, (int)ggaSeconds,
                 ble ? "; corrected NMEA to BLE Nordic UART clients" : "");
    return true;
}

void ucxapp_stream_bridge_stop(ucxapp_stream_t *s)
{
    for (int i = MAX_BRIDGES - 1; i >= 0; i--) {
        if (s->br[i] != NULL) {
            bridgeStop(s->br[i]);
        }
    }
}

bool ucxapp_stream_bridge_stop_one(ucxapp_stream_t *s, int n)
{
    if (n < 1 || n > MAX_BRIDGES || s->br[n - 1] == NULL) {
        return false;
    }
    bridgeStop(s->br[n - 1]);
    return true;
}

int ucxapp_stream_bridge_count(const ucxapp_stream_t *s)
{
    int n = 0;

    for (int i = 0; i < MAX_BRIDGES; i++) {
        n += s->br[i] != NULL ? 1 : 0;
    }
    return n;
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
    return ucxapp_stream_bridge_count(s) > 0;
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

int ucxapp_stream_bridge_stop_port(const char *port)
{
    ucxapp_stream_t *s[MAX_STREAMS];
    int n = openStreams(s);
    int stopped = 0;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < MAX_BRIDGES; j++) {
            if (s[i]->br[j] != NULL && strcmp(s[i]->br[j]->outPort, port) == 0) {
                ucxapp_print("stopping %s.bridge %d (it uses %s)\n", s[i]->cfg.kind, j + 1, port);
                bridgeStop(s[i]->br[j]);
                stopped++;
            }
        }
    }
    return stopped;
}

const char *ucxapp_stream_open_port(const char *kind)
{
    static char port[32];

    port[0] = '\0';
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (gStreams[i] != NULL && gStreams[i]->cfg.kind != NULL && strcmp(gStreams[i]->cfg.kind, kind) == 0) {
            snprintf(port, sizeof(port), "%s", gStreams[i]->port);
            break;
        }
    }
    regUnlock();
    return port[0] != '\0' ? port : NULL;
}

size_t ucxapp_stream_cell_users(char *out, size_t size)
{
    static const char *const kName[] = { "off", "TCP", "UDP", "BLE", "UART", "BLE SPS", "MQTT" };
    size_t n = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        const ucxapp_stream_t *s = gStreams[i];
        bool relay = s != NULL && s->cfg.kind != NULL && strcmp(s->cfg.kind, "cell") == 0;
        for (int j = 0; s != NULL && j < MAX_BRIDGES && n + 1 < size; j++) {
            const bridge_t *b = s->br[j];
            int w;
            if (b == NULL || (!relay && !b->cellLink)) {
                continue;
            }
            if (relay) {
                w = snprintf(out + n, size - n, "RELAY cell.bridge %d %s%s%s, %u B to it, %u B back\n", j + 1,
                             b->bridge == BR_TCP && b->connectHost[0] ? "TCP client " : kName[b->bridge],
                             b->bridge == BR_TCP && b->connectHost[0] ? b->connectHost : "",
                             b->bridge == BR_TCP && !b->connectHost[0] ? (b->nClients > 0 ? " (client on)" : " (no client)")
                                                                      : "",
                             (unsigned)b->sent, (unsigned)b->received);
            } else {
                w = snprintf(out + n, size - n, "USER  %s.bridge %d%s, %u B out, %u B in, %u reconnect(s)%s\n",
                             s->cfg.kind != NULL ? s->cfg.kind : "?", j + 1, b->ntrip ? " NTRIP" : "", (unsigned)b->sent,
                             (unsigned)b->received, (unsigned)b->relinks, b->out == NULL ? ", reconnecting" : "");
            }
            if (w < 0) {
                break;
            }
            n += (size_t)w < size - n ? (size_t)w : size - n - 1;
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
        ucxapp_stream_bridge_status(s[i], "                   ");
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
        stopped += ucxapp_stream_bridge_count(s[i]);
        ucxapp_stream_bridge_stop(s[i]);
    }
    ucxapp_print("%d bridge(s) stopped\n", stopped);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kBridgeCmds[] = {
    { "bridge.list", "", "Every open source (gnss, imu, cell, can, hw) and where its bridges go", 0, cmdBridgeList },
    { "bridge.off", "", "Stop every running bridge", 0, cmdBridgeOff },
};

void ucxapp_register_bridge_cmds(void)
{
    (void)ucxapp_cmd_register(kBridgeCmds, sizeof(kBridgeCmds) / sizeof(kBridgeCmds[0]));
}

static void bridgeDescribe(const bridge_t *b)
{
    static const char *const kName[] = { "off", "TCP server", "UDP", "BLE Nordic UART", "UART", "BLE SPS", "MQTT" };
    char frames[160] = "";

    if (b->corr != NULL) {
        frames[0] = ' ';
        frames[1] = '(';
        ucxapp_corr_describe(b->corr, frames + 2, sizeof(frames) - 3);
        strcat(frames, ")");
    }
    if (b->bridge == BR_MQTT) {
        ucxapp_print("MQTT %s:%d, %s, %u messages (%u bytes) to the receiver%s\n", b->connectHost, (int)b->connectPort,
                     b->mqttUp ? "connected" : "reconnecting", (unsigned)b->mqttMsgs, (unsigned)b->received, frames);
        return;
    }
    if (b->ntrip) {
        char where[100];

        /* right after the start the caster has usually not answered yet: give it a moment */
        for (int ms = 0; ms < 5000 && !b->ntripStreaming && !b->ntripFailed && !b->senderStop; ms += 20) {
            ucxapp_platform_sleep_ms(20);
        }
        if (b->bridge == BR_UART) {
            snprintf(where, sizeof(where), "over %s", b->outPort);
        } else {
            snprintf(where, sizeof(where), "%s:%d", b->connectHost, (int)b->connectPort);
        }
        ucxapp_print("NTRIP %s/%s, %s, %u bytes of corrections%s\n", where,
                     b->ntripMount, b->ntripFailed ? "refused by the caster"
                                    : b->bridge == BR_TCP && b->nClients == 0 ? "reconnecting"
                                    : !b->ntripStreaming ? "waiting for the caster"
                                    : b->gga[0] == '\0' ? "streaming, no GGA from the receiver yet" : "streaming",
                     (unsigned)b->received, frames);
        return;
    }
    ucxapp_print("%s", b->bridge == BR_TCP && b->connectHost[0] ? "TCP client"
                       : b->bridge == BR_BLE && b->bleCustom ? "BLE custom service" : kName[b->bridge]);
    if (b->bridge == BR_TCP && b->connectHost[0]) {
        ucxapp_print(" %s:%d, %s", b->connectHost, (int)b->connectPort, b->nClients > 0 ? "connected" : "reconnecting");
    } else if (b->bridge == BR_TCP) {
        ucxapp_print("%s port %d, %d client(s)", b->tlsSrv != NULL ? " (TLS)" : "", (int)b->tcpPort, b->nClients);
        U_CX_MUTEX_LOCK(gClientLock);
        for (int c = 0; c < b->nClients; c++) {
            if (b->tls[c] != NULL && ucxapp_tls_peer(b->tls[c])[0] != '\0') {
                ucxapp_print(" [%s]", ucxapp_tls_peer(b->tls[c]));
            }
        }
        U_CX_MUTEX_UNLOCK(gClientLock);
    } else if (b->bridge == BR_UDP) {
        ucxapp_print(" port %d", (int)b->udpPort);
    } else if (b->bridge == BR_UART) {
        ucxapp_print(" %s at %d baud", b->outPort, (int)b->outBaud);
        if (b->cellLink) {
            ucxapp_print(", cellular, %s%u reconnect(s)", b->out == NULL ? "reconnecting, " : "", (unsigned)b->relinks);
        }
    } else {
        ucxapp_print(", %s", ucxapp_handle_current(UCXAPP_HANDLE_CONN) < 0 ? "no client connected" : "client connected");
    }
    ucxapp_print(", %u bytes out, %u in, %u dropped\n", (unsigned)b->sent, (unsigned)b->received,
                 (unsigned)b->dropped);
}

void ucxapp_stream_bridge_status(ucxapp_stream_t *s, const char *indent)
{
    bool first = true;

    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] != NULL) {
            ucxapp_print("%sbridge %d: ", first ? "" : indent, i + 1);
            bridgeDescribe(s->br[i]);
            first = false;
        }
    }
    if (first) {
        ucxapp_print("bridge: off\n");
    }
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

/* bridge subcommand shared by gnss, imu, cell, can and hw: argv[1] is tcp|udp|ble|uart|off|status. */
static int bridgeCmd(ucxapp_stream_t *s, ucxapp_t *app, int argc, char *argv[], int32_t defaultPort)
{
    int32_t port = defaultPort;
    int32_t baud = 115200;

    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "status") == 0) {
        ucxapp_stream_bridge_status(s, "");
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0 && argc == 3) {
        int32_t n;
        if (!ucxapp_parse_int(argv[2], &n)) {
            return UCXAPP_ERR_USAGE;
        }
        if (!ucxapp_stream_bridge_stop_one(s, (int)n)) {
            ucxapp_print("ERROR: no bridge %d on the %s (see %s.bridge status)\n", (int)n, s->cfg.kind, s->cfg.kind);
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("bridge %d stopped\n", (int)n);
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0 && argc == 2) {
        int n = ucxapp_stream_bridge_count(s);
        ucxapp_stream_bridge_stop(s);
        if (n > 1) {
            ucxapp_print("%d bridges stopped\n", n);
        } else {
            ucxapp_print("bridge stopped\n");
        }
        return UCXAPP_OK;
    }
    /* tcps/udpc say which side the module is; "tcp" and "udp" are their old names, kept for scripts */
    if ((strcasecmp(argv[1], "tcps") == 0 || strcasecmp(argv[1], "tcp") == 0) && argc <= 3) {
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
    if ((strcasecmp(argv[1], "udpc") == 0 || strcasecmp(argv[1], "udp") == 0) && (argc == 4 || argc == 5)) {
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
            ucxapp_print(ucxapp_cell_relayed()
                             ? "ERROR: the cellular data link is relayed by cell.bridge (cell.bridge off first)\n"
                             : "ERROR: no cellular data link (use cell.connect first)\n");
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

/* Starting the same bridge twice (a script or example run again) reports the running one instead of taking a slot. */
int ucxapp_stream_bridge_cmd(ucxapp_stream_t *s, ucxapp_t *app, int argc, char *argv[], int32_t defaultPort)
{
    char key[sizeof(((bridge_t *)0)->args)];
    bridge_t *before[MAX_BRIDGES];
    size_t n = 0;
    int rc;

    key[0] = '\0';
    for (int i = 1; i < argc && n < sizeof(key); i++) {
        int w = snprintf(key + n, sizeof(key) - n, i > 1 ? " %s" : "%s", argv[i]);
        n += w > 0 ? (size_t)w : 0;
    }
    if (argc < 2 || strcasecmp(argv[1], "status") == 0 || strcasecmp(argv[1], "off") == 0 || n >= sizeof(key)) {
        return bridgeCmd(s, app, argc, argv, defaultPort);
    }
    ucxapp_stream_lock(s);
    for (int i = 0; i < MAX_BRIDGES; i++) {
        before[i] = s->br[i];
        if (s->br[i] != NULL && strcasecmp(s->br[i]->args, key) == 0) {
            ucxapp_stream_unlock(s);
            ucxapp_print("%s.bridge %s is already running as bridge %d\n", s->cfg.kind, key, i + 1);
            return UCXAPP_OK;
        }
    }
    ucxapp_stream_unlock(s);
    rc = bridgeCmd(s, app, argc, argv, defaultPort);
    ucxapp_stream_lock(s);
    for (int i = 0; i < MAX_BRIDGES; i++) {
        if (s->br[i] != NULL && s->br[i] != before[i]) {
            snprintf(s->br[i]->args, sizeof(s->br[i]->args), "%s", key);
        }
    }
    ucxapp_stream_unlock(s);
    return rc;
}

/* ---- URC hooks ---- */

/* Module has data on a socket: flag the bridge that owns it. */
void ucxapp_stream_on_data(int32_t sock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            bridge_t *b = gStreams[i]->br[j];
            if (b != NULL && (b->bridge == BR_TCP || (b->bridge == BR_UDP && b->udpSock == sock))) {
                b->rxPending = true;
            }
        }
    }
    regUnlock();
}

/* A BLE client wrote to the Nordic UART RX characteristic. */
void ucxapp_stream_on_ble_rx(const uint8_t *data, size_t len)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            bridge_t *b = gStreams[i]->br[j];
            if (b != NULL && b->bridge == BR_BLE) {
                (void)ucxapp_stream_write(gStreams[i], data, len);
                b->received += (uint32_t)len;
            }
        }
    }
    regUnlock();
}

void ucxapp_stream_on_sps_data(void)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            bridge_t *b = gStreams[i]->br[j];
            if (b != NULL && b->bridge == BR_SPS) {
                b->rxPending = true;
            }
        }
    }
    regUnlock();
}

void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            bridge_t *b = gStreams[i]->br[j];
            if (b != NULL && b->bridge == BR_TCP && b->listenSock == listenSock && b->nAccepted < EVT_QUEUE) {
                b->accepted[b->nAccepted++] = sock;
            }
        }
    }
    regUnlock();
}

void ucxapp_stream_on_closed(int32_t sock)
{
    regLock();
    for (int i = 0; i < MAX_STREAMS; i++) {
        for (int j = 0; gStreams[i] != NULL && j < MAX_BRIDGES; j++) {
            bridge_t *b = gStreams[i]->br[j];
            if (b != NULL && b->bridge == BR_TCP && b->nClosed < EVT_QUEUE) {
                b->closed[b->nClosed++] = sock;
            }
        }
    }
    regUnlock();
}
