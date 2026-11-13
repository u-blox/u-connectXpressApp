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

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_gatt_server.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef UCXAPP_CAN_HW
#include "ucxapp_stream.h"
#include "ucxapp_thread.h"
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
    if (ucxapp_module_is_virtual(app)) {
        ucxapp_print("SocketCAN needs Linux; %s is the emulated loopback for the virtual module demo\n", argv[1]);
        return UCXAPP_OK;
    }
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

/* ---- bridge: 17-byte records both ways (SLCAN text over NUS is the exception) ---- */

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

/* Bridges run by this file (on the board all of them, on the host the BLE ones; the host's other bridges are
   ucxapp_stream.c's). One at a time, serviced from a task (board) or thread (host) of their own under gBrLock. */
#ifdef UCXAPP_CAN_HW
enum { BR_OFF, BR_GATT, BR_SLCAN, BR_TCP, BR_TCPC, BR_UDP, BR_SPS };
static const char *const kBrNames[] = { "off", "ble gatt", "ble slcan", "tcps", "tcpc", "udp", "ble sps" };
#else
enum { BR_OFF, BR_GATT, BR_SLCAN };
static const char *const kBrNames[] = { "off", "ble gatt", "ble slcan" };
#endif

static U_CX_MUTEX_HANDLE gBrLock;
static volatile int gBr = BR_OFF;
static ucxapp_t *gBrApp;
static uint32_t gBrSent;                   /* bytes */
static uint32_t gBrRecv;
static uint32_t gBrLost;                   /* bytes nobody took (no peer yet, or the write failed) */
static uint8_t gBrOut[UCXAPP_CAN_BATCH * UCXAPP_CAN_REC_LEN];
static uint8_t gBrIn[256];

static bool due(uint32_t at)
{
    return (int32_t)(ucxapp_platform_tick_ms() - at) >= 0;
}

/* ---- BLE bridge: the u-blox CAN GATT service, or SLCAN (Lawicel) text over the Nordic UART Service ----
 * Service 81ea0001-f62b-4fec-94ee-7169818db897
 *   RX     81ea0002-...  Write, Write Without Response: records to send on the bus (a record may span writes)
 *   TX     81ea0003-...  Notify: received frames, as many records per notification as the MTU takes
 *   Config 81ea0004-...  Read, Write: bit rate u32 LE, mode u8 (0 normal, 1 loopback, 2 listen-only)
 *   Status 81ea0005-...  Read, Notify: bus rx, bus tx, dropped, bad records (u32 LE each), once a second */

#define BLE_IN_SIZE   1024
#define BLE_REC_MAX   14u      /* (247 - 3) / 17 records per notification at the largest MTU */
#define BLE_PAYLOAD   20u      /* notification payload at the default MTU of 23 */
#define BLE_PAYLOAD_MAX 244u
#define BLE_STATUS_MS 1000
#define BLE_RETRY_MS  1000
#define BLE_PROP_READ     0x02
#define BLE_PROP_WRITE_NR 0x04
#define BLE_PROP_WRITE    0x08
#define BLE_PROP_NOTIFY   0x10
#define SL_LINE       40
#define SL_OUT        512
#define SL_FRAME_MAX  32       /* T + 8 id + dlc + 16 data + 4 timestamp + CR */

#define CAN_UUID(n) { 0x81, 0xea, 0x00, (n), 0xf6, 0x2b, 0x4f, 0xec, 0x94, 0xee, 0x71, 0x69, 0x81, 0x8d, 0xb8, 0x97 }

enum { H_RX, H_TX, H_CFG, H_STAT, H_COUNT };

static bool gBleDefined;
static int32_t gH[H_COUNT];
static int32_t gHCccd[H_COUNT];
static U_CX_MUTEX_HANDLE gInLock;          /* the URC task fills gBleIn, the bridge task empties it */
static uint8_t gBleIn[BLE_IN_SIZE];
static size_t gBleInHead;
static size_t gBleInLen;
static uint32_t gBleInDropped;             /* bytes of writes that did not fit */
static uint8_t gCfgNew[5];
static volatile bool gCfgPending;
static volatile bool gBleWake;             /* a CCCD write: ask for the MTU again and retry notifying */
static volatile bool gStatSub;
static int32_t gBleConn;
static size_t gBlePayload;
static bool gTxOk;
static uint32_t gTxRetry;
static uint32_t gLostFrames;
static uint32_t gStatAt;
static uint8_t gStatLast[16];
static int32_t gCfgBitrate;                /* what the config characteristic holds */
static ucxapp_can_mode_t gCfgMode;

