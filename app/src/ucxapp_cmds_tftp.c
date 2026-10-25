/* tftp.* - TFTP (RFC 1350, options RFC 2347/2348/2349) over a module UDP socket. The server runs on its own thread
 * and serves the working folder; uploads only with 'rw'. Host builds only. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_platform.h"
#include "ucxapp_thread.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5
#define OP_OACK  6

#define ERR_NOT_FOUND 1
#define ERR_ACCESS    2
#define ERR_DISK_FULL 3
#define ERR_ILLEGAL   4
#define ERR_TID       5
#define ERR_OPTION    8

#define BLK_DEFAULT   512
#define BLK_MAX       1456             /* AT+USOSTB takes up to 1460 bytes: 4 header + 1456 data */
#define PKT_MAX       (4 + BLK_MAX)
#define TRY_MS        1000
#define TRIES         5
#define IDLE_POLL_MS  20               /* server waiting for a request */
#define NAME_MAX_LEN  128
#define FILE_MAX      (64L * 1024 * 1024)
#define DEFAULT_PORT  69

typedef struct {
    uCxHandle_t *ucx;
    int32_t sock;
    uSockIpAddress_t ip;
    int32_t port;
    bool locked;                       /* peer port (TID) known */
    volatile bool *stop;
    uint8_t rx[PKT_MAX + 4];
    int rxLen;
    char err[96];
} xfer_t;

static volatile bool gStop;
static volatile bool gRunning;
static bool gWrite;
static int32_t gPort;
static int32_t gSock = -1;
static ucxapp_t *gApp;
static ucxapp_thread_t *gThread;
static unsigned gFiles;
static long gBytes;
static char gLast[160];

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static bool strieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
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

static bool openSocket(uCxHandle_t *ucx, int32_t port, int32_t *sock)
{
    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_UDP, sock) != 0) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int32_t p = port > 0 ? port : 49152 + (int32_t)((ucxapp_platform_tick_ms() + (uint32_t)i * 7919u) % 16000u);
        if (uCxSocketBind(ucx, *sock, p) == 0) {
            return true;
        }
        if (port > 0) {
            break;
        }
    }
    uCxSocketClose(ucx, *sock);
    *sock = -1;
    return false;
}

static bool sendPkt(xfer_t *x, const uint8_t *pkt, size_t len)
{
    return uCxSocketSendTo(x->ucx, x->sock, &x->ip, x->port, pkt, (int32_t)len) >= 0;
}

static void sendError(xfer_t *x, uint16_t code, const char *msg)
{
    uint8_t pkt[100];
    size_t n = strlen(msg);

    n = n > sizeof(pkt) - 5 ? sizeof(pkt) - 5 : n;
    put16(pkt, OP_ERROR);
    put16(pkt + 2, code);
    memcpy(pkt + 4, msg, n);
    pkt[4 + n] = 0;
    sendPkt(x, pkt, 5 + n);
}

static bool sameIp(const uSockIpAddress_t *a, const uSockIpAddress_t *b)
{
    char sa[48];
    char sb[48];

    return uCxIpAddressToString(a, sa, sizeof(sa)) > 0 && uCxIpAddressToString(b, sb, sizeof(sb)) > 0 &&
           strcmp(sa, sb) == 0;
}

/* Sends pkt and waits for a reply with opcode op and block blk (or an OACK when acceptOack); retries on timeout.
 * Returns the reply length in x->rx, -1 on timeout or stop, -2 on a TFTP error from the peer. */
