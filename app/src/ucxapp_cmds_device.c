#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_general.h"
#include "u_cx_power.h"
#include "u_cx_security.h"
#include "u_cx_system.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static int cmdDeviceInfo(ucxapp_t *app, int argc, char *argv[])
{
    const char *text = NULL;
    (void)argc;
    (void)argv;

    if (uCxGeneralGetManufacturerIdentificationBegin(app->ucx, &text) && text != NULL) {
        ucxapp_print("Manufacturer:     %s\n", text);
    }
    uCxEnd(app->ucx);

    text = NULL;
    if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
        ucxapp_print("Model:            %s\n", text);
    }
    uCxEnd(app->ucx);

    text = NULL;
    if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
        ucxapp_print("Firmware Version: %s\n", text);
    }
    uCxEnd(app->ucx);

    return UCXAPP_OK;
}

#ifndef UCXAPP_LITE
/* "3.5.0-156" -> {3,5,0}; false if no x.y.z. */
static bool versionOf(const char *s, int v[3])
{
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9' && sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3) {
            return true;
        }
    }
    return false;
}

static int versionCmp(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* Newest firmware known for the model: the built-in release, or a newer one fw.update last saw on GitHub. */
static bool knownLatest(const char *model, int v[3])
{
    static const struct {
        const char *family;
        int v[3];
    } kReleases[] = { { "NORA-W36", { 3, 5, 0 } } };
    bool have = false;
    size_t len = 0;
    char *saved;

    for (size_t i = 0; i < sizeof(kReleases) / sizeof(kReleases[0]); i++) {
        if (strncmp(model, kReleases[i].family, strlen(kReleases[i].family)) == 0) {
            memcpy(v, kReleases[i].v, sizeof(kReleases[i].v));
            have = true;
        }
    }
    saved = (char *)ucxapp_store_load(UCXAPP_FW_LATEST_FILE, &len);
    if (saved != NULL) {
        int c[3];

        saved[strcspn(saved, "\r\n")] = '\0';
        if (strlen(model) >= 8 && strncmp(saved, model, 8) == 0 && versionOf(saved + 8, c) &&
            (!have || versionCmp(c, v) > 0)) {
            memcpy(v, c, sizeof(c));
            have = true;
        }
        free(saved);
    }
    return have;
}

static bool firstLine(const char *file, char *out, size_t size)
{
    size_t len = 0;
    char *data = (char *)ucxapp_store_load(file, &len);

    if (data == NULL) {
        return false;
    }
    data[strcspn(data, "\r\n")] = '\0';
    snprintf(out, size, "%s", data);
    free(data);
    return out[0] != '\0';
}

/* dev.summary: what is worth knowing on connect, a handful of AT queries. */
static int cmdDeviceSummary(ucxapp_t *app, int argc, char *argv[])
{
    const char *text = NULL;
    char maker[32] = "", model[48] = "", fw[48] = "", ssid[64], ip[48], last[64];
    int32_t handles[16];
    bool wifi, wifiUp = false, thread = false, lastWifi, lastBle;
    int ble, socks, cur[3], latest[3];

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxGeneralGetManufacturerIdentificationBegin(app->ucx, &text) && text != NULL) {
        snprintf(maker, sizeof(maker), "%s", text);
    }
    uCxEnd(app->ucx);
    text = NULL;
    if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
        snprintf(model, sizeof(model), "%s", text);
    }
    uCxEnd(app->ucx);
    text = NULL;
    if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
        snprintf(fw, sizeof(fw), "%s", text);
    }
    uCxEnd(app->ucx);
    if (model[0] == '\0') {
        ucxapp_print("ERROR: no answer to the model query\n");
        return UCXAPP_ERR_FAILED;
    }
    wifi = ucxapp_module_has_wifi(app);
    char role[16];
    thread = ucxapp_thread_dash(app, role, sizeof(role));
    ucxapp_print("Module:           %s%s%s, firmware %s\n", maker, maker[0] != '\0' ? " " : "", model, fw);
    ucxapp_print("Radios:           %sBluetooth LE%s\n", wifi ? "Wi-Fi, " : "", thread ? ", Thread" : "");
    if (versionOf(fw, cur) && knownLatest(model, latest) && versionCmp(cur, latest) < 0) {
        ucxapp_print("Update:           firmware %d.%d.%d is out, update with fw.update\n", latest[0], latest[1],
                     latest[2]);
    }
    if (wifi && ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        wifiUp = true;
        ucxapp_print("Wi-Fi:            '%s', IP %s\n", ssid, ip);
    }
    if (thread && strcmp(role, "off") != 0) {
        ucxapp_print("Thread:           %s\n", role);
    }
    ble = ucxapp_handle_list(UCXAPP_HANDLE_CONN, handles, 16);
    socks = wifi ? ucxapp_handle_list(UCXAPP_HANDLE_SOCK, handles, 16) : 0;
    if (ble > 0 || socks > 0) {
        ucxapp_print("Open:             %d BLE link(s), %d socket(s)\n", ble, socks);
    }
    lastWifi = wifi && !wifiUp && firstLine(UCXAPP_WIFI_LAST_FILE, ssid, sizeof(ssid));
    lastBle = ble == 0 && firstLine(UCXAPP_BLE_LAST_FILE, last, sizeof(last));
    if (lastWifi || lastBle) {
        ucxapp_print("Last used:        ");
        if (lastWifi) {
            ucxapp_print("Wi-Fi '%s' (wifi.profile.auto)%s", ssid, lastBle ? ", " : "");
        }
        if (lastBle) {
            ucxapp_print("BLE '%s' (bt.profile.auto)", last);
        }
        ucxapp_print("\n");
    }
    return UCXAPP_OK;
}
#endif

