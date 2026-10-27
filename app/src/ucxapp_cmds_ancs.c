/* ancs.* - Apple Notification Center Service client: shows iPhone notifications and answers them (accept/decline).
 * The module advertises an ANCS solicitation; the iPhone pairs from Settings > Bluetooth and the module subscribes.
 * Host builds only. Spec: Apple "ANCS Specification" (Notification Source, Control Point, Data Source). */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_platform.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_gatt_client.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define EV_MAX       32
#define DS_MAX       1024
#define REPLY_MS     3000
#define TITLE_MAX    64
#define MESSAGE_MAX  256

#define EVT_ADDED    0
#define EVT_MODIFIED 1
#define EVT_REMOVED  2

#define CMD_GET_ATTRS  0
#define CMD_ACTION     2

#define ATTR_APP      0
#define ATTR_TITLE    1
#define ATTR_SUBTITLE 2
#define ATTR_MESSAGE  3
#define ATTR_DATE     5
#define ATTR_POS      6
#define ATTR_NEG      7

#define FLAG_SILENT    0x01
#define FLAG_IMPORTANT 0x02
#define FLAG_PREEXIST  0x04
#define FLAG_POS       0x08
#define FLAG_NEG       0x10

typedef struct {
    uint8_t event;
    uint8_t flags;
    uint8_t category;
    uint8_t count;
    uint32_t uid;
} ancsEvent_t;

/* UUIDs in display order, as ucxapp_gatt_client_find128 expects. */
static const uint8_t kNs[16] = { 0x9F, 0xBF, 0x12, 0x0D, 0x63, 0x01, 0x42, 0xD9,
                                 0x8C, 0x58, 0x25, 0xE6, 0x99, 0xA2, 0x1D, 0xBD };
static const uint8_t kCp[16] = { 0x69, 0xD1, 0xD8, 0xF3, 0x45, 0xE1, 0x49, 0xA8,
                                 0x98, 0x21, 0x9B, 0xBD, 0xFD, 0xAA, 0xD9, 0xD9 };
static const uint8_t kDs[16] = { 0x22, 0xEA, 0xC6, 0xE9, 0x24, 0xD6, 0x4B, 0xB5,
                                 0xBE, 0x44, 0xB3, 0x6A, 0xCE, 0x7C, 0x7B, 0xFB };

static const char *const kCategory[] = { "other", "incoming call", "missed call", "voicemail", "social", "schedule",
                                         "email", "news", "health", "business", "location", "entertainment" };
static const char *const kEvent[] = { "added", "modified", "removed" };

static volatile int32_t gConn = -1;
static int32_t gNs = -1;
static int32_t gCp = -1;
static int32_t gDs = -1;
static ancsEvent_t gEv[EV_MAX];
static volatile int gEvCount;
static uint8_t gDsBuf[DS_MAX];
static volatile size_t gDsLen;
static bool gQuiet;

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *categoryName(uint8_t c)
{
    return c < sizeof(kCategory) / sizeof(kCategory[0]) ? kCategory[c] : "?";
}

static bool parseEvent(const uint8_t *d, size_t n, ancsEvent_t *ev)
{
    if (n < 8 || d[0] > EVT_REMOVED) {
        return false;
    }
    ev->event = d[0];
    ev->flags = d[1];
    ev->category = d[2];
    ev->count = d[3];
    ev->uid = get32(d + 4);
    return true;
}

static void storeEvent(const ancsEvent_t *ev)
{
    int i;

    for (i = 0; i < gEvCount && gEv[i].uid != ev->uid; i++) {
    }
    if (ev->event == EVT_REMOVED) {
        if (i < gEvCount) {
            memmove(&gEv[i], &gEv[i + 1], (size_t)(gEvCount - i - 1) * sizeof(gEv[0]));
            gEvCount--;
        }
        return;
    }
    if (i == gEvCount) {
        if (gEvCount == EV_MAX) {
            memmove(&gEv[0], &gEv[1], (EV_MAX - 1) * sizeof(gEv[0]));
            i = EV_MAX - 1;
        } else {
            gEvCount++;
        }
    }
    gEv[i] = *ev;
}

