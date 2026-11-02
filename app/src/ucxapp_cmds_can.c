/* can.* - CAN controller and its own bridge: can.bridge carries fixed 17-byte records (batched per write) both ways.
 * The controller is emulated on Windows and Linux (or a SocketCAN interface on Linux, can.open) and is the board's
 * bxCAN/FDCAN on STM32 (UCXAPP_CAN_HW). The host bridge runs over tcp, udp, ble, uart or cell (ucxapp_stream.c); on
 * the board it runs on its own task over the module's sockets or SPS. */

#include "ucxapp_cmd.h"
#include "ucxapp_can.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>

#include "ucxapp_platform.h"
#include "u_port.h"

#ifndef UCXAPP_CAN_HW
#include "ucxapp_stream.h"
#else
#include "u_cx_socket.h"
#include "u_cx_sps.h"
#endif

#if !defined(UCXAPP_CAN_HW) && defined(__linux__) && !defined(__EMSCRIPTEN__)
#define CAN_SOCKETCAN 1
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define CAN_QUEUE       64
#define CAN_BRIDGE_PORT 7001
#define CAN_POLL_MS     1

static int32_t gBitrate = 500000;
static ucxapp_can_mode_t gMode = UCXAPP_CAN_NORMAL;
static uint32_t gTx;
static uint32_t gRx;
static uint32_t gDropped;
static uint32_t gBadRecords;   /* bridge records with a bad length or DLC */
static U_CX_MUTEX_HANDLE gLock;

static const char *modeName(ucxapp_can_mode_t m)
{
    return m == UCXAPP_CAN_LOOPBACK ? "loopback" : m == UCXAPP_CAN_LISTEN ? "listen-only" : "normal";
}

#ifndef UCXAPP_CAN_HW

/* ---- controller: emulated loopback transceiver, or a SocketCAN interface on Linux ---- */

static ucxapp_can_frame_t gQueue[CAN_QUEUE];
static int gHead;
static int gCount;

#ifdef CAN_SOCKETCAN
static int gSock = -1;
static char gIfName[IFNAMSIZ];

static bool sockSend(const ucxapp_can_frame_t *f)
{
    struct can_frame cf;

    memset(&cf, 0, sizeof(cf));
    cf.can_id = f->id > CAN_SFF_MASK ? ((f->id & CAN_EFF_MASK) | CAN_EFF_FLAG) : f->id;
    cf.can_dlc = f->len > 8 ? 8 : f->len;
    memcpy(cf.data, f->data, cf.can_dlc);
    return send(gSock, &cf, sizeof(cf), MSG_DONTWAIT) == (ssize_t)sizeof(cf);
}

