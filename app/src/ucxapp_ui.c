/* Shared core for the web page and the text UI (see ucxapp_ui.h). */

#include "ucxapp_ui.h"
#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

#include "ucxapp_thread.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define LOG_SIZE   8192      /* powers of two so the counters can wrap */
#define EVT_LINES  64
#define EVT_LEN    96
#define MAX_JOB    256

static int gRefs;
static ucxapp_t *gApp;
static U_CX_MUTEX_HANDLE gLock;
static volatile bool gStop;
static ucxapp_thread_t *gWorker;

static char gLog[LOG_SIZE];
static uint32_t gLogTotal;
static char gEv[EVT_LINES][EVT_LEN];
static uint32_t gEvTotal;

static char gJob[MAX_JOB];
static bool gJobPending, gJobRunning;
static uint32_t gDoneSeq;
static int gDoneRc;

/* ---- streams ---------------------------------------------------------------- */

static void logSink(const char *text, size_t len)
{
    U_CX_MUTEX_LOCK(gLock);
    for (size_t i = 0; i < len; i++) {
        gLog[gLogTotal++ % LOG_SIZE] = text[i];
    }
    U_CX_MUTEX_UNLOCK(gLock);
}

void ucxapp_ui_event(const char *text)
{
    if (gRefs <= 0) {
        return;
    }
    U_CX_MUTEX_LOCK(gLock);
    snprintf(gEv[gEvTotal++ % EVT_LINES], EVT_LEN, "%s", text);
    U_CX_MUTEX_UNLOCK(gLock);
}

static volatile int gWifiUp = -1;   /* -1 unknown */
static volatile uint32_t gWifiVersion;

void ucxapp_ui_wifi_set(bool up)
{
    gWifiUp = up ? 1 : 0;
    gWifiVersion++;
}

static size_t appendHandles(char *out, size_t size, size_t n, const char *label, ucxapp_handle_kind_t kind)
{
    int32_t ids[8];
    int count = ucxapp_handle_list(kind, ids, 8);
    int32_t cur = ucxapp_handle_current(kind);

    n += (size_t)snprintf(out + n, size - n, "%s ", label);
    for (int i = 0; i < count && n < size; i++) {
        n += (size_t)snprintf(out + n, size - n, "%s%d%s", i > 0 ? " " : "", (int)ids[i], ids[i] == cur ? "*" : "");
    }
    if (count == 0 && n < size) {
        n += (size_t)snprintf(out + n, size - n, "-");
    }
    return n < size ? n : size - 1;
}

uint32_t ucxapp_ui_state(char *out, size_t size)
{
    uint32_t version = ucxapp_handle_version() + gWifiVersion;
    size_t n = (size_t)snprintf(out, size, "Wi-Fi %s | ", gWifiUp < 0 ? "?" : gWifiUp ? "up" : "down");

    if (n >= size) {
        return version;
    }
    n = appendHandles(out, size, n, "BLE", UCXAPP_HANDLE_CONN);
    if (n + 14 < size) {
        n += (size_t)snprintf(out + n, size - n, " | ");
        n = appendHandles(out, size, n, "Sockets", UCXAPP_HANDLE_SOCK);
    }
    size_t (*const kExtra[])(char *, size_t) = { ucxapp_gatt_summary, ucxapp_gnss_summary, ucxapp_imu_summary };
    for (size_t i = 0; i < sizeof(kExtra) / sizeof(kExtra[0]); i++) {
        char part[48];
        if (kExtra[i](part, sizeof(part)) > 0 && n + strlen(part) + 4 < size) {
            n += (size_t)snprintf(out + n, size - n, " | %s", part);
            for (const char *p = part; *p != '\0'; p++) {
                version = version * 31u + (uint8_t)*p;
            }
        }
    }
    return version;
}

uint32_t ucxapp_ui_log_start(void)
{
    uint32_t start;

    U_CX_MUTEX_LOCK(gLock);
    start = gLogTotal > LOG_SIZE ? gLogTotal - LOG_SIZE : 0;
    U_CX_MUTEX_UNLOCK(gLock);
    return start;
}