static int cmdDeviceStatus(ucxapp_t *app, int argc, char *argv[])
{
    char ssid[64], ip[48];
    int32_t handles[16];
    int certs = 0;
    uCxSecListCertificates_t cert;

    cmdDeviceInfo(app, argc, argv);
    if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("Wi-Fi:            connected to '%s', IP %s\n", ssid, ip);
    } else {
        ucxapp_print("Wi-Fi:            not connected\n");
    }
    ucxapp_print("Sockets:          %d open\n", ucxapp_handle_list(UCXAPP_HANDLE_SOCK, handles, 16));
    ucxapp_print("BLE connections:  %d\n", ucxapp_handle_list(UCXAPP_HANDLE_CONN, handles, 16));
    uCxSecurityListCertificatesBegin(app->ucx);
    while (uCxSecurityListCertificatesGetNext(app->ucx, &cert)) {
        certs++;
    }
    uCxEnd(app->ucx);
    ucxapp_print("Certificates:     %d\n", certs);
    ucxapp_print("Power:            %s\n", ucxapp_power_name(ucxapp_power_get()));
    return UCXAPP_OK;
}

/* Wait for +STARTUP so dev.uptime is exact, then restore the session settings the reboot cleared. */
static void waitStartup(ucxapp_t *app)
{
    if (!ucxapp_events_wait(URC_FLAG_STARTUP, 10)) {
        ucxapp_print("warning: no +STARTUP received\n");
    }
    U_CX_PORT_SLEEP_MS(200);
    (void)ucxapp_session_restore(app);
}

bool ucxapp_session_restore(ucxapp_t *app)
{
    bool ok = true;

    (void)ucxapp_session_lost_take();
    if (uCxSystemSetEchoOff(app->ucx) != 0) {
        ucxapp_print("warning: could not disable echo\n");
        ok = false;
    }
    if (uCxSystemSetExtendedError(app->ucx, U_SYS_EXTENDED_ERRORS_ON) != 0) {
        ucxapp_print("warning: could not enable extended error codes\n");
        ok = false;
    }
    return ok;
}

