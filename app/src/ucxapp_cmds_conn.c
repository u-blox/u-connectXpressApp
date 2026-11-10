/* conn: the module's live links (Wi-Fi station and access point, BLE with Channel Sounding distance, Thread and
 * sockets) as a table, and as compact lines for the web Connections view (host and the STM32 web boards).
 *
 * Snapshot lines, fields split by '|' (text fields never hold '|' or a newline):
 *   m|port|model|firmware|uptime_s|exact|bt_addr   module; exact 0 when no +STARTUP was seen since connect
 *   w|ssid|rssi|channel|ip|gateway                 Wi-Fi station, only when connected
 *   a|ssid|channel|ip|stations|mac,mac,...         Wi-Fi access point, only when it has an address
 *   b|conn|addr|rssi|mtu|role|txphy|cs_mm|cs_age_ms  BLE link; role c(entral) or p(eripheral), cs empty if none
 *   t|role|rloc16                                  Thread, only when enabled
 *   n|rloc16|role|rssi|link_quality                Thread neighbour
 *   s|handle|tcp/udp|state|remote                  socket; state listen, open or conn
 *   q|broker:port|client_id                         MQTT, only while connected
 *   g|port|status                                  GNSS receiver on the host (gnss.open)
 *   c|port|status                                  cellular modem on the host (cell.open) */

#include "ucxapp_cmd.h"
#include "ucxapp_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_general.h"
#ifndef UCXAPP_LITE
#include "u_cx_mqtt.h"
#endif
#include "u_cx_socket.h"
#include "u_cx_system.h"
#include "u_cx_thread.h"
#include "u_cx_wifi.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define CONN_MAX_BLE   8
#define CONN_MAX_SOCK  8
#define CONN_MAX_NEIGH 8
#define CONN_MAX_STA   8

typedef struct {
    char *out;
    size_t size;
    size_t n;
} snap_t;

static void put(snap_t *s, const char *fmt, ...)
{
    va_list ap;

    if (s->n + 1 >= s->size) {
        return;
    }
    va_start(ap, fmt);
    int w = vsnprintf(s->out + s->n, s->size - s->n, fmt, ap);
    va_end(ap);
    if (w < 0 || s->n + (size_t)w >= s->size) {
        /* drop the partial line so the reader never sees half a record */
        s->out[s->n] = '\0';
        s->n = s->size;
        return;
    }
    s->n += (size_t)w;
}

/* Copies text without the record and field separators. */
static void clean(char *dst, size_t size, const char *src)
{
    size_t n = 0;

    for (; src != NULL && *src != '\0' && n + 1 < size; src++) {
        dst[n++] = (*src == '|' || *src == '\n' || *src == '\r') ? ' ' : *src;
    }
    dst[n] = '\0';
}

static int wifiInt(uCxHandle_t *ucx, uWifiStatusId_t id, int fallback)
{
    uCxWifiStationStatus_t st;
    int v = fallback;

    if (uCxWifiStationStatusBegin(ucx, id, &st)) {
        v = (int)st.rsp.StatusIdInt.int_val;
    }
    uCxEnd(ucx);
    return v;
}

static void ipText(int32_t r, uSockIpAddress_t *ip, char *out, size_t size)
{
    if (r != 0 || uCxIpAddressToString(ip, out, size) <= 0) {
        out[0] = '\0';
    }
}

static void wifiLines(uCxHandle_t *ucx, snap_t *s)
{
    char ssid[40], ip[40], gw[40], tmp[40];
    uSockIpAddress_t addr;
    uCxWifiApGetConnectionParams_t ap;
    uMacAddress_t mac;
    char macs[CONN_MAX_STA * 18 + 1] = "";
    size_t used = 0;
    int count = 0;

    if (ucxapp_wifi_get_status(ucx, tmp, sizeof(tmp), ip, sizeof(ip))) {
        clean(ssid, sizeof(ssid), tmp);
        ipText(uCxWifiStationGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_GATE_WAY, &addr), &addr, gw, sizeof(gw));
        put(s, "w|%s|%d|%d|%s|%s\n", ssid, wifiInt(ucx, U_WIFI_STATUS_ID_RSSI, 0),
            wifiInt(ucx, U_WIFI_STATUS_ID_CHANNEL, 0), ip, gw);
    }
    ipText(uCxWifiApGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_IPV4, &addr), &addr, ip, sizeof(ip));
    if (ip[0] == '\0' || strcmp(ip, "0.0.0.0") == 0) {
        return;
    }
    ssid[0] = '\0';
    int ch = 0;
    if (uCxWifiApGetConnectionParamsBegin(ucx, &ap)) {
        clean(ssid, sizeof(ssid), ap.ssid);
        ch = (int)ap.channel;
    }
    uCxEnd(ucx);
    uCxWifiApListStationsBegin(ucx);
    while (uCxWifiApListStationsGetNext(ucx, &mac)) {
        if (count < CONN_MAX_STA) {
            used += (size_t)snprintf(macs + used, sizeof(macs) - used, "%s%02X:%02X:%02X:%02X:%02X:%02X",
                                     count > 0 ? "," : "", mac.address[0], mac.address[1], mac.address[2],
                                     mac.address[3], mac.address[4], mac.address[5]);
        }
        count++;
    }
    uCxEnd(ucx);
    put(s, "a|%s|%d|%s|%d|%s\n", ssid, ch, ip, count, macs);
}