/* Data frames only; remote and error frames are skipped. */
static bool sockRecv(ucxapp_can_frame_t *f)
{
    struct can_frame cf;

    while (recv(gSock, &cf, sizeof(cf), MSG_DONTWAIT) == (ssize_t)sizeof(cf)) {
        if ((cf.can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0) {
            continue;
        }
        f->id = (cf.can_id & CAN_EFF_FLAG) != 0 ? (cf.can_id & CAN_EFF_MASK) : (cf.can_id & CAN_SFF_MASK);
        f->len = cf.can_dlc > 8 ? 8 : cf.can_dlc;
        memcpy(f->data, cf.data, sizeof(f->data));
        return true;
    }
    return false;
}

/* Loopback receives the frames this socket sends (the interface echoes them). */
static void sockApplyMode(void)
{
    int own = gMode == UCXAPP_CAN_LOOPBACK ? 1 : 0;

    (void)setsockopt(gSock, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &own, sizeof(own));
}
#endif

bool ucxapp_can_send(const ucxapp_can_frame_t *f)
{
    bool ok = true;

    U_CX_MUTEX_LOCK(gLock);
    if (gMode == UCXAPP_CAN_LISTEN) {
        ok = false;
#ifdef CAN_SOCKETCAN
    } else if (gSock >= 0) {
        ok = sockSend(f);
#endif
    } else {
        if (gCount == CAN_QUEUE) {
            gHead = (gHead + 1) % CAN_QUEUE;
            gCount--;
            gDropped++;
        }
        gQueue[(gHead + gCount++) % CAN_QUEUE] = *f;
    }
    if (ok) {
        gTx++;
    } else {
        gDropped++;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return ok;
}

bool ucxapp_can_recv(ucxapp_can_frame_t *f)
{
    bool got = false;

    U_CX_MUTEX_LOCK(gLock);
#ifdef CAN_SOCKETCAN
    if (gSock >= 0) {
        got = sockRecv(f);
    } else
#endif
    if (gCount > 0) {
        *f = gQueue[gHead];
        gHead = (gHead + 1) % CAN_QUEUE;
        gCount--;
        got = true;
    }
    if (got) {
        gRx++;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return got;
}

static int queued(void)
{
    int n;

    U_CX_MUTEX_LOCK(gLock);
    n = gCount;
    U_CX_MUTEX_UNLOCK(gLock);
    return n;
}

/* The emulated transceiver has nothing to start; SocketCAN gets the mode, the bit rate is the interface's. */
static const char *start(void)
{
#ifdef CAN_SOCKETCAN
    if (gSock >= 0) {
        sockApplyMode();
    }
#endif
    return NULL;
}

static const char *backendName(void)
{
#ifdef CAN_SOCKETCAN
    if (gSock >= 0) {
        return gIfName;
    }
#endif
    return "emulated";
}

#else

/* ---- controller: the board's CAN peripheral; its receive interrupt fills a single-producer ring ---- */

static ucxapp_can_frame_t gRing[CAN_QUEUE];
static uint32_t gRingHead;   /* next to read, task side */
static uint32_t gRingTail;   /* next to write, interrupt side */
static uint32_t gRxLost;     /* the ring was full */
static bool gStarted;

void ucxapp_can_rx_isr(const ucxapp_can_frame_t *f)
{
    uint32_t t = __atomic_load_n(&gRingTail, __ATOMIC_RELAXED);

    if (t - __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE) >= CAN_QUEUE) {
        gRxLost++;
        return;
    }
    gRing[t % CAN_QUEUE] = *f;
    __atomic_store_n(&gRingTail, t + 1, __ATOMIC_RELEASE);
}

/* Starts the controller with the current settings; NULL or the reason it did not start. */
static const char *start(void)
{
    const char *err = ucxapp_can_hw_start(gBitrate, gMode);

    gStarted = err == NULL;
    return err;
}

bool ucxapp_can_send(const ucxapp_can_frame_t *f)
{
    bool ok;

    U_CX_MUTEX_LOCK(gLock);
    ok = gMode != UCXAPP_CAN_LISTEN && (gStarted || start() == NULL) && ucxapp_can_hw_send(f);
    if (ok) {
        gTx++;
    } else {
        gDropped++;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return ok;
}

bool ucxapp_can_recv(ucxapp_can_frame_t *f)
{
    bool got = false;

    U_CX_MUTEX_LOCK(gLock);
    if (!gStarted) {
        (void)start();
    }
    uint32_t h = gRingHead;
    if (h != __atomic_load_n(&gRingTail, __ATOMIC_ACQUIRE)) {
        *f = gRing[h % CAN_QUEUE];
        __atomic_store_n(&gRingHead, h + 1, __ATOMIC_RELEASE);
        gRx++;
        got = true;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return got;
}

static int queued(void)
{
    return (int)(__atomic_load_n(&gRingTail, __ATOMIC_ACQUIRE) - gRingHead);
}

#endif /* UCXAPP_CAN_HW */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t ucxapp_can_pack(uint32_t ts_ms, const ucxapp_can_frame_t *f, uint8_t *out)
{
    uint8_t len = f->len > 8 ? 8 : f->len;

    put32(out, ts_ms);
    put32(out + 4, f->id);
    out[8] = len;
    memset(out + 9, 0, 8);
    memcpy(out + 9, f->data, len);
    return UCXAPP_CAN_REC_LEN;
}

bool ucxapp_can_unpack(const uint8_t *in, uint32_t *ts_ms, ucxapp_can_frame_t *f)
{
    if (in[8] > 8) {
        return false;
    }
    *ts_ms = get32(in);
    f->id = get32(in + 4);
    f->len = in[8];
    memcpy(f->data, in + 9, 8);
    return true;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    /* ts 0x12345678, id 0x123, dlc 3, data AA BB CC. */
    static const uint8_t expect[UCXAPP_CAN_REC_LEN] = {
        0x78, 0x56, 0x34, 0x12, 0x23, 0x01, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0, 0
    };
    ucxapp_can_frame_t in = { 0x123, 3, { 0xAA, 0xBB, 0xCC } };
    ucxapp_can_frame_t out;
    uint8_t rec[UCXAPP_CAN_REC_LEN];
    uint32_t ts = 0;
    uint8_t bad[UCXAPP_CAN_REC_LEN];

    if (ucxapp_can_pack(0x12345678u, &in, rec) != UCXAPP_CAN_REC_LEN || memcmp(rec, expect, sizeof(expect)) != 0) {
        ucxapp_print("ERROR: CAN record encoding differs\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_can_unpack(rec, &ts, &out) || ts != 0x12345678u || out.id != 0x123 || out.len != 3 ||
        memcmp(out.data, in.data, 3) != 0) {
        ucxapp_print("ERROR: CAN record decoding differs\n");
        return UCXAPP_ERR_FAILED;
    }
    memcpy(bad, rec, sizeof(bad));
    bad[8] = 9;
    if (ucxapp_can_unpack(bad, &ts, &out)) {
        ucxapp_print("ERROR: DLC 9 accepted\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("can selftest: record format ok\n");
    return UCXAPP_OK;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
        return (c | 0x20) - 'a' + 10;
    }
    return -1;
}

/* Data bytes as "11 22", "0x11" or "1122"; -1 on a bad token or more than 8 bytes. */
static int parseData(int argc, char *argv[], int first, uint8_t *out)
{
    int n = 0;

    for (int a = first; a < argc; a++) {
        const char *s = argv[a];
        size_t len;

        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
        len = strlen(s);
        if (len == 0 || (len > 2 && (len & 1))) {
            return -1;
        }
        for (size_t i = 0; i < len; i += (len == 1 ? 1 : 2)) {
            int hi = hexVal(s[i]);
            int lo = len == 1 ? 0 : hexVal(s[i + 1]);
            if (hi < 0 || lo < 0 || n >= 8) {
                return -1;
            }
            out[n++] = (uint8_t)(len == 1 ? hi : hi * 16 + lo);
        }
    }
    return n;
}

static int cmdCfg(ucxapp_t *app, int argc, char *argv[])
{
    int32_t rate;
    ucxapp_can_mode_t mode = UCXAPP_CAN_NORMAL;
    const char *err;

    (void)app;
    if (argc < 2 || argc > 3 || !ucxapp_parse_int(argv[1], &rate) || rate < 10000 || rate > 1000000) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3) {
        if (strcasecmp(argv[2], "loopback") == 0) {
            mode = UCXAPP_CAN_LOOPBACK;
        } else if (strcasecmp(argv[2], "listen") == 0) {
            mode = UCXAPP_CAN_LISTEN;
        } else if (strcasecmp(argv[2], "normal") != 0) {
            return UCXAPP_ERR_USAGE;
        }
    }
    U_CX_MUTEX_LOCK(gLock);
    gBitrate = rate;
    gMode = mode;
    err = start();
    U_CX_MUTEX_UNLOCK(gLock);
    if (err != NULL) {
        ucxapp_print("ERROR: CAN not started: %s\n", err);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("can bitrate %d%s%s\n", (int)rate, mode == UCXAPP_CAN_NORMAL ? "" : ", ",
                 mode == UCXAPP_CAN_NORMAL ? "" : modeName(mode));
#ifdef CAN_SOCKETCAN
    if (gSock >= 0 && strncmp(gIfName, "vcan", 4) != 0) {
        ucxapp_print("note: SocketCAN takes the bit rate%s from the interface: sudo ip link set %s down; "
                     "sudo ip link set %s type can bitrate %d%s; sudo ip link set %s up\n",
                     mode == UCXAPP_CAN_LISTEN ? " and listen-only" : "", gIfName, gIfName, (int)rate,
                     mode == UCXAPP_CAN_LISTEN ? " listen-only on" : "", gIfName);
    }
#endif
    return UCXAPP_OK;
}

#ifndef UCXAPP_CAN_HW
/* can.open [emulated|<interface>]: the emulated loopback, or a SocketCAN interface (can0, vcan0) on Linux. */
static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 1) {
        ucxapp_print("can on %s\n", backendName());
        return UCXAPP_OK;
    }
#ifdef CAN_SOCKETCAN
    U_CX_MUTEX_LOCK(gLock);
    if (gSock >= 0) {
        close(gSock);
        gSock = -1;
    }
    U_CX_MUTEX_UNLOCK(gLock);
#endif
    if (strcasecmp(argv[1], "emulated") == 0) {
        ucxapp_print("can on emulated loopback\n");
        return UCXAPP_OK;
    }
#ifdef CAN_SOCKETCAN
    {
        struct ifreq ifr;
        struct sockaddr_can addr;
        int s;

        if (strlen(argv[1]) >= IFNAMSIZ) {
            return UCXAPP_ERR_USAGE;
        }
        s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (s < 0) {
            ucxapp_print("ERROR: no SocketCAN in this kernel (%s)\n", strerror(errno));
            return UCXAPP_ERR_FAILED;
        }
        memset(&ifr, 0, sizeof(ifr));
        memcpy(ifr.ifr_name, argv[1], strlen(argv[1]));
        if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
            ucxapp_print("ERROR: no CAN interface %s (ip -br link shows them; a CANable is can0 after "
                         "sudo ip link set can0 type can bitrate 500000 up)\n", argv[1]);
            close(s);
            return UCXAPP_ERR_FAILED;
        }
        memset(&addr, 0, sizeof(addr));
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;
        if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ucxapp_print("ERROR: cannot bind to %s (%s)\n", argv[1], strerror(errno));
            close(s);
            return UCXAPP_ERR_FAILED;
        }
        U_CX_MUTEX_LOCK(gLock);
        gSock = s;
        memcpy(gIfName, ifr.ifr_name, sizeof(gIfName));
        sockApplyMode();
        U_CX_MUTEX_UNLOCK(gLock);
        ucxapp_print("can on %s (SocketCAN), %s; the bit rate is the interface's\n", gIfName, modeName(gMode));
        return UCXAPP_OK;
    }
#else
    ucxapp_print("ERROR: SocketCAN interfaces need Linux; here can.* runs on the emulated loopback\n");
    return UCXAPP_ERR_FAILED;
#endif
}
#endif

static int cmdSend(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_can_frame_t f;
    int32_t id;
    int n = 0;

    (void)app;
    memset(&f, 0, sizeof(f));
    if (argc < 2 || !ucxapp_parse_int(argv[1], &id) || id < 0 || id > 0x1FFFFFFF ||
        (argc > 2 && (n = parseData(argc, argv, 2, f.data)) < 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (gMode == UCXAPP_CAN_LISTEN) {
        ucxapp_print("ERROR: CAN is listen-only (can.cfg <bitrate> normal sends)\n");
        return UCXAPP_ERR_FAILED;
    }
    f.id = (uint32_t)id;
    f.len = (uint8_t)n;
    if (!ucxapp_can_send(&f)) {
        ucxapp_print("ERROR: CAN frame not sent (controller not started or every transmit buffer busy, see can.status)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent 0x%X [%d]\n", (unsigned)f.id, n);
    return UCXAPP_OK;
}

static void printFrame(const ucxapp_can_frame_t *f)
{
    ucxapp_print("0x%X [%d]", (unsigned)f->id, f->len);
    for (int i = 0; i < f->len; i++) {
        ucxapp_print(" %02X", f->data[i]);
    }
    ucxapp_print("\n");
}

static int cmdRecv(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_can_frame_t f;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_can_recv(&f)) {
        ucxapp_print("no frame\n");
        return UCXAPP_OK;
    }
    printFrame(&f);
    return UCXAPP_OK;
}

/* can.watch [seconds]: every received frame with its time, until the time is up or a key is pressed. */
static int cmdWatch(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 10;
    uint32_t start0 = ucxapp_platform_tick_ms();
    uint32_t frames = 0;
    ucxapp_can_frame_t f;

    (void)app;
    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &seconds) || seconds < 1 || seconds > 3600))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("watching CAN for %d s (any key stops)\n", (int)seconds);
    while (ucxapp_platform_tick_ms() - start0 < (uint32_t)seconds * 1000u) {
        if (ucxapp_platform_key_pressed()) {
            (void)ucxapp_platform_get_key();
            break;
        }
        if (!ucxapp_can_recv(&f)) {
            ucxapp_platform_sleep_ms(CAN_POLL_MS);
            continue;
        }
        frames++;
        uint32_t ms = ucxapp_platform_tick_ms() - start0;
        ucxapp_print("%4u.%03u ", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u));
        printFrame(&f);
    }
    ucxapp_print("%u frame(s)\n", (unsigned)frames);
    return UCXAPP_OK;
}