/* Called from the URC task. */
bool ucxapp_ancs_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    ancsEvent_t ev;

    if (conn != gConn || (handle != gNs && handle != gDs)) {
        return false;
    }
    if (handle == gDs) {
        size_t n = gDsLen + len <= DS_MAX ? len : DS_MAX - gDsLen;
        memcpy(gDsBuf + gDsLen, data, n);
        gDsLen += n;
        return true;
    }
    if (parseEvent(data, len, &ev)) {
        storeEvent(&ev);
        if (!gQuiet && !(ev.flags & FLAG_PREEXIST)) {
            ucxapp_print("[ANCS] %s #%u %s%s\n", kEvent[ev.event], (unsigned)ev.uid, categoryName(ev.category),
                         (ev.flags & FLAG_IMPORTANT) ? " (important)" : "");
        }
    }
    return true;
}

/* Parses a Get Notification Attributes reply: [0][uid u32] then ([id][len u16][value])*. Returns attrs found, or -1
 * if the reply is incomplete. */
static int parseAttrs(const uint8_t *d, size_t n, uint32_t uid, int want, char out[8][MESSAGE_MAX + 1])
{
    size_t p = 5;
    int found = 0;

    if (n < 5) {
        return -1;
    }
    if (d[0] != CMD_GET_ATTRS || get32(d + 1) != uid) {
        return 0;
    }
    while (found < want) {
        if (p + 3 > n) {
            return -1;
        }
        uint8_t id = d[p];
        size_t len = (size_t)(d[p + 1] | (d[p + 2] << 8));
        if (p + 3 + len > n) {
            return -1;
        }
        if (id < 8) {
            size_t k = len > MESSAGE_MAX ? MESSAGE_MAX : len;
            memcpy(out[id], d + p + 3, k);
            out[id][k] = '\0';
        }
        p += 3 + len;
        found++;
    }
    return found;
}

static size_t buildGetAttrs(uint8_t *m, uint32_t uid)
{
    size_t n = 0;

    m[n++] = CMD_GET_ATTRS;
    for (int i = 0; i < 4; i++) {
        m[n++] = (uint8_t)(uid >> (8 * i));
    }
    m[n++] = ATTR_APP;
    m[n++] = ATTR_TITLE;
    m[n++] = TITLE_MAX;
    m[n++] = 0;
    m[n++] = ATTR_SUBTITLE;
    m[n++] = TITLE_MAX;
    m[n++] = 0;
    m[n++] = ATTR_MESSAGE;
    m[n++] = (uint8_t)(MESSAGE_MAX & 0xFF);
    m[n++] = (uint8_t)(MESSAGE_MAX >> 8);
    m[n++] = ATTR_DATE;
    m[n++] = ATTR_POS;
    m[n++] = ATTR_NEG;
    return n;
}

#define GET_ATTR_COUNT 7

static bool needSub(void)
{
    if (gConn < 0) {
        ucxapp_print("ERROR: not subscribed (ancs.sub <conn> after the iPhone has paired)\n");
        return false;
    }
    return true;
}