static void bleLines(uCxHandle_t *ucx, snap_t *s)
{
    uCxBtListConnections_t c;
    uBtLeAddress_t addr[CONN_MAX_BLE];
    int32_t handle[CONN_MAX_BLE];
    int count = 0;

    /* List first, then ask per link: a query inside the listing would end it. */
    uCxBluetoothListConnectionsBegin(ucx);
    while (uCxBluetoothListConnectionsGetNext(ucx, &c)) {
        if (count < CONN_MAX_BLE) {
            handle[count] = c.conn_handle;
            addr[count] = c.bd_addr;
            count++;
        }
    }
    uCxEnd(ucx);
    for (int i = 0; i < count; i++) {
        int32_t rssi = 0, mtu = 0, role = 0, phy = 0, mm = 0;
        uint32_t age = 0;
        char cs[24] = "|";
        const uint8_t *a = addr[i].address;

        (void)uCxBluetoothGetRssi(ucx, handle[i], &rssi);
        (void)uCxBluetoothGetConnectionStatus(ucx, handle[i], U_BT_PROP_ID_MTU_SIZE, &mtu);
        (void)uCxBluetoothGetConnectionStatus(ucx, handle[i], U_BT_PROP_ID_LOCAL_ROLE, &role);
        (void)uCxBluetoothGetConnectionStatus(ucx, handle[i], U_BT_PROP_ID_TX_PHY, &phy);
        if (ucxapp_bt_cs_last(handle[i], &mm, &age)) {
            snprintf(cs, sizeof(cs), "%d|%u", (int)mm, (unsigned)age);
        }
        put(s, "b|%d|%02X:%02X:%02X:%02X:%02X:%02X|%d|%d|%s|%s|%s\n", (int)handle[i], a[0], a[1], a[2], a[3], a[4],
            a[5], (int)rssi, (int)mtu, role == 1 ? "c" : role == 2 ? "p" : "", phy & 4 ? "coded" : phy & 2 ? "2M" : "1M",
            cs);
    }
}

static void threadLines(ucxapp_t *app, snap_t *s)
{
    uEnabled_t on = U_ENABLED_NO;
    uCxThreadGetStatus_t st;
    uCxThreadListNeighbors_t nb;
    int count = 0;
    char role[16] = "on";
    int rloc = -1;

    if (!ucxapp_module_has_thread(app) || uCxThreadGetEnabled(app->ucx, &on) != 0 || on != U_ENABLED_YES) {
        return;
    }
    if (uCxThreadGetStatusBegin(app->ucx, &st)) {
        snprintf(role, sizeof(role), "%s", ucxapp_thread_role_name(st.device_role));
        rloc = (int)st.rloc16;
    }
    uCxEnd(app->ucx);
    put(s, "t|%s|%04x\n", role, rloc < 0 ? 0xfffe : rloc);
    uCxThreadListNeighborsBegin(app->ucx);
    while (uCxThreadListNeighborsGetNext(app->ucx, &nb) && count++ < CONN_MAX_NEIGH) {
        put(s, "n|%04x|%s|%d|%d\n", (int)nb.rloc16, ucxapp_thread_role_name(nb.neighbor_role), (int)nb.rssi,
            (int)nb.link_quality);
    }
    uCxEnd(app->ucx);
}