static bool gSlOpen;
static bool gSlTs;
static bool gSlLong;                       /* the current line is too long; it is answered with an error */
static int32_t gSlRate;
static char gSlLine[SL_LINE];
static size_t gSlLineLen;
static uint8_t gSlOut[SL_OUT];
static size_t gSlOutLen;

/* URC task (ucxapp_cmds_gatt.c): only copies and flags here, the bridge task does the AT work. A write that does
   not fit is dropped whole so records stay aligned. */
static void bleInPut(const uint8_t *d, size_t n)
{
    U_CX_MUTEX_LOCK(gInLock);
    if (gBleInLen + n > BLE_IN_SIZE) {
        gBleInDropped += (uint32_t)n;
    } else {
        for (size_t i = 0; i < n; i++) {
            gBleIn[(gBleInHead + gBleInLen++) % BLE_IN_SIZE] = d[i];
        }
    }
    U_CX_MUTEX_UNLOCK(gInLock);
}

static size_t bleInTake(uint8_t *out, size_t max)
{
    size_t n = 0;

    U_CX_MUTEX_LOCK(gInLock);
    while (n < max && gBleInLen > 0) {
        out[n++] = gBleIn[gBleInHead];
        gBleInHead = (gBleInHead + 1) % BLE_IN_SIZE;
        gBleInLen--;
    }
    U_CX_MUTEX_UNLOCK(gInLock);
    return n;
}

bool ucxapp_can_gatt_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    if (!gBleDefined || handle <= 0) {
        return false;
    }
    if (handle == gH[H_RX]) {
        if (gBr == BR_GATT) {
            bleInPut(data, len);
        }
    } else if (handle == gH[H_CFG]) {
        if (len == sizeof(gCfgNew)) {
            U_CX_MUTEX_LOCK(gInLock);
            memcpy(gCfgNew, data, sizeof(gCfgNew));
            gCfgPending = true;
            U_CX_MUTEX_UNLOCK(gInLock);
        }
    } else if (handle == gHCccd[H_TX]) {
        gBleWake = true;
    } else if (handle == gHCccd[H_STAT]) {
        gStatSub = len >= 1 && (data[0] & 0x01) != 0;
    } else {
        return false;
    }
    return true;
}

bool ucxapp_can_nus_rx(const uint8_t *data, size_t len)
{
    if (gBr != BR_SLCAN) {
        return false;
    }
    bleInPut(data, len);
    return true;
}

static void cfgBytes(uint8_t out[5])
{
    put32(out, (uint32_t)gBitrate);
    out[4] = (uint8_t)gMode;
}

