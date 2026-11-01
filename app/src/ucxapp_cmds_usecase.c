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

/* Everyday use cases. Row kinds: 'w' needs Wi-Fi on the module, 'g' a GNSS receiver, anything else is fixed.
   A row with a command is a gap when that command is not in this build. Names stay under 15 characters. */
typedef struct {
    char kind;
    const char *verdict;
    const char *what;
    const char *how;
    const char *cmd;
} ucRow_t;

typedef struct {
    const char *id;
    const char *title;
    const ucRow_t *rows;
    size_t rowCount;
} ucCase_t;

static const ucRow_t kBoat[] = {
    { 'g', "[check]", "GNSS receiver", "any NMEA receiver on a serial port: gnss.open finds port and baud", "gnss.open" },
    { 'w', "[ok]", "Onboard Wi-Fi", "the module is the access point: wifi.ap.enable (WPA2, max 5 tablets or phones)", NULL },
    { 0, "[ok]", "Chart app", "NMEA over TCP port 10110 (OpenCPN, Navionics, iNavX): gnss.bridge tcp 10110", "gnss.bridge" },
    { 0, "[ok]", "Phone over BLE", "NMEA over Nordic UART instead: gnss.bridge ble nus", "gnss.bridge" },
    { 0, "[ok]", "Trip log", "the raw receiver stream to a file: gnss.log trip.nmea", "gnss.log" },
    { 0, "[limit]", "One bridge", "one gnss.bridge at a time: TCP for the tablet or BLE for the phone", NULL },
};

static const ucRow_t kRtk[] = {
    { 'g', "[check]", "RTK receiver", "needs an RTK receiver (ZED-F9P, ZED-X20P); a plain GNSS gets metres, not cm", "gnss.open" },
    { 'w', "[ok]", "Home Wi-Fi", "the corrections come over the station link: wifi.connect", NULL },
    { 0, "[ok]", "NTRIP client", "RTCM or SPARTN from a caster to the receiver, GGA back every 10 s", "gnss.bridge" },
    { 0, "[check]", "Caster", "a base within about 20 km (RTK2go) or a PointPerfect plan", NULL },
    { 0, "[ok]", "Phone or robot", "the corrected NMEA over BLE: add ble to gnss.bridge ntrip", "gnss.bridge" },
};

static const ucRow_t kMower[] = {
    { 0, "[ok]", "Owner's phone", "first setup over BLE, the phone sends the home Wi-Fi: gatt.server.wps 120", "gatt.server.wps" },
    { 'w', "[ok]", "Garden Wi-Fi", "station on the home network: wifi.connect, wifi.profile.add for next time", NULL },
    { 'g', "[check]", "HP GNSS", "a high precision receiver (ZED-F9P, ZED-X20P) for cm; plain GNSS gets metres", "gnss.open" },
    { 0, "[ok]", "PointPerfect", "SPARTN over NTRIP: gnss.bridge ntrip <caster> 2101 NEAR-SPARTN <user> <pass>", "gnss.bridge" },
    { 0, "[ok]", "Phone status", "add ble: the corrected position to the owner's app over Nordic UART", "gnss.bridge" },
    { 0, "[check]", "Edge of garden", "corrections stop where Wi-Fi ends: add cell to gnss.bridge ntrip", "cell.connect" },
    { 0, "[ok]", "Theft alert", "position to your server over cellular: cell.connect, gnss.bridge cell", "gnss.bridge" },
    { 0, "[limit]", "One bridge", "corrections and the theft link share the receiver; one gnss.bridge at a time", NULL },
};

static const ucRow_t kFarm[] = {
    { 'g', "[check]", "HP GNSS", "a high precision receiver (ZED-F9P, ZED-X20P) for auto-steer at 2-3 cm", "gnss.open" },
    { 0, "[check]", "Field link", "no Wi-Fi in the field: a cellular modem and SIM, cell.open, cell.attach on", "cell.attach" },
    { 0, "[ok]", "PointPerfect", "NTRIP over cellular: gnss.bridge ntrip <caster> 2101 NEAR-SPARTN <user> <pass> cell", "gnss.bridge" },
    { 0, "[ok]", "Cab tablet", "add ble: the corrected NMEA to the guidance app over Nordic UART", "gnss.bridge" },
    { 0, "[ok]", "Field log", "the track to a file while driving: gnss.log field.nmea nmea", "gnss.log" },
    { 'w', "[ok]", "Farmyard Wi-Fi", "back at the yard: wifi.connect, then upload the log with tftp.put or ft.put", NULL },
    { 0, "[limit]", "Coverage", "over cellular the bridge does not reconnect; restart it after a dead zone", NULL },
};

