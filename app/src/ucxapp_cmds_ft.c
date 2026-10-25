/* ft.* - file transfer over a custom BLE GATT service or a module TCP socket, TFTP-like. The server runs on its own
 * thread and serves the working folder; uploads and deletes only with 'rw'. Every file carries a CRC-32 (IEEE 802.3)
 * checked end to end. Host builds only.
 *
 * Service 7a3f0001-5c1e-4d2b-9f6a-3e8b2c4d5e6f
 *   CTRL 7a3f0002-...  Write, Write Without Response, Notify  (requests and replies, little endian)
 *   DATA 7a3f0003-...  Write Without Response, Notify          ([block u16][payload])
 * Over TCP (default port 6970) the same packets are framed as [len u16][channel u8: 0 CTRL, 1 DATA][packet].
 * Client -> server CTRL: 01 GET [blk u16][name], 02 PUT [blk u16][size u32][crc u32][name], 03 LIST, 04 DEL [name],
 *                        05 ACK [next u16], 06 ABORT, 07 GET from [blk u16][offset u32][name],
 *                        08 PUT resume [blk u16][size u32][crc u32][name]
 * Server -> client CTRL: 81 [status][size u32][crc u32], 82 [status], 83 [status][size u32][name] (status FF ends the
 *                        list), 84 [status], 85 ACK [next u16], 86 error [status], 87 PUT verified [status],
 *                        88 [status][offset u32] (bytes the server already has in <name>.part)
 * A resumed transfer numbers its blocks from the offset; the CRC-32 still covers the whole file.
 * Blocks are sent go-back-N with a window of 8; the receiver acks every 8 blocks, the last block and on idle. A block
 * shorter than blk (or empty) ends the file. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "ucxapp_platform.h"
#include "ucxapp_thread.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_gatt_client.h"
#include "u_cx_gatt_server.h"
#include "u_cx_socket.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define PROP_WRITE_NR 0x04
#define PROP_WRITE    0x08
#define PROP_NOTIFY   0x10

#define OP_GET    0x01
#define OP_PUT    0x02
#define OP_LIST   0x03
#define OP_DEL    0x04
#define OP_ACK    0x05
#define OP_ABORT  0x06
#define OP_GETR   0x07
#define OP_PUTR   0x08
#define RSP       0x80
#define OP_ERROR  0x86
#define OP_PUTOK  0x87

#define ST_OK        0
#define ST_NOT_FOUND 1
#define ST_ACCESS    2
#define ST_DISK_FULL 3
#define ST_ILLEGAL   4
#define ST_BUSY      5
#define ST_CRC       6
#define ST_END       0xFF

#define PKT_MAX      244               /* ATT MTU 247 - 3 */
#define BLK_MAX      (PKT_MAX - 2)
#define BLK_MIN      16
#define BLK_DEFAULT  240
#define WINDOW       8
#define ACK_WAIT_MS  2000
#define TRIES        5
#define IDLE_ACK_MS  500
#define RECV_IDLE_MS 10000
#define LINGER_MS    1000
#define REPLY_MS     5000
#define NAME_MAX_LEN 128
#define FILE_MAX     (64L * 1024 * 1024)
#define QUEUE_LEN    32
#define TCP_PORT     6970
#define TCP_HDR      3
#define TCP_PKT_MAX  (1460 - TCP_HDR)  /* one TCP segment per module socket write */
#define TCP_BLK_MAX  (TCP_PKT_MAX - 2)

typedef struct {
    int32_t conn;
    bool data;                         /* DATA characteristic, else CTRL */
    uint16_t len;
    uint8_t buf[TCP_PKT_MAX];
} pkt_t;

typedef struct {
    pkt_t p[QUEUE_LEN];
    volatile int head;                 /* written by the URC task */
    volatile int tail;
} ring_t;

typedef struct {
    volatile int32_t sock;
    volatile bool pending;             /* set by the data URC */
    volatile bool closed;
    uint8_t acc[TCP_HDR + TCP_PKT_MAX];
    size_t n;
} tcpRx_t;

typedef struct {
    ucxapp_t *app;
    int32_t conn;                      /* BLE connection or TCP socket */
    bool server;
    ring_t *rx;
    tcpRx_t *tcp;                      /* NULL over BLE */
    volatile bool *stop;
    char err[96];
} link_t;

static const uint8_t kSvc[16] = { 0x7a, 0x3f, 0x00, 0x01, 0x5c, 0x1e, 0x4d, 0x2b,
                                  0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };
static const uint8_t kCtrl[16] = { 0x7a, 0x3f, 0x00, 0x02, 0x5c, 0x1e, 0x4d, 0x2b,
                                   0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };
static const uint8_t kData[16] = { 0x7a, 0x3f, 0x00, 0x03, 0x5c, 0x1e, 0x4d, 0x2b,
                                   0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };

static ring_t gSrvQ;
static ring_t gCliQ;
static volatile bool gStop;
static volatile bool gRunning;
static volatile bool gCliStop;
static bool gDefined;
static bool gWrite;
static int32_t gSrvCtrl = -1;
static int32_t gSrvData = -1;
static volatile int32_t gCliConn = -1;
static volatile int32_t gCliCtrl = -1;
static volatile int32_t gCliData = -1;
static ucxapp_t *gApp;
static ucxapp_thread_t *gThread;
static unsigned gFiles;
static long gBytes;
static char gLast[160];
static tcpRx_t gSrvTcp = { -1 };
static tcpRx_t gCliTcp = { -1 };
static volatile int32_t gListen = -1;
static volatile int32_t gAccepted = -1;
static int32_t gPort;
static ucxapp_t *gCliApp;

/* ---- helpers ---------------------------------------------------------------------------------------------------- */

static uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n-- > 0) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Only plain names in the working folder: no paths, drives or hidden files. */
static bool validName(const char *name)
{
    size_t n = strlen(name);

    if (n == 0 || n >= NAME_MAX_LEN || name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

static const char *statusName(uint8_t st)
{
    switch (st) {
    case ST_OK:        return "ok";
    case ST_NOT_FOUND: return "file not found";
    case ST_ACCESS:    return "access denied";
    case ST_DISK_FULL: return "disk full";
    case ST_ILLEGAL:   return "illegal request";
    case ST_BUSY:      return "busy";
    case ST_CRC:       return "CRC mismatch";
    default:           return "unknown";
    }
}

static int clampBlk(int blk, int max)
{
    return blk < BLK_MIN ? BLK_MIN : blk > max ? max : blk;
}

/* Size and CRC-32 of an open file; leaves the position at the start. */
static long fileInfo(FILE *f, uint32_t *crc)
{
    uint8_t buf[512];
    long size = 0;
    size_t n;

    *crc = 0;
    fseek(f, 0, SEEK_SET);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        *crc = crc32Update(*crc, buf, n);
        size += (long)n;
    }
    fseek(f, 0, SEEK_SET);
    return size;
}

/* Size and CRC-32 of a partial file, 0 if it does not exist. */
static long partInfo(const char *path, uint32_t *crc)
{
    FILE *f = fopen(path, "rb");
    long size;

    *crc = 0;
    if (f == NULL) {
        return 0;
    }
    size = fileInfo(f, crc);
    fclose(f);
    return size;
}

static bool commitPart(const char *part, const char *name)
{
    remove(name);
    return rename(part, name) == 0;
}

/* ---- queues (filled on the URC task) ---------------------------------------------------------------------------- */

static void ringClear(ring_t *q)
{
    q->tail = q->head;
}

static void ringPush(ring_t *q, int32_t conn, bool data, const uint8_t *buf, size_t len)
{
    int next = (q->head + 1) % QUEUE_LEN;

    if (next == q->tail || len > TCP_PKT_MAX) {
        return;                        /* full: the sender times out and resends */
    }
    pkt_t *p = &q->p[q->head];
    p->conn = conn;
    p->data = data;
    p->len = (uint16_t)len;
    memcpy(p->buf, buf, len);
    q->head = next;
}

static bool ringPop(ring_t *q, pkt_t *out)
{
    if (q->tail == q->head) {
        return false;
    }
    *out = q->p[q->tail];
    q->tail = (q->tail + 1) % QUEUE_LEN;
    return true;
}

bool ucxapp_ft_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    if (!gDefined || (handle != gSrvCtrl && handle != gSrvData)) {
        return false;
    }
    if (gRunning) {
        ringPush(&gSrvQ, conn, handle == gSrvData, data, len);
    }
    return true;
}

bool ucxapp_ft_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    if (conn != gCliConn || (handle != gCliCtrl && handle != gCliData)) {
        return false;
    }
    ringPush(&gCliQ, conn, handle == gCliData, data, len);
    return true;
}

void ucxapp_ft_on_sock_data(int32_t sock)
{
    if (sock == gSrvTcp.sock) {
        gSrvTcp.pending = true;
    }
    if (sock == gCliTcp.sock) {
        gCliTcp.pending = true;
    }
}

void ucxapp_ft_on_sock_closed(int32_t sock)
{
    if (sock == gSrvTcp.sock) {
        gSrvTcp.closed = true;
    }
    if (sock == gCliTcp.sock) {
        gCliTcp.closed = true;
    }
}

void ucxapp_ft_on_sock_incoming(int32_t sock, int32_t listenSock)
{
    if (listenSock >= 0 && listenSock == gListen) {
        gAccepted = sock;
    }
}

/* Reads what the module has for the socket and queues every complete frame. */
static void tcpPump(ucxapp_t *app, tcpRx_t *t, ring_t *q)
{
    uint8_t buf[1460];
    int32_t n;

    if (t->sock < 0 || !t->pending) {
        return;
    }
    t->pending = false;
    do {
        n = uCxSocketRead(app->ucx, t->sock, (int32_t)sizeof(buf), buf);
        for (int32_t i = 0; i < n; i++) {
            t->acc[t->n++] = buf[i];
            if (t->n < TCP_HDR) {
                continue;
            }
            size_t len = get16(t->acc);
            if (len > TCP_PKT_MAX) {
                t->closed = true;      /* not an ft peer */
                t->n = 0;
            } else if (t->n == TCP_HDR + len) {
                ringPush(q, t->sock, t->acc[2] == 1, t->acc + TCP_HDR, len);
                t->n = 0;
            }
        }
    } while (n == (int32_t)sizeof(buf));
}

static bool tcpWrite(uCxHandle_t *ucx, int32_t sock, const uint8_t *p, size_t n)
{
    size_t off = 0;
    int idle = 0;

    while (off < n) {
        int32_t r = uCxSocketWrite(ucx, sock, p + off, (int32_t)(n - off));
        if (r > 0) {
            off += (size_t)r;
            idle = 0;
        } else if (r < 0 || ++idle > 1000) {
            return false;
        } else {
            ucxapp_platform_sleep_ms(1);
        }
    }
    return true;
}

/* ---- link ------------------------------------------------------------------------------------------------------- */

static bool sendPkt(link_t *l, bool data, const uint8_t *buf, size_t len)
{
    uCxHandle_t *ucx = l->app->ucx;
    int32_t r;

    if (l->tcp != NULL) {
        uint8_t f[TCP_HDR + TCP_PKT_MAX];
        put16(f, (uint16_t)len);
        f[2] = data ? 1 : 0;
        memcpy(f + TCP_HDR, buf, len);
        return tcpWrite(ucx, l->conn, f, len + TCP_HDR);
    }
    if (l->server) {
        r = uCxGattServerSendNotification(ucx, l->conn, data ? gSrvData : gSrvCtrl, buf, (int32_t)len);
    } else {
        r = uCxGattClientWriteNoRsp(ucx, l->conn, data ? gCliData : gCliCtrl, buf, (int32_t)len);
    }
    return r == 0;
}

static bool sendCtrl1(link_t *l, uint8_t op, uint8_t status)
{
    uint8_t m[2] = { op, status };
    return sendPkt(l, false, m, sizeof(m));
}

static bool stopped(const link_t *l)
{
    return (l->stop != NULL && *l->stop) || (l->tcp != NULL && l->tcp->closed);
}

static bool linkPop(link_t *l, pkt_t *out)
{
    if (l->tcp != NULL) {
        tcpPump(l->app, l->tcp, l->rx);
    }
    return ringPop(l->rx, out);
}