static bool bleDefine(ucxapp_t *app)
{
    static const uint8_t kSvc[16] = CAN_UUID(0x01);
    static const uint8_t kUuid[H_COUNT][16] = { CAN_UUID(0x02), CAN_UUID(0x03), CAN_UUID(0x04), CAN_UUID(0x05) };
    static const uint8_t kProps[H_COUNT] = { BLE_PROP_WRITE | BLE_PROP_WRITE_NR, BLE_PROP_NOTIFY,
                                             BLE_PROP_READ | BLE_PROP_WRITE, BLE_PROP_READ | BLE_PROP_NOTIFY };
    static const uint8_t kLen[H_COUNT] = { 1, 1, 5, 16 };
    uint8_t init[16];
    int32_t svc;
    int32_t r;

    if (gBleDefined) {
        return true;
    }
    if (!ucxapp_gatt_server_fresh(app)) {
        return false;
    }
    r = uCxGattServerServiceDefine(app->ucx, kSvc, 16, &svc);
    for (int i = 0; r == 0 && i < H_COUNT; i++) {
        uint8_t props = kProps[i];
        uCxGattServerCharDefine_t rsp;

        memset(init, 0, sizeof(init));
        if (i == H_CFG) {
            cfgBytes(init);
        }
        /* Writing to the bus or changing its bit rate needs an encrypted link. */
        r = uCxGattServerCharDefine6(app->ucx, kUuid[i], 16, &props, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     (props & (BLE_PROP_WRITE | BLE_PROP_WRITE_NR)) != 0
                                         ? U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED
                                         : U_GATT_SERVER_WRITE_SECURITY_NONE,
                                     init, kLen[i], (int32_t)BLE_PAYLOAD_MAX, &rsp);
        if (r == 0) {
            gH[i] = rsp.value_handle;
            gHCccd[i] = rsp.cccd_handle;
        }
    }
    if (r == 0) {
        r = uCxGattServerServiceActivate(app->ucx);
    }
    if (r != 0) {
        ucxapp_print("ERROR: cannot define the CAN GATT service (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    gBleDefined = true;
    return true;
}

static void bleMtu(uCxHandle_t *ucx)
{
    int32_t mtu = 0;

    gBlePayload = BLE_PAYLOAD;
    if (gBleConn >= 0 && uCxBluetoothGetConnectionStatus(ucx, gBleConn, U_BT_PROP_ID_MTU_SIZE, &mtu) == 0 &&
        mtu > 23) {
        gBlePayload = (size_t)mtu - 3 > BLE_PAYLOAD_MAX ? BLE_PAYLOAD_MAX : (size_t)mtu - 3;
    }
}

/* Config characteristic write: bit rate and mode, applied like can.cfg. */
static void bleApplyCfg(void)
{
    uint8_t c[5];
    uint32_t rate;

    U_CX_MUTEX_LOCK(gInLock);
    memcpy(c, gCfgNew, sizeof(c));
    gCfgPending = false;
    U_CX_MUTEX_UNLOCK(gInLock);
    rate = get32(c);
    if (rate < 10000 || rate > 1000000 || c[4] > UCXAPP_CAN_LISTEN) {
        gCfgBitrate = -1;   /* republish the settings in use */
        return;
    }
    U_CX_MUTEX_LOCK(gLock);
    gBitrate = (int32_t)rate;
    gMode = (ucxapp_can_mode_t)c[4];
    (void)start();
    U_CX_MUTEX_UNLOCK(gLock);
}

static bool bleNotify(uCxHandle_t *ucx, const uint8_t *d, size_t n)
{
    if (gBr == BR_GATT) {
        return uCxGattServerSendNotification(ucx, gBleConn, gH[H_TX], d, (int32_t)n) == 0;
    }
    return ucxapp_gatt_notify_nus(gBrApp, d, n);
}

/* Sends n bytes in pieces of at most `piece`; on a failure the rest is lost and sending pauses for a while. */
static void bleSend(uCxHandle_t *ucx, const uint8_t *d, size_t n, size_t piece)
{
    size_t off = 0;

    if (gBleConn >= 0 && !gTxOk && due(gTxRetry)) {
        gTxOk = true;
        bleMtu(ucx);
    }
    while (off < n && gBleConn >= 0 && gTxOk) {
        size_t k = n - off < piece ? n - off : piece;
        if (!bleNotify(ucx, d + off, k)) {
            gTxOk = false;   /* not subscribed, or the link went away */
            gTxRetry = ucxapp_platform_tick_ms() + BLE_RETRY_MS;
            break;
        }
        gBrSent += (uint32_t)k;
        off += k;
    }
    gBrLost += (uint32_t)(n - off);
    if (gBr == BR_GATT) {
        gLostFrames += (uint32_t)((n - off) / UCXAPP_CAN_REC_LEN);
    }
}

/* ---- SLCAN ---- */

static void slPut(const char *s, size_t n)
{
    if (gSlOutLen + n > SL_OUT) {
        gBrLost += (uint32_t)n;
        return;
    }
    memcpy(gSlOut + gSlOutLen, s, n);
    gSlOutLen += n;
}

static bool slHex(const char *s, size_t n, uint32_t *v)
{
    *v = 0;
    for (size_t i = 0; i < n; i++) {
        int h = hexVal(s[i]);
        if (h < 0) {
            return false;
        }
        *v = *v * 16u + (uint32_t)h;
    }
    return true;
}

/* tIIILDD.. or TIIIIIIIILDD..; the reply, or NULL for an error. */
static const char *slFrame(const char *l, size_t n)
{
    bool ext = l[0] == 'T';
    size_t idLen = ext ? 8 : 3;
    uint32_t id;
    uint32_t dlc;
    ucxapp_can_frame_t f;

    if (n < idLen + 2 || !slHex(l + 1, idLen, &id) || !slHex(l + 1 + idLen, 1, &dlc) || dlc > 8 ||
        n != idLen + 2 + dlc * 2 || id > (ext ? 0x1FFFFFFFu : 0x7FFu)) {
        return NULL;
    }
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.len = (uint8_t)dlc;
    for (uint32_t i = 0; i < dlc; i++) {
        uint32_t b;
        if (!slHex(l + idLen + 2 + i * 2, 2, &b)) {
            return NULL;
        }
        f.data[i] = (uint8_t)b;
    }
    if (!gSlOpen || !ucxapp_can_send(&f)) {
        return NULL;
    }
    return ext ? "Z\r" : "z\r";
}

static void slCommand(const char *l, size_t n)
{
    static const int32_t kRates[9] = { 10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000 };
    const char *reply = NULL;

    switch (l[0]) {
    case 'S':
        if (n == 2 && !gSlOpen && l[1] >= '0' && l[1] <= '8') {
            gSlRate = kRates[l[1] - '0'];
            reply = "\r";
        }
        break;
    case 'O':
    case 'L':
        if (n == 1 && !gSlOpen) {
            const char *err;
            U_CX_MUTEX_LOCK(gLock);
            gBitrate = gSlRate;
            gMode = l[0] == 'O' ? UCXAPP_CAN_NORMAL : UCXAPP_CAN_LISTEN;
            err = start();
            U_CX_MUTEX_UNLOCK(gLock);
            gSlOpen = err == NULL;
            reply = gSlOpen ? "\r" : NULL;
        }
        break;
    case 'C':
        if (n == 1) {
            gSlOpen = false;
            reply = "\r";
        }
        break;
    case 't':
    case 'T':
        reply = slFrame(l, n);
        break;
    case 'V':
        reply = n == 1 ? "V1013\r" : NULL;
        break;
    case 'N':
        reply = n == 1 ? "NUCX1\r" : NULL;
        break;
    case 'F':
        reply = n == 1 ? "F00\r" : NULL;
        break;
    case 'Z':
        if (n == 2 && (l[1] == '0' || l[1] == '1')) {
            gSlTs = l[1] == '1';
            reply = "\r";
        }
        break;
    case 'M':
    case 'm':
        reply = n == 9 ? "\r" : NULL;   /* acceptance filters are not applied: every frame passes */
        break;
    case 'X':
        reply = n == 2 ? "\r" : NULL;
        break;
    default:
        break;
    }
    slPut(reply != NULL ? reply : "\a", reply != NULL ? strlen(reply) : 1);
}

static void slFeed(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\r' || c == '\n') {
            if (gSlLong) {
                slPut("\a", 1);
            } else if (gSlLineLen > 0) {
                slCommand(gSlLine, gSlLineLen);
            }
            gSlLineLen = 0;
            gSlLong = false;
        } else if (gSlLineLen < SL_LINE) {
            gSlLine[gSlLineLen++] = c;
        } else {
            gSlLong = true;
        }
    }
}