size_t ucxapp_ui_log_peek(uint32_t *cursor, char *out, size_t size)
{
    size_t n;

    U_CX_MUTEX_LOCK(gLock);
    if (gLogTotal - *cursor > LOG_SIZE) {
        *cursor = gLogTotal - LOG_SIZE;
    }
    size_t avail = gLogTotal - *cursor;
    n = avail < size ? avail : size;
    size_t end = n;
    while (end > 0 && end < avail && ((unsigned char)gLog[(*cursor + end) % LOG_SIZE] & 0xC0) == 0x80) {
        end--;   /* the chunk would end inside a UTF-8 sequence */
    }
    n = end > 0 ? end : n;
    for (size_t i = 0; i < n; i++) {
        out[i] = gLog[(*cursor + i) % LOG_SIZE];
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return n;
}

uint32_t ucxapp_ui_event_start(void)
{
    uint32_t start;

    U_CX_MUTEX_LOCK(gLock);
    start = gEvTotal > EVT_LINES ? gEvTotal - EVT_LINES : 0;
    U_CX_MUTEX_UNLOCK(gLock);
    return start;
}

bool ucxapp_ui_event_peek(uint32_t *cursor, char *line, size_t size)
{
    bool have;

    U_CX_MUTEX_LOCK(gLock);
    if (gEvTotal - *cursor > EVT_LINES) {
        *cursor = gEvTotal - EVT_LINES;
    }
    have = *cursor != gEvTotal;
    if (have) {
        snprintf(line, size, "%s", gEv[*cursor % EVT_LINES]);
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return have;
}

/* ---- command runner --------------------------------------------------------- */

static void workerLoop(void *arg)
{
    char line[MAX_JOB];

    (void)arg;
    while (!gStop) {
        bool have = false;

        U_CX_MUTEX_LOCK(gLock);
        if (gJobPending) {
            memcpy(line, gJob, sizeof(line));
            gJobPending = false;
            gJobRunning = true;
            have = true;
        }
        U_CX_MUTEX_UNLOCK(gLock);
        if (!have) {
            U_CX_PORT_SLEEP_MS(20);
            continue;
        }
        int rc = ucxapp_cmd_exec(gApp, line);
        U_CX_MUTEX_LOCK(gLock);
        gJobRunning = false;
        gDoneRc = rc;
        gDoneSeq++;
        U_CX_MUTEX_UNLOCK(gLock);
    }
}

bool ucxapp_ui_submit(const char *line)
{
    bool busy;

    U_CX_MUTEX_LOCK(gLock);
    busy = gJobPending || gJobRunning;
    if (!busy) {
        snprintf(gJob, sizeof(gJob), "%s", line);
        gJobPending = true;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return !busy;
}

bool ucxapp_ui_busy(void)
{
    bool busy;

    U_CX_MUTEX_LOCK(gLock);
    busy = gJobPending || gJobRunning;
    U_CX_MUTEX_UNLOCK(gLock);
    return busy;
}

uint32_t ucxapp_ui_done_seq(int *rc)
{
    uint32_t seq;

    U_CX_MUTEX_LOCK(gLock);
    seq = gDoneSeq;
    *rc = gDoneRc;
    U_CX_MUTEX_UNLOCK(gLock);
    return seq;
}

bool ucxapp_ui_acquire(ucxapp_t *app)
{
    if (gRefs > 0) {
        gRefs++;
        return true;
    }
    gApp = app;
    if (app->ucx != NULL) {
        char ssid[40], ip[24];

        ucxapp_ui_wifi_set(ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip)));
    }
    gStop = false;
    gLogTotal = gEvTotal = gDoneSeq = 0;
    gJobPending = gJobRunning = false;
    U_CX_MUTEX_CREATE(gLock);
    gWorker = ucxapp_thread_start(workerLoop, NULL);
    if (gWorker == NULL) {
        U_CX_MUTEX_DELETE(gLock);
        return false;
    }
    ucxapp_io_add_sink(logSink);
    gRefs = 1;
    return true;
}

void ucxapp_ui_release(void)
{
    if (gRefs <= 0 || --gRefs > 0) {
        return;
    }
    ucxapp_io_remove_sink(logSink);
    gStop = true;
    ucxapp_thread_join(gWorker);
    gWorker = NULL;
    U_CX_MUTEX_DELETE(gLock);
}

/* ---- catalog ---------------------------------------------------------------- */

/* Module groups follow the AT command manual; host groups go from general tools to peripherals. */
static const char *const kOrder[] = {
    "at", "device", "log", "stats", "fw", "bt", "gatt", "hid", "ancs", "notify", "wifi", "net", "sock", "mqtt", "http",
    "cert",
    "app", "dash", "session", "prod", "utils", "file", "dtls", "coap", "tftp", "ft", "web", "qr",
    "bridge", "gnss", "imu", "cell", "can", "hw"
};

static const struct {
    const char *group;
    const char *help;
} kGroupHelp[] = {
    { "at",     "Raw AT commands and the AT terminal" },
    { "device", "Module info, settings, reboot, factory reset and diagnostics" },
    { "log",    "Module log output" },
    { "stats",  "Module statistics" },
    { "fw",     "Module firmware: download, bootloader, flash and update from GitHub" },
    { "bt",     "Bluetooth LE: scan, advertise, connect and bond" },
    { "gatt",   "GATT client and server, including predefined services" },
    { "wifi",   "Wi-Fi station and access point" },
    { "net",    "Network status, DNS and ping" },
    { "sock",   "TCP and UDP sockets on the module" },
    { "mqtt",   "MQTT client on the module" },
    { "http",   "HTTP client on the module" },
    { "cert",   "Certificates and keys stored on the module" },
    { "app",    "About this app: version, memory, licences, wall clock" },
    { "utils",  "Host tools: zip, unzip, hashes and app TLS certificates" },
    { "file",   "Files in the app store on this PC or board" },
    { "dtls",   "DTLS client and server over module UDP sockets" },
    { "coap",   "CoAP requests over module UDP sockets" },
    { "tftp",   "TFTP file server and client over module UDP sockets" },
    { "ft",     "File transfer over a custom BLE GATT service, CRC-32 checked" },
    { "web",    "Web page to control the app from a browser" },
    { "qr",     "QR codes" },
    { "gnss",   "GNSS receiver: detect, position and bridge" },
    { "imu",    "IMU sensor: detect, read and bridge" },
    { "cell",   "Cellular modem" },
    { "can",    "CAN bus controller and bridge" },
    { "hw",     "Board I/O bridge: GPIO, I2C and more" },
    { "hid",    "Bluetooth HID: keyboard, mouse and media keys, or read a BLE HID device" },
    { "ancs",   "Apple notifications from a bonded iPhone (GATT client)" },
    { "notify", "Receive phone notifications over a custom GATT service (Android or any app)" },
    { "bridge", "All running bridges (gnss, imu, cell, can, hw): list and stop" },
    { "dash",   "One status card for the module and every attached peripheral" },
    { "session", "Record commands to a .ucx script and replay it" },
    { "prod",   "Production test with per-unit reports" },
};

const char *ucxapp_ui_group_help(const char *group)
{
    for (size_t i = 0; i < sizeof(kGroupHelp) / sizeof(kGroupHelp[0]); i++) {
        if (strcmp(kGroupHelp[i].group, group) == 0) {
            return kGroupHelp[i].help;
        }
    }
    return "";
}

/* Groups whose work is done by this PC (crypto, local sockets, files, the web UI, peripherals on USB).
 * Every other group sends AT commands that the NORA module executes. */
static const char *const kHostGroups[] = {
    "app", "file", "web", "dtls", "coap", "tftp", "ft", "qr", "utils", "gnss", "imu", "cell",
    "can", "bridge", "dash", "session", "prod"
};

static void groupOf(const char *name, char *out)
{
    const char *dot = strchr(name, '.');

    if (dot == NULL) {
        snprintf(out, UCXAPP_UI_GROUP_LEN, "at");
    } else {
        size_t len = (size_t)(dot - name);
        snprintf(out, UCXAPP_UI_GROUP_LEN, "%.*s", (int)(len < UCXAPP_UI_GROUP_LEN - 1 ? len : UCXAPP_UI_GROUP_LEN - 1), name);
    }
}

static int rankOf(const char *group)
{
    for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
        if (strcmp(kOrder[i], group) == 0) {
            return (int)i;
        }
    }
    return 100;
}