bool ucxapp_module_reboot(ucxapp_t *app)
{
    ucxapp_events_clear(URC_FLAG_STARTUP);
    int32_t r = uCxSystemReboot(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: reboot failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    waitStartup(app);
    return true;
}

static int cmdDeviceReboot(ucxapp_t *app, int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!ucxapp_module_reboot(app)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("module rebooted\n");
    return UCXAPP_OK;
}

/* AT+USYFR takes effect only after AT&W and a reboot (AT command manual) */
bool ucxapp_module_factory_reset(ucxapp_t *app)
{
    int32_t r = uCxSystemFactoryReset(app->ucx);

    if (r == 0) {
        r = uCxSystemStoreConfiguration(app->ucx);
    }
    if (r != 0) {
        ucxapp_print("ERROR: factory reset failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    return ucxapp_module_reboot(app);
}

static int cmdDeviceFactory(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_module_factory_reset(app)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("module reset to factory settings and rebooted\n");
    return UCXAPP_OK;
}

static int cmdDeviceUptime(ucxapp_t *app, int argc, char *argv[])
{
    bool exact;
    uint32_t s = (ucxapp_platform_tick_ms() - ucxapp_module_boot_ms(&exact)) / 1000u;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    /* The module has no uptime command; the app times it from the last +STARTUP it saw. */
    ucxapp_print("%s%u days %02u:%02u:%02u%s\n", exact ? "" : "at least ", (unsigned)(s / 86400),
                 (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60),
                 exact ? "" : " (no +STARTUP seen since connect; dev.reboot gives an exact count)");
    return UCXAPP_OK;
}

#ifndef UCXAPP_LITE
/* Developer command AT+USYTS: +USYTS:"<task>",<stack size>,<used>,<usage %> per task. */
static int cmdDeviceStack(ucxapp_t *app, int argc, char *argv[])
{
    uCxAtClient_t *client = app->ucx->pAtClient;
    char *line;
    int rows = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxAtClientCmdBeginF(client, "AT+USYTS", "", NULL);
    while ((line = uCxAtClientCmdGetRspParamLine(client, "+USYTS:", NULL, NULL)) != NULL) {
        char name[32] = "";
        unsigned long size = 0;
        unsigned long used = 0;
        const char *p = line;
        size_t n = 0;

        if (*p == '"') {
            p++;
        }
        while (*p != '\0' && *p != '"' && *p != ',' && n < sizeof(name) - 1) {
            name[n++] = *p++;
        }
        name[n] = '\0';
        p = strchr(p, ',');
        if (p != NULL) {
            char *end;

            size = strtoul(p + 1, &end, 10);
            p = end > p + 1 && *end == ',' ? end + 1 : NULL;
            if (p != NULL) {
                used = strtoul(p, &end, 10);
                p = end > p ? end : NULL;
            }
        }
        if (p == NULL) {
            ucxapp_print("%s\n", line);
            continue;
        }
        ucxapp_print_usage_row("Task", name, "stack", size, used);
        rows++;
    }
    if (uCxAtClientCmdEnd(client) != 0) {
        ucxapp_print("ERROR: task stack info not supported by this firmware\n");
        return UCXAPP_ERR_FAILED;
    }
    if (rows == 0) {
        ucxapp_print("no task information\n");
    }
    return UCXAPP_OK;
}

/* Developer command AT+USYCI?: crash info stored in module flash; the module clears it once read. */
static int cmdDeviceCrash(ucxapp_t *app, int argc, char *argv[])
{
    uCxAtClient_t *client = app->ucx->pAtClient;
    char *line;
    int rows = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uCxAtClientCmdBeginF(client, "AT+USYCI?", "", NULL);
    /* The client only returns lines that match the prefix; with NULL the record would be read (and cleared) unseen. */
    while ((line = uCxAtClientCmdGetRspParamLine(client, "+USYCI:", NULL, NULL)) != NULL) {
        ucxapp_print("+USYCI:%s\n", line);
        rows++;
    }
    if (uCxAtClientCmdEnd(client) != 0) {
        ucxapp_print("ERROR: crash info not supported by this firmware\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print(rows == 0 ? "no crash information stored\n" : "(crash information cleared on the module)\n");
    return UCXAPP_OK;
}
#endif

static int cmdDeviceCaps(ucxapp_t *app, int argc, char *argv[])
{
    char model[48] = "";
    char fw[48] = "";
    const char *text = NULL;
    bool wifi;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
        snprintf(model, sizeof(model), "%s", text);
    }
    uCxEnd(app->ucx);
    text = NULL;
    if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
        snprintf(fw, sizeof(fw), "%s", text);
    }
    uCxEnd(app->ucx);
    if (model[0] == '\0') {
        ucxapp_print("ERROR: no answer to the model query\n");
        return UCXAPP_ERR_FAILED;
    }
    /* NORA-B2x (B20, B26, B27) has no Wi-Fi; NORA-W36 has Wi-Fi and Bluetooth LE. Key: value lines stay one word
       (tests, --json and --doctor parse them); a Bluetooth LE only module gets no Wi-Fi feature lines. */
    wifi = strstr(model, "-B2") == NULL;
    const char *region = wifi ? ucxapp_wifi_regdomain_name() : NULL;
    bool thread = ucxapp_module_has_thread(app);
    ucxapp_print("Model:            %s\n", model);
    ucxapp_print("Firmware:         %s\n", fw);
    ucxapp_print("-- radio\n");
    ucxapp_print("Wi-Fi:            %s\n", wifi ? "yes" : "no");
    ucxapp_print("Bluetooth LE:     yes\n");
    ucxapp_print("Thread:           %s\n", thread ? "yes" : "no");
    if (wifi) {
        ucxapp_print("Region:           %s\n", region != NULL ? region : "unset");
        ucxapp_print("-- Wi-Fi features\n");
        ucxapp_print("Sockets:          yes\n");
        ucxapp_print("MQTT:             %s\n", ucxapp_cmd_find("mqtt.connect") != NULL ? "yes" : "no");
        ucxapp_print("TLS:              %s\n", ucxapp_cmd_find("cert.list") != NULL ? "yes" : "no");
        ucxapp_print("HTTP:             %s\n", ucxapp_cmd_find("http.get") != NULL ? "yes" : "no");
    }
    ucxapp_print("-- Bluetooth LE features\n");
    ucxapp_print("GATT:             %s\n", ucxapp_cmd_find("gatt.read") != NULL ? "yes" : "no");
    if (!wifi && thread) {
        ucxapp_print("-- next  thread.scan, thread.network new or thread.join <pskd>, bt.scan\n");
    } else if (!wifi) {
        ucxapp_print("-- next  bt.scan, then bt.connect <number>\n");
    } else if (region == NULL) {
        ucxapp_print("-- next  wifi.regdomain (pick where you are), then wifi.scan and bt.scan\n");
    } else {
        ucxapp_print("-- next  wifi.scan, wifi.connect <number>, bt.scan\n");
    }
    return UCXAPP_OK;
}

typedef struct {
    int passed, failed, notRun;
} diag_t;

/* One status line per check: PASS, FAIL <code>, NOT RUN or NOT SUPPORTED, with a hint for the next step. */
static void diagStatus(const char *title, const char *status, const char *hint)
{
    ucxapp_print("Check %-13s %-14s%s%s\n", title, status, hint[0] != '\0' ? "  " : "", hint);
}

/* Runs a check command if this build has it; prints the outcome line and counts it. */
static void diagRun(ucxapp_t *app, const char *title, const char *line, const char *hint, diag_t *d)
{
    char buf[128];
    size_t nameLen = strcspn(line, " ");
    char name[48];
    char status[32];
    int rc;

    snprintf(name, sizeof(name), "%.*s", (int)(nameLen < sizeof(name) ? nameLen : sizeof(name) - 1), line);
    if (ucxapp_cmd_find(name) == NULL) {
        diagStatus(title, "NOT SUPPORTED", "not in this build");
        return;
    }
    ucxapp_print("--- %s (%s)\n", title, line);
    snprintf(buf, sizeof(buf), "%s", line);
    rc = ucxapp_cmd_exec(app, buf);
    if (rc == UCXAPP_OK) {
        d->passed++;
        diagStatus(title, "PASS", "");
    } else {
        d->failed++;
        snprintf(status, sizeof(status), "FAIL %s", ucxapp_err_code(rc));
        diagStatus(title, status, hint);
    }
}

static int cmdDeviceDiagnose(ucxapp_t *app, int argc, char *argv[])
{
    char ssid[64], ip[48];
    char host[96];
    diag_t d = { 0, 0, 0 };
    bool wifi;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(host, sizeof(host), "%s", argc == 2 ? argv[1] : "u-blox.com");
    diagRun(app, "device", "dev.caps", "check the cable and the port; try dev.reboot", &d);
    diagRun(app, "uptime", "dev.uptime", "try dev.reboot", &d);
    wifi = ucxapp_module_has_wifi(app);
    if (!wifi) {
        static const char *const kNet[] = { "wifi", "dns", "internet", "certificates" };
        for (size_t i = 0; i < sizeof(kNet) / sizeof(kNet[0]); i++) {
            diagStatus(kNet[i], "NOT SUPPORTED", "no Wi-Fi on this module");
        }
    } else if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        char line[128];
        diagRun(app, "wifi", "wifi.status", "try wifi.disconnect, then wifi.connect", &d);
        snprintf(line, sizeof(line), "net.dns %s", host);
        diagRun(app, "dns", line, "the access point gives no working DNS server; try wifi.ip", &d);
        snprintf(line, sizeof(line), "net.ping %s 2", host);
        diagRun(app, "internet", line, "the access point has no internet access, or blocks ping", &d);
    } else {
        static const char *const kNet[] = { "wifi", "dns", "internet" };
        for (size_t i = 0; i < sizeof(kNet) / sizeof(kNet[0]); i++) {
            diagStatus(kNet[i], "NOT RUN", i == 0 ? "not connected: wifi.connect <ssid> <password>" : "needs Wi-Fi");
            d.notRun++;
        }
    }
    if (wifi) {
        diagRun(app, "certificates", "cert.list", "try dev.reboot", &d);
    }
    ucxapp_print("Result:             %s (%d passed, %d failed, %d not run)\n",
                 d.failed > 0 ? "FAIL" : d.notRun > 0 ? "PARTIAL" : "PASS", d.passed, d.failed, d.notRun);
    return d.failed == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

#ifndef UCXAPP_LITE
#define POWER_WAKE_TRIES      12
#define POWER_WAKE_TIMEOUT_MS 300
#define POWER_SLEEP_SETTLE_MS 500

static int powerFailed(const char *what, int32_t r, const char *needs)
{
    ucxapp_print("ERROR: %s failed (%d%s); %s\n", what, (int)r, ucxapp_errname(r), needs);
    return UCXAPP_ERR_FAILED;
}

static int cmdDevicePower(ucxapp_t *app, int argc, char *argv[])
{
    int32_t level, timeoutMs = 0, r;

    if (argc == 1) {
        r = uCxPowerGetPowerSaveLevel(app->ucx, &level);
        if (r != 0) {
            ucxapp_print("no power save level on this module: dev.power sleep for light sleep, "
                         "dev.power deep for deep sleep\n");
            return UCXAPP_OK;
        }
        ucxapp_power_save_level(level == 1);
        ucxapp_print("power save level %d%s\n", (int)level, level == 0 ? " (off, fastest response)" : " (saves power after the idle timeout)");
        ucxapp_print("state: %s\n", ucxapp_power_name(ucxapp_power_get()));
        return UCXAPP_OK;
    }
    if (strcmp(argv[1], "auto") == 0) {
        /* Friendly form of save: seconds instead of a level and milliseconds. */
        int32_t seconds = 30;
        bool off = argc == 3 && strcmp(argv[2], "off") == 0;
        if (argc > 3 || (argc == 3 && !off && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1 || seconds > 3600))) {
            return UCXAPP_ERR_USAGE;
        }
        r = uCxPowerSetPowerSaveLevel(app->ucx, off ? 0 : 1);
        if (r == 0 && !off) {
            r = uCxPowerSetPowerSaveTimeout(app->ucx, seconds * 1000);
        }
        if (r == 0) {
            r = uCxSystemStoreConfiguration(app->ucx);
        }
        if (r != 0) {
            return powerFailed("set power save", r, "it needs a NORA-W36 (NORA-B26: dev.power sleep)");
        }
        ucxapp_power_save_level(!off);
        if (off) {
            ucxapp_print("power save off: always active, fastest response\n");
        } else {
            ucxapp_print("power save on: the module saves power after %d s idle and wakes on traffic; "
                         "use 115200 baud or lower\n", (int)seconds);
        }
        return UCXAPP_OK;
    }
    if (strcmp(argv[1], "save") == 0) {
        if (argc < 3 || argc > 4 || !ucxapp_parse_int(argv[2], &level) || level < 0 || level > 1 ||
            (argc == 4 && (!ucxapp_parse_int(argv[3], &timeoutMs) || timeoutMs < 1))) {
            return UCXAPP_ERR_USAGE;
        }
        r = uCxPowerSetPowerSaveLevel(app->ucx, level);
        if (r == 0 && argc == 4) {
            r = uCxPowerSetPowerSaveTimeout(app->ucx, timeoutMs);
        }
        if (r == 0) {
            r = uCxSystemStoreConfiguration(app->ucx);
        }
        if (r != 0) {
            return powerFailed("set the power save level", r, "it needs a NORA-W36 (NORA-B26: dev.power sleep)");
        }
        ucxapp_power_save_level(level == 1);
        ucxapp_print("power save level %d stored", (int)level);
        if (argc == 4) {
            ucxapp_print(", idle timeout %d ms", (int)timeoutMs);
        }
        ucxapp_print(level == 1 ? "; use 115200 baud or lower, more latency after the idle time\n" : "\n");
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "sleep") == 0) {
        bool wifi = ucxapp_module_has_wifi(app);
        if (wifi) {
            /* NORA-W36 (3.5.0+) only sleeps with Bluetooth off; say so instead of a bare ERROR. */
            uBtMode_t mode = U_BT_MODE_DISABLED;
            if (uCxBluetoothGetMode(app->ucx, &mode) == 0 && mode != U_BT_MODE_DISABLED) {
                ucxapp_print("ERROR: light sleep on NORA-W36 needs Bluetooth off: AT+UBTM=0, AT&W, AT+CPWROFF, "
                             "then connect Wi-Fi and run dev.power sleep again\n");
                return UCXAPP_ERR_FAILED;
            }
        }
        r = uCxPowerSleep(app->ucx);
        if (r != 0) {
            return powerFailed("light sleep", r, wifi ? "NORA-W36 needs firmware 3.5.0 or later (fw.update)"
                                                      : "this module has no light sleep (dev.power save 1)");
        }
        /* The module falls asleep a little after its OK; a command in between is lost half way. */
        U_CX_PORT_SLEEP_MS(POWER_SLEEP_SETTLE_MS);
        ucxapp_power_set(UCXAPP_POWER_LIGHT);
        ucxapp_print("light sleep: %s links stay up and events still come; dev.power wake (or the WAKE_UP pin) "
                     "wakes it for commands\n", wifi ? "Wi-Fi" : "Bluetooth");
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "wake") == 0) {
        bool pulsed = false;
        uint32_t startMs = ucxapp_platform_tick_ms();
        /* A character wakes light sleep; deep sleep needs a falling edge on WAKE_UP (DTR on EVKs), then a boot. */
        for (int i = 0; i < POWER_WAKE_TRIES; i++) {
            if (i == 1) {
                pulsed = ucxapp_platform_pulse_dtr(app->ucx->pAtClient->uartHandle);
            }
            uCxAtClientSetCommandTimeout(app->ucx->pAtClient, POWER_WAKE_TIMEOUT_MS, false);
            if (uCxGeneralAttention(app->ucx) == 0) {
                ucxapp_power_awake();
                ucxapp_print(i == 0 ? "awake\n" : "awake after %u ms\n", (unsigned)(ucxapp_platform_tick_ms() - startMs));
                return UCXAPP_OK;
            }
            U_CX_PORT_SLEEP_MS(50);
        }
        ucxapp_print("ERROR: no answer%s; pull WAKE_UP (GPIO_J9) low or press reset (NORA-B26: dev.power deep uart "
                     "wakes on UART)\n", pulsed ? " after a WAKE_UP pulse through DTR" : "");
        return UCXAPP_ERR_FAILED;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "recover") == 0) {
        uint32_t holdMs = argc == 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : UCXAPP_DTR_HOLD_MS;
        if (holdMs == 0 || holdMs > 10000) {
            return UCXAPP_ERR_USAGE;
        }
        ucxapp_events_clear(URC_FLAG_STARTUP);
        if (!ucxapp_platform_recover_dtr(app->ucx->pAtClient->uartHandle, holdMs)) {
            ucxapp_print("ERROR: this port has no DTR\n");
            return UCXAPP_ERR_FAILED;
        }
        uint32_t startMs = ucxapp_platform_tick_ms();
        bool started = ucxapp_events_wait(URC_FLAG_STARTUP, 10);
        uint32_t tookMs = ucxapp_platform_tick_ms() - startMs;
        ucxapp_platform_pulse_dtr(app->ucx->pAtClient->uartHandle);
        U_CX_PORT_SLEEP_MS(200);
        uCxAtClientSetCommandTimeout(app->ucx->pAtClient, POWER_WAKE_TIMEOUT_MS, false);
        (void)ucxapp_session_lost_take();
        bool answers = uCxSystemSetEchoOff(app->ucx) == 0;
        uCxSystemSetExtendedError(app->ucx, U_SYS_EXTENDED_ERRORS_ON);
        if (started) {
            ucxapp_print("+STARTUP %u ms after the DTR sequence (low %u ms, 5 pulses)%s\n", (unsigned)tookMs,
                         (unsigned)holdMs, answers ? "" : ", but no answer to ATE0");
            return answers ? UCXAPP_OK : UCXAPP_ERR_FAILED;
        }
        ucxapp_print("ERROR: no +STARTUP within 10 s of the DTR sequence (%s); power-cycle the module\n",
                     answers ? "it still answers, so it did not restart" : "no answer either");
        return UCXAPP_ERR_FAILED;
    }
    if (strcmp(argv[1], "deep") == 0 && argc <= 3) {
        static const char *const kWake[] = { "pin", "reset", "uart" };
        int mode = 0;

        while (argc == 3 && mode < 3 && strcmp(argv[2], kWake[mode]) != 0) {
            mode++;
        }
        if (mode == 3) {
            return UCXAPP_ERR_USAGE;
        }
        r = mode == 0 ? uCxPowerDeepSleepWithGpioWakeup(app->ucx) : uCxPowerDeepSleep(app->ucx, (uPowerWakeupMode_t)mode);
        if (r != 0) {
            return powerFailed("deep sleep", r, "wake-up by reset or UART needs a NORA-B26");
        }
        U_CX_PORT_SLEEP_MS(POWER_SLEEP_SETTLE_MS);
        ucxapp_power_set(UCXAPP_POWER_DEEP);
        ucxapp_print("deep sleep: no radio; wake it with the %s, then it starts as after a reset\n",
                     mode == 0 ? "wake-up pin (pull low)" : mode == 1 ? "reset pin" : "UART RX pin");
        return UCXAPP_OK;
    }
    return UCXAPP_ERR_USAGE;
}
#endif