static int cmdOn(ucxapp_t *app, int argc, char *argv[])
{
    /* 128-bit service solicitation for ANCS (little endian), short name; the module adds the flags itself and
     * takes at most 28 bytes for a connectable advertiser. */
    static const uint8_t kAdv[] = { 0x11, 0x15, 0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
                                    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
                                    0x09, 0x09, 'u', 'c', 'x', ' ', 'A', 'N', 'C', 'S' };

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_bt_security_defaults(app);
    int32_t r = uCxBluetoothSetLegacyAdvertiseDataAndType1(app->ucx, kAdv, sizeof(kAdv));
    if (r != 0) {
        ucxapp_print("ERROR: cannot set the ANCS advertisement (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_gatt_ensure_advertising(app);
    ucxapp_print("advertising as 'ucx ANCS': pair it from the iPhone (Settings > Bluetooth), allow notifications,\n"
                 "then ancs.sub <conn> (bt.conns lists the connection)\n");
    return UCXAPP_OK;
}

static int cmdSub(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn = 0;
    int32_t ns;
    int32_t cp;
    int32_t ds;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &conn) || conn < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_gatt_client_find128(app, conn, kNs, &ns) || !ucxapp_gatt_client_find128(app, conn, kCp, &cp) ||
        !ucxapp_gatt_client_find128(app, conn, kDs, &ds)) {
        ucxapp_print("ERROR: no ANCS on connection %d (an iPhone shows it only after pairing)\n", (int)conn);
        return UCXAPP_ERR_FAILED;
    }
    gNs = ns;
    gCp = cp;
    gDs = ds;
    gEvCount = 0;
    gDsLen = 0;
    gQuiet = true;
    gConn = conn;
    /* Data Source first, so attribute replies are never lost. */
    if (uCxGattClientConfigWrite(app->ucx, conn, ds + 1, U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS) != 0 ||
        uCxGattClientConfigWrite(app->ucx, conn, ns + 1, U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS) != 0) {
        gConn = -1;
        gQuiet = false;
        ucxapp_print("ERROR: cannot subscribe (the link must be encrypted: pair first)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_platform_sleep_ms(500);
    gQuiet = false;
    ucxapp_print("ANCS subscribed on connection %d: %d notification(s) on the phone; ancs.list, ancs.show <uid>\n",
                 (int)conn, gEvCount);
    return UCXAPP_OK;
}

static int cmdOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gConn >= 0) {
        uCxGattClientConfigWrite(app->ucx, gConn, gNs + 1, U_GATT_CLIENT_CONFIG_NONE);
        uCxGattClientConfigWrite(app->ucx, gConn, gDs + 1, U_GATT_CLIENT_CONFIG_NONE);
    }
    gConn = -1;
    ucxapp_print("ANCS off\n");
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needSub()) {
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 0; i < gEvCount; i++) {
        const ancsEvent_t *e = &gEv[i];
        ucxapp_print("%10u  %-14s %s%s%s\n", (unsigned)e->uid, categoryName(e->category),
                     (e->flags & FLAG_IMPORTANT) ? "important " : "", (e->flags & FLAG_POS) ? "pos " : "",
                     (e->flags & FLAG_NEG) ? "neg" : "");
    }
    ucxapp_print("%d notification(s)\n", gEvCount);
    return UCXAPP_OK;
}