typedef struct {
    char (*names)[UCXAPP_UI_GROUP_LEN];
    int max;
    int count;
} groupCtx_t;

static void visitGroup(const ucxapp_cmd_t *cmd, void *user)
{
    groupCtx_t *c = (groupCtx_t *)user;
    char group[UCXAPP_UI_GROUP_LEN];

    groupOf(cmd->name, group);
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->names[i], group) == 0) {
            return;
        }
    }
    if (c->count < c->max) {
        memcpy(c->names[c->count++], group, sizeof(group));
    }
}

int ucxapp_ui_groups(char names[][UCXAPP_UI_GROUP_LEN], int max)
{
    groupCtx_t ctx = { names, max, 0 };

    ucxapp_cmd_foreach(visitGroup, &ctx);
    for (int i = 1; i < ctx.count; i++) {
        char key[UCXAPP_UI_GROUP_LEN];
        int j = i - 1;

        memcpy(key, names[i], sizeof(key));
        while (j >= 0 && rankOf(names[j]) > rankOf(key)) {
            memcpy(names[j + 1], names[j], sizeof(key));
            j--;
        }
        memcpy(names[j + 1], key, sizeof(key));
    }
    return ctx.count;
}

typedef struct {
    const char *group;
    const ucxapp_cmd_t **out;
    int max;
    int count;
} cmdCtx_t;