static const ucRow_t kTracker[] = {
    { 'g', "[check]", "GNSS receiver", "open it first: gnss.open, then gnss.watch for the fix", "gnss.open" },
    { 'w', "[ok]", "At home", "Wi-Fi to your server: gnss.bridge tcpc <host> <port> (reconnects)", "gnss.bridge" },
    { 0, "[check]", "On the road", "a cellular modem (u-blox, Quectel, SIMCom, Telit, nRF91): cell.connect", "cell.connect" },
    { 0, "[ok]", "Server", "receives plain NMEA lines over TCP; any TCP listener or GPS logger app", NULL },
    { 0, "[limit]", "Power", "the host and receiver run all the time; no sleep schedule in this app", NULL },
};

static const ucRow_t kHome[] = {
    { 'w', "[ok]", "Home Wi-Fi", "station on the home network: wifi.connect, wifi.profile.add for next time", NULL },
    { 0, "[ok]", "MQTT", "publish and subscribe to a local broker (Mosquitto in Home Assistant)", "mqtt.connect" },
    { 0, "[limit]", "Broker login", "mqtt.connect takes host and port only: allow anonymous on the local broker", NULL },
    { 0, "[limit]", "Message size", "max 511 bytes per message, QoS 0, not retained", NULL },
    { 0, "[ok]", "Sensor", "an IMU on the washing machine or door (imu.open, imu.status)", "imu.open" },
};

static const ucRow_t kProvision[] = {
    { 0, "[ok]", "BLE", "every NORA module: the phone connects without a password", NULL },
    { 'w', "[ok]", "Wi-Fi", "the phone sends SSID and password, the module joins", NULL },
    { 0, "[ok]", "Provisioning", "u-blox Wi-Fi Provisioning service: gatt.server.wps 120", "gatt.server.wps" },
    { 0, "[check]", "Phone app", "an app that writes the provisioning service (or a generic GATT app)", NULL },
    { 0, "[ok]", "QR instead", "or join the device's own AP from a QR code: wifi.ap.enable", NULL },
};

static const ucRow_t kHotel[] = {
    { 'w', "[ok]", "Guest Wi-Fi", "open or WPA2-Personal: wifi.connect <ssid> [password]", NULL },
    { 0, "[ok]", "Login page", "wifi.portal check finds it, wifi.portal login fills in and submits the form", "wifi.portal" },
    { 0, "[limit]", "Page types", "plain HTML forms; pages built by JavaScript or with a captcha need a browser", NULL },
    { 0, "[check]", "Session", "many portals tie the login to the MAC address and expire after a day", NULL },
    { 0, "[ok]", "Check", "wifi.portal check again, then net.ping", "net.ping" },
};

static const ucRow_t kNotify[] = {
    { 0, "[ok]", "iPhone", "Apple Notification Center Service: pair from Settings > Bluetooth, then ancs.sub", "ancs.on" },
    { 0, "[ok]", "Actions", "answer or decline a call, dismiss a message: ancs.action <uid> pos|neg", "ancs.action" },
    { 0, "[check]", "Android", "no ANCS: a companion app writes to the notify.on service", "notify.on" },
    { 0, "[limit]", "Links", "one phone at a time, notifications only while it is connected", NULL },
};

static const ucRow_t kRemote[] = {
    { 0, "[ok]", "Keyboard", "BLE HID keyboard, mouse and media keys: hid.setup", "hid.setup" },
    { 0, "[ok]", "Clicker", "next slide with hid.key space, back with hid.key backspace", "hid.key" },
    { 0, "[ok]", "Media", "volume and play/pause for a TV or phone: hid.media volup", "hid.media" },
    { 0, "[check]", "Host", "pair from the Bluetooth settings; HOGP works on Windows, macOS, iOS, Android", NULL },
    { 0, "[limit]", "Keys", "characters, enter, esc, tab, backspace and space; no arrow keys", NULL },
};

