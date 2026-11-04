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

static volatile int gWifiUp = -1;   /* -1 unknown or no module, -2 module without Wi-Fi */
static volatile uint32_t gWifiVersion;

void ucxapp_ui_wifi_set(bool up)
{
    gWifiUp = up ? 1 : 0;
    gWifiVersion++;
}

int ucxapp_ui_wifi(void)
{
    return gWifiUp;
}

static char gPromptText[96];
static char gPromptFill[48];
static char gPromptCancel[48];
static uint32_t gPromptSeq;

void ucxapp_ui_prompt(const char *text, const char *prefill, const char *cancel)
{
    if (gRefs <= 0) {
        return;
    }
    U_CX_MUTEX_LOCK(gLock);
    snprintf(gPromptText, sizeof(gPromptText), "%s", text);
    snprintf(gPromptFill, sizeof(gPromptFill), "%s", prefill);
    snprintf(gPromptCancel, sizeof(gPromptCancel), "%s", cancel);
    gPromptSeq++;
    U_CX_MUTEX_UNLOCK(gLock);
}

uint32_t ucxapp_ui_prompt_get(char *text, size_t textSize, char *prefill, size_t fillSize, char *cancel,
                              size_t cancelSize)
{
    uint32_t seq;

    U_CX_MUTEX_LOCK(gLock);
    snprintf(text, textSize, "%s", gPromptText);
    snprintf(prefill, fillSize, "%s", gPromptFill);
    snprintf(cancel, cancelSize, "%s", gPromptCancel);
    seq = gPromptSeq;
    U_CX_MUTEX_UNLOCK(gLock);
    return seq;
}

/* Filled by the worker between commands, so the UI thread never talks to the module. */
static char gWifiText[96];
static char gCellText[96];
static char gThreadRole[16];   /* empty: no Thread firmware */
static volatile uint32_t gThreadVersion;
static volatile uint32_t gThreadDirty;

void ucxapp_ui_thread_changed(void)
{
    gThreadDirty++;
}

void ucxapp_ui_thread_text(char *out, size_t size)
{
    U_CX_MUTEX_LOCK(gLock);
    snprintf(out, size, "%s", gThreadRole);
    U_CX_MUTEX_UNLOCK(gLock);
}

void ucxapp_ui_dash_text(char *wifi, size_t wifiSize, char *cell, size_t cellSize)
{
    U_CX_MUTEX_LOCK(gLock);
    snprintf(wifi, wifiSize, "%s", gWifiText);
    snprintf(cell, cellSize, "%s", gCellText);
    U_CX_MUTEX_UNLOCK(gLock);
}