static void visitCmd(const ucxapp_cmd_t *cmd, void *user)
{
    cmdCtx_t *c = (cmdCtx_t *)user;
    char group[UCXAPP_UI_GROUP_LEN];

    groupOf(cmd->name, group);
    if (strcmp(group, c->group) == 0 && c->count < c->max) {
        c->out[c->count++] = cmd;
    }
}

const char *ucxapp_ui_runs_on(const char *group)
{
    for (size_t i = 0; i < sizeof(kHostGroups) / sizeof(kHostGroups[0]); i++) {
        if (strcmp(kHostGroups[i], group) == 0) {
            return "host";
        }
    }
    return "module";
}

const char *ucxapp_ui_cmd_runs_on(const ucxapp_cmd_t *cmd)
{
    char group[UCXAPP_UI_GROUP_LEN];

    groupOf(cmd->name, group);
    return ucxapp_ui_runs_on(group);
}

/* Display order inside a group: most used first. Unlisted commands follow in registration order. */
static const char *const kCmdOrder[] = {
    "device.info", "device.status", "device.reboot", "device.uptime", "device.diagnose", "device.caps",
    "device.stack", "device.crash", "device.factory",
    "log.level", "log.save", "log.stop", "log.clear",
    "fw.update", "fw.download", "fw.flash", "fw.bootloader",
    "bt.scan", "bt.connect", "bt.disconnect", "bt.list", "bt.bonds", "bt.profile.connect", "bt.profile.auto",
    "bt.profile.add", "bt.profile.list", "bt.profile.remove", "bt.pairing", "bt.security", "bt.iocap",
    "bt.confirm", "bt.passkey", "bt.autoconfirm", "bt.unbond", "bt.defaults",
    "gatt.services", "gatt.chars", "gatt.read", "gatt.write", "gatt.subscribe", "gatt.listen", "gatt.send",
    "gatt.credits", "gatt.writelong",
    "gatt.client.hrs", "gatt.client.bas", "gatt.client.dis", "gatt.client.nus", "gatt.client.sps",
    "gatt.client.ess", "gatt.client.cts", "gatt.client.lns", "gatt.client.aio", "gatt.client.wit",
    "gatt.client.wps", "gatt.client.ras", "gatt.wps",
    "gatt.server.list", "gatt.server.set", "gatt.server.notify", "gatt.server.indicate", "gatt.server.demo",
    "gatt.server.hrs", "gatt.server.bas", "gatt.server.nus", "gatt.server.sps", "gatt.server.ess",
    "gatt.server.cts", "gatt.server.lns", "gatt.server.aio", "gatt.server.wps", "gatt.server.ras",
    "wifi.scan", "wifi.connect", "wifi.disconnect", "wifi.status", "wifi.ip", "wifi.rssi", "wifi.profile.auto",
    "wifi.profile.connect", "wifi.profile.add", "wifi.profile.list", "wifi.profile.remove",
    "wifi.ap.enable", "wifi.ap.disable", "wifi.ap.status",
    "net.ping", "net.dns", "net.ntp", "net.iperf",
    "sock.list", "sock.open", "sock.connect", "sock.send", "sock.read", "sock.close", "sock.tls",
    "mqtt.connect", "mqtt.disconnect", "mqtt.subscribe", "mqtt.publish", "mqtt.read", "mqtt.unsubscribe",
    "app.info", "app.version", "app.memory", "app.uptime", "app.time",
    "utils.zip", "utils.unzip", "utils.hash.text", "utils.tls.list", "utils.tls.ca.create",
    "utils.tls.server.create", "utils.tls.client.create", "utils.tls.pem", "utils.tls.remove",
    "file.list", "file.view", "file.hash", "file.remove",
    "dtls.client", "dtls.server",
    "web.start", "web.status", "web.stop",
    "gnss.detect", "gnss.open", "gnss.status", "gnss.sats", "gnss.log", "gnss.bridge", "gnss.close",
    "imu.detect", "imu.open", "imu.status", "imu.log", "imu.bridge", "imu.close",
    "cell.detect", "cell.open", "cell.status", "cell.connect", "cell.disconnect", "cell.ping",
    "can.status", "can.cfg", "can.send", "can.recv", "can.bridge",
};