#define CASE(id, title, rows) { id, title, rows, sizeof(rows) / sizeof(rows[0]) }
static const ucCase_t kCases[] = {
    CASE("boat", "Boat or camper: GPS to the chart app on a tablet", kBoat),
    CASE("rtk", "Survey pole or rover: centimetre position with RTK", kRtk),
    CASE("mower", "Robotic lawn mower: owner's phone, HP GNSS with PointPerfect, cellular theft alert", kMower),
    CASE("farm", "Tractor or field robot: auto-steer with PointPerfect over cellular", kFarm),
    CASE("tracker", "Car, bike or pet tracker: position to your server", kTracker),
    CASE("home", "Smart home sensor: MQTT to Home Assistant", kHome),
    CASE("provision", "New smart device: Wi-Fi set up from a phone over BLE", kProvision),
    CASE("hotel", "Hotel or cafe Wi-Fi with a login page", kHotel),
    CASE("notify", "Phone notifications on a small display or wearable", kNotify),
    CASE("remote", "Presentation clicker or media remote (BLE keyboard)", kRemote),
};
#define CASE_COUNT (sizeof(kCases) / sizeof(kCases[0]))

static int cmdCheck(ucxapp_t *app, int argc, char *argv[])
{
    const ucCase_t *uc = NULL;
    bool wifi = true;
    char ssid[40] = "", ip[48] = "", gnss[64] = "";

    if (argc == 2 && strcmp(argv[1], "tool") == 0) {
        return cmdTool(app, 1, argv);
    }
    for (size_t i = 0; argc == 2 && i < CASE_COUNT; i++) {
        if (strcmp(argv[1], kCases[i].id) == 0) {
            uc = &kCases[i];
        }
    }
    if (uc == NULL) {
        if (argc == 2) {
            ucxapp_print("ERROR: no use case %s (see usecase.list)\n", argv[1]);
        }
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx != NULL) {
        wifi = ucxapp_module_has_wifi(app);
    }
    (void)ucxapp_gnss_summary(gnss, sizeof(gnss));
    ucxapp_print("%s\n", uc->title);
    for (size_t i = 0; i < uc->rowCount; i++) {
        const ucRow_t *r = &uc->rows[i];

        if (r->cmd != NULL && ucxapp_cmd_find(r->cmd) == NULL) {
            char how[96];

            snprintf(how, sizeof(how), "%s is not in this build; use Windows or Linux", r->cmd);
            row("[gap]", r->what, how);
        } else if (r->kind == 'w' && !wifi) {
            row("[gap]", r->what, "this module has no Wi-Fi (NORA-B26); use a NORA-W36");
        } else if (r->kind == 'g' && gnss[0] != '\0') {
            char how[96];

            snprintf(how, sizeof(how), "open: %s", gnss);
            row("[ok]", r->what, how);
        } else {
            row(r->verdict, r->what, r->how);
        }
    }
    if (app->ucx == NULL) {
        ucxapp_print("Now: no module connected\n");
    } else if (wifi && ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("Now: station on %s (%s)\n", ssid, ip);
    } else {
        ucxapp_print("Now: module connected, station not connected\n");
    }
    ucxapp_print("Steps: examples uc-%s, run one with examples uc-%s <number>\n", uc->id, uc->id);
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("%-10s%s\n", "tool", "Connected tool: factory Wi-Fi (Enterprise), AP for laptops, HTTPS, trays, BLE");
    for (size_t i = 0; i < CASE_COUNT; i++) {
        ucxapp_print("%-10s%s\n", kCases[i].id, kCases[i].title);
    }
    ucxapp_print("Fit check: usecase.check <name>; steps: usecase.ex (tool) or examples uc-<name>\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gUsecaseCmds[] = {
    { "usecase.list",  "",          "Use cases with a fit check and ready-made steps", 0, cmdList },
    { "usecase.check", "<name>",    "Fit check of a use case against the module and this build; names from usecase.list", 0, cmdCheck },
    { "usecase.tool",  "[mbit/s]",  "Fit check of the connected tool use case; with a measured rate, the download time", 0, cmdTool },
};

void ucxapp_register_usecase_cmds(void)
{
    ucxapp_cmd_register(gUsecaseCmds, sizeof(gUsecaseCmds) / sizeof(gUsecaseCmds[0]));
}