/* Waits for a CTRL packet with opcode op (or OP_ABORT/OP_ERROR); data packets are dropped. */
static bool waitCtrl(link_t *l, uint8_t op, uint32_t ms, pkt_t *out)
{
    uint32_t start = ucxapp_platform_tick_ms();

    while (!stopped(l) && ucxapp_platform_tick_ms() - start < ms) {
        if (!linkPop(l, out)) {
            ucxapp_platform_sleep_ms(1);
            continue;
        }
        if (out->conn != l->conn || out->data || out->len == 0) {
            continue;
        }
        if (out->buf[0] == op || out->buf[0] == OP_ABORT || out->buf[0] == OP_ERROR) {
            return true;
        }
    }
    return false;
}

/* Sends a file as numbered blocks, go-back-N. Returns the byte count or -1. */
/* Sends bytes from..size of f as blocks numbered from 0. Returns size or -1. */
static long sendFile(link_t *l, FILE *f, long size, long from, int blk)
{
    uint8_t pkt[TCP_PKT_MAX];
    uint16_t blocks = (uint16_t)((size - from) / blk + 1);
    uint16_t base = 0;
    uint16_t next = 0;
    uint8_t ackOp = l->server ? OP_ACK : (uint8_t)(RSP | OP_ACK);
    int tries = 0;

    if ((size - from) / blk + 1 > 0xFFFF) {
        snprintf(l->err, sizeof(l->err), "file too large for block size %d", blk);
        return -1;
    }
    while (base < blocks) {
        while (next < blocks && next - base < WINDOW) {
            fseek(f, from + (long)next * blk, SEEK_SET);
            size_t n = fread(pkt + 2, 1, (size_t)blk, f);
            put16(pkt, next);
            if (!sendPkt(l, true, pkt, n + 2)) {
                break;                 /* module busy: wait for the ACK or the timeout */
            }
            next++;
        }
        pkt_t in;
        if (!waitCtrl(l, ackOp, ACK_WAIT_MS, &in)) {
            if (stopped(l) || ++tries >= TRIES) {
                snprintf(l->err, sizeof(l->err), "no ACK after block %u", (unsigned)base);
                return -1;
            }
            next = base;               /* go back */
            continue;
        }
        if (in.buf[0] != ackOp) {
            snprintf(l->err, sizeof(l->err), "aborted by peer");
            return -1;
        }
        if (in.len >= 3) {
            uint16_t acked = get16(in.buf + 1);
            if (acked > base && acked <= blocks) {
                base = acked;
                tries = 0;
                if (next < base) {
                    next = base;
                }
            } else if (acked == base) {
                next = base;           /* receiver asks for a resend */
            }
        }
    }
    return size;
}

/* Appends numbered blocks to f until a short block. have is what f already holds and *crc its CRC-32 (0 if empty);
   returns the file size or -1 and leaves the CRC of the whole file in *crc. */
static long recvFile(link_t *l, FILE *f, int blk, long have, long expect, uint32_t *crc)
{
    uint8_t ackOp = l->server ? (uint8_t)(RSP | OP_ACK) : OP_ACK;
    uint8_t ack[3] = { ackOp, 0, 0 };
    uint16_t want = 0;
    uint16_t sinceAck = 0;
    long total = have;
    bool done = false;
    uint32_t last = ucxapp_platform_tick_ms();
    uint32_t doneAt = 0;

    for (;;) {
        pkt_t in;
        uint32_t now = ucxapp_platform_tick_ms();
        if (stopped(l)) {
            snprintf(l->err, sizeof(l->err), "stopped");
            return -1;
        }
        if (done && now - doneAt >= LINGER_MS) {
            return total;
        }
        if (!linkPop(l, &in)) {
            if (!done && now - last >= RECV_IDLE_MS) {
                snprintf(l->err, sizeof(l->err), "timeout at block %u", (unsigned)want);
                return -1;
            }
            if (!done && sinceAck > 0 && now - last >= IDLE_ACK_MS) {
                put16(ack + 1, want);
                sendPkt(l, false, ack, sizeof(ack));
                sinceAck = 0;
                last = now;
            } else if (!done && sinceAck == 0 && now - last >= ACK_WAIT_MS) {
                put16(ack + 1, want);  /* nothing came: re-ack so the sender resends */
                sendPkt(l, false, ack, sizeof(ack));
                last += ACK_WAIT_MS / 2;
            }
            ucxapp_platform_sleep_ms(1);
            continue;
        }
        if (in.conn != l->conn) {
            continue;
        }
        if (!in.data) {
            if (in.len > 0 && in.buf[0] == (l->server ? OP_ABORT : OP_ERROR)) {
                snprintf(l->err, sizeof(l->err), "aborted by peer");
                return -1;
            }
            continue;
        }
        if (in.len < 2) {
            continue;
        }
        last = now;
        uint16_t b = get16(in.buf);
        size_t n = (size_t)in.len - 2;
        if (b != want || done) {
            put16(ack + 1, want);      /* duplicate or gap: tell the sender where to resume */
            sendPkt(l, false, ack, sizeof(ack));
            continue;
        }
        if (n > 0 && fwrite(in.buf + 2, 1, n, f) != n) {
            sendCtrl1(l, l->server ? OP_ERROR : OP_ABORT, ST_DISK_FULL);
            snprintf(l->err, sizeof(l->err), "write failed (disk full?)");
            return -1;
        }
        *crc = crc32Update(*crc, in.buf + 2, n);
        total += (long)n;
        if (total > FILE_MAX || (expect >= 0 && total > expect)) {
            sendCtrl1(l, l->server ? OP_ERROR : OP_ABORT, ST_ILLEGAL);
            snprintf(l->err, sizeof(l->err), "more data than announced");
            return -1;
        }
        want++;
        sinceAck++;
        if (n < (size_t)blk) {
            done = true;
            doneAt = now;
        }
        if (done || sinceAck >= WINDOW) {
            put16(ack + 1, want);
            sendPkt(l, false, ack, sizeof(ack));
            sinceAck = 0;
        }
    }
}

/* ---- server ----------------------------------------------------------------------------------------------------- */

