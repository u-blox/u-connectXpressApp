/* trigger.*: rules of the form "when <source> <condition> do <command>". A bridge moves a byte stream between two
 * ends; a trigger watches a value or an event and runs any command of this shell when its condition becomes true:
 *
 *   trigger.add near cs below 2 do matter.set light off        Channel Sounding distance under 2 m
 *   trigger.add weak rssi below -75 do hw.led on               Wi-Fi signal of the station
 *   trigger.add home ssid MyNet do wifi.profile.auto           a network came in range
 *   trigger.add tag bt MyTag lost do hw.led off                a Bluetooth LE device went away
 *   trigger.add btn button do hw.led toggle                    the board's user button
 *   trigger.add lamp event "matter: light = on" do hw.led on   any line of the event log
 *
 * A rule is edge triggered: it runs once when the condition turns true (also when it is true at the first check)
 * and is armed again when the condition turns false. Two rules with a gap between their limits give hysteresis.
 *
 * Distances and events arrive on the URC task, which must not send AT commands: they only mark a rule. One worker
 * thread polls the other sources (RSSI, scans, the button) and runs the commands, so a command may use the module. */

#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

#include "ucxapp_adv.h"
#include "ucxapp_hw.h"
#include "ucxapp_thread.h"
#include "ucxapp_ui.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_port.h"
#include "u_cx_at_client.h"
#include "u_cx_bluetooth.h"
#include "u_cx_wifi.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define MAX_RULES    8
#define NAME_LEN     16
#define ARG_LEN      48
#define ACTION_LEN   160
#define TICK_MS      20      /* worker period: button sampling and the delay before a marked rule runs */
#define RSSI_MS      2000
#define WIFI_SCAN_MS 15000
#define BT_SCAN_MS   10000
#define BT_LISTEN_MS 3000    /* length of one Bluetooth LE discovery */
#define NO_RSSI      (-32768)

typedef enum { SRC_CS, SRC_RSSI, SRC_BT_RSSI, SRC_SSID, SRC_BT, SRC_BUTTON, SRC_EVENT } source_t;
typedef enum { COND_BELOW, COND_ABOVE, COND_FOUND, COND_LOST, COND_PRESSED, COND_RELEASED, COND_MATCH } cond_t;

typedef struct {
    bool used;
    bool enabled;
    uint32_t id;              /* changes when the slot gets another rule, so a late poll result is dropped */
    char name[NAME_LEN];
    source_t source;
    cond_t cond;
    char arg[ARG_LEN];        /* SSID, device name or address, event text */
    bool isAddr;
    uint8_t addr[6];
    int32_t conn;             /* -1: any connection (cs), the newest one (bt.rssi) */
    int32_t limit;            /* mm or dBm */
    uint32_t periodMs;
    char action[ACTION_LEN];
    /* state */
    int state;                /* -1 not checked yet, 0 condition false, 1 true */
    bool seen;                /* a reading arrived */
    bool present;
    int32_t value;
    bool pending;             /* the command is due */
    char cause[64];           /* what made it due, for the log line */
    bool polled;
    uint32_t lastPollMs;
    uint32_t fired;
    uint32_t failed;
} rule_t;

static rule_t gRules[MAX_RULES];
static uint32_t gNextId;
static U_CX_MUTEX_HANDLE gLock;     /* the rules */
static U_CX_MUTEX_HANDLE gRun;      /* held by the worker while it uses the module or runs a command */
static bool gLockInit;
static ucxapp_t *gApp;
static ucxapp_thread_t *gWorker;
static volatile bool gStop;
static bool gRegApplied;
static bool gScanBySsid = true;     /* AT+UWSSC=<mode>,<ssid>; the plain scan when the module refuses it */

static const char *const kSources[] = { "cs", "rssi", "bt.rssi", "ssid", "bt", "button", "event" };

static void lockRules(void)
{
    if (gLockInit) {
        U_CX_MUTEX_LOCK(gLock);
    }
}

static void unlockRules(void)
{
    if (gLockInit) {
        U_CX_MUTEX_UNLOCK(gLock);
    }
}

/* ---- text ------------------------------------------------------------------- */