static const ucxapp_cmd_t gDeviceCmds[] = {
    { "dev.caps", "", "Model, firmware, Wi-Fi, Bluetooth LE and the features of this build", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceCaps },
    { "dev.diagnose", "[host]", "Support report with PASS/FAIL: module, uptime, Wi-Fi, DNS and ping (default u-blox.com), certificates", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceDiagnose },
    { "dev.reboot", "", "Reboot the module (drops GATT services and connections); make it the last command", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceReboot },
    { "dev.factory", "<confirm>", "Erase all settings (Wi-Fi, Bluetooth, certificates) and reboot; type 'confirm'", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdDeviceFactory },
    { "at.reset", "", "Reboot the module (AT+CPWROFF), same as dev.reboot", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceReboot },
    { "at.factory", "<confirm>", "Factory reset (AT+USYFR) and reboot, same as dev.factory; type 'confirm'", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdDeviceFactory },
    { "dev.info", "", "Manufacturer, model and firmware version", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceInfo },
#ifndef UCXAPP_LITE
    { "dev.summary", "", "Short overview shown on connect: model, firmware, radios, Wi-Fi and Thread state, open links and sockets", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceSummary },
#endif
    { "dev.status", "", "Overview: module, Wi-Fi, open sockets, BLE connections, certificates", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceStatus },
    { "dev.uptime", "", "Time since the module's last +STARTUP", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceUptime },
#ifndef UCXAPP_LITE
    { "dev.stack", "", "Developer: stack size, used and usage % of every module task", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceStack },
    { "dev.crash", "", "Developer: last crash info in module flash; reading clears it", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceCrash },
    { "dev.power", "[auto [<s>|off]|save <0|1> [idle_ms]|sleep|wake|recover [hold_ms]|deep [pin|reset|uart]]", "Power state (active, power save, light sleep, deep sleep); auto: the module saves power after <s> idle (default 30) and wakes on traffic (NORA-W36, stored); save: level 0 off or 1 on with the idle time in ms; light sleep keeping Wi-Fi (NORA-W36 3.5.0+, Bluetooth off) or Bluetooth links (NORA-B26) and wake, DTR recovery of a stuck module (DTR low 1200 ms, 5 pulses), or deep sleep (no radio, wakes as after a reset)", UCXAPP_CMD_NEEDS_DEVICE, cmdDevicePower },
#endif
};