static void serveGet(link_t *l, const pkt_t *req)
{
    uint8_t rsp[10] = { RSP | OP_GET, ST_OK };
    char name[NAME_MAX_LEN];
    uint32_t crc;
    size_t hdr = req->buf[0] == OP_GETR ? 7 : 3;

    if (req->len < hdr + 1 || req->len - hdr >= NAME_MAX_LEN) {
        sendCtrl1(l, RSP | OP_GET, ST_ILLEGAL);
        return;
    }
    int blk = clampBlk(get16(req->buf + 1), l->tcp != NULL ? TCP_BLK_MAX : BLK_MAX);
    long from = hdr == 7 ? (long)get32(req->buf + 3) : 0;
    memcpy(name, req->buf + hdr, (size_t)req->len - hdr);
    name[req->len - hdr] = '\0';
    FILE *f = validName(name) ? fopen(name, "rb") : NULL;
    if (f == NULL) {
        sendCtrl1(l, RSP | OP_GET, validName(name) ? ST_NOT_FOUND : ST_ACCESS);
        return;
    }
    long size = fileInfo(f, &crc);
    if (size > FILE_MAX || from < 0 || from > size || (size - from) / blk + 1 > 0xFFFF) {
        fclose(f);
        sendCtrl1(l, RSP | OP_GET, ST_ILLEGAL);
        return;
    }
    put32(rsp + 2, (uint32_t)size);
    put32(rsp + 6, crc);
    ringClear(l->rx);
    sendPkt(l, false, rsp, sizeof(rsp));
    long sent = sendFile(l, f, size, from, blk);
    fclose(f);
    if (sent >= 0) {
        gFiles++;
        gBytes += sent;
        snprintf(gLast, sizeof(gLast), "sent %s, %ld bytes, conn %d", name, sent, (int)l->conn);
    } else {
        snprintf(gLast, sizeof(gLast), "send %s failed: %s", name, l->err);
    }
}

static void servePut(link_t *l, const pkt_t *req)
{
    char name[NAME_MAX_LEN];
    char part[NAME_MAX_LEN + 8];
    uint32_t crc = 0;
    bool resume = req->buf[0] == OP_PUTR;
    uint8_t rspOp = resume ? (uint8_t)(RSP | OP_PUTR) : (uint8_t)(RSP | OP_PUT);

    if (req->len < 12 || req->len - 11 >= NAME_MAX_LEN) {
        sendCtrl1(l, rspOp, ST_ILLEGAL);
        return;
    }
    int blk = clampBlk(get16(req->buf + 1), l->tcp != NULL ? TCP_BLK_MAX : BLK_MAX);
    long size = (long)get32(req->buf + 3);
    uint32_t want = get32(req->buf + 7);
    memcpy(name, req->buf + 11, (size_t)req->len - 11);
    name[req->len - 11] = '\0';
    if (!gWrite || !validName(name)) {
        sendCtrl1(l, rspOp, ST_ACCESS);
        return;
    }
    if (size > FILE_MAX || size / blk + 1 > 0xFFFF) {
        sendCtrl1(l, rspOp, ST_ILLEGAL);
        return;
    }
    snprintf(part, sizeof(part), "%s.part", name);
    long have = resume ? partInfo(part, &crc) : 0;
    if (have > size) {
        have = 0;
    }
    if (have == 0) {
        crc = 0;
    }
    FILE *f = fopen(part, have > 0 ? "ab" : "wb");
    if (f == NULL) {
        sendCtrl1(l, rspOp, ST_ACCESS);
        return;
    }
    ringClear(l->rx);
    if (resume) {
        uint8_t rsp[6] = { RSP | OP_PUTR, ST_OK };
        put32(rsp + 2, (uint32_t)have);
        sendPkt(l, false, rsp, sizeof(rsp));
    } else {
        sendCtrl1(l, RSP | OP_PUT, ST_OK);
    }
    long got = recvFile(l, f, blk, have, size, &crc);
    fclose(f);
    uint8_t st = got < 0 ? ST_ILLEGAL : (got != size || crc != want) ? ST_CRC : ST_OK;
    if (st == ST_OK && !commitPart(part, name)) {
        st = ST_ACCESS;
    }
    if (st != ST_OK && got >= 0) {
        remove(part);                  /* an interrupted .part stays for a resume */
    }
    sendCtrl1(l, OP_PUTOK, st);
    if (st == ST_OK) {
        gFiles++;
        gBytes += got;
        snprintf(gLast, sizeof(gLast), "received %s, %ld bytes, conn %d", name, got, (int)l->conn);
    } else {
        snprintf(gLast, sizeof(gLast), "receive %s failed: %s", name, got < 0 ? l->err : statusName(st));
    }
}

static void listEntry(link_t *l, const char *name, long size)
{
    uint8_t m[6 + NAME_MAX_LEN];
    size_t n = strlen(name);

    if (!validName(name) || n > PKT_MAX - 6) {
        return;
    }
    m[0] = RSP | OP_LIST;
    m[1] = ST_OK;
    put32(m + 2, (uint32_t)size);
    memcpy(m + 6, name, n);
    for (int i = 0; i < 50 && !sendPkt(l, false, m, n + 6); i++) {
        ucxapp_platform_sleep_ms(10);
    }
}