/* ---- bridge: 17-byte records both ways ---- */

static uint8_t gPart[UCXAPP_CAN_REC_LEN];
static size_t gPartLen;

/* Records from the far side are sent on the bus; a record may span two writes. */
static void feedRecords(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t take = UCXAPP_CAN_REC_LEN - gPartLen;
        ucxapp_can_frame_t f;
        uint32_t ts;

        if (take > len) {
            take = len;
        }
        memcpy(gPart + gPartLen, data, take);
        gPartLen += take;
        data += take;
        len -= take;
        if (gPartLen == UCXAPP_CAN_REC_LEN) {
            gPartLen = 0;
            if (ucxapp_can_unpack(gPart, &ts, &f)) {
                (void)ucxapp_can_send(&f);
            } else {
                gBadRecords++;
            }
        }
    }
}

/* Up to UCXAPP_CAN_BATCH received frames as records; the record count. */
static size_t takeBatch(uint8_t *batch)
{
    ucxapp_can_frame_t f;
    size_t n = 0;

    while (n < UCXAPP_CAN_BATCH && ucxapp_can_recv(&f)) {
        ucxapp_can_pack(ucxapp_platform_tick_ms(), &f, batch + n * UCXAPP_CAN_REC_LEN);
        n++;
    }
    return n;
}