static int exchange(xfer_t *x, const uint8_t *pkt, size_t len, uint16_t op, uint16_t blk, bool acceptOack)
{
    for (int t = 0; t < TRIES; t++) {
        if (!sendPkt(x, pkt, len)) {
            snprintf(x->err, sizeof(x->err), "send failed");
            return -1;
        }
        uint32_t end = ucxapp_platform_tick_ms() + TRY_MS;
        while ((int32_t)(end - ucxapp_platform_tick_ms()) > 0) {
            if (x->stop != NULL && *x->stop) {
                snprintf(x->err, sizeof(x->err), "stopped");
                return -1;
            }
            uCxSocketReceiveFrom_t from;
            memset(&from, 0, sizeof(from));
            int32_t rc = uCxSocketReceiveFrom(x->ucx, x->sock, PKT_MAX, x->rx, &from);
            if (rc <= 0) {
                ucxapp_platform_sleep_ms(1);
                continue;
            }
            if (!sameIp(&from.remote_ip, &x->ip) || (x->locked && from.remote_port != x->port)) {
                xfer_t other = *x;
                other.ip = from.remote_ip;
                other.port = from.remote_port;
                sendError(&other, ERR_TID, "Unknown transfer ID");
                continue;
            }
            if (rc < 4) {
                continue;
            }
            if (!x->locked) {
                x->port = from.remote_port;
                x->locked = true;
            }
            uint16_t rop = get16(x->rx);
            if (rop == OP_ERROR) {
                x->rx[rc < PKT_MAX ? rc : PKT_MAX - 1] = 0;
                snprintf(x->err, sizeof(x->err), "peer error %u: %s", (unsigned)get16(x->rx + 2), (char *)x->rx + 4);
                return -2;
            }
            if ((rop == op && get16(x->rx + 2) == blk) || (acceptOack && rop == OP_OACK)) {
                x->rxLen = (int)rc;
                return (int)rc;
            }
        }
    }
    snprintf(x->err, sizeof(x->err), "timeout");
    return -1;
}

/* Sends the file as DATA blocks 1..n; the peer has already acknowledged block 0 (or the OACK). */
static long sendFile(xfer_t *x, FILE *f, int blksize)
{
    uint8_t pkt[PKT_MAX];
    uint16_t blk = 1;
    long total = 0;

    for (;;) {
        size_t n = fread(pkt + 4, 1, (size_t)blksize, f);
        put16(pkt, OP_DATA);
        put16(pkt + 2, blk);
        if (exchange(x, pkt, 4 + n, OP_ACK, blk, false) < 0) {
            return -1;
        }
        total += (long)n;
        if (n < (size_t)blksize) {
            return total;
        }
        blk++;
    }
}

/* Writes DATA blocks; x->rx holds DATA block 1 already. */
static long recvFile(xfer_t *x, FILE *f, int blksize)
{
    uint8_t ack[4];
    uint16_t blk = 1;
    long total = 0;

    for (;;) {
        size_t n = (size_t)x->rxLen - 4;
        total += (long)n;
        if (total > FILE_MAX) {
            sendError(x, ERR_DISK_FULL, "File too large");
            snprintf(x->err, sizeof(x->err), "file larger than %ld bytes", FILE_MAX);
            return -1;
        }
        if (n > 0 && fwrite(x->rx + 4, 1, n, f) != n) {
            sendError(x, ERR_DISK_FULL, "Write failed");
            snprintf(x->err, sizeof(x->err), "write failed");
            return -1;
        }
        put16(ack, OP_ACK);
        put16(ack + 2, blk);
        if (n < (size_t)blksize) {
            sendPkt(x, ack, sizeof(ack));
            return total;
        }
        blk++;
        if (exchange(x, ack, sizeof(ack), OP_DATA, blk, false) < 0) {
            return -1;
        }
    }
}

/* Parses "blksize" and "tsize" from an option list (name\0value\0...). */
static void parseOptions(const uint8_t *p, const uint8_t *end, int *blksize, long *tsize, bool *haveBlk, bool *haveTsize)
{
    while (p < end) {
        const char *name = (const char *)p;
        const uint8_t *z = memchr(p, 0, (size_t)(end - p));
        if (z == NULL || z + 1 >= end) {
            return;
        }
        const char *value = (const char *)z + 1;
        const uint8_t *z2 = memchr(z + 1, 0, (size_t)(end - z - 1));
        if (z2 == NULL) {
            return;
        }
        if (strieq(name, "blksize")) {
            long v = strtol(value, NULL, 10);
            if (v >= 8) {
                *blksize = v > BLK_MAX ? BLK_MAX : (int)v;
                *haveBlk = true;
            }
        } else if (strieq(name, "tsize")) {
            *tsize = strtol(value, NULL, 10);
            *haveTsize = true;
        }
        p = z2 + 1;
    }
}

static size_t buildOack(uint8_t *pkt, bool haveBlk, int blksize, bool haveTsize, long tsize)
{
    size_t n = 2;

    put16(pkt, OP_OACK);
    if (haveBlk) {
        n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "blksize") + 1;
        n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "%d", blksize) + 1;
    }
    if (haveTsize) {
        n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "tsize") + 1;
        n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "%ld", tsize) + 1;
    }
    return n;
}