static void sockLines(uCxHandle_t *ucx, snap_t *s)
{
    uCxSocketListStatus_t st;
    uCxSocketListStatus_t list[CONN_MAX_SOCK];
    int count = 0;

    uCxSocketListStatusBegin(ucx);
    while (uCxSocketListStatusGetNext(ucx, &st)) {
        if (ucxapp_sock_listed(&st) && count < CONN_MAX_SOCK) {
            list[count++] = st;
        }
    }
    uCxEnd(ucx);
    for (int i = 0; i < count; i++) {
        uCxSocketGetPeerAddress_t peer;
        char remote[48] = "", ip[40];

        if (list[i].status == 2 && uCxSocketGetPeerAddress(ucx, list[i].socket_handle, &peer) == 0 &&
            uCxIpAddressToString(&peer.remote_ip, ip, sizeof(ip)) > 0) {
            snprintf(remote, sizeof(remote), "%s:%d", ip, (int)peer.remote_port);
        }
        put(s, "s|%d|%s|%s|%s\n", (int)list[i].socket_handle,
            list[i].protocol == U_SOCKET_PROTOCOL_TCP ? "tcp" : "udp",
            list[i].status == 1 ? "listen" : list[i].status == 2 ? "conn" : "open", remote);
    }
}

static void mqttLine(uCxHandle_t *ucx, snap_t *s)
{
#ifndef UCXAPP_LITE
    uCxMqttGetConnectionParams_t p;
    char host[64] = "", id[40] = "";
    int32_t port = 0;

    if (!ucxapp_mqtt_up()) {
        return;
    }
    if (uCxMqttGetConnectionParamsBegin(ucx, 0, &p)) {
        clean(host, sizeof(host), p.hostname);
        clean(id, sizeof(id), p.client_id);
        port = p.port;
    }
    uCxEnd(ucx);
    if (host[0] != '\0') {
        put(s, "q|%s:%d|%s\n", host, (int)port, id);
    } else {
        put(s, "q||\n");
    }
#else
    (void)ucx;
    (void)s;
#endif
}

/* GNSS receiver or cellular modem on the host, from their cached status (the module is not asked). */
static void hostLine(snap_t *s, char tag, const char *kind, bool (*dash)(char *, size_t))
{
    char text[128], line[128], port[40];

    if (!dash(text, sizeof(text))) {
        return;
    }
    clean(line, sizeof(line), text);
    const char *p = ucxapp_stream_open_port(kind);

    if (p == NULL && tag == 'c') {
        p = ucxapp_cell_port();   /* the modem is a stream only while it is bridged */
    }
    clean(port, sizeof(port), p);
    put(s, "%c|%s|%s\n", tag, port, line);
}

/* Model, firmware and BLE address do not change while the port stays the same: read them once. */
static void moduleLine(ucxapp_t *app, snap_t *s, const char *port)
{
    static char sPort[40], sModel[32], sFw[40], sBt[20];
    static uint32_t sBoot;
    const char *text = NULL;
    bool exact;
    uint32_t boot = ucxapp_module_boot_ms(&exact);
    uint32_t up = (ucxapp_platform_tick_ms() - boot) / 1000u;

    if (strcmp(sPort, port) != 0 || sModel[0] == '\0' || boot != sBoot) {
        uMacAddress_t mac;

        sBoot = boot;
        snprintf(sPort, sizeof(sPort), "%s", port);
        sModel[0] = sFw[0] = sBt[0] = '\0';
        if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
            clean(sModel, sizeof(sModel), text);
        }
        uCxEnd(app->ucx);
        text = NULL;
        if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
            clean(sFw, sizeof(sFw), text);
        }
        uCxEnd(app->ucx);
        if (uCxSystemGetLocalAddress(app->ucx, U_INTERFACE_ID_BLUETOOTH, &mac) == 0) {
            snprintf(sBt, sizeof(sBt), "%02X:%02X:%02X:%02X:%02X:%02X", mac.address[0], mac.address[1],
                     mac.address[2], mac.address[3], mac.address[4], mac.address[5]);
        }
    }
    put(s, "m|%s|%s|%s|%lu|%d|%s\n", port, sModel, sFw, (unsigned long)up, exact ? 1 : 0, sBt);
}

size_t ucxapp_conn_snapshot(ucxapp_t *app, char *out, size_t size)
{
    snap_t s = { out, size, 0 };
    char port[40];

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (app != NULL && app->ucx != NULL) {
        clean(port, sizeof(port), ucxapp_device_port());
        moduleLine(app, &s, port);
        if (ucxapp_module_has_wifi(app)) {
            wifiLines(app->ucx, &s);
        }
        bleLines(app->ucx, &s);
        threadLines(app, &s);
        if (ucxapp_module_has_wifi(app) || ucxapp_module_has_thread(app)) {
            sockLines(app->ucx, &s);
            mqttLine(app->ucx, &s);
        }
    }
    hostLine(&s, 'g', "gnss", ucxapp_gnss_dash);
    hostLine(&s, 'c', "cell", ucxapp_cell_dash);
    return s.n < size ? s.n : strlen(out);
}