static void refreshDashCache(bool force)
{
    static uint32_t seenWifi = 0xFFFFFFFFu;
    static const void *seenUcx;
    static uint32_t wifiMs, cellMs, threadMs, seenThread;
    uint32_t now = ucxapp_platform_tick_ms();
    char text[96];

    /* A module opened or closed after the UI started (web, Web Serial) */
    if (gApp != NULL && (const void *)gApp->ucx != seenUcx) {
        seenUcx = gApp->ucx;
        force = true;
        if (gApp->ucx == NULL) {
            gWifiUp = -1;
            gWifiVersion++;
            U_CX_MUTEX_LOCK(gLock);
            gWifiText[0] = '\0';
            gThreadRole[0] = '\0';
            U_CX_MUTEX_UNLOCK(gLock);
            gThreadVersion++;
        }
    }
    if (gApp != NULL && gApp->ucx != NULL &&
        (force || seenWifi != gWifiVersion || (gWifiUp > 0 && now - wifiMs > 30000u))) {
        int up = !ucxapp_module_has_wifi(gApp) ? -2 : ucxapp_wifi_dash(gApp->ucx, text, sizeof(text)) ? 1 : 0;
        if (up == -2) {
            text[0] = '\0';
        }
        if (up != gWifiUp) {
            gWifiUp = up;
            gWifiVersion++;
        }
        seenWifi = gWifiVersion;
        wifiMs = now;
        U_CX_MUTEX_LOCK(gLock);
        snprintf(gWifiText, sizeof(gWifiText), "%s", text);
        U_CX_MUTEX_UNLOCK(gLock);
    }
#ifndef UCXAPP_LITE
    if (gApp != NULL && gApp->ucx != NULL && (force || seenThread != gThreadDirty || now - threadMs > 10000u)) {
        threadMs = now;
        seenThread = gThreadDirty;
        if (!ucxapp_thread_dash(gApp, text, sizeof(text))) {
            text[0] = '\0';
        }
        U_CX_MUTEX_LOCK(gLock);
        if (strcmp(text, gThreadRole) != 0) {
            snprintf(gThreadRole, sizeof(gThreadRole), "%.15s", text);
            gThreadVersion++;
        }
        U_CX_MUTEX_UNLOCK(gLock);
    }
#endif
    if (force || now - cellMs > 10000u) {
        cellMs = now;
        if (!ucxapp_cell_dash(text, sizeof(text))) {
            text[0] = '\0';
        }
        U_CX_MUTEX_LOCK(gLock);
        snprintf(gCellText, sizeof(gCellText), "%s", text);
        U_CX_MUTEX_UNLOCK(gLock);
    }
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
    uint32_t version = ucxapp_handle_version() + gWifiVersion + gThreadVersion * 7u;
    bool bleOnly = gWifiUp == -2;   /* Bluetooth LE only module: no Wi-Fi or socket chips */
    size_t n = bleOnly ? 0 : (size_t)snprintf(out, size, "Wi-Fi %s | ", gWifiUp < 0 ? "-" : gWifiUp ? "up" : "down");

    if (n >= size) {
        return version;
    }
    n = appendHandles(out, size, n, "BLE", UCXAPP_HANDLE_CONN);
    U_CX_MUTEX_LOCK(gLock);
    if (gThreadRole[0] != '\0' && n + strlen(gThreadRole) + 11 < size) {
        n += (size_t)snprintf(out + n, size - n, " | Thread %s", gThreadRole);
    }
    U_CX_MUTEX_UNLOCK(gLock);
    if (!bleOnly && n + 14 < size) {
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
    refreshDashCache(true);
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
            refreshDashCache(false);
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

/* The menu, in display order. Module groups (AT commands the NORA module executes) go from the device
 * through the radios to the IP services, like the AT manual; host groups (work done by this PC or board)
 * from the app itself through protocols to the peripherals. Groups not listed here come last. */
typedef struct {
    const char *group;
    const char *section;
    const char *help;
} groupInfo_t;

static const groupInfo_t kModuleGroups[] = {
    { "dev",    "Device",      "Module info, status, diagnostics, reboot, factory reset and power" },
    { "cfg",    "Device",      "Module settings as AT commands: show, save, load and compare" },
    { "fw",     "Device",      "Module firmware: download, flash, bootloader and update from GitHub" },
    { "stats",  "Device",      "Payload bytes sent and received per channel" },
    { "at",     "Device",      "Raw AT commands, the AT terminal, traces and replay" },
    { "bt",     "Bluetooth",   "Bluetooth LE: scan, connect, bond, profiles and Channel Sounding" },
    { "gatt",   "Bluetooth",   "GATT client and server, with predefined and custom services" },
    { "hid",    "Bluetooth",   "Bluetooth HID: be a keyboard, mouse or remote, or read one" },
    { "ancs",   "Bluetooth",   "iPhone notifications (ANCS) from a bonded phone" },
    { "notify", "Bluetooth",   "Phone notifications over a custom GATT service (Android or any app)" },
    { "wifi",   "Wi-Fi, Thread", "Wi-Fi station and access point, profiles, enterprise, portal, locate" },
    { "thread", "Wi-Fi, Thread", "Thread mesh: network, join, role, addresses, neighbours, SRP and DNS" },
    { "net",    "IP services", "Ping, DNS, NTP and iperf" },
    { "sock",   "IP services", "TCP and UDP sockets on the module" },
    { "http",   "IP services", "HTTP(S) client on the module" },
    { "mqtt",   "IP services", "MQTT client on the module" },
    { "cert",   "IP services", "Certificates and keys stored on the module" },
    { "perf",   "IP services", "Latency and throughput through the module: TCP, UDP, SPS, phone" },
};

static const groupInfo_t kHostGroups[] = {
    { "app",     "App",         "This app: status card, log, version, memory, licences, wall clock, examples" },
    { "usecase", "App",         "Use cases: fit check against the module limits and ready-made steps" },
    { "session", "App",         "Record the commands typed to a .ucx script and replay it" },
    { "prod",    "App",         "Production test with per-unit reports" },
    { "file",    "App",         "Files in the app store on this PC or board" },
    { "utils",   "App",         "Host tools: zip, hashes, certificate files, encryption, app TLS, QR codes" },
    { "dtls",    "Protocols",   "DTLS client and server over module UDP sockets" },
    { "coap",    "Protocols",   "CoAP requests over module UDP sockets" },
    { "tftp",    "Protocols",   "TFTP server and client over module UDP sockets" },
    { "ft",      "Protocols",   "File transfer over a custom BLE GATT service or TCP, CRC-32 checked" },
    { "xplorer", "Protocols",   "u-connectXplorer phone app: remote control and echo peer" },
    { "web",     "Protocols",   "Web page to control the app from a browser" },
    { "bridge",  "Peripherals", "All running bridges (gnss, imu, cell, can, hw): list and stop" },
    { "gnss",    "Peripherals", "GNSS receiver: detect, position, corrections and bridge" },
    { "imu",     "Peripherals", "IMU sensor: detect, read and bridge" },
    { "cell",    "Peripherals", "Cellular modem: attach, data link and bridge" },
    { "can",     "Peripherals", "CAN bus controller and bridge" },
    { "hw",      "Peripherals", "Board I/O: GPIO, I2C, SPI, ADC, PWM and their bridge" },
};

#define N_MODULE (sizeof(kModuleGroups) / sizeof(kModuleGroups[0]))
#define N_HOST   (sizeof(kHostGroups) / sizeof(kHostGroups[0]))

/* Menu rank of a group; the first host group is at N_MODULE, unknown groups come after everything. */
static int rankOf(const char *group)
{
    for (size_t i = 0; i < N_MODULE; i++) {
        if (strcmp(kModuleGroups[i].group, group) == 0) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < N_HOST; i++) {
        if (strcmp(kHostGroups[i].group, group) == 0) {
            return (int)(N_MODULE + i);
        }
    }
    return (int)(N_MODULE + N_HOST);
}

static const groupInfo_t *infoOf(const char *group)
{
    int r = rankOf(group);

    if (r < (int)N_MODULE) {
        return &kModuleGroups[r];
    }
    return r < (int)(N_MODULE + N_HOST) ? &kHostGroups[r - N_MODULE] : NULL;
}

const char *ucxapp_ui_group_help(const char *group)
{
    const groupInfo_t *g = infoOf(group);

    return g != NULL ? g->help : "";
}

const char *ucxapp_ui_group_section(const char *group)
{
    const groupInfo_t *g = infoOf(group);

    return g != NULL ? g->section : "";
}

/* "wifi" for "wifi.scan"; commands without a dot are the app's own (dash, examples) except at. */
static void groupOf(const char *name, char *out)
{
    const char *dot = strchr(name, '.');

    if (dot == NULL) {
        snprintf(out, UCXAPP_UI_GROUP_LEN, "%s", strcmp(name, "at") == 0 ? "at" : "app");
    } else {
        size_t len = (size_t)(dot - name);
        snprintf(out, UCXAPP_UI_GROUP_LEN, "%.*s", (int)(len < UCXAPP_UI_GROUP_LEN - 1 ? len : UCXAPP_UI_GROUP_LEN - 1), name);
    }
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

    /* a Bluetooth LE only module (NORA-B26) gets no Wi-Fi, socket or network groups */
    if ((cmd->flags & UCXAPP_CMD_NEEDS_DEVICE) && ucxapp_cmd_needs_wifi(cmd->name) && gApp != NULL &&
        gApp->ucx != NULL && !ucxapp_module_has_wifi(gApp)) {
        return;
    }
    if ((cmd->flags & UCXAPP_CMD_NEEDS_DEVICE) && ucxapp_cmd_needs_thread(cmd->name) && gApp != NULL &&
        gApp->ucx != NULL && !ucxapp_module_has_thread(gApp)) {
        return;
    }
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
    return rankOf(group) >= (int)N_MODULE ? "host" : "module";
}

const char *ucxapp_ui_cmd_runs_on(const ucxapp_cmd_t *cmd)
{
    char group[UCXAPP_UI_GROUP_LEN];

    groupOf(cmd->name, group);
    return ucxapp_ui_runs_on(group);
}

/* Display order inside a group: most used first. Unlisted commands follow in registration order. */
static const char *const kCmdOrder[] = {
    "dev.info", "dev.status", "dev.reboot", "dev.uptime", "dev.diagnose", "dev.caps",
    "dev.stack", "dev.crash", "dev.factory",
    "app.log.level", "app.log.save", "app.log.stop", "app.log.clear", "app.defaults",
    "fw.update", "fw.download", "fw.flash", "fw.bootloader",
    "bt.scan", "bt.connect", "bt.disconnect", "bt.list", "bt.bonds", "bt.profile.connect", "bt.profile.auto",
    "bt.profile.add", "bt.profile.list", "bt.profile.remove", "bt.pairing", "bt.security", "bt.iocap",
    "bt.confirm", "bt.passkey", "bt.autoconfirm", "bt.unbond", "bt.defaults",
    "gatt.services", "gatt.chars", "gatt.read", "gatt.write", "gatt.subscribe", "gatt.listen", "gatt.send",
    "gatt.sps.credits", "gatt.writelong",
    "gatt.client.hrs", "gatt.client.bas", "gatt.client.dis", "gatt.client.nus", "gatt.client.sps",
    "gatt.client.ess", "gatt.client.cts", "gatt.client.lns", "gatt.client.aio", "gatt.client.witmotion",
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
    "dash", "app.info", "app.version", "app.memory", "app.uptime", "app.time",
    "utils.zip", "utils.unzip", "utils.hash.text", "utils.tls.list", "utils.tls.ca.create",
    "utils.tls.server.create", "utils.tls.client.create", "utils.tls.pem", "utils.tls.remove",
    "utils.cert.info", "utils.cert.convert", "utils.encrypt", "utils.decrypt",
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

/* Usage strings to form fields. Placeholders are <x> (required) and [x] (optional); a|b is a choice;
 * x=v gives a default or a suggested value; top-level "a ... | b ..." and groups like [a|b c] are forms. */

static const char *const kFlagWords[] = {
    "force", "full", "filter", "norsp", "nostore", "hidden", "double", "resume", "all", "stop", "secure",
    "auth", "flow", "boot", "rw", "reliable", "more", "errors", "clientauth", "ble", "cell", "watch", "latest",
    "retain",
};

static const char *const kValueWords[] = {
    "seconds", "ssid", "n", "password", "name", "handle", "file", "number", "addr", "digits", "host",
    "server", "hex", "unix", "conn", "port", "text", "url", "uuid",
};

static bool inWords(const char *const *words, size_t count, const char *s, size_t len)
{
    for (size_t i = 0; i < count; i++) {
        if (strlen(words[i]) == len && strncmp(words[i], s, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool isValueAlt(const char *s, size_t len)
{
    return (len > 2 && s[0] == '<' && s[len - 1] == '>') || memchr(s, ':', len) != NULL ||
           (((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')) && memchr(s, '.', len) != NULL) ||
           inWords(kValueWords, sizeof(kValueWords) / sizeof(kValueWords[0]), s, len);
}

/* a|b|<c>: every alternative a keyword or a placeholder; *open when one of them is a placeholder. */
static bool isChoice(const char *s, size_t len, bool *open)
{
    size_t start = 0;
    bool bar = false;

    *open = false;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && s[i] != '|') {
            char ch = s[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                  ch == '-' || ch == '.' || ch == ':' || ch == '<' || ch == '>')) {
                return false;
            }
            continue;
        }
        if (i == start) {
            return false;
        }
        if (isValueAlt(s + start, i - start)) {
            *open = true;
        } else if (memchr(s + start, '<', i - start) != NULL || memchr(s + start, '>', i - start) != NULL) {
            return false;
        }
        bar = bar || i < len;
        start = i + 1;
    }
    return bar;
}

/* Length of the token at p: up to the next space or '|' outside brackets (stopBar) or space only. */
static size_t tokenLen(const char *p, size_t len, bool stopBar)
{
    int depth = 0;
    size_t i = 0;

    for (; i < len; i++) {
        char ch = p[i];
        if (ch == '<' || ch == '[') {
            depth++;
        } else if (ch == '>' || ch == ']') {
            depth--;
        } else if (depth == 0 && (ch == ' ' || (stopBar && ch == '|'))) {
            break;
        }
    }
    return i;
}

static bool hasTopLevel(const char *s, size_t len, char what)
{
    int depth = 0;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '<' || s[i] == '[') {
            depth++;
        } else if (s[i] == '>' || s[i] == ']') {
            depth--;
        } else if (depth == 0 && s[i] == what) {
            return true;
        }
    }
    return false;
}

static bool hasDots(const char *s, size_t len)
{
    for (size_t i = 0; i + 3 <= len; i++) {
        if (s[i] == '.' && s[i + 1] == '.' && s[i + 2] == '.') {
            return true;
        }
    }
    return false;
}

static bool isGroup(const char *t, size_t len)
{
    return len >= 2 && ((t[0] == '<' && t[len - 1] == '>') || (t[0] == '[' && t[len - 1] == ']'));
}

static void copyText(char *out, size_t size, const char *s, size_t len)
{
    snprintf(out, size, "%.*s", (int)len, s);
}

static bool addFields(ucxapp_ui_variant_t *v, const char *s, size_t len, bool inGroup, bool opt);

static ucxapp_ui_field_t *newField(ucxapp_ui_variant_t *v, bool opt)
{
    ucxapp_ui_field_t *f;

    if (v->count >= UCXAPP_UI_MAX_FIELDS) {
        return NULL;
    }
    f = &v->fields[v->count++];
    memset(f, 0, sizeof(*f));
    f->optional = opt;
    f->kind = UCXAPP_UI_TEXT;
    return f;
}

/* One placeholder without spaces: name, name=default, key=<name>, a|b, name... */
static bool addLeaf(ucxapp_ui_variant_t *v, const char *s, size_t len, bool opt)
{
    ucxapp_ui_field_t *f = newField(v, opt);
    const char *eq = memchr(s, '=', len);
    bool open;

    if (f == NULL) {
        return false;
    }
    if (hasDots(s, len)) {
        copyText(f->name, sizeof(f->name), s, len);
        f->kind = UCXAPP_UI_REST;
        return true;
    }
    if (eq != NULL && eq + 1 < s + len && eq[1] == '<' && s[len - 1] == '>') {
        copyText(f->key, sizeof(f->key), s, (size_t)(eq + 1 - s));
        copyText(f->name, sizeof(f->name), eq + 2, (size_t)(s + len - eq - 3));
        return true;
    }
    if (eq != NULL) {
        copyText(f->def, sizeof(f->def), eq + 1, (size_t)(s + len - eq - 1));
        len = (size_t)(eq - s);
    }
    copyText(f->name, sizeof(f->name), s, len);
    if (isChoice(s, len, &open)) {
        size_t n = 0, start = 0;

        f->kind = open ? UCXAPP_UI_OPEN : UCXAPP_UI_PICK;
        for (size_t i = 0; i <= len; i++) {
            if (i < len && s[i] != '|') {
                continue;
            }
            if (!isValueAlt(s + start, i - start) && n + (i - start) + 2 < sizeof(f->choices)) {
                n += (size_t)snprintf(f->choices + n, sizeof(f->choices) - n, "%s%.*s", n > 0 ? "|" : "",
                                      (int)(i - start), s + start);
            }
            start = i + 1;
        }
    } else if (opt && inWords(kFlagWords, sizeof(kFlagWords) / sizeof(kFlagWords[0]), s, len)) {
        f->kind = UCXAPP_UI_FLAG;
    }
    return true;
}

static bool addToken(ucxapp_ui_variant_t *v, const char *t, size_t len, bool inGroup, bool opt)
{
    bool open;

    if (len >= 3 && (t[0] == '<' || t[0] == '[') && hasDots(t, len)) {
        ucxapp_ui_field_t *f = newField(v, opt || t[0] == '[');
        size_t end = len;

        if (f == NULL) {
            return false;
        }
        while (end > 1 && (t[end - 1] == '.' || t[end - 1] == ']' || t[end - 1] == '>' || t[end - 1] == ' ')) {
            end--;
        }
        copyText(f->name, sizeof(f->name), t + 1, end - 1);
        snprintf(f->name + strlen(f->name), sizeof(f->name) - strlen(f->name), "%s", f->name[0] != '\0' ? " ..." : "more ...");
        f->kind = UCXAPP_UI_REST;
        return true;
    }
    if (isGroup(t, len)) {
        const char *inner = t + 1;
        size_t n = len - 2;
        bool o = opt || t[0] == '[';

        if (n == 0) {
            return false;
        }
        if (!hasTopLevel(inner, n, ' ')) {
            return addLeaf(v, inner, n, o);
        }
        return addFields(v, inner, n, true, o);
    }
    if (inGroup || memchr(t, '=', len) != NULL || (memchr(t, '|', len) != NULL && isChoice(t, len, &open))) {
        return addLeaf(v, t, len, opt);
    }
    if (v->count == 0) {
        size_t used = strlen(v->literal);
        if (used + len + 2 > sizeof(v->literal)) {
            return false;
        }
        snprintf(v->literal + used, sizeof(v->literal) - used, "%s%.*s", used > 0 ? " " : "", (int)len, t);
        return true;
    }
    {
        ucxapp_ui_field_t *f = newField(v, false);
        if (f == NULL) {
            return false;
        }
        copyText(f->name, sizeof(f->name), t, len);
        copyText(f->choices, sizeof(f->choices), t, len);
        f->kind = UCXAPP_UI_PICK;
    }
    return true;
}

static bool addFields(ucxapp_ui_variant_t *v, const char *s, size_t len, bool inGroup, bool opt)
{
    size_t i = 0;

    while (i < len) {
        size_t n;
        if (s[i] == ' ') {
            i++;
            continue;
        }
        n = tokenLen(s + i, len - i, false);
        if (!addToken(v, s + i, n, inGroup, opt)) {
            return false;
        }
        i += n;
    }
    return true;
}

typedef struct {
    ucxapp_ui_variant_t *out;
    int max;
    int count;
    bool failed;
} formCtx_t;

static void trimSpan(const char **s, size_t *len)
{
    while (*len > 0 && **s == ' ') {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && (*s)[*len - 1] == ' ') {
        (*len)--;
    }
}

/* "a b | c": a top-level '|' next to a space separates forms; "<x> a|b" is one form with a choice. */
static bool hasSpacedBar(const char *s, size_t len)
{
    int depth = 0;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '<' || s[i] == '[') {
            depth++;
        } else if (s[i] == '>' || s[i] == ']') {
            depth--;
        } else if (depth == 0 && s[i] == '|' && ((i > 0 && s[i - 1] == ' ') || (i + 1 < len && s[i + 1] == ' '))) {
            return true;
        }
    }
    return false;
}

static void addForm(formCtx_t *ctx, const char *s, size_t len, const char *tag, size_t tagLen, int depth)
{
    trimSpan(&s, &len);
    if (ctx->failed || depth > 6) {
        ctx->failed = true;
        return;
    }
    if (depth == 0 ? hasSpacedBar(s, len) : hasTopLevel(s, len, '|')) {
        size_t i = 0;
        while (i <= len && !ctx->failed) {
            size_t n = tokenLen(s + i, len - i, true);
            while (i + n < len && s[i + n] == ' ') {
                /* a space ends a token, not an alternative: extend to the next top-level '|' */
                n += 1 + tokenLen(s + i + n + 1, len - i - n - 1, true);
            }
            addForm(ctx, s + i, n, tag != NULL ? tag : s + i, tag != NULL ? tagLen : n, depth + 1);
            i += n + 1;
        }
        return;
    }
    for (size_t i = 0; i < len;) {
        size_t n;
        if (s[i] == ' ') {
            i++;
            continue;
        }
        n = tokenLen(s + i, len - i, false);
        if (isGroup(s + i, n) && n > 2 && hasTopLevel(s + i + 1, n - 2, '|') && hasTopLevel(s + i + 1, n - 2, ' ') &&
            !hasDots(s + i, n)) {
            char buf[320];
            const char *inner = s + i + 1;
            size_t innerLen = n - 2, k = 0;

            if (s[i] == '[') {
                char without[32];
                size_t first = tokenLen(inner, innerLen, true);
                int w = snprintf(buf, sizeof(buf), "%.*s %.*s", (int)i, s, (int)(len - i - n), s + i + n);
                int t = snprintf(without, sizeof(without), "no %.*s", (int)first, inner);

                addForm(ctx, buf, (size_t)w < sizeof(buf) ? (size_t)w : sizeof(buf) - 1,
                        tag != NULL ? tag : without, tag != NULL ? tagLen : (size_t)t < sizeof(without) ? (size_t)t : 0,
                        depth + 1);
            }
            while (k <= innerLen && !ctx->failed) {
                size_t m = tokenLen(inner + k, innerLen - k, true);
                while (k + m < innerLen && inner[k + m] == ' ') {
                    m += 1 + tokenLen(inner + k + m + 1, innerLen - k - m - 1, true);
                }
                int w = snprintf(buf, sizeof(buf), "%.*s %.*s %.*s", (int)i, s, (int)m, inner + k,
                                 (int)(len - i - n), s + i + n);
                if (w < 0 || (size_t)w >= sizeof(buf)) {
                    ctx->failed = true;
                    return;
                }
                addForm(ctx, buf, (size_t)w, inner + k, m, depth + 1);
                k += m + 1;
            }
            return;
        }
        i += n;
    }
    if (ctx->count >= ctx->max) {
        ctx->failed = true;
        return;
    }
    ucxapp_ui_variant_t *v = &ctx->out[ctx->count++];
    memset(v, 0, sizeof(*v));
    if (!addFields(v, s, len, false, false)) {
        ctx->failed = true;
        return;
    }
    if (v->literal[0] != '\0') {
        snprintf(v->label, sizeof(v->label), "%s", v->literal);
    } else if (len == 0) {
        snprintf(v->label, sizeof(v->label), "(none)");
    } else if (tag != NULL && tagLen > 0) {
        trimSpan(&tag, &tagLen);
        if (tag[0] == '[' && tokenLen(tag, tagLen, false) == tagLen) {
            tag++;
            tagLen -= 2;
        }
        copyText(v->label, sizeof(v->label), tag, tagLen);
    } else {
        copyText(v->label, sizeof(v->label), s, len);
    }
}

int ucxapp_ui_parse_variants(const char *usage, ucxapp_ui_variant_t *out, int max)
{
    formCtx_t ctx = { out, max, 0, false };

    addForm(&ctx, usage, strlen(usage), NULL, 0, 0);
    return ctx.failed ? -1 : ctx.count;
}

int ucxapp_ui_parse_usage(const char *usage, ucxapp_ui_field_t *fields, int max)
{
    static ucxapp_ui_variant_t v[UCXAPP_UI_MAX_VARIANTS];
    int n = ucxapp_ui_parse_variants(usage, v, UCXAPP_UI_MAX_VARIANTS);

    if (n != 1 || v[0].literal[0] != '\0' || v[0].count > max) {
        return -1;
    }
    memcpy(fields, v[0].fields, (size_t)v[0].count * sizeof(fields[0]));
    return v[0].count;
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
