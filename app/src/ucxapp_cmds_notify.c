/* notify.* - phone notifications over a custom GATT service, for Android (which has no ANCS) or any phone app.
 * A companion app connects, pairs and writes each notification as UTF-8 "<id>|<app>|<title>|<text>" to the
 * Notification characteristic; the module answers on Action (notify) with "<id>|<action>" (e.g. "17|dismiss").
 * Host builds only.
 *
 * Service      7a3f0010-5c1e-4d2b-9f6a-3e8b2c4d5e6f
 *   Notification 7a3f0011-...  Write, Write Without Response (encrypted link)
 *   Action       7a3f0012-...  Notify */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_gatt_server.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define PROP_WRITE_NR 0x04
#define PROP_WRITE    0x08
#define PROP_NOTIFY   0x10
#define PKT_MAX       244
#define KEEP          16

typedef struct {
    char id[12];
    char app[32];
    char title[64];
    char text[160];
} note_t;

static const uint8_t kSvc[16] = { 0x7a, 0x3f, 0x00, 0x10, 0x5c, 0x1e, 0x4d, 0x2b,
                                  0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };
static const uint8_t kNote[16] = { 0x7a, 0x3f, 0x00, 0x11, 0x5c, 0x1e, 0x4d, 0x2b,
                                   0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };
static const uint8_t kAction[16] = { 0x7a, 0x3f, 0x00, 0x12, 0x5c, 0x1e, 0x4d, 0x2b,
                                     0x9f, 0x6a, 0x3e, 0x8b, 0x2c, 0x4d, 0x5e, 0x6f };

static bool gDefined;
static volatile bool gOn;
static int32_t gNoteHandle = -1;
static int32_t gActionHandle = -1;
static volatile int32_t gConn = -1;
static note_t gNotes[KEEP];
static volatile unsigned gCount;

static void copyField(char *out, size_t size, const char *s, size_t n)
{
    if (n >= size) {
        n = size - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)out[i] < 0x20) {
            out[i] = ' ';              /* no control characters on the console */
        }
    }
}

/* "<id>|<app>|<title>|<text>"; the text may contain '|'. False if fewer than four fields. */
static bool parseNote(const uint8_t *data, size_t len, note_t *n)
{
    const char *p = (const char *)data;
    const char *end = p + len;
    const char *f[3];

    for (int i = 0; i < 3; i++) {
        const char *bar = memchr(p, '|', (size_t)(end - p));
        if (bar == NULL) {
            return false;
        }
        f[i] = bar;
        p = bar + 1;
    }
    const char *s = (const char *)data;
    copyField(n->id, sizeof(n->id), s, (size_t)(f[0] - s));
    copyField(n->app, sizeof(n->app), f[0] + 1, (size_t)(f[1] - f[0] - 1));
    copyField(n->title, sizeof(n->title), f[1] + 1, (size_t)(f[2] - f[1] - 1));
    copyField(n->text, sizeof(n->text), f[2] + 1, (size_t)(end - f[2] - 1));
    return n->id[0] != '\0';
}

/* URC task: a phone wrote a notification. */
bool ucxapp_notify_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    note_t n;

    if (!gDefined || handle != gNoteHandle) {
        return false;
    }
    if (!gOn) {
        return true;
    }
    gConn = conn;
    if (!parseNote(data, len, &n)) {
        ucxapp_print("[notify] ignored a write that is not <id>|<app>|<title>|<text>\n");
        return true;
    }
    gNotes[gCount % KEEP] = n;
    gCount++;
    ucxapp_print("[notify] #%s %s: %s - %s\n", n.id, n.app, n.title, n.text);
    return true;
}