static void serveList(link_t *l)
{
#ifdef _WIN32
    struct _finddata_t fd;
    intptr_t h = _findfirst("*", &fd);
    if (h != -1) {
        do {
            if ((fd.attrib & _A_SUBDIR) == 0) {
                listEntry(l, fd.name, (long)fd.size);
            }
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR *d = opendir(".");
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            if (stat(e->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                listEntry(l, e->d_name, (long)st.st_size);
            }
        }
        closedir(d);
    }
#endif
    sendCtrl1(l, RSP | OP_LIST, ST_END);
}

static void serveDel(link_t *l, const pkt_t *req)
{
    char name[NAME_MAX_LEN];

    if (req->len < 2 || req->len - 1 >= NAME_MAX_LEN) {
        sendCtrl1(l, RSP | OP_DEL, ST_ILLEGAL);
        return;
    }
    memcpy(name, req->buf + 1, (size_t)req->len - 1);
    name[req->len - 1] = '\0';
    if (!gWrite || !validName(name)) {
        sendCtrl1(l, RSP | OP_DEL, ST_ACCESS);
        return;
    }
    uint8_t st = remove(name) == 0 ? ST_OK : ST_NOT_FOUND;
    sendCtrl1(l, RSP | OP_DEL, st);
    snprintf(gLast, sizeof(gLast), "delete %s: %s", name, statusName(st));
}

/* One TCP client at a time; later ones are closed until it leaves. */
static void serverTcpService(void)
{
    int32_t s = gAccepted;

    if (gSrvTcp.sock >= 0 && gSrvTcp.closed) {
        (void)uCxSocketClose(gApp->ucx, gSrvTcp.sock);
        gSrvTcp.sock = -1;
    }
    if (s < 0) {
        return;
    }
    gAccepted = -1;
    if (gSrvTcp.sock >= 0) {
        (void)uCxSocketClose(gApp->ucx, s);
        return;
    }
    gSrvTcp.n = 0;
    gSrvTcp.closed = false;
    gSrvTcp.pending = true;
    ringClear(&gSrvQ);
    gSrvTcp.sock = s;
}

static void serverLoop(void *arg)
{
    static link_t l;
    pkt_t req;
    (void)arg;

    while (!gStop) {
        if (gListen >= 0) {
            serverTcpService();
            tcpPump(gApp, &gSrvTcp, &gSrvQ);
        }
        if (!ringPop(&gSrvQ, &req)) {
            ucxapp_platform_sleep_ms(1);
            continue;
        }
        if (req.data || req.len == 0) {
            continue;                  /* stray data outside a transfer */
        }
        memset(&l, 0, sizeof(l));
        l.app = gApp;
        l.conn = req.conn;
        l.server = true;
        l.rx = &gSrvQ;
        l.tcp = gListen >= 0 ? &gSrvTcp : NULL;
        l.stop = &gStop;
        switch (req.buf[0]) {
        case OP_GET:
        case OP_GETR: serveGet(&l, &req); break;
        case OP_PUT:
        case OP_PUTR: servePut(&l, &req); break;
        case OP_LIST: serveList(&l); break;
        case OP_DEL:  serveDel(&l, &req); break;
        case OP_ACK:
        case OP_ABORT:
            break;                     /* late packets of a finished transfer */
        default:      sendCtrl1(&l, OP_ERROR, ST_ILLEGAL); break;
        }
    }
}

static void serverStop(void)
{
    gStop = true;
    if (gThread != NULL) {
        ucxapp_thread_join(gThread);
        gThread = NULL;
    }
    if (gSrvTcp.sock >= 0) {
        (void)uCxSocketClose(gApp->ucx, gSrvTcp.sock);
        gSrvTcp.sock = -1;
    }
    if (gListen >= 0) {
        int32_t s = gListen;
        gListen = -1;
        (void)uCxSocketClose(gApp->ucx, s);
    }
    gRunning = false;
}

static bool defineService(ucxapp_t *app)
{
    static const uint8_t kInit[1] = { 0 };
    uint8_t ctrlProps = PROP_WRITE | PROP_WRITE_NR | PROP_NOTIFY;
    uint8_t dataProps = PROP_WRITE_NR | PROP_NOTIFY;
    uCxGattServerCharDefine_t rsp;
    int32_t svc;
    int32_t r;

    r = uCxGattServerServiceDefine(app->ucx, kSvc, 16, &svc);
    if (r == 0) {
        r = uCxGattServerCharDefine6(app->ucx, kCtrl, 16, &ctrlProps, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED, kInit, 1, PKT_MAX, &rsp);
        gSrvCtrl = rsp.value_handle;
    }
    if (r == 0) {
        r = uCxGattServerCharDefine6(app->ucx, kData, 16, &dataProps, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED, kInit, 1, PKT_MAX, &rsp);
        gSrvData = rsp.value_handle;
    }
    if (r == 0) {
        r = uCxGattServerServiceActivate(app->ucx);
    }
    if (r != 0) {
        ucxapp_print("ERROR: cannot define the file transfer service (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    gDefined = true;
    return true;
}

/* ft.on [rw] [tcp[:port]] [seconds] */
static int cmdOn(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 0;
    int32_t port = 0;
    bool rw = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "rw") == 0) {
            rw = true;
        } else if (strcmp(argv[i], "tcp") == 0) {
            port = TCP_PORT;
        } else if (strncmp(argv[i], "tcp:", 4) == 0 && ucxapp_parse_int(argv[i] + 4, &port) && port > 0 &&
                   port < 65536) {
            continue;
        } else if (seconds == 0 && ucxapp_parse_int(argv[i], &seconds) && seconds > 0) {
            continue;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (gRunning) {
        ucxapp_print("already running; ft.off first\n");
        return UCXAPP_ERR_FAILED;
    }
    if (port > 0) {
        int32_t s;
        if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_TCP, &s) != 0) {
            ucxapp_print("ERROR: cannot create a socket (is Wi-Fi up?)\n");
            return UCXAPP_ERR_FAILED;
        }
        if (uCxSocketListen2(app->ucx, s, port) != 0) {
            (void)uCxSocketClose(app->ucx, s);
            ucxapp_print("ERROR: cannot listen on port %d\n", (int)port);
            return UCXAPP_ERR_FAILED;
        }
        gAccepted = -1;
        gSrvTcp.sock = -1;
        gListen = s;
    } else {
        if (!gDefined && !defineService(app)) {
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_gatt_ensure_advertising(app);
    }
    gPort = port;
    gApp = app;
    gWrite = rw;
    gStop = false;
    gFiles = 0;
    gBytes = 0;
    gLast[0] = '\0';
    ringClear(&gSrvQ);
    gRunning = true;
    gThread = ucxapp_thread_start(serverLoop, NULL);
    if (gThread == NULL) {
        gRunning = false;
        serverStop();
        ucxapp_print("ERROR: cannot start the file transfer thread\n");
        return UCXAPP_ERR_FAILED;
    }
    if (port > 0) {
        char ssid[40];
        char ip[48];
        bool up = ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip));
        ucxapp_print("TCP file transfer server on %s:%d (%s), serving the working folder; no authentication: trusted "
                     "networks only\n", up ? ip : "<module-ip>", (int)port, rw ? "read/write" : "read only");
    } else {
        ucxapp_print("BLE file transfer server on (%s), serving the working folder; peers need an encrypted link\n",
                     rw ? "read/write" : "read only");
    }
    if (seconds > 0) {
        ucxapp_platform_sleep_ms((uint32_t)seconds * 1000u);
        serverStop();
        ucxapp_print("file transfer server stopped after %d s: %u file(s), %ld bytes\n", (int)seconds, gFiles,
                     gBytes);
    }
    return UCXAPP_OK;
}