static int cmdRank(const ucxapp_cmd_t *cmd, int index)
{
    for (size_t i = 0; i < sizeof(kCmdOrder) / sizeof(kCmdOrder[0]); i++) {
        if (strcmp(kCmdOrder[i], cmd->name) == 0) {
            return (int)i;
        }
    }
    return 1000 + index;
}

int ucxapp_ui_group_cmds(const char *group, const ucxapp_cmd_t **out, int max)
{
    cmdCtx_t ctx = { group, out, max, 0 };
    int rank[UCXAPP_UI_MAX_GROUP_CMDS];

    ucxapp_cmd_foreach(visitCmd, &ctx);
    for (int i = 0; i < ctx.count && i < UCXAPP_UI_MAX_GROUP_CMDS; i++) {
        const ucxapp_cmd_t *cmd = out[i];
        int r = cmdRank(cmd, i), j = i - 1;

        while (j >= 0 && rank[j] > r) {
            out[j + 1] = out[j];
            rank[j + 1] = rank[j];
            j--;
        }
        out[j + 1] = cmd;
        rank[j + 1] = r;
    }
    return ctx.count;
}

static bool isChoice(const char *s, size_t len)
{
    bool bar = false;

    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '|') {
            if (i == 0 || i == len - 1 || s[i - 1] == '|') {
                return false;
            }
            bar = true;
        } else if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return bar;
}

int ucxapp_ui_parse_usage(const char *usage, ucxapp_ui_field_t *fields, int max)
{
    int count = 0;
    const char *p = usage;

    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            return count;
        }
        const char *start = p;
        int depth = 0;
        for (; *p != '\0' && !(*p == ' ' && depth == 0); p++) {
            if (*p == '<' || *p == '[') {
                depth++;
            } else if (*p == '>' || *p == ']') {
                depth--;
            }
        }
        size_t len = (size_t)(p - start);
        if (len < 3 || count >= max || (start[0] != '<' && start[0] != '[') ||
            (start[len - 1] != '>' && start[len - 1] != ']') || memchr(start + 1, ' ', len - 2) != NULL) {
            return -1;
        }
        size_t inner = len - 2;
        ucxapp_ui_field_t *f = &fields[count++];
        snprintf(f->name, sizeof(f->name), "%.*s", (int)inner, start + 1);
        f->optional = start[0] == '[';
        f->choices[0] = '\0';
        if (isChoice(start + 1, inner) && inner < sizeof(f->choices)) {
            snprintf(f->choices, sizeof(f->choices), "%.*s", (int)inner, start + 1);
        }
    }
}

static void put(char *out, size_t size, size_t *n, char ch)
{
    if (*n + 1 < size) {
        out[(*n)++] = ch;
    }
}

static void putStr(char *out, size_t size, size_t *n, const char *s)
{
    while (*s != '\0') {
        put(out, size, n, *s++);
    }
}

void ucxapp_ui_compose(const char *name, const char *const *values, int count, bool raw, char *out, size_t size)
{
    size_t n = 0;

    putStr(out, size, &n, name);
    while (count > 0 && values[count - 1][0] == '\0') {
        count--;
    }
    if (raw) {
        if (count > 0) {
            put(out, size, &n, ' ');
            putStr(out, size, &n, values[0]);
        }
    } else {
        for (int i = 0; i < count; i++) {
            const char *v = values[i];
            bool quote = v[0] == '\0' || strpbrk(v, " \t\"") != NULL;

            put(out, size, &n, ' ');
            if (quote) {
                put(out, size, &n, '"');
            }
            for (; *v != '\0'; v++) {
                if (quote && (*v == '"' || *v == '\\')) {
                    put(out, size, &n, '\\');
                }
                put(out, size, &n, *v);
            }
            if (quote) {
                put(out, size, &n, '"');
            }
        }
    }
    out[n] = '\0';
}