static void slEncode(const ucxapp_can_frame_t *f)
{
    char s[SL_FRAME_MAX + 8];
    int n = snprintf(s, sizeof(s), f->id > 0x7FF ? "T%08X%u" : "t%03X%u", (unsigned)f->id, (unsigned)f->len);

    for (int i = 0; i < f->len && i < 8; i++) {
        n += snprintf(s + n, sizeof(s) - (size_t)n, "%02X", f->data[i]);
    }
    if (gSlTs) {
        n += snprintf(s + n, sizeof(s) - (size_t)n, "%04X", (unsigned)(ucxapp_platform_tick_ms() % 60000u));
    }
    s[n++] = '\r';
    slPut(s, (size_t)n);
}

/* ---- the BLE bridge round, from the bridge task with gBrLock held; true if it moved data ---- */

static void bleStatus(uCxHandle_t *ucx)
{
    uint8_t st[16];
    uint32_t dropped = gDropped + gLostFrames + gBleInDropped / UCXAPP_CAN_REC_LEN;

#ifdef UCXAPP_CAN_HW
    dropped += gRxLost;
#endif
    if (gCfgBitrate != gBitrate || gCfgMode != gMode) {
        uint8_t c[5];
        cfgBytes(c);
        if (uCxGattServerSetAttrValue(ucx, gH[H_CFG], c, (int32_t)sizeof(c)) == 0) {
            gCfgBitrate = gBitrate;
            gCfgMode = gMode;
        }
    }
    if (!due(gStatAt)) {
        return;
    }
    gStatAt = ucxapp_platform_tick_ms() + BLE_STATUS_MS;
    put32(st, gRx);
    put32(st + 4, gTx);
    put32(st + 8, dropped);
    put32(st + 12, gBadRecords);
    if (memcmp(st, gStatLast, sizeof(st)) == 0 ||
        uCxGattServerSetAttrValue(ucx, gH[H_STAT], st, (int32_t)sizeof(st)) != 0) {
        return;
    }
    memcpy(gStatLast, st, sizeof(st));
    if (gStatSub && gBleConn >= 0) {
        (void)uCxGattServerSendNotification(ucx, gBleConn, gH[H_STAT], st, (int32_t)sizeof(st));
    }
}