#ifndef UCXAPP_CAN_HW

static ucxapp_stream_t *gStream;

/* Received frames go out in batches of up to UCXAPP_CAN_BATCH records, one push per poll. */
static void bridgePoll(void *ctx, ucxapp_stream_t *s)
{
    uint8_t batch[UCXAPP_CAN_BATCH * UCXAPP_CAN_REC_LEN];
    size_t n;

    (void)ctx;
    n = takeBatch(batch);
    if (n > 0) {
        ucxapp_stream_push(s, batch, n * UCXAPP_CAN_REC_LEN);
    }
}

static bool bridgeWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    feedRecords(data, len);
    return true;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_cfg_t cfg = { "can", NULL, bridgePoll, CAN_POLL_MS, NULL, bridgeWrite };
    bool off;
    bool status;
    int r;

    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    off = strcasecmp(argv[1], "off") == 0;
    status = strcasecmp(argv[1], "status") == 0;
    if (gStream == NULL && (off || status)) {
        ucxapp_print("bridge: off\n");
        return UCXAPP_OK;
    }
    if (gStream == NULL) {
        gStream = ucxapp_stream_open("virt:can", 0, &cfg);
        if (gStream == NULL) {
            return UCXAPP_ERR_FAILED;
        }
        gPartLen = 0;
    }
    r = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, CAN_BRIDGE_PORT);
    if (!ucxapp_stream_bridge_active(gStream)) {
        ucxapp_stream_close(gStream);
        gStream = NULL;
    }
    return r;
}

