#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

/* Connected tool: a host with a NORA-W36 on the factory network, a local AP for laptops, BLE for accessories.
   The limits are the NORA-W36 3.5.0 user guide and data sheet figures. */
#define HISTORY_MBIT      7200   /* 900 MB */
#define WINDOW_S          1800   /* 30 min */
#define UART_TRANSPARENT  25     /* Mbit/s x10 at 4 Mbaud */
#define UART_AT           10

/* "2.5" -> 25; false unless it is a positive number with at most one decimal. */
static bool parseTenths(const char *s, int32_t *out)
{
    int32_t v = 0;
    int dec = -1;

    for (; *s != '\0'; s++) {
        if (*s == '.' && dec < 0) {
            dec = 0;
        } else if (*s >= '0' && *s <= '9' && dec < 1 && v < 10000) {
            v = v * 10 + (*s - '0');
            dec = dec >= 0 ? dec + 1 : dec;
        } else {
            return false;
        }
    }
    *out = dec == 1 ? v : v * 10;
    return *out > 0;
}

static int32_t minutesAt(int32_t tenths)
{
    return (HISTORY_MBIT * 10 / tenths + 59) / 60;
}

static void row(const char *verdict, const char *what, const char *how)
{
    ucxapp_print(" %-8s %-15s %s\n", verdict, what, how);
}

static int cmdTool(ucxapp_t *app, int argc, char *argv[])
{
    int32_t measured = 0;
    bool wifi = true;
    char ssid[40] = "", ip[48] = "";

    if (argc > 2 || (argc == 2 && !parseTenths(argv[1], &measured))) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx != NULL) {
        wifi = ucxapp_module_has_wifi(app);
    }
    ucxapp_print("Connected tool: factory Wi-Fi, local AP for laptops, BLE for accessories (NORA-W36 3.5.0 limits)\n");
    row("[ok]", "Host", "STM32H7 with Zephyr runs this app: ucxapp --stm32-zephyr --board h753");
    if (!wifi) {
        row("[gap]", "Wi-Fi", "this module has no Wi-Fi (NORA-B26); use a NORA-W36");
    } else {
        row("[ok]", "Station + AP", "both at once: wifi.peap or wifi.eap, then wifi.ap.enable (WPA2, max 5 stations)");
        row("[check]", "Enterprise", "EAP-TLS (wifi.eap) and PEAP (wifi.peap); confirm WPA3-Enterprise with the factory AP");
    }
    row("[ok]", "Protocols", "the host runs them over module sockets (sock.*) or MQTT (mqtt.*)");
    row("[limit]", "HTTPS, 5 PCs", "TLS ends on the host (web.start 443 <token> https); 6 module sockets, the listener takes 2");
    if (measured > 0) {
        char line[96];

        snprintf(line, sizeof(line), "measured %d.%d Mbit/s: 900 MB in %d min (needs 4.0 Mbit/s for 30 min)",
                 (int)(measured / 10), (int)(measured % 10), (int)minutesAt(measured));
        row(measured * WINDOW_S >= HISTORY_MBIT * 10 ? "[ok]" : "[gap]", "900 MB/30 min", line);
    } else {
        char line[112];

        snprintf(line, sizeof(line), "needs 4.0 Mbit/s; 4 Mbaud UART gives 2.5 transparent, 1.0 in AT mode (%d / %d min)",
                 (int)minutesAt(UART_TRANSPARENT), (int)minutesAt(UART_AT));
        row("[gap]", "900 MB/30 min", line);
    }
    row("[limit]", "8 trays", "the AP takes 5 stations: trays on the factory network over one UDP socket, or BLE (3 links)");
    row("[ok]", "BLE", "phone setup (gatt.server.wps), service link (gatt.server.sps), accessories (bt.connect)");
    ucxapp_print("Coexistence: at most 1 TLS client, 1 BLE link and 2 TCP clients at the same time\n");
    if (app->ucx != NULL && wifi) {
        if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
            ucxapp_print("Now: station on %s (%s)\n", ssid, ip);
        } else {
            ucxapp_print("Now: station not connected\n");
        }
    }
    if (measured == 0) {
        ucxapp_print("Measure with net.iperf, then: usecase.tool <Mbit/s>\n");
    }
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("tool  Connected tool: factory Wi-Fi (Enterprise), AP for laptops, HTTPS, trays, BLE (usecase.tool, usecase.ex)\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gUsecaseCmds[] = {
    { "usecase.list", "",          "Use cases with a fit check and ready-made steps", 0, cmdList },
    { "usecase.tool", "[mbit/s]",  "Fit check of the connected tool use case; with a measured rate, the download time", 0, cmdTool },
};

void ucxapp_register_usecase_cmds(void)
{
    ucxapp_cmd_register(gUsecaseCmds, sizeof(gUsecaseCmds) / sizeof(gUsecaseCmds[0]));
}