static void fmtValue(source_t source, int32_t value, char *out, size_t size)
{
    if (source == SRC_CS) {
        int32_t a = value < 0 ? -value : value;

        snprintf(out, size, "%s%d.%02d m", value < 0 ? "-" : "", (int)(a / 1000), (int)(a % 1000 / 10));
    } else {
        snprintf(out, size, "%d dBm", (int)value);
    }
}

static void describeWhen(const rule_t *r, char *out, size_t size)
{
    static const char *const kConds[] = { "below", "above", "found", "lost", "pressed", "released", "" };
    char limit[24] = "";
    char conn[16] = "";
    int n = snprintf(out, size, "%s", kSources[r->source]);

    if (r->conn >= 0) {
        snprintf(conn, sizeof(conn), " %d", (int)r->conn);
    }
    if (r->cond == COND_BELOW || r->cond == COND_ABOVE) {
        limit[0] = ' ';
        fmtValue(r->source, r->limit, limit + 1, sizeof(limit) - 1);
    }
    if (r->source == SRC_EVENT) {
        snprintf(out + n, size - (size_t)n, " \"%s\"", r->arg);
    } else {
        snprintf(out + n, size - (size_t)n, "%s%s%s %s%s", conn, r->arg[0] != '\0' ? " " : "", r->arg,
                 kConds[r->cond], limit);
    }
}

/* The last reading: "1.85 m", "found, -60 dBm", "pressed"; empty for events. */
static void describeNow(const rule_t *r, char *out, size_t size)
{
    char value[24];

    out[0] = '\0';
    if (r->source == SRC_EVENT) {
        return;
    }
    if (!r->seen) {
        snprintf(out, size, "no reading yet");
        return;
    }
    fmtValue(r->source, r->value, value, sizeof(value));
    switch (r->source) {
    case SRC_BUTTON:
        snprintf(out, size, "%s", r->value != 0 ? "pressed" : "released");
        break;
    case SRC_SSID:
    case SRC_BT:
        snprintf(out, size, "%s%s", r->present ? "found, " : "not found", r->present ? value : "");
        break;
    default:
        snprintf(out, size, "%s", r->present ? value : "no reading");
        break;
    }
}

/* ---- evaluation (rules locked) ---------------------------------------------- */

static bool holds(const rule_t *r, bool present, int32_t value)
{
    switch (r->cond) {
    case COND_BELOW:    return present && value < r->limit;
    case COND_ABOVE:    return present && value > r->limit;
    case COND_LOST:     return !present;
    case COND_PRESSED:  return present && value != 0;
    case COND_RELEASED: return present && value == 0;
    default:            return present;
    }
}

/* A new reading. prime: the first reading only sets the state (a button that is released is not "released now"). */
static void evaluate(rule_t *r, bool present, int32_t value, bool prime)
{
    bool hit = holds(r, present, value);
    bool first = r->state < 0;

    r->seen = true;
    r->present = present;
    r->value = value;
    if (hit && r->state != 1 && !(prime && first)) {
        r->pending = true;
        describeNow(r, r->cause, sizeof(r->cause));
    }
    r->state = hit ? 1 : 0;
}

void ucxapp_trigger_distance(int32_t conn, int32_t mm)
{
    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        rule_t *r = &gRules[i];

        if (r->used && r->enabled && r->source == SRC_CS && (r->conn < 0 || r->conn == conn)) {
            evaluate(r, true, mm, false);
        }
    }
    unlockRules();
}

void ucxapp_trigger_event(const char *text)
{
    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        rule_t *r = &gRules[i];

        if (r->used && r->enabled && r->source == SRC_EVENT && strstr(text, r->arg) != NULL) {
            r->pending = true;
            snprintf(r->cause, sizeof(r->cause), "%s", text);
        }
    }
    unlockRules();
}

/* ---- polled sources (worker thread, gRun held, module connected) ------------- */