static const char *bridgeState(void)
{
    return gStream != NULL ? "on" : "off";
}

#else

/* ---- bridge on the board: records through the module (TCP, UDP or SPS) from its own task; one at a time ---- */

#define BR_RETRY_MS      2000
#define BR_BLE_CHUNK     120u
#define BR_BLE_CHUNK_MIN 20u
#define BR_READS         4
#define BR_IDLE_MS       20

enum { BR_OFF, BR_TCP, BR_TCPC, BR_UDP, BR_SPS };
static const char *const kBrNames[] = { "off", "tcp", "tcpc", "udp", "ble sps" };

static U_CX_MUTEX_HANDLE gBrLock;
static volatile int gBr = BR_OFF;
static bool gBrTask;
static ucxapp_t *gBrApp;
static int32_t gBrSock = -1;               /* the UDP socket, the TCP client or the accepted TCP peer */
static int32_t gBrListenSock = -1;
static volatile int32_t gBrAccepted = -1;  /* tcp: peer accepted in the URC, adopted by the task */
static volatile bool gBrClosed;
static volatile bool gBrPending;           /* data arrived since the last read */
static int32_t gBrSpsConn = -1;
static uSockIpAddress_t gBrAddr;
static int32_t gBrPort;                    /* udp, tcpc: far port; udps: 0 until someone writes */
static int32_t gBrLocal;
static bool gBrUdpListen;
static char gBrHost[64];
static uint32_t gBrNextTry;
static size_t gBrChunk;
static uint32_t gBrSent;                   /* bytes */
static uint32_t gBrRecv;
static uint32_t gBrLost;                   /* bytes nobody took (no peer yet, or the write failed) */
static uint8_t gBrOut[UCXAPP_CAN_BATCH * UCXAPP_CAN_REC_LEN];
static uint8_t gBrIn[256];

/* Module URCs (ucxapp_urc.c), from the AT client's receive task: only flags here, the bridge task does the AT. */
void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock)
{
    if (gBr == BR_TCP && listenSock == gBrListenSock) {
        gBrAccepted = sock;
    }
}

void ucxapp_stream_on_closed(int32_t sock)
{
    if (gBr != BR_OFF && sock == gBrSock) {
        gBrClosed = true;
    }
}

void ucxapp_stream_on_data(int32_t sock)
{
    if (gBr != BR_OFF && sock == gBrSock) {
        gBrPending = true;
    }
}

void ucxapp_stream_on_sps_data(void)
{
    if (gBr == BR_SPS) {
        gBrPending = true;
    }
}

static bool due(uint32_t at)
{
    return (int32_t)(ucxapp_platform_tick_ms() - at) >= 0;
}

static void brConnect(uCxHandle_t *ucx)
{
    int32_t sock;

    gBrNextTry = ucxapp_platform_tick_ms() + BR_RETRY_MS;
    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        return;
    }
    if (uCxSocketConnect(ucx, sock, gBrHost, gBrPort) != 0) {
        (void)uCxSocketClose(ucx, sock);
        return;
    }
    gBrSock = sock;
    gBrPending = true;
}

static bool brSpsReady(uCxHandle_t *ucx)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    if (conn < 0) {
        gBrSpsConn = -1;
        return false;
    }
    if (conn != gBrSpsConn) {
        if (uCxSpsConnect1(ucx, conn) != 0) {
            return false;
        }
        gBrSpsConn = conn;
    }
    return true;
}

static bool brSend(uCxHandle_t *ucx, const uint8_t *d, size_t n)
{
    size_t off = 0;

    switch (gBr) {
    case BR_TCP:
    case BR_TCPC:
        return gBrSock >= 0 && uCxSocketWrite(ucx, gBrSock, d, (int32_t)n) >= 0;
    case BR_UDP:
        return gBrPort > 0 && uCxSocketSendTo(ucx, gBrSock, &gBrAddr, gBrPort, d, (int32_t)n) >= 0;
    case BR_SPS:
        if (!brSpsReady(ucx)) {
            return false;
        }
        while (off < n) {
            size_t piece = n - off < gBrChunk ? n - off : gBrChunk;
            bool ok = uCxSpsWrite(ucx, gBrSpsConn, d + off, (int32_t)piece) >= 0;
            if (!ok && gBrChunk > BR_BLE_CHUNK_MIN) {
                gBrChunk = BR_BLE_CHUNK_MIN;   /* the link has the default 23 byte MTU */
                piece = piece < gBrChunk ? piece : gBrChunk;
                ok = uCxSpsWrite(ucx, gBrSpsConn, d + off, (int32_t)piece) >= 0;
            }
            if (!ok) {
                return false;
            }
            off += piece;
        }
        return true;
    default:
        return false;
    }
}