static bool defineService(ucxapp_t *app)
{
    static const uint8_t kInit[1] = { 0 };
    uint8_t noteProps = PROP_WRITE | PROP_WRITE_NR;
    uint8_t actionProps = PROP_NOTIFY;
    uCxGattServerCharDefine_t rsp;
    int32_t svc;
    int32_t r;

    r = uCxGattServerServiceDefine(app->ucx, kSvc, 16, &svc);
    if (r == 0) {
        r = uCxGattServerCharDefine6(app->ucx, kNote, 16, &noteProps, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED, kInit, 1, PKT_MAX, &rsp);
        gNoteHandle = rsp.value_handle;
    }
    if (r == 0) {
        r = uCxGattServerCharDefine6(app->ucx, kAction, 16, &actionProps, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED, kInit, 1, PKT_MAX, &rsp);
        gActionHandle = rsp.value_handle;
    }
    if (r == 0) {
        r = uCxGattServerServiceActivate(app->ucx);
    }
    if (r != 0) {
        ucxapp_print("ERROR: cannot define the notification service (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    gDefined = true;
    return true;
}

static int cmdOn(ucxapp_t *app, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gDefined && !defineService(app)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_gatt_ensure_advertising(app);
    gOn = true;
    ucxapp_print("notification service on: the phone app pairs, then writes <id>|<app>|<title>|<text> to "
                 "7a3f0011-5c1e-4d2b-9f6a-3e8b2c4d5e6f\n");
    return UCXAPP_OK;
}

static int cmdOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gOn = false;
    ucxapp_print("notifications are no longer shown (the service stays defined until reboot)\n");
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    unsigned count = gCount;
    unsigned first = count > KEEP ? count - KEEP : 0;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (unsigned i = first; i < count; i++) {
        const note_t *n = &gNotes[i % KEEP];
        ucxapp_print("#%-6s %-16s %s - %s\n", n->id, n->app, n->title, n->text);
    }
    ucxapp_print("%u notification(s)%s\n", count - first, count > KEEP ? " (older ones dropped)" : "");
    return UCXAPP_OK;
}

static int cmdAction(ucxapp_t *app, int argc, char *argv[])
{
    char msg[PKT_MAX];

    if (argc != 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gDefined || gConn < 0) {
        ucxapp_print("ERROR: no phone has written a notification yet\n");
        return UCXAPP_ERR_FAILED;
    }
    int n = snprintf(msg, sizeof(msg), "%s|%s", argv[1], argv[2]);
    if (n <= 0 || n >= (int)sizeof(msg)) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = uCxGattServerSendNotification(app->ucx, gConn, gActionHandle, (const uint8_t *)msg, n);
    if (r != 0) {
        ucxapp_print("ERROR: send failed (%d%s); the app must enable notifications on Action\n", (int)r,
                     ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent %s\n", msg);
    return UCXAPP_OK;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const char kGood[] = "17|Messages|Anna|See you at 5 | ok?";
    static const char kBad[] = "17|Messages|Anna";
    note_t n;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!parseNote((const uint8_t *)kGood, sizeof(kGood) - 1, &n) || strcmp(n.id, "17") != 0 ||
        strcmp(n.app, "Messages") != 0 || strcmp(n.title, "Anna") != 0 || strcmp(n.text, "See you at 5 | ok?") != 0 ||
        parseNote((const uint8_t *)kBad, sizeof(kBad) - 1, &n)) {
        ucxapp_print("ERROR: notify selftest: parsing\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("notify selftest: parsing ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gNotifyCmds[] = {
    { "notify.on",       "",              "Phone notifications (Android or any app) over a custom GATT service: the app writes <id>|<app>|<title>|<text>", UCXAPP_CMD_NEEDS_DEVICE, cmdOn },
    { "notify.off",      "",              "Stop showing phone notifications",                       0, cmdOff },
    { "notify.list",     "",              "The last 16 notifications",                              0, cmdList },
    { "notify.action",   "<id> <action>", "Send <id>|<action> to the phone app (e.g. dismiss, open)", UCXAPP_CMD_NEEDS_DEVICE, cmdAction },
    { "notify.selftest", "",              "Offline check of the notification format",               0, cmdSelftest },
};

void ucxapp_register_notify_cmds(void)
{
    ucxapp_cmd_register(gNotifyCmds, sizeof(gNotifyCmds) / sizeof(gNotifyCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_notify_cmds(void)
{
}

bool ucxapp_notify_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    return false;
}

#endif