int ucxapp_find_modules(char names[][UCXAPP_PLATFORM_PORT_NAME_LEN], int maxNames)
{
    ucxapp_serial_port_t ports[64];
    int found = 0;
    int count = ucxapp_platform_list_serial_ports(ports, (int)(sizeof(ports) / sizeof(ports[0])));

    for (int i = 0; i < count && found < maxNames; i++) {
        const ucxapp_serial_port_t *p = &ports[i];
        bool nora = strstr(p->desc, "NORA-W36") != NULL || strstr(p->desc, "NORA-B26") != NULL;
        if (nora && p->available && (p->label[0] == '\0' || strcmp(p->label, "AT") == 0)) {
            snprintf(names[found++], UCXAPP_PLATFORM_PORT_NAME_LEN, "%.*s", UCXAPP_PLATFORM_PORT_NAME_LEN - 1, p->name);
        }
    }
    return found;
}

/* Nordic PPK2 (USB 1915:C00A): it takes every byte as an opcode ('\r' sets its regulator), so no detector may probe
 * it; ppk.* finds it by these IDs. */
bool ucxapp_port_is_ppk(const ucxapp_serial_port_t *sp)
{
    return (sp->vid == 0x1915 && sp->pid == 0xC00A) || strstr(sp->desc, "PPK2") != NULL;
}