static bool bleService(uCxHandle_t *ucx)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);
    bool busy = false;
    size_t n;

    if (conn != gBleConn || gBleWake) {
        gBleConn = conn;
        gBleWake = false;
        gTxOk = true;
        bleMtu(ucx);
    }
    if (gCfgPending) {
        bleApplyCfg();
    }
    n = bleInTake(gBrIn, sizeof(gBrIn));
    if (n > 0) {
        gBrRecv += (uint32_t)n;
        if (gBr == BR_GATT) {
            feedRecords(gBrIn, n);
        } else {
            slFeed(gBrIn, n);
        }
        busy = true;
    }
    if (gBr == BR_GATT) {
        size_t per = gBlePayload / UCXAPP_CAN_REC_LEN;
        n = takeBatch(gBrOut);
        if (n > 0) {
            per = per < 1 ? 1 : per > BLE_REC_MAX ? BLE_REC_MAX : per;
            bleSend(ucx, gBrOut, n * UCXAPP_CAN_REC_LEN, per * UCXAPP_CAN_REC_LEN);
            busy = true;
        }
        bleStatus(ucx);
    } else {
        ucxapp_can_frame_t f;
        while (gSlOutLen + SL_FRAME_MAX <= SL_OUT && ucxapp_can_recv(&f)) {
            if (gSlOpen) {
                slEncode(&f);
            }
            busy = true;
        }
        if (gSlOutLen > 0) {
            bleSend(ucx, gSlOut, gSlOutLen, gBlePayload);
            gSlOutLen = 0;
            busy = true;
        }
    }
    return busy;
}

static void bleReset(ucxapp_t *app)
{
    U_CX_MUTEX_LOCK(gInLock);
    gBleInHead = gBleInLen = 0;
    gBleInDropped = 0;
    gCfgPending = false;
    U_CX_MUTEX_UNLOCK(gInLock);
    gBrApp = app;
    gBrSent = gBrRecv = gBrLost = 0;
    gLostFrames = 0;
    gPartLen = 0;
    gBleConn = -2;   /* the first round asks for the MTU */
    gBlePayload = BLE_PAYLOAD;
    gTxOk = true;
    gStatAt = 0;
    memset(gStatLast, 0xFF, sizeof(gStatLast));
    gCfgBitrate = -1;
    gSlOpen = gSlTs = gSlLong = false;
    gSlRate = gBitrate;
    gSlLineLen = gSlOutLen = 0;
}