static void brPull(uCxHandle_t *ucx)
{
    int32_t n = 0;

    if (gBr == BR_UDP) {
        uCxSocketReceiveFrom_t from;
        for (int i = 0; i < BR_READS; i++) {
            n = ucxapp_udp_recvfrom(ucx, gBrSock, (int32_t)sizeof(gBrIn), gBrIn, &from);
            if (n <= 0) {
                break;
            }
            if (gBrUdpListen) {
                gBrAddr = from.remote_ip;
                gBrPort = from.remote_port;
            }
            gBrRecv += (uint32_t)n;
            feedRecords(gBrIn, (size_t)n);
        }
    } else if ((gBr == BR_TCP || gBr == BR_TCPC) && gBrSock >= 0) {
        for (int i = 0; i < BR_READS; i++) {
            n = uCxSocketRead(ucx, gBrSock, (int32_t)sizeof(gBrIn), gBrIn);
            if (n <= 0) {
                break;
            }
            gBrRecv += (uint32_t)n;
            feedRecords(gBrIn, (size_t)n);
        }
    } else if (gBr == BR_SPS && gBrSpsConn >= 0) {
        /* One read per data event: a read that overlaps the module raising the next one asserts NORA-W36 3.5.0. */
        n = uCxSpsRead(ucx, gBrSpsConn, (int32_t)sizeof(gBrIn), gBrIn);
        if (n > 0) {
            gBrRecv += (uint32_t)n;
            feedRecords(gBrIn, (size_t)n);
        }
        return;
    }
    if (n > 0) {
        gBrPending = true;   /* more than BR_READS reads waiting */
    }
}

/* One round; true if it moved data. */
static bool brService(uCxHandle_t *ucx)
{
    bool busy = false;
    size_t n;

    if (gBrClosed) {
        gBrClosed = false;
        if (gBr == BR_TCP || gBr == BR_TCPC) {
            gBrSock = -1;
            gBrNextTry = ucxapp_platform_tick_ms() + BR_RETRY_MS;
        }
    }
    if (gBr == BR_TCP && gBrAccepted >= 0) {
        int32_t s = gBrAccepted;
        gBrAccepted = -1;
        if (gBrSock >= 0) {
            (void)uCxSocketClose(ucx, gBrSock);   /* the newest peer takes over */
        }
        gBrSock = s;
        gBrPending = true;
    }
    if (gBr == BR_TCPC && gBrSock < 0 && due(gBrNextTry)) {
        brConnect(ucx);
    }
    if (gBr == BR_SPS) {
        (void)brSpsReady(ucx);
    }
    n = takeBatch(gBrOut);
    if (n > 0) {
        size_t bytes = n * UCXAPP_CAN_REC_LEN;
        if (brSend(ucx, gBrOut, bytes)) {
            gBrSent += (uint32_t)bytes;
        } else {
            gBrLost += (uint32_t)bytes;
        }
        busy = true;
    }
    if (gBrPending) {
        gBrPending = false;
        brPull(ucx);
        busy = true;
    }
    return busy;
}

static void brTask(void)
{
    for (;;) {
        bool busy = false;
        bool on;

        U_CX_MUTEX_LOCK(gBrLock);
        on = gBr != BR_OFF;
        if (on && gBrApp->ucx != NULL) {
            busy = brService(gBrApp->ucx);
        }
        U_CX_MUTEX_UNLOCK(gBrLock);
        if (!busy) {
            ucxapp_platform_sleep_ms(on ? CAN_POLL_MS : BR_IDLE_MS);
        }
    }
}

static void brStop(uCxHandle_t *ucx)
{
    U_CX_MUTEX_LOCK(gBrLock);
    if (ucx != NULL) {
        if (gBrSock >= 0) {
            (void)uCxSocketClose(ucx, gBrSock);
        }
        if (gBrListenSock >= 0) {
            (void)uCxSocketClose(ucx, gBrListenSock);
        }
    }
    gBr = BR_OFF;
    gBrSock = -1;
    gBrListenSock = -1;
    gBrAccepted = -1;
    gBrSpsConn = -1;
    U_CX_MUTEX_UNLOCK(gBrLock);
}