static long fileSize(FILE *f)
{
    if (fseek(f, 0, SEEK_END) != 0) {
        return -1;
    }
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return size;
}

static bool commitPart(const char *part, const char *name)
{
    remove(name);
    return rename(part, name) == 0;
}

/* ---- server ------------------------------------------------------------- */

static void serveRequest(const uint8_t *req, int len, const uSockIpAddress_t *ip, int32_t port)
{
    xfer_t *x = calloc(1, sizeof(*x));
    char peer[48] = "?";
    char part[NAME_MAX_LEN + 8];
    uint8_t pkt[PKT_MAX];
    int blksize = BLK_DEFAULT;
    long tsize = 0;
    bool haveBlk = false;
    bool haveTsize = false;
    long total = -1;

    if (x == NULL) {
        return;
    }
    x->ucx = gApp->ucx;
    x->ip = *ip;
    x->port = port;
    x->locked = true;
    x->stop = &gStop;
    uCxIpAddressToString(ip, peer, sizeof(peer));
    if (!openSocket(x->ucx, 0, &x->sock)) {
        free(x);
        return;
    }

    uint16_t op = get16(req);
    const uint8_t *end = req + len;
    const char *name = (const char *)req + 2;
    const uint8_t *z = memchr(req + 2, 0, (size_t)(len - 2));
    const char *mode = z != NULL && z + 1 < end ? (const char *)z + 1 : NULL;
    const uint8_t *z2 = mode != NULL ? memchr(z + 1, 0, (size_t)(end - z - 1)) : NULL;

    if ((op != OP_RRQ && op != OP_WRQ) || z2 == NULL) {
        sendError(x, ERR_ILLEGAL, "Illegal TFTP operation");
    } else if (!strieq(mode, "octet") && !strieq(mode, "netascii")) {
        sendError(x, ERR_ILLEGAL, "Only octet mode");
    } else if (!validName(name)) {
        sendError(x, ERR_ACCESS, "Plain file names only");
        snprintf(gLast, sizeof(gLast), "%s: refused name from %s", op == OP_RRQ ? "get" : "put", peer);
    } else if (op == OP_WRQ && !gWrite) {
        sendError(x, ERR_ACCESS, "Read only (tftp.on rw allows uploads)");
        snprintf(gLast, sizeof(gLast), "put %s from %s: refused, read only", name, peer);
    } else {
        parseOptions(z2 + 1, end, &blksize, &tsize, &haveBlk, &haveTsize);
        bool options = haveBlk || haveTsize;
        if (op == OP_RRQ) {
            FILE *f = fopen(name, "rb");
            if (f == NULL) {
                sendError(x, ERR_NOT_FOUND, "File not found");
                snprintf(gLast, sizeof(gLast), "get %s from %s: not found", name, peer);
            } else {
                tsize = fileSize(f);
                if (!options || exchange(x, pkt, buildOack(pkt, haveBlk, blksize, haveTsize, tsize), OP_ACK, 0, false) >= 0) {
                    total = sendFile(x, f, blksize);
                }
                fclose(f);
                snprintf(gLast, sizeof(gLast), "get %s by %s: %s", name, peer, total >= 0 ? "ok" : x->err);
            }
        } else if (haveTsize && tsize > FILE_MAX) {
            sendError(x, ERR_DISK_FULL, "File too large");
            snprintf(gLast, sizeof(gLast), "put %s from %s: too large", name, peer);
        } else {
            snprintf(part, sizeof(part), "%s.part", name);
            FILE *f = fopen(part, "wb");
            if (f == NULL) {
                sendError(x, ERR_ACCESS, "Cannot create file");
            } else {
                uint8_t ack0[4];
                size_t n = 4;
                put16(ack0, OP_ACK);
                put16(ack0 + 2, 0);
                if (options) {
                    n = buildOack(pkt, haveBlk, blksize, haveTsize, tsize);
                }
                if (exchange(x, options ? pkt : ack0, n, OP_DATA, 1, false) >= 0) {
                    total = recvFile(x, f, blksize);
                }
                fclose(f);
                if (total < 0 || !commitPart(part, name)) {
                    remove(part);
                    total = -1;
                }
            }
            snprintf(gLast, sizeof(gLast), "put %s from %s: %s", name, peer, total >= 0 ? "ok" : x->err);
        }
    }
    if (total >= 0) {
        gFiles++;
        gBytes += total;
        ucxapp_print("[TFTP] %s, %ld bytes\n", gLast, total);
    } else if (gLast[0] != '\0') {
        ucxapp_print("[TFTP] %s\n", gLast);
    }
    uCxSocketClose(x->ucx, x->sock);
    free(x);
}