bool ucxapp_port_skip(const ucxapp_serial_port_t *sp)
{
    static const char *const kSkip[] = { "Virtual", "Bluetooth", "Management", "SOL", "NORA-" };

    if (!sp->available || sp->label[0] != '\0' || ucxapp_port_is_ppk(sp)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); i++) {
        if (strstr(sp->desc, kSkip[i]) != NULL) {
            return true;
        }
    }
    return false;
}

bool ucxapp_port_is_gnss(const ucxapp_serial_port_t *sp)
{
    static const char *const kGnss[] = { "u-blox", "GNSS", "GPS", "F9P", "F9R", "ZED", "NEO-", "MAX-", "LEA-" };

    if (sp->vid == 0x1546) {
        return true;
    }
    for (size_t i = 0; i < sizeof(kGnss) / sizeof(kGnss[0]); i++) {
        if (strstr(sp->desc, kGnss[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void lastPath(const char *kind, char *path, size_t size)
{
    const char *dir = getenv("TEMP");

    if (dir == NULL) {
        dir = getenv("TMPDIR");
    }
    snprintf(path, size, "%s/ucxapp_last_%s.txt", dir != NULL ? dir : "/tmp", kind);
}

bool ucxapp_last_port_load(const char *kind, char *port, size_t size, int32_t *baud)
{
    char path[300], line[64];
    FILE *f;
    int b = 0;
    bool ok = false;

    lastPath(kind, path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    if (fgets(line, sizeof(line), f) != NULL) {
        size_t n = strcspn(line, " \t\r\n");
        char *end;

        if (n > 0 && n < UCXAPP_PLATFORM_PORT_NAME_LEN) {
            b = (int)strtol(line + n, &end, 10);
            if (end > line + n && b > 0) {
                snprintf(port, size, "%.*s", (int)n, line);
                *baud = b;
                ok = true;
            }
        }
    }
    fclose(f);
    return ok;
}

void ucxapp_last_port_save(const char *kind, const char *port, int32_t baud)
{
    char path[300];
    FILE *f;

    lastPath(kind, path, sizeof(path));
    f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "%s %d\n", port, (int)baud);
        fclose(f);
    }
}

void ucxapp_register_device_cmds(void)
{
    ucxapp_cmd_register(gDeviceCmds, sizeof(gDeviceCmds) / sizeof(gDeviceCmds[0]));
}