static bool brUdp(uCxHandle_t *ucx, const char *host, int32_t port, int32_t local)
{
    int32_t bindPort = local > 0 ? local : port;

    gBrUdpListen = host == NULL;
    if (host != NULL && uCxStringToIpAddress(host, &gBrAddr) != 0 && uCxSocketGetHostByName(ucx, host, &gBrAddr) != 0) {
        ucxapp_print("ERROR: cannot resolve %s\n", host);
        return false;
    }
    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_UDP, &gBrSock) != 0) {
        ucxapp_print("ERROR: cannot create UDP socket (is Wi-Fi connected?)\n");
        gBrSock = -1;
        return false;
    }
    if (uCxSocketBind(ucx, gBrSock, bindPort) != 0) {
        if (local > 0 || host == NULL) {
            ucxapp_print("ERROR: cannot bind UDP port %d\n", (int)bindPort);
            return false;
        }
        ucxapp_print("note: UDP port %d is busy, receiving is off\n", (int)bindPort);
    }
    gBrPort = host != NULL ? port : 0;
    gBrLocal = bindPort;
    return true;
}

static int brStart(ucxapp_t *app, int kind, const char *host, int32_t port, int32_t local)
{
    uCxHandle_t *ucx = app->ucx;
    bool ok = true;

    if (ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (gBr != BR_OFF) {
        ucxapp_print("ERROR: the %s bridge is on (can.bridge off first)\n", kBrNames[gBr]);
        return UCXAPP_ERR_FAILED;
    }
    if (!gBrTask) {
        gBrTask = ucxapp_can_hw_task(brTask);
        if (!gBrTask) {
            ucxapp_print("ERROR: no memory for the bridge task\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    gBrApp = app;
    gBrSent = gBrRecv = gBrLost = 0;
    gBrPending = gBrClosed = false;
    gBrChunk = BR_BLE_CHUNK;
    gPartLen = 0;
    if (kind == BR_TCP) {
        if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &gBrListenSock) != 0 ||
            uCxSocketListen2(ucx, gBrListenSock, port) != 0) {
            ucxapp_print("ERROR: cannot listen on TCP port %d (is Wi-Fi connected?)\n", (int)port);
            ok = false;
        }
    } else if (kind == BR_TCPC) {
        snprintf(gBrHost, sizeof(gBrHost), "%s", host);
        gBrPort = port;
        brConnect(ucx);
        if (gBrSock < 0) {
            ucxapp_print("note: %s:%d does not answer yet, retrying every %d s\n", host, (int)port, BR_RETRY_MS / 1000);
        }
    } else if (kind == BR_UDP) {
        ok = brUdp(ucx, host, port, local);
    } else if (uCxSpsSetServiceEnable(ucx, U_SPS_SERVICE_OPTION_ENABLE) != 0 ||
               uCxSpsSetDataMode(ucx, U_READ_MODE_BUFFERED) != 0) {
        ucxapp_print("ERROR: cannot enable the SPS service\n");
        ok = false;
    } else {
        ucxapp_gatt_ensure_advertising(app);
    }
    if (!ok) {
        brStop(ucx);
        return UCXAPP_ERR_FAILED;
    }
    U_CX_MUTEX_LOCK(gBrLock);
    gBr = kind;
    U_CX_MUTEX_UNLOCK(gBrLock);
    if (kind == BR_TCP) {
        ucxapp_print("can bridge: TCP server on port %d\n", (int)port);
    } else if (kind == BR_TCPC) {
        ucxapp_print("can bridge: TCP to %s:%d\n", host, (int)port);
    } else if (kind == BR_UDP && host != NULL) {
        ucxapp_print("can bridge: UDP to %s:%d, listening on %d\n", host, (int)port, (int)gBrLocal);
    } else if (kind == BR_UDP) {
        ucxapp_print("can bridge: UDP listening on %d, replies go to the last sender\n", (int)gBrLocal);
    } else {
        ucxapp_print("can bridge: connect a BLE client to the u-blox Serial Port Service\n");
    }
    return UCXAPP_OK;
}

static bool parsePort(const char *s, int32_t *port)
{
    return ucxapp_parse_int(s, port) && *port >= 1 && *port <= 65535;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = CAN_BRIDGE_PORT;
    int32_t local = 0;

    if (argc < 2 || argc > 5) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "status") == 0 && argc == 2) {
        if (gBr == BR_OFF) {
            ucxapp_print("bridge: off\n");
        } else {
            ucxapp_print("bridge: %s%s, sent %u B, received %u B, not delivered %u B\n", kBrNames[gBr],
                         (gBr == BR_TCP || gBr == BR_TCPC) && gBrSock < 0 ? " (no peer)" : "", (unsigned)gBrSent,
                         (unsigned)gBrRecv, (unsigned)gBrLost);
        }
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0 && argc == 2) {
        bool was = gBr != BR_OFF;
        brStop(app->ucx);
        ucxapp_print("%s\n", was ? "bridge stopped" : "bridge: off");
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "tcp") == 0 && argc <= 3) {
        if (argc == 3 && !parsePort(argv[2], &port)) {
            return UCXAPP_ERR_USAGE;
        }
        return brStart(app, BR_TCP, NULL, port, 0);
    }
    if (strcasecmp(argv[1], "tcpc") == 0 && argc == 4) {
        if (strlen(argv[2]) >= sizeof(gBrHost) || !parsePort(argv[3], &port)) {
            return UCXAPP_ERR_USAGE;
        }
        return brStart(app, BR_TCPC, argv[2], port, 0);
    }
    if (strcasecmp(argv[1], "udp") == 0 && (argc == 4 || argc == 5)) {
        if (!parsePort(argv[3], &port) || (argc == 5 && !parsePort(argv[4], &local))) {
            return UCXAPP_ERR_USAGE;
        }
        return brStart(app, BR_UDP, argv[2], port, local);
    }
    if (strcasecmp(argv[1], "udps") == 0 && argc == 3) {
        if (!parsePort(argv[2], &port)) {
            return UCXAPP_ERR_USAGE;
        }
        return brStart(app, BR_UDP, NULL, 0, port);
    }
    if (strcasecmp(argv[1], "ble") == 0 && argc == 3 && strcasecmp(argv[2], "sps") == 0) {
        return brStart(app, BR_SPS, NULL, 0, 0);
    }
    return UCXAPP_ERR_USAGE;
}