/* Defines the service (or NUS) and resets the bridge; the caller starts the task and sets gBr. */
static bool blePrepare(ucxapp_t *app, int kind)
{
    if (kind == BR_GATT ? !bleDefine(app) : ucxapp_gatt_server_ensure(app, "nus") != UCXAPP_OK) {
        return false;
    }
    ucxapp_gatt_ensure_advertising(app);
    bleReset(app);
    return true;
}

static void blePrintStarted(int kind)
{
    if (kind == BR_GATT) {
        ucxapp_print("can bridge: BLE CAN service 81ea0001-f62b-4fec-94ee-7169818db897 (17-byte records on TX "
                     "and RX, bit rate and mode on Config)\n");
    } else {
        ucxapp_print("can bridge: SLCAN over the Nordic UART Service; the client sends S6 and O to open the bus\n");
    }
}

static void blePrintStatus(void)
{
    ucxapp_print("bridge: %s, %s", kBrNames[gBr], gBleConn >= 0 ? "connected" : "no client");
    if (gBleConn >= 0) {
        ucxapp_print(" (conn %d, %u B per notification%s)", (int)gBleConn, (unsigned)gBlePayload,
                     gTxOk ? "" : ", not subscribed");
    }
    if (gBr == BR_SLCAN) {
        ucxapp_print(", bus %s", gSlOpen ? "open" : "closed");
    }
    ucxapp_print(", sent %u B, received %u B, not delivered %u B\n", (unsigned)gBrSent, (unsigned)gBrRecv,
                 (unsigned)(gBrLost + gBleInDropped));
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

static ucxapp_thread_t *gBleThread;

static void bleLoop(void *arg)
{
    (void)arg;
    while (gBr != BR_OFF) {
        bool busy = false;

        U_CX_MUTEX_LOCK(gBrLock);
        if (gBr != BR_OFF && gBrApp->ucx != NULL) {
            busy = bleService(gBrApp->ucx);
        }
        U_CX_MUTEX_UNLOCK(gBrLock);
        if (!busy) {
            ucxapp_platform_sleep_ms(CAN_POLL_MS);
        }
    }
}

static int bleStart(ucxapp_t *app, int kind)
{
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (gStream != NULL) {
        ucxapp_print("ERROR: a CAN bridge is on (can.bridge off first)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!blePrepare(app, kind)) {
        return UCXAPP_ERR_FAILED;
    }
    gBr = kind;
    gBleThread = ucxapp_thread_start(bleLoop, NULL);
    if (gBleThread == NULL) {
        gBr = BR_OFF;
        ucxapp_print("ERROR: cannot start the bridge thread\n");
        return UCXAPP_ERR_FAILED;
    }
    blePrintStarted(kind);
    return UCXAPP_OK;
}

static void bleStop(void)
{
    U_CX_MUTEX_LOCK(gBrLock);
    gBr = BR_OFF;
    U_CX_MUTEX_UNLOCK(gBrLock);
    ucxapp_thread_join(gBleThread);
    gBleThread = NULL;
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
    if (gBr != BR_OFF) {
        if (status && argc == 2) {
            blePrintStatus();
        } else if (off && argc == 2) {
            bleStop();
            ucxapp_print("bridge stopped\n");
        } else {
            ucxapp_print("ERROR: the %s bridge is on (can.bridge off first)\n", kBrNames[gBr]);
            return UCXAPP_ERR_FAILED;
        }
        return UCXAPP_OK;
    }
    if (argc == 3 && strcasecmp(argv[1], "ble") == 0 &&
        (strcasecmp(argv[2], "gatt") == 0 || strcasecmp(argv[2], "slcan") == 0)) {
        return bleStart(app, strcasecmp(argv[2], "gatt") == 0 ? BR_GATT : BR_SLCAN);
    }
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
    return gStream != NULL ? "on" : kBrNames[gBr];
}

#else

/* ---- bridge on the board: records through the module (TCP, UDP or SPS) from its own task; one at a time ---- */

#define BR_RETRY_MS      2000
#define BR_BLE_CHUNK     120u
#define BR_BLE_CHUNK_MIN 20u
#define BR_READS         4
#define BR_IDLE_MS       20

static bool gBrTask;
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
        return gBrSock >= 0 && ucxapp_sock_write_all(ucx, gBrSock, d, n) >= 0;
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
            busy = gBr <= BR_SLCAN ? bleService(gBrApp->ucx) : brService(gBrApp->ucx);
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
    if (kind == BR_GATT || kind == BR_SLCAN) {
        ok = blePrepare(app, kind);
    } else if (kind == BR_TCP) {
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
        ucxapp_print("can bridge: UDP listening on %d, replies go to the last sender\n",         (int)gBrLocal);
    } else if (kind == BR_SPS) {
        ucxapp_print("can bridge: connect a BLE client to the u-blox Serial Port Service\n");
    } else {
        blePrintStarted(kind);
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
        } else if (gBr <= BR_SLCAN) {
            blePrintStatus();
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
    if ((strcasecmp(argv[1], "tcps") == 0 || strcasecmp(argv[1], "tcp") == 0) && argc <= 3) {
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
    if ((strcasecmp(argv[1], "udpc") == 0 || strcasecmp(argv[1], "udp") == 0) && (argc == 4 || argc == 5)) {
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
    if (strcasecmp(argv[1], "ble") == 0 && argc == 3 && strcasecmp(argv[2], "gatt") == 0) {
        return brStart(app, BR_GATT, NULL, 0, 0);
    }
    if (strcasecmp(argv[1], "ble") == 0 && argc == 3 && strcasecmp(argv[2], "slcan") == 0) {
        return brStart(app, BR_SLCAN, NULL, 0, 0);
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
    { "can.cfg",      "<bitrate=500000> [normal|loopback|listen]", "Bit rate and mode, then (re)start the controller; "
      "loopback receives its own frames without a bus, listen only receives (no acknowledge)", 0, cmdCfg },
#ifndef UCXAPP_CAN_HW
    { "can.open",     "[emulated|<interface>]", "CAN on the emulated loopback (default) or a SocketCAN interface "
      "(Linux: can0 for a CANable, vcan0); no argument shows which", 0, cmdOpen },
#endif
    { "can.send",     "<id> [hex...]", "Send a CAN frame (id above 0x7FF is extended)", 0, cmdSend },
    { "can.recv",     "",              "Take the next received CAN frame", 0, cmdRecv },
    { "can.watch",    "[seconds=10]",     "Print received CAN frames with their time (any key stops)", 0, cmdWatch },
    { "can.status",   "",              "Controller, counters and bridge state", 0, cmdStatus },
#ifndef UCXAPP_CAN_HW
    { "can.bridge",   "tcps [port] | tcpc <host> <port> | udpc <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | "
      "ble gatt | ble slcan | uart <port> [baud] | cell | off [n] | status", "Carry CAN frames both ways as 17-byte records (ts, id, dlc, data[8]); "
      "ble gatt is the u-blox CAN GATT service (records, bit rate, mode, counters), ble slcan is SLCAN text over "
      "the Nordic UART Service, cell uses the cellular data link (cell.connect) and reconnects it; up to 4 bridges "
      "at once (ble gatt and ble slcan run alone)", 0,
      cmdBridge },
#else
    { "can.bridge",   "tcps [port] | tcpc <host> <port> | udpc <host> <port> [local] | udps <port> | ble sps | ble gatt | "
      "ble slcan | off | status",
      "Carry CAN frames both ways as 17-byte records (ts, id, dlc, data[8]) through the module, in a task of its own; "
      "ble gatt is the u-blox CAN GATT service, ble slcan is SLCAN text over the Nordic UART Service; one bridge at a time",
      0, cmdBridge },
#endif
    { "can.selftest", "",              "Offline check of the CAN record format", 0, cmdSelftest },
};

void ucxapp_register_can_cmds(void)
{
    static bool init;

    if (!init) {
        U_CX_MUTEX_CREATE(gLock);
        U_CX_MUTEX_CREATE(gBrLock);
        U_CX_MUTEX_CREATE(gInLock);
        init = true;
    }
    ucxapp_cmd_register(gCanCmds, sizeof(gCanCmds) / sizeof(gCanCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_can_cmds(void)
{
}

bool ucxapp_can_gatt_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    return false;
}

bool ucxapp_can_nus_rx(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return false;
}

#endif /* UCXAPP_LITE */