/* Splits one snapshot line into at most max fields (in place). */
static int fields(char *line, char *f[], int max)
{
    int n = 0;

    f[n++] = line;
    for (char *p = line; *p != '\0' && n < max; p++) {
        if (*p == '|') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    for (int i = n; i < max; i++) {
        f[i] = "";
    }
    return n;
}

static void printTable(char *snap)
{
    int links = 0;
    char *next = snap;

    if (snap[0] != 'm') {
        ucxapp_print("no module open\n");
    }
    while (*next != '\0') {
        char *line = next;
        char *end = strchr(line, '\n');
        char *f[10];

        if (end != NULL) {
            *end = '\0';
            next = end + 1;
        } else {
            next = line + strlen(line);
        }
        (void)fields(line, f, 10);
        switch (f[0][0]) {
        case 'm': {
            unsigned long up = strtoul(f[4], NULL, 10);

            ucxapp_print("module  %s %s on %s, up %s%lu days %02lu:%02lu:%02lu%s%s\n", f[2], f[3], f[1],
                         f[5][0] == '1' ? "" : "at least ", up / 86400, up / 3600 % 24, up / 60 % 60, up % 60,
                         f[6][0] ? ", BT " : "", f[6]);
            break;
        }
        case 'w':
            ucxapp_print("  wifi    %-20s %4s dBm  ch %-3s %s gw %s\n", f[1], f[2], f[3], f[4][0] ? f[4] : "no IP",
                         f[5][0] ? f[5] : "-");
            links++;
            break;
        case 'a':
            ucxapp_print("  ap      %-20s ch %-3s %s, %s station(s) %s\n", f[1], f[2], f[3], f[4], f[5]);
            links++;
            break;
        case 'b':
            ucxapp_print("  ble %-3s %s  %4s dBm  mtu %-3s %-10s %s", f[1], f[2], f[3], f[4],
                         f[5][0] == 'c' ? "central" : f[5][0] == 'p' ? "peripheral" : "-", f[6]);
            if (f[7][0] != '\0') {
                int mm = (int)strtol(f[7], NULL, 10);
                ucxapp_print("  distance %d.%02d m (%s ms ago)", mm / 1000, (mm < 0 ? -mm : mm) % 1000 / 10, f[8]);
            }
            ucxapp_print("\n");
            links++;
            break;
        case 't':
            ucxapp_print("  thread  %-10s rloc16 %s\n", f[1], f[2]);
            links++;
            break;
        case 'n':
            ucxapp_print("    nbr   %s %-8s %4s dBm  lq %s\n", f[1], f[2], f[3], f[4]);
            break;
        case 's':
            ucxapp_print("  sock %-2s %s %-6s %s\n", f[1], f[2], f[3], f[4]);
            links++;
            break;
        case 'g':
            ucxapp_print("  gnss    %-8s %s\n", f[1], f[2]);
            links++;
            break;
        case 'c':
            ucxapp_print("  cell    %-8s %s\n", f[1], f[2]);
            links++;
            break;
        default:
            break;
        }
    }
    if (links == 0) {
        ucxapp_print("  no links\n");
    }
}

/* conn.status [raw]: one look at every link the module has. */
static int cmdConnStatus(ucxapp_t *app, int argc, char *argv[])
{
    static char snap[UCXAPP_CONN_SNAPSHOT];
    bool raw = argc == 2 && strcmp(argv[1], "raw") == 0;

    if (argc > 2 || (argc == 2 && !raw)) {
        return UCXAPP_ERR_USAGE;
    }
    (void)ucxapp_conn_snapshot(app, snap, sizeof(snap));
    if (raw) {
        ucxapp_print("%s", snap);
    } else {
        printTable(snap);
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kCmds[] = {
    { "conn.status", "[raw]", "Every live link: Wi-Fi station and access point, BLE (RSSI, MTU, role, PHY, Channel Sounding distance), Thread neighbours, sockets, and the GNSS receiver and cellular modem on the host; raw prints the lines the web Connections view reads", 0, cmdConnStatus },
};

void ucxapp_register_conn_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
