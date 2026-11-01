/* can.* - CAN controller and its own bridge: can.bridge carries fixed 17-byte records (batched per write) both ways
 * over tcp, udp, ble or uart. Host builds only. */

#include "ucxapp_cmd.h"
#include "ucxapp_can.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>

#include "ucxapp_platform.h"
#include "ucxapp_stream.h"
#include "u_port.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define CAN_QUEUE       64
#define CAN_BRIDGE_PORT 7001
#define CAN_POLL_MS     1

/* ---- controller: emulated loopback transceiver ---- */

static ucxapp_can_frame_t gQueue[CAN_QUEUE];
static int gHead;
static int gCount;
static int32_t gBitrate = 500000;
static uint32_t gTx;
static uint32_t gRx;
static uint32_t gDropped;
static U_CX_MUTEX_HANDLE gLock;

bool ucxapp_can_send(const ucxapp_can_frame_t *f)
{
    U_CX_MUTEX_LOCK(gLock);
    if (gCount == CAN_QUEUE) {
        gHead = (gHead + 1) % CAN_QUEUE;
        gCount--;
        gDropped++;
    }
    gQueue[(gHead + gCount++) % CAN_QUEUE] = *f;
    gTx++;
    U_CX_MUTEX_UNLOCK(gLock);
    return true;
}

bool ucxapp_can_recv(ucxapp_can_frame_t *f)
{
    bool got = false;

    U_CX_MUTEX_LOCK(gLock);
    if (gCount > 0) {
        *f = gQueue[gHead];
        gHead = (gHead + 1) % CAN_QUEUE;
        gCount--;
        gRx++;
        got = true;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return got;
}

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

    (void)app;
    if (argc != 2 || !ucxapp_parse_int(argv[1], &rate) || rate < 10000 || rate > 1000000) {
        return UCXAPP_ERR_USAGE;
    }
    gBitrate = rate;
    ucxapp_print("can bitrate %d\n", (int)rate);
    return UCXAPP_OK;
}

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
    f.id = (uint32_t)id;
    f.len = (uint8_t)n;
    if (!ucxapp_can_send(&f)) {
        ucxapp_print("ERROR: can send failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent 0x%X [%d]\n", (unsigned)f.id, n);
    return UCXAPP_OK;
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
    ucxapp_print("0x%X [%d]", (unsigned)f.id, f.len);
    for (int i = 0; i < f.len; i++) {
        ucxapp_print(" %02X", f.data[i]);
    }
    ucxapp_print("\n");
    return UCXAPP_OK;
}

/* ---- bridge: 17-byte records both ways ---- */

static ucxapp_stream_t *gStream;
static uint8_t gPart[UCXAPP_CAN_REC_LEN];
static size_t gPartLen;
static uint32_t gBadRecords;

/* Received frames go out in batches of up to UCXAPP_CAN_BATCH records, one push per poll. */
static void bridgePoll(void *ctx, ucxapp_stream_t *s)
{
    uint8_t batch[UCXAPP_CAN_BATCH * UCXAPP_CAN_REC_LEN];
    ucxapp_can_frame_t f;
    size_t n = 0;

    (void)ctx;
    while (n < UCXAPP_CAN_BATCH && ucxapp_can_recv(&f)) {
        ucxapp_can_pack(ucxapp_platform_tick_ms(), &f, batch + n * UCXAPP_CAN_REC_LEN);
        n++;
    }
    if (n > 0) {
        ucxapp_stream_push(s, batch, n * UCXAPP_CAN_REC_LEN);
    }
}

/* Records from the far side are sent on the bus; a record may span two writes. */
static bool bridgeWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
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

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    int queued;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    U_CX_MUTEX_LOCK(gLock);
    queued = gCount;
    U_CX_MUTEX_UNLOCK(gLock);
    ucxapp_print("can emulated loopback, %d bit/s, tx %u, rx %u, queued %d, dropped %u, bad records %u, bridge %s\n",
                 (int)gBitrate, (unsigned)gTx, (unsigned)gRx, queued, (unsigned)gDropped, (unsigned)gBadRecords,
                 gStream != NULL ? "on" : "off");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gCanCmds[] = {
    { "can.cfg",      "<bitrate>",     "Set the CAN bit rate", 0, cmdCfg },
    { "can.send",     "<id> [hex...]", "Send a CAN frame (id above 0x7FF is extended)", 0, cmdSend },
    { "can.recv",     "",              "Take the next received CAN frame", 0, cmdRecv },
    { "can.status",   "",              "Controller, counters and bridge state", 0, cmdStatus },
    { "can.bridge",   "tcp [port] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | "
      "uart <port> [baud] | off [n] | status", "Carry CAN frames both ways as 17-byte records (ts, id, dlc, data[8]); up to 4 bridges at once", 0,
      cmdBridge },
    { "can.selftest", "",              "Offline check of the CAN record format", 0, cmdSelftest },
};

void ucxapp_register_can_cmds(void)
{
    static bool init;

    if (!init) {
        U_CX_MUTEX_CREATE(gLock);
        init = true;
    }
    ucxapp_cmd_register(gCanCmds, sizeof(gCanCmds) / sizeof(gCanCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_can_cmds(void)
{
}

#endif /* UCXAPP_LITE */
