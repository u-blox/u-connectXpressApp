/* dash: one card with a line per area (Wi-Fi, BLE, GNSS, cellular, IMU), host only. */

#include "ucxapp_cmd.h"
#include "ucxapp_stream.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_wifi.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define U8_DOT   "\xE2\x97\x8F"
#define U8_RING  "\xE2\x97\x8B"
#define U8_HLINE "\xE2\x95\x90"
#define U8_VLINE "\xE2\x95\x91"

#define DASH_MAX_DEV 8

static char gOthers[DASH_MAX_DEV][32];
static int gOtherCount;

static void rule(const char *title)
{
    int n = 39;

    if (title != NULL) {
        ucxapp_print(U8_HLINE U8_HLINE U8_HLINE " %s ", title);
        n -= 5 + (int)strlen(title);
    }
    for (int i = 0; i < n; i++) {
        ucxapp_print(U8_HLINE);
    }
    ucxapp_print("\n");
}

/* name is printed on the first line of an area only; who is the port. */
static void line(const char *name, bool active, const char *who, const char *text)
{
    ucxapp_print(U8_VLINE " %-5s %s %-8s %s\n", name, active ? U8_DOT : U8_RING, who, text);
}

/* Other modules are not opened (the AT link is single); point the user at device.use. */
static void otherModules(const char *area, bool first)
{
    char text[64];

    for (int i = 0; i < gOtherCount; i++) {
        snprintf(text, sizeof(text), "standby (device.use %s)", gOthers[i]);
        line(first && i == 0 ? area : "", false, gOthers[i], text);
    }
}

static void wifiLine(ucxapp_t *app)
{
    char ssid[40];
    char ip[40];
    char text[128];
    uCxWifiStationStatus_t status;
    int rssi = 0;
    const char *port = ucxapp_device_port();

    if (app->ucx == NULL) {
        if (gOtherCount == 0) {
            line("WIFI", false, "-", "no module");
        }
        otherModules("WIFI", true);
        return;
    }
    if (!ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        line("WIFI", false, port, "not connected");
    } else {
        if (uCxWifiStationStatusBegin(app->ucx, U_WIFI_STATUS_ID_RSSI, &status)) {
            rssi = (int)status.rsp.StatusIdInt.int_val;
        }
        uCxEnd(app->ucx);
        snprintf(text, sizeof(text), "%s  %d dBm  %s", ssid, rssi, ip[0] ? ip : "no IP");
        line("WIFI", true, port, text);
    }
    otherModules("WIFI", false);
}

static void bleLine(ucxapp_t *app)
{
    uCxBtListConnections_t conn;
    char text[96];
    char gatt[64];
    int count = 0;

    if (app->ucx == NULL) {
        if (gOtherCount == 0) {
            line("BLE", false, "-", "no module");
        }
        otherModules("BLE", true);
        return;
    }
    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &conn)) {
        count++;
    }
    uCxEnd(app->ucx);
    (void)ucxapp_gatt_summary(gatt, sizeof(gatt));
    snprintf(text, sizeof(text), "%d link(s)%s%s", count, gatt[0] ? "  " : "", gatt);
    line("BLE", count > 0, ucxapp_device_port(), text);
    otherModules("BLE", false);
}

static void stream(const char *name, const char *kind, bool (*get)(char *, size_t))
{
    char text[128];
    bool open = get(text, sizeof(text));
    const char *port = ucxapp_stream_open_port(kind);

    line(name, open, port != NULL ? port : "-", open ? text : "not connected");
}

/* GNSS receivers recognised by their USB descriptor, without opening them. */
static void gnssLines(void)
{
    static ucxapp_serial_port_t ports[32];
    char text[128];
    int shown = 0;
    const char *open = ucxapp_stream_open_port("gnss");

    if (ucxapp_gnss_dash(text, sizeof(text))) {
        line("GNSS", true, open != NULL ? open : "-", text);
        shown++;
    }
    int count = ucxapp_platform_list_serial_ports(ports, (int)(sizeof(ports) / sizeof(ports[0])));
    for (int i = 0; i < count; i++) {
        if (!ucxapp_port_skip(&ports[i]) && ucxapp_port_is_gnss(&ports[i]) &&
            (open == NULL || strcmp(open, ports[i].name) != 0)) {
            snprintf(text, sizeof(text), "found, not open (gnss.open %s)", ports[i].name);
            line(shown == 0 ? "GNSS" : "", false, ports[i].name, text);
            shown++;
        }
    }
    if (shown == 0) {
        line("GNSS", false, "-", "not connected");
    }
}

static void printDash(ucxapp_t *app)
{
    gOtherCount = ucxapp_find_modules(gOthers, DASH_MAX_DEV);
    rule("ucxapp dash");
    wifiLine(app);
    bleLine(app);
    gnssLines();
    stream("CELL", "cell", ucxapp_cell_dash);
    stream("IMU", "imu", ucxapp_imu_dash);
    rule(NULL);
}

/* dash [watch [seconds]]: once, or refreshed every second (default 10 s, any key stops). */
static int cmdDash(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 1;

    if (argc > 3 || (argc >= 2 && strcmp(argv[1], "watch") != 0) ||
        (argc == 3 && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1 || seconds > 3600))) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        seconds = 10;
    }
    for (int32_t s = 0; s < seconds; s++) {
        printDash(app);
        if (s + 1 == seconds) {
            break;
        }
        for (int t = 0; t < 20; t++) {
            if (ucxapp_platform_key_pressed()) {
                (void)ucxapp_platform_get_key();
                return UCXAPP_OK;
            }
            ucxapp_platform_sleep_ms(50);
        }
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kCmds[] = {
    { "dash", "[watch [seconds]]", "One card with Wi-Fi, BLE, GNSS, cellular and IMU status; watch refreshes every second (default 10 s, any key stops)", 0, cmdDash },
};

void ucxapp_register_dash_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