static bool equalNoCase(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;

        if (x != y) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool pollWifiRssi(ucxapp_t *app, int32_t *rssi)
{
    uCxWifiStationStatus_t status;
    bool ok = uCxWifiStationStatusBegin(app->ucx, U_WIFI_STATUS_ID_RSSI, &status);

    if (ok) {
        *rssi = status.rsp.StatusIdInt.int_val;
    }
    uCxEnd(app->ucx);
    return ok && *rssi != NO_RSSI;
}

static bool pollBtRssi(ucxapp_t *app, const rule_t *r, int32_t *rssi)
{
    int32_t conn = r->conn >= 0 ? r->conn : ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    return conn >= 0 && uCxBluetoothGetRssi(app->ucx, conn, rssi) == 0;
}

/* One Wi-Fi scan; the strongest access point with the SSID. *failed: the module refused the scan. */
static bool scanSsid(ucxapp_t *app, const rule_t *r, bool bySsid, int32_t *rssi, bool *failed)
{
    uCxWifiStationScan_t net;
    uCxWifiStationScanDefault_t any;
    bool found = false;

    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, 60000, false);
    if (bySsid) {
        uCxWifiStationScan2Begin(app->ucx, U_WIFI_SCAN_MODE_ACTIVE, r->arg);
    } else {
        uCxWifiStationScanDefaultBegin(app->ucx);
    }
    for (;;) {
        const char *ssid;
        int32_t level;

        if (bySsid) {
            if (!uCxWifiStationScan2GetNext(app->ucx, &net)) {
                break;
            }
            ssid = net.ssid;
            level = net.rssi;
        } else {
            if (!uCxWifiStationScanDefaultGetNext(app->ucx, &any)) {
                break;
            }
            ssid = any.ssid;
            level = any.rssi;
        }
        if (ssid != NULL && strcmp(ssid, r->arg) == 0 && (!found || level > *rssi)) {
            found = true;
            *rssi = level;
        }
    }
    *failed = uCxEnd(app->ucx) != 0;
    return found;
}

static bool pollSsid(ucxapp_t *app, const rule_t *r, int32_t *rssi)
{
    bool failed;
    bool found;

    if (!gRegApplied) {
        if (ucxapp_wifi_regdomain_name() == NULL || !ucxapp_wifi_apply_regdomain(app->ucx)) {
            return false;
        }
        gRegApplied = true;
    }
    found = scanSsid(app, r, gScanBySsid, rssi, &failed);
    if (failed && !found && gScanBySsid) {
        gScanBySsid = false;
        found = scanSsid(app, r, false, rssi, &failed);
    }
    if (failed) {
        gRegApplied = false;   /* a restarted module lost the regulatory domain: set it again next time */
    }
    return found;
}

static bool pollBt(ucxapp_t *app, const rule_t *r, int32_t *rssi)
{
    uCxBtDiscovery_t dev;
    bool found = false;

    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, BT_LISTEN_MS + 5000, false);
    uCxBluetoothDiscovery3Begin(app->ucx, U_BT_DISCOVERY_TYPE_ALL, U_BT_DISCOVERY_MODE_ACTIVE, BT_LISTEN_MS);
    while (uCxBluetoothDiscovery3GetNext(app->ucx, &dev)) {
        bool match;

        if (r->isAddr) {
            match = memcmp(dev.bd_addr.address, r->addr, sizeof(r->addr)) == 0;
        } else {
            char name[ARG_LEN] = "";

            match = (dev.device_name != NULL && equalNoCase(dev.device_name, r->arg)) ||
                    (dev.data.pData != NULL &&
                     ucxapp_adv_local_name(dev.data.pData, dev.data.length, name, sizeof(name)) &&
                     equalNoCase(name, r->arg));
        }
        if (match && (!found || dev.rssi > *rssi)) {
            found = true;
            *rssi = dev.rssi;
        }
    }
    uCxEnd(app->ucx);
    return found;
}

static bool isPolled(source_t source)
{
    return source == SRC_RSSI || source == SRC_BT_RSSI || source == SRC_SSID || source == SRC_BT;
}

static void pollRule(ucxapp_t *app, int slot, uint32_t now)
{
    rule_t r;
    bool due;
    bool present;
    int32_t value = 0;

    lockRules();
    r = gRules[slot];
    due = r.used && r.enabled && isPolled(r.source) && app->ucx != NULL &&
          (!r.polled || now - r.lastPollMs >= r.periodMs);
    if (due) {
        gRules[slot].polled = true;
        gRules[slot].lastPollMs = now;
    }
    unlockRules();
    if (!due) {
        return;
    }
    switch (r.source) {
    case SRC_RSSI:    present = pollWifiRssi(app, &value); break;
    case SRC_BT_RSSI: present = pollBtRssi(app, &r, &value); break;
    case SRC_SSID:    present = pollSsid(app, &r, &value); break;
    default:          present = pollBt(app, &r, &value); break;
    }
    lockRules();
    if (gRules[slot].used && gRules[slot].enabled && gRules[slot].id == r.id) {
        evaluate(&gRules[slot], present, value, false);
    }
    unlockRules();
}