static int cmdShow(ucxapp_t *app, int argc, char *argv[])
{
    static char attrs[8][MESSAGE_MAX + 1];
    uint8_t m[24];
    int32_t uid;
    int found = -1;

    if (argc != 2 || !ucxapp_parse_int(argv[1], &uid) || uid < 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needSub()) {
        return UCXAPP_ERR_FAILED;
    }
    memset(attrs, 0, sizeof(attrs));
    gDsLen = 0;
    size_t n = buildGetAttrs(m, (uint32_t)uid);
    if (uCxGattClientWrite(app->ucx, gConn, gCp, m, (int32_t)n) != 0) {
        ucxapp_print("ERROR: the phone refused the request (unknown uid?)\n");
        return UCXAPP_ERR_FAILED;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    while (ucxapp_platform_tick_ms() - start < REPLY_MS) {
        found = parseAttrs(gDsBuf, gDsLen, (uint32_t)uid, GET_ATTR_COUNT, attrs);
        if (found >= 0) {
            break;
        }
        ucxapp_platform_sleep_ms(1);
    }
    if (found <= 0) {
        ucxapp_print("ERROR: no reply from the phone\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("app:      %s\n", attrs[ATTR_APP]);
    ucxapp_print("title:    %s\n", attrs[ATTR_TITLE]);
    if (attrs[ATTR_SUBTITLE][0] != '\0') {
        ucxapp_print("subtitle: %s\n", attrs[ATTR_SUBTITLE]);
    }
    ucxapp_print("message:  %s\n", attrs[ATTR_MESSAGE]);
    ucxapp_print("date:     %s\n", attrs[ATTR_DATE]);
    if (attrs[ATTR_POS][0] != '\0' || attrs[ATTR_NEG][0] != '\0') {
        ucxapp_print("actions:  pos=%s neg=%s\n", attrs[ATTR_POS], attrs[ATTR_NEG]);
    }
    return UCXAPP_OK;
}

static int cmdAction(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t m[6];
    int32_t uid;

    if (argc != 3 || !ucxapp_parse_int(argv[1], &uid) || uid < 0 ||
        (strcmp(argv[2], "pos") != 0 && strcmp(argv[2], "neg") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needSub()) {
        return UCXAPP_ERR_FAILED;
    }
    m[0] = CMD_ACTION;
    for (int i = 0; i < 4; i++) {
        m[1 + i] = (uint8_t)((uint32_t)uid >> (8 * i));
    }
    m[5] = strcmp(argv[2], "pos") == 0 ? 0 : 1;
    if (uCxGattClientWrite(app->ucx, gConn, gCp, m, sizeof(m)) != 0) {
        ucxapp_print("ERROR: the phone refused the action (no %s action on #%d?)\n", argv[2], (int)uid);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent %s action to #%d\n", argv[2], (int)uid);
    return UCXAPP_OK;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const uint8_t kAdded[] = { 0x00, 0x12, 0x01, 0x01, 0x2A, 0x00, 0x00, 0x00 };
    static const uint8_t kReply[] = { 0x00, 0x2A, 0x00, 0x00, 0x00,
                                      0x00, 0x03, 0x00, 'a', 'p', 'p', 0x01, 0x02, 0x00, 'H', 'i',
                                      0x02, 0x00, 0x00, 0x03, 0x03, 0x00, 'm', 's', 'g',
                                      0x05, 0x02, 0x00, '2', '6', 0x06, 0x00, 0x00, 0x07, 0x00, 0x00 };
    static char attrs[8][MESSAGE_MAX + 1];
    uint8_t m[24];
    ancsEvent_t ev;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!parseEvent(kAdded, sizeof(kAdded), &ev) || ev.uid != 42 || ev.category != 1 ||
        strcmp(categoryName(ev.category), "incoming call") != 0 || !(ev.flags & FLAG_NEG)) {
        ucxapp_print("ERROR: ancs selftest: event\n");
        return UCXAPP_ERR_FAILED;
    }
    if (parseAttrs(kReply, sizeof(kReply) - 4, 42, GET_ATTR_COUNT, attrs) != -1 ||
        parseAttrs(kReply, sizeof(kReply), 42, GET_ATTR_COUNT, attrs) != GET_ATTR_COUNT ||
        strcmp(attrs[ATTR_APP], "app") != 0 || strcmp(attrs[ATTR_TITLE], "Hi") != 0 ||
        strcmp(attrs[ATTR_MESSAGE], "msg") != 0) {
        ucxapp_print("ERROR: ancs selftest: attributes\n");
        return UCXAPP_ERR_FAILED;
    }
    if (buildGetAttrs(m, 42) != 18 || m[1] != 42 || m[7] != TITLE_MAX || m[13] != (MESSAGE_MAX & 0xFF)) {
        ucxapp_print("ERROR: ancs selftest: request\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("ancs selftest: event, attribute reply and request encoding ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gAncsCmds[] = {
    { "ancs.on",       "",              "Advertise for an iPhone (ANCS solicitation); pair from the phone, then ancs.sub", UCXAPP_CMD_NEEDS_DEVICE, cmdOn },
    { "ancs.sub",      "[conn]",        "Subscribe to the iPhone's notifications on a connection (default 0)", UCXAPP_CMD_NEEDS_DEVICE, cmdSub },
    { "ancs.off",      "",              "Stop receiving iPhone notifications",                      UCXAPP_CMD_NEEDS_DEVICE, cmdOff },
    { "ancs.list",     "",              "Notifications currently on the phone (uid, category, actions)", UCXAPP_CMD_NEEDS_DEVICE, cmdList },
    { "ancs.show",     "<uid>",         "App, title, message and date of one notification",        UCXAPP_CMD_NEEDS_DEVICE, cmdShow },
    { "ancs.action",   "<uid> pos|neg", "Positive or negative action, e.g. answer or decline a call", UCXAPP_CMD_NEEDS_DEVICE, cmdAction },
    { "ancs.selftest", "",              "Offline check of the event and attribute parsing",          0, cmdSelftest },
};

void ucxapp_register_ancs_cmds(void)
{
    ucxapp_cmd_register(gAncsCmds, sizeof(gAncsCmds) / sizeof(gAncsCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_ancs_cmds(void)
{
}

bool ucxapp_ancs_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    return false;
}

#endif