static void serverLoop(void *arg)
{
    static uint8_t req[PKT_MAX];
    (void)arg;

    while (!gStop) {
        uCxSocketReceiveFrom_t from;
        memset(&from, 0, sizeof(from));
        int32_t rc = uCxSocketReceiveFrom(gApp->ucx, gSock, (int32_t)sizeof(req) - 1, req, &from);
        if (rc >= 4) {
            req[rc] = 0;
            gLast[0] = '\0';
            serveRequest(req, (int)rc, &from.remote_ip, from.remote_port);
        } else {
            ucxapp_platform_sleep_ms(IDLE_POLL_MS);
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
    if (gSock >= 0) {
        uCxSocketClose(gApp->ucx, gSock);
        gSock = -1;
    }
    gRunning = false;
}

/* tftp.on [rw] [port] [seconds] */
static int cmdOn(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = DEFAULT_PORT;
    int32_t seconds = 0;
    int numbers = 0;
    bool rw = false;
    char ssid[64];
    char ip[48];

    for (int i = 1; i < argc; i++) {
        int32_t v;
        if (strcmp(argv[i], "rw") == 0) {
            rw = true;
        } else if (numbers < 2 && ucxapp_parse_int(argv[i], &v) && v >= 1 && v <= 65535) {
            *(numbers++ == 0 ? &port : &seconds) = v;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (gRunning) {
        ucxapp_print("already running; tftp.off first\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip)) || ip[0] == '\0') {
        ucxapp_print("ERROR: connect Wi-Fi first (wifi.connect)\n");
        return UCXAPP_ERR_FAILED;
    }
    gApp = app;
    if (!openSocket(app->ucx, port, &gSock)) {
        ucxapp_print("ERROR: cannot bind UDP port %d\n", (int)port);
        return UCXAPP_ERR_FAILED;
    }
    gPort = port;
    gWrite = rw;
    gStop = false;
    gFiles = 0;
    gBytes = 0;
    gLast[0] = '\0';
    gThread = ucxapp_thread_start(serverLoop, NULL);
    if (gThread == NULL) {
        uCxSocketClose(app->ucx, gSock);
        gSock = -1;
        ucxapp_print("ERROR: cannot start the TFTP thread\n");
        return UCXAPP_ERR_FAILED;
    }
    gRunning = true;
    ucxapp_print("TFTP server on %s:%d (%s), serving the working folder; no authentication, use on trusted networks\n",
                 ip, (int)port, rw ? "read/write" : "read only");
    ucxapp_print("try: tftp -i %s GET <file>   or   curl -T <file> tftp://%s/\n", ip, ip);
    if (seconds > 0) {
        ucxapp_platform_sleep_ms((uint32_t)seconds * 1000u);
        serverStop();
        ucxapp_print("TFTP server stopped after %d s: %u file(s), %ld bytes\n", (int)seconds, gFiles, gBytes);
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
    ucxapp_print("TFTP server stopped\n");
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
        ucxapp_print("tftp: not running\n");
        return UCXAPP_OK;
    }
    ucxapp_print("tftp: port %d, %s, %u file(s), %ld bytes\n", (int)gPort, gWrite ? "read/write" : "read only", gFiles,
                 gBytes);
    if (gLast[0] != '\0') {
        ucxapp_print("last: %s\n", gLast);
    }
    return UCXAPP_OK;
}

/* ---- client ------------------------------------------------------------- */

static bool clientOpen(ucxapp_t *app, xfer_t *x, const char *host, int32_t port)
{
    memset(x, 0, sizeof(*x));
    x->ucx = app->ucx;
    x->port = port;
    if (uCxStringToIpAddress(host, &x->ip) != 0 && uCxSocketGetHostByName(app->ucx, host, &x->ip) != 0) {
        ucxapp_print("ERROR: cannot resolve %s\n", host);
        return false;
    }
    if (!openSocket(app->ucx, 0, &x->sock)) {
        ucxapp_print("ERROR: failed to create a UDP socket\n");
        return false;
    }
    return true;
}

static size_t buildRequest(uint8_t *pkt, uint16_t op, const char *name, long tsize)
{
    size_t n = 2;

    put16(pkt, op);
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "%s", name) + 1;
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "octet") + 1;
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "blksize") + 1;
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "%d", BLK_MAX) + 1;
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "tsize") + 1;
    n += (size_t)snprintf((char *)pkt + n, PKT_MAX - n, "%ld", tsize) + 1;
    return n;
}