static int cmdOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        ucxapp_print("not running\n");
        return UCXAPP_OK;
    }
    serverStop();
    ucxapp_print("file transfer server stopped\n");
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        ucxapp_print("ft: not running\n");
        return UCXAPP_OK;
    }
    if (gPort > 0) {
        ucxapp_print("ft: TCP port %d, %s, %s, %u file(s), %ld bytes\n", (int)gPort, gWrite ? "read/write" : "read only",
                     gSrvTcp.sock >= 0 ? "client connected" : "no client", gFiles, gBytes);
    } else {
        ucxapp_print("ft: BLE, %s, CTRL handle %d, DATA handle %d, %u file(s), %ld bytes\n",
                     gWrite ? "read/write" : "read only", (int)gSrvCtrl, (int)gSrvData, gFiles, gBytes);
    }
    if (gLast[0] != '\0') {
        ucxapp_print("last: %s\n", gLast);
    }
    return UCXAPP_OK;
}

/* ---- client ----------------------------------------------------------------------------------------------------- */

static bool isTcpTarget(const char *target)
{
    int32_t v;
    return !ucxapp_parse_int(target, &v);
}

static bool clientOpenTcp(ucxapp_t *app, link_t *l, const char *target)
{
    char host[64];
    int32_t port = TCP_PORT;
    int32_t sock;
    const char *colon = strrchr(target, ':');

    snprintf(host, sizeof(host), "%.*s", colon != NULL ? (int)(colon - target) : (int)strlen(target), target);
    if (host[0] == '\0' || (colon != NULL && (!ucxapp_parse_int(colon + 1, &port) || port <= 0 || port > 65535))) {
        return false;
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        ucxapp_print("ERROR: cannot create a socket (is Wi-Fi up?)\n");
        return false;
    }
    gCliTcp.n = 0;
    gCliTcp.pending = false;
    gCliTcp.closed = false;
    ringClear(&gCliQ);
    gCliTcp.sock = sock;
    if (uCxSocketConnect(app->ucx, sock, host, port) != 0) {
        gCliTcp.sock = -1;
        (void)uCxSocketClose(app->ucx, sock);
        ucxapp_print("ERROR: cannot connect to %s:%d (start ft.on tcp there)\n", host, (int)port);
        return false;
    }
    gCliApp = app;
    gCliStop = false;
    l->app = app;
    l->conn = sock;
    l->server = false;
    l->rx = &gCliQ;
    l->tcp = &gCliTcp;
    l->stop = &gCliStop;
    return true;
}

/* target: a BLE connection number, or host[:port] for TCP. */
static bool clientOpen(ucxapp_t *app, link_t *l, const char *target)
{
    int32_t conn;
    int32_t ctrl;
    int32_t data;

    memset(l, 0, sizeof(*l));
    if (isTcpTarget(target)) {
        return clientOpenTcp(app, l, target);
    }
    if (!ucxapp_parse_int(target, &conn) || conn < 0) {
        return false;
    }
    if (gRunning) {
        ucxapp_print("ERROR: stop the local server first (ft.off)\n");
        return false;
    }
    gCliConn = -1;
    if (!ucxapp_gatt_client_find128(app, conn, kCtrl, &ctrl) || !ucxapp_gatt_client_find128(app, conn, kData, &data)) {
        ucxapp_print("ERROR: the peer has no file transfer service (start ft.on there)\n");
        return false;
    }
    if (uCxGattClientConfigWrite(app->ucx, conn, ctrl + 1, U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS) != 0 ||
        uCxGattClientConfigWrite(app->ucx, conn, data + 1, U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS) != 0) {
        ucxapp_print("ERROR: cannot enable notifications (the link must be encrypted: pair first)\n");
        return false;
    }
    gCliCtrl = ctrl;
    gCliData = data;
    ringClear(&gCliQ);
    gCliConn = conn;
    gCliStop = false;
    l->app = app;
    l->conn = conn;
    l->server = false;
    l->rx = &gCliQ;
    l->stop = &gCliStop;
    return true;
}

static void clientClose(void)
{
    gCliConn = -1;
    if (gCliTcp.sock >= 0) {
        int32_t s = gCliTcp.sock;
        gCliTcp.sock = -1;
        (void)uCxSocketClose(gCliApp->ucx, s);
    }
}

static bool request(link_t *l, const uint8_t *m, size_t n, uint8_t op, pkt_t *rsp)
{
    if (!sendPkt(l, false, m, n)) {
        ucxapp_print("ERROR: cannot send the request\n");
        return false;
    }
    if (!waitCtrl(l, op, REPLY_MS, rsp) || rsp->len < 2) {
        ucxapp_print("ERROR: no reply from the peer\n");
        return false;
    }
    if (rsp->buf[0] != op || rsp->buf[1] != ST_OK) {
        ucxapp_print("ERROR: %s\n", statusName(rsp->buf[1]));
        return false;
    }
    return true;
}

static int parseBlk(int argc, char *argv[], int idx, int *blk)
{
    int32_t v;
    int max = isTcpTarget(argv[1]) ? TCP_BLK_MAX : BLK_MAX;

    *blk = isTcpTarget(argv[1]) ? TCP_BLK_MAX : BLK_DEFAULT;
    if (argc > idx) {
        if (!ucxapp_parse_int(argv[idx], &v) || v < BLK_MIN || v > max) {
            return -1;
        }
        *blk = (int)v;
    }
    return 0;
}

static const char *baseName(const char *path)
{
    const char *s = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = s > b ? s : b;
    return p != NULL ? p + 1 : path;
}

static size_t buildGet(uint8_t *m, int blk, long from, const char *name)
{
    size_t n = strlen(name);
    size_t hdr = from > 0 ? 7 : 3;
    m[0] = from > 0 ? OP_GETR : OP_GET;
    put16(m + 1, (uint16_t)blk);
    if (from > 0) {
        put32(m + 3, (uint32_t)from);
    }
    memcpy(m + hdr, name, n);
    return n + hdr;
}

static size_t buildPut(uint8_t *m, int blk, long size, uint32_t crc, bool resume, const char *name)
{
    size_t n = strlen(name);
    m[0] = resume ? OP_PUTR : OP_PUT;
    put16(m + 1, (uint16_t)blk);
    put32(m + 3, (uint32_t)size);
    put32(m + 7, crc);
    memcpy(m + 11, name, n);
    return n + 11;
}

static void printRate(const char *what, const char *a, const char *b, long total, uint32_t start)
{
    uint32_t ms = ucxapp_platform_tick_ms() - start;
    ucxapp_print("%s %s -> %s: %ld bytes in %u ms (%ld B/s), CRC-32 ok\n", what, a, b, total, (unsigned)ms,
                 ms > 0 ? (long)((double)total * 1000.0 / ms) : total);
}

