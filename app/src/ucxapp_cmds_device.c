#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
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
    return UCXAPP_OK;
}

/* Wait for +STARTUP so dev.uptime is exact, then restore the session settings the reboot cleared. */
static void waitStartup(ucxapp_t *app)
{
    if (!ucxapp_events_wait(URC_FLAG_STARTUP, 10)) {
        ucxapp_print("warning: no +STARTUP received\n");
    }
    U_CX_PORT_SLEEP_MS(200);
    if (uCxSystemSetEchoOff(app->ucx) != 0) {
        ucxapp_print("warning: could not disable echo\n");
    }
    if (uCxSystemSetExtendedError(app->ucx, U_SYS_EXTENDED_ERRORS_ON) != 0) {
        ucxapp_print("warning: could not enable extended error codes\n");
    }
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

static int cmdDeviceFactory(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = uCxSystemFactoryReset(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: factory reset failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_events_clear(URC_FLAG_STARTUP);
    r = uCxSystemReboot(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: reboot failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    waitStartup(app);
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
        if (p == NULL || sscanf(p, ",%lu,%lu", &size, &used) != 2) {
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
    while ((line = uCxAtClientCmdGetRspParamLine(client, NULL, NULL, NULL)) != NULL) {
        ucxapp_print("%s\n", line);
        rows++;
    }
    if (uCxAtClientCmdEnd(client) != 0) {
        ucxapp_print("ERROR: crash info not supported by this firmware\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print(rows == 0 ? "no crash information stored\n" : "(crash information cleared on the module)\n");
    return UCXAPP_OK;
}

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
    /* NORA-B26 is Bluetooth LE only; NORA-W36 has Wi-Fi and Bluetooth LE. */
    wifi = strstr(model, "B26") == NULL;
    ucxapp_print("Model:            %s\n", model);
    ucxapp_print("Firmware:         %s\n", fw);
    ucxapp_print("Wi-Fi:            %s\n", wifi ? "yes" : "no");
    ucxapp_print("Bluetooth LE:     yes\n");
    ucxapp_print("Sockets:          %s\n", wifi ? "yes" : "no");
    ucxapp_print("MQTT:             %s\n", wifi && ucxapp_cmd_find("mqtt.connect") != NULL ? "yes" : "no");
    ucxapp_print("TLS:              %s\n", wifi && ucxapp_cmd_find("cert.list") != NULL ? "yes" : "no");
    ucxapp_print("HTTP:             %s\n", wifi && ucxapp_cmd_find("http.get") != NULL ? "yes" : "no");
    ucxapp_print("GATT:             %s\n", ucxapp_cmd_find("gatt.read") != NULL ? "yes" : "no");
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

static const ucxapp_cmd_t gDeviceCmds[] = {
    { "dev.caps", "", "Module capabilities: model, firmware, Wi-Fi, Bluetooth LE and the features this build has", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceCaps },
    { "dev.diagnose", "[host]", "One support report: module, uptime, Wi-Fi, DNS and ping (default u-blox.com), certificates; PASS/FAIL", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceDiagnose },
    { "dev.reboot", "", "Reboot the module (clears GATT services and connections); use as the last command", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceReboot },
    { "dev.factory", "<confirm>", "Erase all settings (Wi-Fi, Bluetooth, certificates) and reboot; type 'confirm' to proceed", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdDeviceFactory },
    { "at.reset", "", "Reboot the module (AT+CPWROFF), same as dev.reboot", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceReboot },
    { "at.factory", "<confirm>", "Factory reset (AT+USYFR) and reboot, same as dev.factory; type 'confirm' to proceed", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdDeviceFactory },
    { "dev.info", "", "Show manufacturer, model and firmware version", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceInfo },
    { "dev.status", "", "Overview: module info, Wi-Fi, open sockets, BLE connections, certificates", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceStatus },
    { "dev.uptime", "", "Time since the module started (counted from its last +STARTUP)", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceUptime },
    { "dev.stack", "", "Developer: stack size, used and usage % of every module task", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceStack },
    { "dev.crash", "", "Developer: last crash info stored in module flash; reading clears it", UCXAPP_CMD_NEEDS_DEVICE, cmdDeviceCrash },
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

bool ucxapp_port_skip(const ucxapp_serial_port_t *sp)
{
    static const char *const kSkip[] = { "Virtual", "Bluetooth", "Management", "SOL", "NORA-" };

    if (!sp->available || sp->label[0] != '\0') {
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
        char name[UCXAPP_PLATFORM_PORT_NAME_LEN];
        if (sscanf(line, "%31s %d", name, &b) == 2 && b > 0) {
            snprintf(port, size, "%s", name);
            *baud = b;
            ok = true;
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