static int clientArgs(int argc, char *argv[], int32_t *port)
{
    *port = DEFAULT_PORT;
    if (argc < 3 || argc > 5) {
        return -1;
    }
    if (argc == 5 && (!ucxapp_parse_int(argv[4], port) || *port < 1 || *port > 65535)) {
        return -1;
    }
    return 0;
}

/* tftp.get <host> <remote> [local] [port] */
static int cmdGet(ucxapp_t *app, int argc, char *argv[])
{
    static xfer_t x;
    uint8_t pkt[PKT_MAX];
    char part[300];
    int32_t port;
    long total = -1;

    if (clientArgs(argc, argv, &port) != 0) {
        return UCXAPP_ERR_USAGE;
    }
    const char *local = argc >= 4 ? argv[3] : argv[2];
    if (!clientOpen(app, &x, argv[1], port)) {
        return UCXAPP_ERR_FAILED;
    }
    snprintf(part, sizeof(part), "%s.part", local);
    FILE *f = fopen(part, "wb");
    if (f == NULL) {
        uCxSocketClose(app->ucx, x.sock);
        ucxapp_print("ERROR: cannot create %s\n", part);
        return UCXAPP_ERR_FAILED;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    int r = exchange(&x, pkt, buildRequest(pkt, OP_RRQ, argv[2], 0), OP_DATA, 1, true);
    int blksize = BLK_DEFAULT;
    if (r > 0 && get16(x.rx) == OP_OACK) {
        long tsize = 0;
        bool hb = false;
        bool ht = false;
        parseOptions(x.rx + 2, x.rx + x.rxLen, &blksize, &tsize, &hb, &ht);
        if (ht && tsize > 0) {
            ucxapp_print("size %ld bytes\n", tsize);
        }
        uint8_t ack0[4];
        put16(ack0, OP_ACK);
        put16(ack0 + 2, 0);
        r = exchange(&x, ack0, sizeof(ack0), OP_DATA, 1, false);
    }
    if (r > 0) {
        total = recvFile(&x, f, blksize);
    }
    fclose(f);
    uCxSocketClose(app->ucx, x.sock);
    if (total < 0 || !commitPart(part, local)) {
        remove(part);
        ucxapp_print("ERROR: get %s: %s\n", argv[2], x.err[0] != '\0' ? x.err : "cannot save");
        return UCXAPP_ERR_FAILED;
    }
    uint32_t ms = ucxapp_platform_tick_ms() - start;
    ucxapp_print("got %s -> %s: %ld bytes in %u ms (%ld B/s)\n", argv[2], local, total, (unsigned)ms,
                 ms > 0 ? (long)((double)total * 1000.0 / ms) : total);
    return UCXAPP_OK;
}

/* tftp.put <host> <local> [remote] [port] */
static int cmdPut(ucxapp_t *app, int argc, char *argv[])
{
    static xfer_t x;
    uint8_t pkt[PKT_MAX];
    int32_t port;
    long total = -1;

    if (clientArgs(argc, argv, &port) != 0) {
        return UCXAPP_ERR_USAGE;
    }
    const char *remote = argc >= 4 ? argv[3] : argv[2];
    const char *slash = strrchr(remote, '/');
    const char *bslash = strrchr(remote, '\\');
    if (argc < 4) {
        remote = slash != NULL && slash > (bslash != NULL ? bslash : remote) ? slash + 1 : bslash != NULL ? bslash + 1
                                                                                                            : remote;
    }
    FILE *f = fopen(argv[2], "rb");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    long size = fileSize(f);
    if (!clientOpen(app, &x, argv[1], port)) {
        fclose(f);
        return UCXAPP_ERR_FAILED;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    int blksize = BLK_DEFAULT;
    int r = exchange(&x, pkt, buildRequest(pkt, OP_WRQ, remote, size), OP_ACK, 0, true);
    if (r > 0 && get16(x.rx) == OP_OACK) {
        long tsize = 0;
        bool hb = false;
        bool ht = false;
        parseOptions(x.rx + 2, x.rx + x.rxLen, &blksize, &tsize, &hb, &ht);
    }
    if (r > 0) {
        total = sendFile(&x, f, blksize);
    }
    fclose(f);
    uCxSocketClose(app->ucx, x.sock);
    if (total < 0) {
        ucxapp_print("ERROR: put %s: %s\n", remote, x.err);
        return UCXAPP_ERR_FAILED;
    }
    uint32_t ms = ucxapp_platform_tick_ms() - start;
    ucxapp_print("put %s -> %s: %ld bytes in %u ms (%ld B/s)\n", argv[2], remote, total, (unsigned)ms,
                 ms > 0 ? (long)((double)total * 1000.0 / ms) : total);
    return UCXAPP_OK;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kGood[] = { "fw.bin", "notes.txt", "a b-c_1.log" };
    static const char *const kBad[] = { "", ".hidden", "../x", "dir/x", "dir\\x", "C:x", "a*b", "a\tb" };
    static const uint8_t kOpts[] = { 'B', 'L', 'K', 'S', 'I', 'Z', 'E', 0, '9', '9', '9', '9', 0,
                                     't', 's', 'i', 'z', 'e', 0, '4', '2', 0 };
    uint8_t pkt[PKT_MAX];
    int blksize = BLK_DEFAULT;
    long tsize = 0;
    bool hb = false;
    bool ht = false;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < sizeof(kGood) / sizeof(kGood[0]); i++) {
        if (!validName(kGood[i])) {
            ucxapp_print("ERROR: tftp selftest: '%s' refused\n", kGood[i]);
            return UCXAPP_ERR_FAILED;
        }
    }
    for (size_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); i++) {
        if (validName(kBad[i])) {
            ucxapp_print("ERROR: tftp selftest: '%s' accepted\n", kBad[i]);
            return UCXAPP_ERR_FAILED;
        }
    }
    parseOptions(kOpts, kOpts + sizeof(kOpts), &blksize, &tsize, &hb, &ht);
    if (!hb || !ht || blksize != BLK_MAX || tsize != 42) {
        ucxapp_print("ERROR: tftp selftest: option parsing\n");
        return UCXAPP_ERR_FAILED;
    }
    static const uint8_t kRrq[] = { 0, 1, 'f', 0, 'o', 'c', 't', 'e', 't', 0, 'b', 'l', 'k', 's', 'i', 'z', 'e', 0,
                                    '1', '4', '5', '6', 0, 't', 's', 'i', 'z', 'e', 0, '0', 0 };
    size_t n = buildRequest(pkt, OP_RRQ, "f", 0);
    if (n != sizeof(kRrq) || memcmp(pkt, kRrq, n) != 0) {
        ucxapp_print("ERROR: tftp selftest: request encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    static const uint8_t kOack[] = { 0, 6, 'b', 'l', 'k', 's', 'i', 'z', 'e', 0, '5', '1', '2', 0 };
    n = buildOack(pkt, true, 512, false, 0);
    if (n != sizeof(kOack) || memcmp(pkt, kOack, n) != 0) {
        ucxapp_print("ERROR: tftp selftest: OACK encoding\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("tftp selftest: names, options, request and OACK ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gTftpCmds[] = {
    { "tftp.on",     "[rw] [port] [seconds]",              "TFTP server on the module (default port 69, read only; 'rw' allows uploads) serving the working folder; with seconds it serves that long, then stops", UCXAPP_CMD_NEEDS_DEVICE, cmdOn },
    { "tftp.off",    "",                                   "Stop the TFTP server",                                        UCXAPP_CMD_NEEDS_DEVICE, cmdOff },
    { "tftp.status", "",                                   "TFTP server state, transfer count and last transfer",         UCXAPP_CMD_NEEDS_DEVICE, cmdStatus },
    { "tftp.get",    "<host> <remote> [local] [port]",     "Download a file from a TFTP server",                          UCXAPP_CMD_NEEDS_DEVICE, cmdGet },
    { "tftp.put",    "<host> <local> [remote] [port]",     "Upload a file to a TFTP server",                              UCXAPP_CMD_NEEDS_DEVICE, cmdPut },
    { "tftp.selftest", "",                                 "Offline check of TFTP name rules, options and packet encoding", 0, cmdSelftest },
};

void ucxapp_register_tftp_cmds(void)
{
    ucxapp_cmd_register(gTftpCmds, sizeof(gTftpCmds) / sizeof(gTftpCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_tftp_cmds(void)
{
}

#endif