/* Drops a trailing "resume" argument; true if it was there. */
static bool takeResume(int *argc, char *argv[])
{
    if (*argc > 3 && strcmp(argv[*argc - 1], "resume") == 0) {
        (*argc)--;
        return true;
    }
    return false;
}

/* ft.get <conn> <remote> [local] [blk] [resume] */
static int cmdGet(ucxapp_t *app, int argc, char *argv[])
{
    static link_t l;
    uint8_t m[PKT_MAX];
    char part[300];
    pkt_t rsp;
    int blk;
    uint32_t crc = 0;
    bool resume = takeResume(&argc, argv);

    if (argc < 3 || argc > 5 || parseBlk(argc, argv, 4, &blk) != 0 || !validName(argv[2])) {
        return UCXAPP_ERR_USAGE;
    }
    const char *local = argc >= 4 ? argv[3] : argv[2];
    if (!clientOpen(app, &l, argv[1])) {
        return UCXAPP_ERR_FAILED;
    }
    snprintf(part, sizeof(part), "%s.part", local);
    long have = resume ? partInfo(part, &crc) : 0;
    if (have > 0) {
        ucxapp_print("resuming at %ld bytes\n", have);
    }
    FILE *f = fopen(part, have > 0 ? "ab" : "wb");
    if (f == NULL) {
        clientClose();
        ucxapp_print("ERROR: cannot create %s\n", part);
        return UCXAPP_ERR_FAILED;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    long total = -1;
    long size = 0;
    uint32_t want = 0;
    bool mismatch = false;
    if (request(&l, m, buildGet(m, blk, have, argv[2]), RSP | OP_GET, &rsp) && rsp.len >= 10) {
        size = (long)get32(rsp.buf + 2);
        want = get32(rsp.buf + 6);
        ucxapp_print("size %ld bytes, CRC-32 %08X\n", size, (unsigned)want);
        total = recvFile(&l, f, blk, have, size, &crc);
        if (total >= 0 && (total != size || crc != want)) {
            snprintf(l.err, sizeof(l.err), "CRC-32 %08X or size %ld does not match", (unsigned)crc, total);
            total = -1;
            mismatch = true;
        }
    } else {
        snprintf(l.err, sizeof(l.err), "request refused");
        mismatch = true;
    }
    fclose(f);
    clientClose();
    if (total < 0 || !commitPart(part, local)) {
        if (mismatch || total >= 0) {
            remove(part);
            ucxapp_print("ERROR: get %s: %s\n", argv[2], total < 0 ? l.err : "cannot save");
        } else {
            ucxapp_print("ERROR: get %s: %s (kept %s; add 'resume' to continue)\n", argv[2], l.err, part);
        }
        return UCXAPP_ERR_FAILED;
    }
    printRate("got", argv[2], local, total, start);
    return UCXAPP_OK;
}

/* ft.put <conn> <local> [remote] [blk] [resume] */
static int cmdPut(ucxapp_t *app, int argc, char *argv[])
{
    static link_t l;
    uint8_t m[PKT_MAX];
    pkt_t rsp;
    int blk;
    uint32_t crc;
    bool resume = takeResume(&argc, argv);

    if (argc < 3 || argc > 5 || parseBlk(argc, argv, 4, &blk) != 0) {
        return UCXAPP_ERR_USAGE;
    }
    const char *remote = argc >= 4 ? argv[3] : baseName(argv[2]);
    if (!validName(remote)) {
        return UCXAPP_ERR_USAGE;
    }
    FILE *f = fopen(argv[2], "rb");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    long size = fileInfo(f, &crc);
    if (size > FILE_MAX || size / blk + 1 > 0xFFFF) {
        fclose(f);
        ucxapp_print("ERROR: %s is too large\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    if (!clientOpen(app, &l, argv[1])) {
        fclose(f);
        return UCXAPP_ERR_FAILED;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    long total = -1;
    if (request(&l, m, buildPut(m, blk, size, crc, resume, remote), resume ? RSP | OP_PUTR : RSP | OP_PUT, &rsp)) {
        long from = resume && rsp.len >= 6 ? (long)get32(rsp.buf + 2) : 0;
        if (from > 0) {
            ucxapp_print("resuming at %ld bytes\n", from);
        }
        total = sendFile(&l, f, size, from, blk);
        if (total >= 0 && (!waitCtrl(&l, OP_PUTOK, REPLY_MS, &rsp) || rsp.len < 2 || rsp.buf[0] != OP_PUTOK ||
                           rsp.buf[1] != ST_OK)) {
            snprintf(l.err, sizeof(l.err), "peer did not verify the file (%s)",
                     rsp.len >= 2 ? statusName(rsp.buf[1]) : "no reply");
            total = -1;
        }
    } else {
        snprintf(l.err, sizeof(l.err), "request refused");
    }
    fclose(f);
    clientClose();
    if (total < 0) {
        ucxapp_print("ERROR: put %s: %s\n", remote, l.err);
        return UCXAPP_ERR_FAILED;
    }
    printRate("put", argv[2], remote, total, start);
    return UCXAPP_OK;
}

static int cmdLs(ucxapp_t *app, int argc, char *argv[])
{
    static link_t l;
    uint8_t m[1] = { OP_LIST };
    pkt_t rsp;
    int count = 0;
    bool end = false;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!clientOpen(app, &l, argv[1])) {
        return UCXAPP_ERR_FAILED;
    }
    if (!sendPkt(&l, false, m, sizeof(m))) {
        clientClose();
        ucxapp_print("ERROR: cannot send the request\n");
        return UCXAPP_ERR_FAILED;
    }
    while (waitCtrl(&l, RSP | OP_LIST, REPLY_MS, &rsp)) {
        if (rsp.buf[0] != (RSP | OP_LIST) || rsp.len < 2 || rsp.buf[1] == ST_END) {
            end = rsp.len >= 2 && rsp.buf[1] == ST_END;
            break;
        }
        if (rsp.len >= 6) {
            ucxapp_print("%10lu  %.*s\n", (unsigned long)get32(rsp.buf + 2), (int)(rsp.len - 6), rsp.buf + 6);
            count++;
        }
    }
    clientClose();
    if (!end) {
        ucxapp_print("ERROR: list incomplete\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%d file(s)\n", count);
    return UCXAPP_OK;
}

static int cmdRm(ucxapp_t *app, int argc, char *argv[])
{
    static link_t l;
    uint8_t m[1 + NAME_MAX_LEN];
    pkt_t rsp;

    if (argc != 3 || !validName(argv[2])) {
        return UCXAPP_ERR_USAGE;
    }
    if (!clientOpen(app, &l, argv[1])) {
        return UCXAPP_ERR_FAILED;
    }
    size_t n = strlen(argv[2]);
    m[0] = OP_DEL;
    memcpy(m + 1, argv[2], n);
    bool ok = request(&l, m, n + 1, RSP | OP_DEL, &rsp);
    clientClose();
    if (!ok) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("deleted %s\n", argv[2]);
    return UCXAPP_OK;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kGood[] = { "fw.bin", "notes.txt", "a b-c_1.log" };
    static const char *const kBad[] = { "", ".hidden", "../x", "dir/x", "dir\\x", "C:x", "a*b", "a\tb" };
    static const uint8_t kGet[] = { OP_GET, 0xF0, 0x00, 'f' };
    static const uint8_t kGetR[] = { OP_GETR, 0xF0, 0x00, 0x34, 0x12, 0, 0, 'f' };
    static const uint8_t kPut[] = { OP_PUT, 0x80, 0x00, 0x2A, 0, 0, 0, 0x26, 0x39, 0xF4, 0xCB, 'f' };
    uint8_t m[PKT_MAX];

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (crc32Update(0, (const uint8_t *)"123456789", 9) != 0xCBF43926u ||
        crc32Update(crc32Update(0, (const uint8_t *)"1234", 4), (const uint8_t *)"56789", 5) != 0xCBF43926u) {
        ucxapp_print("ERROR: ft selftest: CRC-32\n");
        return UCXAPP_ERR_FAILED;
    }
    for (size_t i = 0; i < sizeof(kGood) / sizeof(kGood[0]); i++) {
        if (!validName(kGood[i])) {
            ucxapp_print("ERROR: ft selftest: '%s' refused\n", kGood[i]);
            return UCXAPP_ERR_FAILED;
        }
    }
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        if (validName(kBad[i])) {
            ucxapp_print("ERROR: ft selftest: '%s' accepted\n", kBad[i]);
            return UCXAPP_ERR_FAILED;
        }
    }
    size_t n = buildGet(m, BLK_DEFAULT, 0, "f");
    if (n != sizeof(kGet) || memcmp(m, kGet, n) != 0) {
        ucxapp_print("ERROR: ft selftest: GET encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    n = buildGet(m, BLK_DEFAULT, 0x1234, "f");
    if (n != sizeof(kGetR) || memcmp(m, kGetR, n) != 0) {
        ucxapp_print("ERROR: ft selftest: GET resume encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    n = buildPut(m, 128, 42, 0xCBF43926u, true, "f");
    if (n != sizeof(kPut) || m[0] != OP_PUTR || memcmp(m + 1, kPut + 1, n - 1) != 0) {
        ucxapp_print("ERROR: ft selftest: PUT resume encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    n = buildPut(m, 128, 42, 0xCBF43926u, false, "f");
    if (n != sizeof(kPut) || memcmp(m, kPut, n) != 0) {
        ucxapp_print("ERROR: ft selftest: PUT encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    if (clampBlk(1, BLK_MAX) != BLK_MIN || clampBlk(1000, BLK_MAX) != BLK_MAX ||
        clampBlk(5000, TCP_BLK_MAX) != TCP_BLK_MAX || strcmp(baseName("a/b\\c.txt"), "c.txt") != 0 ||
        !isTcpTarget("192.168.1.10:6970") || isTcpTarget("0")) {
        ucxapp_print("ERROR: ft selftest: block size or name\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("ft selftest: CRC-32, names and request encoding ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gFtCmds[] = {
    { "ft.on",       "[rw] [tcp[:port]] [seconds]",       "File transfer server serving the working folder, over BLE (custom GATT service) or with 'tcp' on a module TCP socket (default port 6970); 'rw' allows uploads and deletes; with seconds it serves that long, then stops", UCXAPP_CMD_NEEDS_DEVICE, cmdOn },
    { "ft.off",      "",                                  "Stop the file transfer server",                            UCXAPP_CMD_NEEDS_DEVICE, cmdOff },
    { "ft.status",   "",                                  "File transfer server link, state, transfer count and last transfer", UCXAPP_CMD_NEEDS_DEVICE, cmdStatus },
    { "ft.get",      "<conn|host[:port]> <remote> [local] [blk] [resume]", "Download a file from a peer's ft server over BLE (conn, blk 16-242, default 240) or TCP (host, blk up to 1455); checked with CRC-32; 'resume' continues an interrupted download from <local>.part", UCXAPP_CMD_NEEDS_DEVICE, cmdGet },
    { "ft.put",      "<conn|host[:port]> <local> [remote] [blk] [resume]", "Upload a file to a peer's ft server (needs 'rw' there); checked with CRC-32; 'resume' continues from what the server already has", UCXAPP_CMD_NEEDS_DEVICE, cmdPut },
    { "ft.ls",       "<conn|host[:port]>",                "List the files on a peer's ft server",                     UCXAPP_CMD_NEEDS_DEVICE, cmdLs },
    { "ft.rm",       "<conn|host[:port]> <name>",         "Delete a file on a peer's ft server (needs 'rw' there)",   UCXAPP_CMD_NEEDS_DEVICE, cmdRm },
    { "ft.selftest", "",                                  "Offline check of the CRC-32, name rules and request encoding", 0, cmdSelftest },
};

void ucxapp_register_ft_cmds(void)
{
    ucxapp_cmd_register(gFtCmds, sizeof(gFtCmds) / sizeof(gFtCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_ft_cmds(void)
{
}

bool ucxapp_ft_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    return false;
}

bool ucxapp_ft_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    return false;
}

void ucxapp_ft_on_sock_data(int32_t sock)
{
    (void)sock;
}

void ucxapp_ft_on_sock_closed(int32_t sock)
{
    (void)sock;
}

void ucxapp_ft_on_sock_incoming(int32_t sock, int32_t listenSock)
{
    (void)sock;
    (void)listenSock;
}

#endif