static const char *bridgeState(void)
{
    return kBrNames[gBr];
}

#endif /* UCXAPP_CAN_HW */

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
#ifdef UCXAPP_CAN_HW
    {
        ucxapp_can_hw_state_t st = { 0, 0, "stopped" };
        if (gStarted) {
            ucxapp_can_hw_state(&st);
        }
        ucxapp_print("can %s, %d bit/s, %s, %s (tec %u, rec %u)\n", ucxapp_can_hw_name(), (int)gBitrate,
                     modeName(gMode), st.state, (unsigned)st.tec, (unsigned)st.rec);
        ucxapp_print("tx %u, rx %u, queued %d, dropped %u, lost %u, bad records %u, bridge %s\n", (unsigned)gTx,
                     (unsigned)gRx, queued(), (unsigned)gDropped, (unsigned)gRxLost, (unsigned)gBadRecords,
                     bridgeState());
    }
#else
    ucxapp_print("can %s%s, %s, %d bit/s, tx %u, rx %u, queued %d, dropped %u, bad records %u, bridge %s\n",
                 backendName(), strcmp(backendName(), "emulated") == 0 ? " loopback" : " (SocketCAN)",
                 modeName(gMode), (int)gBitrate, (unsigned)gTx, (unsigned)gRx, queued(), (unsigned)gDropped,
                 (unsigned)gBadRecords, bridgeState());
#endif
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gCanCmds[] = {
    { "can.cfg",      "<bitrate> [normal|loopback|listen]", "Set the CAN bit rate and mode and (re)start the controller; "
      "loopback receives its own frames without a bus, listen only receives (no acknowledge, nothing sent)", 0, cmdCfg },
#ifndef UCXAPP_CAN_HW
    { "can.open",     "[emulated|<interface>]", "CAN on the emulated loopback (default) or a SocketCAN interface "
      "(Linux: can0 for a CANable, vcan0); no argument shows which", 0, cmdOpen },
#endif
    { "can.send",     "<id> [hex...]", "Send a CAN frame (id above 0x7FF is extended)", 0, cmdSend },
    { "can.recv",     "",              "Take the next received CAN frame", 0, cmdRecv },
    { "can.watch",    "[seconds]",     "Print received CAN frames with their time (default 10 s, any key stops)", 0, cmdWatch },
    { "can.status",   "",              "Controller, counters and bridge state", 0, cmdStatus },
#ifndef UCXAPP_CAN_HW
    { "can.bridge",   "tcp [port] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | "
      "uart <port> [baud] | cell | off [n] | status", "Carry CAN frames both ways as 17-byte records (ts, id, dlc, data[8]); cell sends them over the cellular data link (cell.connect) and reconnects it when it drops; up to 4 bridges at once", 0,
      cmdBridge },
#else
    { "can.bridge",   "tcp [port] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble sps | off | status",
      "Carry CAN frames both ways as 17-byte records (ts, id, dlc, data[8]) through the module, from a task of "
      "its own; one bridge at a time", 0, cmdBridge },
#endif
    { "can.selftest", "",              "Offline check of the CAN record format", 0, cmdSelftest },
};

void ucxapp_register_can_cmds(void)
{
    static bool init;

    if (!init) {
        U_CX_MUTEX_CREATE(gLock);
#ifdef UCXAPP_CAN_HW
        U_CX_MUTEX_CREATE(gBrLock);
#endif
        init = true;
    }
    ucxapp_cmd_register(gCanCmds, sizeof(gCanCmds) / sizeof(gCanCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_can_cmds(void)
{
}

#endif /* UCXAPP_LITE */