/* Two equal samples TICK_MS apart are a level: a bouncing contact changes nothing. */
static void pollButton(void)
{
    static int prev = -2;
    bool wanted = false;
    int level;

    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        wanted = wanted || (gRules[i].used && gRules[i].enabled && gRules[i].source == SRC_BUTTON);
    }
    unlockRules();
    if (!wanted) {
        prev = -2;
        return;
    }
    level = ucxapp_hw_button();
    if (level < 0 || level != prev) {
        prev = level;
        return;
    }
    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        rule_t *r = &gRules[i];

        if (r->used && r->enabled && r->source == SRC_BUTTON) {
            evaluate(r, true, level, true);
        }
    }
    unlockRules();
}

/* ---- running the command ----------------------------------------------------- */

static int runAction(ucxapp_t *app, const char *name, const char *cause, const char *action)
{
    char line[ACTION_LEN];
    char ev[NAME_LEN + 24];
    int rc;

    ucxapp_print("[trigger] %s: %s%s%s\n", name, cause, cause[0] != '\0' ? " -> " : "", action);
    snprintf(ev, sizeof(ev), "trigger: %s fired", name);
    ucxapp_ui_event(ev);   /* not through ucxapp_trigger_event: a rule must not start itself */
    snprintf(line, sizeof(line), "%s", action);
    rc = ucxapp_cmd_exec(app, line);
    if (rc != UCXAPP_OK) {
        ucxapp_print("[trigger] %s: %s failed (%s)\n", name, action, ucxapp_err_code(rc));
    }
    return rc;
}

static void countRun(int slot, uint32_t id, int rc)
{
    lockRules();
    if (gRules[slot].used && gRules[slot].id == id) {
        gRules[slot].fired++;
        gRules[slot].failed += rc != UCXAPP_OK ? 1u : 0u;
    }
    unlockRules();
}

static void runPending(ucxapp_t *app, int slot)
{
    rule_t r;
    bool due;

    lockRules();
    r = gRules[slot];
    due = r.used && r.enabled && r.pending;
    gRules[slot].pending = false;
    unlockRules();
    if (due) {
        countRun(slot, r.id, runAction(app, r.name, r.cause, r.action));
    }
}

static void workerLoop(void *arg)
{
    ucxapp_t *app = (ucxapp_t *)arg;

    while (!gStop) {
        uint32_t now = ucxapp_platform_tick_ms();

        U_CX_MUTEX_LOCK(gRun);
        pollButton();
        for (int i = 0; i < MAX_RULES && !gStop; i++) {
            pollRule(app, i, now);
        }
        for (int i = 0; i < MAX_RULES && !gStop; i++) {
            runPending(app, i);
        }
        U_CX_MUTEX_UNLOCK(gRun);
        U_CX_PORT_SLEEP_MS(TICK_MS);
    }
}

static bool startWorker(ucxapp_t *app)
{
    if (gWorker != NULL) {
        return true;
    }
    gApp = app;
    gStop = false;
    gWorker = ucxapp_thread_start(workerLoop, gApp);
    return gWorker != NULL;
}

void ucxapp_trigger_stop(void)
{
    ucxapp_thread_t *worker = gWorker;

    if (worker == NULL || ucxapp_thread_is_self(worker)) {
        return;   /* a command the worker runs cannot wait for the worker to end */
    }
    gStop = true;
    gWorker = NULL;
    ucxapp_thread_join(worker);
}

void ucxapp_trigger_quiesce(void)
{
    gRegApplied = false;
    if (gWorker != NULL && !ucxapp_thread_is_self(gWorker)) {
        U_CX_MUTEX_LOCK(gRun);
        U_CX_MUTEX_UNLOCK(gRun);
    }
}

/* ---- commands ---------------------------------------------------------------- */

static bool validName(const char *s)
{
    size_t n = strlen(s);

    if (n == 0 || n >= NAME_LEN || strcmp(s, "all") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* "2", "2.5", "0.075" as thousandths; false for anything else. */
static bool parseMilli(const char *s, int32_t *out)
{
    int32_t whole = 0;
    int32_t frac = 0;
    int scale = 100;
    bool digits = false;

    for (; *s >= '0' && *s <= '9'; s++) {
        if (whole > 100000) {
            return false;
        }
        whole = whole * 10 + (*s - '0');
        digits = true;
    }
    if (*s == '.') {
        for (s++; *s >= '0' && *s <= '9'; s++) {
            frac += (*s - '0') * scale;
            scale /= 10;
            digits = true;
        }
    }
    *out = whole * 1000 + frac;
    return digits && *s == '\0';
}

/* "C0:FF:EE:00:11:22" or "C0FFEE001122". */
static bool parseAddr(const char *s, uint8_t addr[6])
{
    int n = 0;

    for (; *s != '\0'; s++) {
        int v;

        if (*s == ':') {
            continue;
        }
        if (*s >= '0' && *s <= '9') {
            v = *s - '0';
        } else if ((*s | 32) >= 'a' && (*s | 32) <= 'f') {
            v = (*s | 32) - 'a' + 10;
        } else {
            return false;
        }
        if (n >= 12) {
            return false;
        }
        addr[n / 2] = (uint8_t)((n % 2 == 0 ? 0 : addr[n / 2] << 4) | v);
        n++;
    }
    return n == 12;
}

/* "below|above <value>" at argv[*i]; metres for cs, dBm for the others. */
static bool parseLimit(rule_t *r, int argc, char *argv[], int *i)
{
    const char *word;
    int32_t value;

    if (*i + 1 >= argc) {
        return false;
    }
    word = argv[*i];
    if (strcmp(word, "below") == 0 || strcmp(word, "<") == 0) {
        r->cond = COND_BELOW;
    } else if (strcmp(word, "above") == 0 || strcmp(word, ">") == 0) {
        r->cond = COND_ABOVE;
    } else {
        return false;
    }
    if (r->source == SRC_CS) {
        if (!parseMilli(argv[*i + 1], &value) || value <= 0 || value > 1000000) {
            return false;
        }
    } else if (!ucxapp_parse_int(argv[*i + 1], &value) || value < -127 || value > 20) {
        return false;
    }
    r->limit = value;
    *i += 2;
    return true;
}

/* The words after "do" as one command line; a single quoted word is the line as it is. */
static bool buildAction(rule_t *r, int argc, char *argv[])
{
    size_t n = 0;

    if (argc == 1) {
        return snprintf(r->action, sizeof(r->action), "%s", argv[0]) < (int)sizeof(r->action);
    }
    for (int i = 0; i < argc; i++) {
        bool quote = argv[i][0] == '\0' || strpbrk(argv[i], " \t\"") != NULL;

        if (i > 0 && n + 1 < sizeof(r->action)) {
            r->action[n++] = ' ';
        }
        if (quote && n + 1 < sizeof(r->action)) {
            r->action[n++] = '"';
        }
        for (const char *p = argv[i]; *p != '\0'; p++) {
            if ((*p == '"' || *p == '\\') && n + 1 < sizeof(r->action)) {
                r->action[n++] = '\\';
            }
            if (n + 1 >= sizeof(r->action)) {
                return false;
            }
            r->action[n++] = *p;
        }
        if (quote && n + 1 < sizeof(r->action)) {
            r->action[n++] = '"';
        }
        if (n + 1 >= sizeof(r->action)) {
            return false;
        }
    }
    r->action[n] = '\0';
    return true;
}

/* The command must exist (raw AT lines pass), and must not be a trigger command: rules do not edit rules. */
static bool actionOk(const char *action)
{
    char name[40];
    size_t n = strcspn(action, " \t");

    snprintf(name, sizeof(name), "%.*s", (int)(n < sizeof(name) - 1 ? n : sizeof(name) - 1), action);
    if (strncmp(name, "trigger.", 8) == 0) {
        ucxapp_print("ERROR: a trigger cannot run %s\n", name);
        return false;
    }
    if (ucxapp_cmd_find(name) == NULL && !((name[0] | 32) == 'a' && (name[1] | 32) == 't')) {
        ucxapp_print("ERROR: unknown command '%s' after do (try 'help')\n", name);
        return false;
    }
    return true;
}

static int findRule(const char *name)
{
    for (int i = 0; i < MAX_RULES; i++) {
        if (gRules[i].used && strcmp(gRules[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool parseSource(rule_t *r, int argc, char *argv[], int *i)
{
    const char *src = argv[2];
    int32_t conn;

    r->conn = -1;
    if (strcmp(src, "cs") == 0 || strcmp(src, "bt.rssi") == 0) {
        r->source = src[0] == 'c' ? SRC_CS : SRC_BT_RSSI;
        if (*i < argc && ucxapp_parse_int(argv[*i], &conn) && conn >= 0) {
            r->conn = conn;
            (*i)++;
        }
        return parseLimit(r, argc, argv, i);
    }
    if (strcmp(src, "rssi") == 0) {
        r->source = SRC_RSSI;
        return parseLimit(r, argc, argv, i);
    }
    if (strcmp(src, "ssid") == 0 || strcmp(src, "bt") == 0 || strcmp(src, "event") == 0) {
        if (*i >= argc || argv[*i][0] == '\0' || strlen(argv[*i]) >= ARG_LEN) {
            return false;
        }
        snprintf(r->arg, sizeof(r->arg), "%s", argv[*i]);
        (*i)++;
        if (src[0] == 'e') {
            r->source = SRC_EVENT;
            r->cond = COND_MATCH;
            return true;
        }
        r->source = src[0] == 's' ? SRC_SSID : SRC_BT;
        r->isAddr = r->source == SRC_BT && parseAddr(r->arg, r->addr);
        r->cond = COND_FOUND;
        if (*i < argc && strcmp(argv[*i], "found") == 0) {
            (*i)++;
        } else if (*i < argc && strcmp(argv[*i], "lost") == 0) {
            r->cond = COND_LOST;
            (*i)++;
        } else if (*i < argc && strcmp(argv[*i], "do") != 0 && strncmp(argv[*i], "every", 5) != 0) {
            return parseLimit(r, argc, argv, i);
        }
        return true;
    }
    if (strcmp(src, "button") == 0) {
        r->source = SRC_BUTTON;
        r->cond = COND_PRESSED;
        if (*i < argc && (strcmp(argv[*i], "pressed") == 0 || strcmp(argv[*i], "released") == 0)) {
            r->cond = argv[*i][0] == 'p' ? COND_PRESSED : COND_RELEASED;
            (*i)++;
        }
        return true;
    }
    return false;
}

static int cmdAdd(ucxapp_t *app, int argc, char *argv[])
{
    rule_t r;
    char when[96];
    int i = 3;
    int slot;
    bool replaced;

    if (argc < 5 || !validName(argv[1])) {
        return UCXAPP_ERR_USAGE;
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.name, sizeof(r.name), "%s", argv[1]);
    if (!parseSource(&r, argc, argv, &i)) {
        return UCXAPP_ERR_USAGE;
    }
    r.periodMs = r.source == SRC_SSID ? WIFI_SCAN_MS : (r.source == SRC_BT ? BT_SCAN_MS : RSSI_MS);
    /* every <s>, or every=<s> as the page's form sends it */
    const char *every = NULL;
    if (i + 1 < argc && strcmp(argv[i], "every") == 0) {
        every = argv[i + 1];
        i += 2;
    } else if (i < argc && strncmp(argv[i], "every=", 6) == 0) {
        every = argv[i] + 6;
        i++;
    }
    if (every != NULL) {
        int32_t seconds;

        if (!isPolled(r.source) || !ucxapp_parse_int(every, &seconds) || seconds < 1 || seconds > 3600) {
            return UCXAPP_ERR_USAGE;
        }
        r.periodMs = (uint32_t)seconds * 1000u;
    }
    if (i + 1 >= argc || strcmp(argv[i], "do") != 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!buildAction(&r, argc - i - 1, &argv[i + 1])) {
        ucxapp_print("ERROR: the command after do is too long (at most %d characters)\n", ACTION_LEN - 1);
        return UCXAPP_ERR_FAILED;
    }
    if (!actionOk(r.action)) {
        return UCXAPP_ERR_FAILED;
    }
    if (r.source == SRC_BUTTON && ucxapp_hw_button() < 0) {
        ucxapp_print("ERROR: no user button on this board (hw.board shows the pins)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (r.source == SRC_SSID && ucxapp_wifi_regdomain_name() == NULL) {
        ucxapp_print("ERROR: scanning needs a Wi-Fi region first: wifi.regdomain\n");
        return UCXAPP_ERR_FAILED;
    }
    slot = findRule(r.name);
    replaced = slot >= 0;
    for (int k = 0; k < MAX_RULES && slot < 0; k++) {
        slot = gRules[k].used ? -1 : k;
    }
    if (slot < 0) {
        ucxapp_print("ERROR: at most %d triggers; remove one with trigger.remove\n", MAX_RULES);
        return UCXAPP_ERR_FAILED;
    }
    if (!startWorker(app)) {
        ucxapp_print("ERROR: cannot start the trigger thread\n");
        return UCXAPP_ERR_FAILED;
    }
    r.used = true;
    r.enabled = true;
    r.state = -1;
    if (r.source == SRC_BUTTON) {
        /* the level it has now is where it starts: only a change from here runs the command */
        r.seen = true;
        r.present = true;
        r.value = ucxapp_hw_button();
        r.state = holds(&r, true, r.value) ? 1 : 0;
    }
    r.id = ++gNextId;
    lockRules();
    gRules[slot] = r;
    unlockRules();
    describeWhen(&r, when, sizeof(when));
    ucxapp_print("trigger %s %s: %s -> %s\n", r.name, replaced ? "replaced" : "added", when, r.action);
    if (app->ucx == NULL && r.source != SRC_BUTTON && r.source != SRC_EVENT) {
        ucxapp_print("no module connected: it starts checking after dev.connect\n");
    }
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    int count = 0;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 0; i < MAX_RULES; i++) {
        rule_t r;
        char when[96];
        char now[64];
        char failed[24] = "";

        lockRules();
        r = gRules[i];
        unlockRules();
        if (!r.used) {
            continue;
        }
        describeWhen(&r, when, sizeof(when));
        describeNow(&r, now, sizeof(now));
        if (r.failed > 0) {
            snprintf(failed, sizeof(failed), ", %u failed", (unsigned)r.failed);
        }
        ucxapp_print("%-12s %-3s  %s -> %s  (%s%sfired %u%s)\n", r.name, r.enabled ? "on" : "off", when,
                     r.action, now, now[0] != '\0' ? ", " : "", (unsigned)r.fired, failed);
        count++;
    }
    ucxapp_print("%d trigger(s)%s\n", count, count == 0 ? "; add one with trigger.add (trigger.example shows how)" : "");
    return UCXAPP_OK;
}

/* The web page's trigger card, one line per rule:
   RULE  name | on|off | state (-1, 0, 1) | fired | failed | when | now | action (last: it may hold '|'). */
bool ucxapp_trigger_card(char *out, size_t size)
{
    size_t n = 0;

    if (size == 0) {
        return false;
    }
    out[0] = '\0';
    for (int i = 0; i < MAX_RULES; i++) {
        rule_t r;
        char when[96];
        char now[64];
        char line[320];
        int len;

        lockRules();
        r = gRules[i];
        unlockRules();
        if (!r.used) {
            continue;
        }
        describeWhen(&r, when, sizeof(when));
        describeNow(&r, now, sizeof(now));
        len = snprintf(line, sizeof(line), "RULE  %s | %s | %d | %u | %u | %s | %s | %.64s\n", r.name,
                       r.enabled ? "on" : "off", r.state, (unsigned)r.fired, (unsigned)r.failed, when, now, r.action);
        if (len <= 0 || (size_t)len >= sizeof(line) || n + (size_t)len >= size) {
            break;   /* whole lines only */
        }
        memcpy(out + n, line, (size_t)len + 1);
        n += (size_t)len;
    }
    return n > 0;
}

/* Applies fn to the named rule or to all; the number of rules it touched, -1 for an unknown name. */
static int forRules(const char *name, void (*fn)(rule_t *r))
{
    bool all = strcmp(name, "all") == 0;
    int count = 0;

    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        if (gRules[i].used && (all || strcmp(gRules[i].name, name) == 0)) {
            fn(&gRules[i]);
            count++;
        }
    }
    unlockRules();
    return count > 0 || all ? count : -1;
}

static void removeRule(rule_t *r)
{
    memset(r, 0, sizeof(*r));
}

static void enableRule(rule_t *r)
{
    if (!r->enabled) {
        r->enabled = true;
        r->state = -1;   /* checked from scratch */
        r->polled = false;
    }
}

static void disableRule(rule_t *r)
{
    r->enabled = false;
    r->pending = false;
}

static int unknownRule(const char *name)
{
    ucxapp_print("ERROR: no trigger '%s' (trigger.list shows them)\n", name);
    return UCXAPP_ERR_FAILED;
}

static int cmdRemove(ucxapp_t *app, int argc, char *argv[])
{
    int count;
    bool any = false;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = forRules(argv[1], removeRule);
    if (count < 0) {
        return unknownRule(argv[1]);
    }
    lockRules();
    for (int i = 0; i < MAX_RULES; i++) {
        any = any || gRules[i].used;
    }
    unlockRules();
    if (!any) {
        ucxapp_trigger_stop();
    }
    ucxapp_print("%d trigger(s) removed\n", count);
    return UCXAPP_OK;
}

static int cmdOnOff(ucxapp_t *app, int argc, char *argv[])
{
    bool on = strcmp(argv[0], "trigger.on") == 0;
    int count;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = forRules(argv[1], on ? enableRule : disableRule);
    if (count < 0) {
        return unknownRule(argv[1]);
    }
    ucxapp_print("%d trigger(s) %s\n", count, on ? "on" : "off");
    return UCXAPP_OK;
}

static int cmdFire(ucxapp_t *app, int argc, char *argv[])
{
    rule_t r;
    int slot;
    int rc;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    lockRules();
    slot = findRule(argv[1]);
    if (slot >= 0) {
        r = gRules[slot];
    }
    unlockRules();
    if (slot < 0) {
        return unknownRule(argv[1]);
    }
    rc = runAction(app, r.name, "fired by hand", r.action);
    countRun(slot, r.id, rc);
    return rc;
}

static const ucxapp_cmd_t gTriggerCmds[] = {
    { "trigger.add", "<name> cs [conn] <below|above> <metres> do <command...> | "
      "<name> rssi <below|above> <dBm> [every=<s>] do <command...> | "
      "<name> bt.rssi [conn] <below|above> <dBm> [every=<s>] do <command...> | "
      "<name> ssid <ssid> [found|lost|below <dBm>|above <dBm>] [every=<s>] do <command...> | "
      "<name> bt <name|address> [found|lost|below <dBm>|above <dBm>] [every=<s>] do <command...> | "
      "<name> button [pressed|released] do <command...> | "
      "<name> event <text> do <command...>",
      "Run a command when something happens: a Channel Sounding distance, the Wi-Fi or Bluetooth LE signal, a network or device coming in range or going away, the user button, or a line of the event log; once each time the condition turns true", 0, cmdAdd },
    { "trigger.list",   "",           "Every trigger: on or off, condition, command, the last reading and how often it ran", 0, cmdList },
    { "trigger.remove", "<name|all>", "Remove a trigger, or all of them",                                    0, cmdRemove },
    { "trigger.on",     "<name|all>", "Switch a trigger on again; its condition is checked from scratch",    0, cmdOnOff },
    { "trigger.off",    "<name|all>", "Keep a trigger but stop checking it",                                 0, cmdOnOff },
    { "trigger.fire",   "<name>",     "Run the command of a trigger now, as a test or from a button on a page", 0, cmdFire },
};

void ucxapp_register_trigger_cmds(void)
{
    if (!gLockInit) {
        U_CX_MUTEX_CREATE(gLock);
        U_CX_MUTEX_CREATE(gRun);
        gLockInit = true;
    }
    (void)ucxapp_cmd_register(gTriggerCmds, sizeof(gTriggerCmds) / sizeof(gTriggerCmds[0]));
}
