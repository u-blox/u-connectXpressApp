/* cell.*: basic cellular module access over a PC serial port using the generic 3GPP AT commands (TS 27.007).
 * Works the same on most modules (Quectel, SIMCom, u-blox, Telit, Sierra, Nordic nRF91, ...). Only ping is
 * vendor specific. Host only; not verified against hardware yet. */

#include "ucxapp_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_port.h"
#include "u_port_uart.h"
#include "ucxapp_platform.h"
#include "ucxapp_stream.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define RESP_SIZE 1024

static const int32_t kBauds[] = { 115200, 9600, 921600, 460800 };

static uPortUartHandle_t gUart;
static bool gOpen;
static char gPort[32];
static int32_t gBaud;
static char gMaker[64];   /* first line of ATI, used to pick the vendor ping command */

enum { AT_OK = 0, AT_ERROR = 1, AT_TIMEOUT = -1 };

/* Sends one command and collects the reply until OK/ERROR; keeps reading extraMs after OK for URC lines. */
static int atCmd(const char *cmd, char *resp, size_t size, int32_t timeoutMs, int32_t extraMs)
{
    uint8_t scratch[64];
    size_t n = 0;
    int result = AT_TIMEOUT;
    uint32_t start;

    while (uPortUartRead(gUart, scratch, sizeof(scratch), 10) > 0) {
        /* drop stale bytes */
    }
    resp[0] = '\0';
    char line[320];
    int len = snprintf(line, sizeof(line), "%s\r", cmd);
    if (len <= 0 || uPortUartWrite(gUart, (const uint8_t *)line, (size_t)len) < 0) {
        return AT_TIMEOUT;
    }
    start = ucxapp_platform_tick_ms();
    uint32_t doneMs = 0;
    while (ucxapp_platform_tick_ms() - start < (uint32_t)timeoutMs) {
        int32_t got = uPortUartRead(gUart, scratch, sizeof(scratch), 50);
        for (int32_t i = 0; i < got && n < size - 1; i++) {
            resp[n++] = (char)scratch[i];
        }
        resp[n] = '\0';
        if (result == AT_TIMEOUT) {
            if (strstr(resp, "\r\nOK\r\n") != NULL || (n >= 4 && strncmp(resp, "OK\r\n", 4) == 0)) {
                result = AT_OK;
            } else if (strstr(resp, "ERROR") != NULL) {
                result = AT_ERROR;
            }
            if (result != AT_TIMEOUT) {
                if (extraMs <= 0) {
                    break;
                }
                doneMs = ucxapp_platform_tick_ms();
            }
        } else if (ucxapp_platform_tick_ms() - doneMs >= (uint32_t)extraMs) {
            break;
        }
    }
    return result;
}

/* Copies the text after "prefix" on the first line that starts with it. */
static bool field(const char *resp, const char *prefix, char *out, size_t size)
{
    size_t plen = strlen(prefix);
    const char *p = resp;

    while (*p != '\0') {
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        if (strncmp(p, prefix, plen) == 0) {
            p += plen;
            while (*p == ' ') {
                p++;
            }
            size_t i = 0;
            while (*p != '\0' && *p != '\r' && *p != '\n' && i < size - 1) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return true;
        }
        while (*p != '\0' && *p != '\r' && *p != '\n') {
            p++;
        }
    }
    return false;
}

/* Prints every reply line except the echoed command and the final OK. */
static void printLines(const char *resp, const char *cmd)
{
    const char *p = resp;
    char line[200];

    while (*p != '\0') {
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        size_t i = 0;
        while (*p != '\0' && *p != '\r' && *p != '\n') {
            if (i < sizeof(line) - 1) {
                line[i++] = *p;
            }
            p++;
        }
        line[i] = '\0';
        if (i > 0 && strcmp(line, "OK") != 0 && strcmp(line, cmd) != 0) {
            ucxapp_print("%s\n", line);
        }
    }
}

static bool needOpen(void)
{
    if (!gOpen) {
        ucxapp_print("ERROR: no cellular module open (use cell.open)\n");
        return false;
    }
    return true;
}

static void learnMaker(void)
{
    char resp[RESP_SIZE];

    gMaker[0] = '\0';
    if (atCmd("ATI", resp, sizeof(resp), 1000, 0) == AT_OK) {
        const char *p = resp;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
        if (strncmp(p, "ATI", 3) == 0) {   /* echo */
            while (*p != '\0' && *p != '\n') {
                p++;
            }
            while (*p == '\r' || *p == '\n') {
                p++;
            }
        }
        size_t i = 0;
        while (*p != '\0' && *p != '\r' && *p != '\n' && i < sizeof(gMaker) - 1) {
            gMaker[i++] = *p++;
        }
        gMaker[i] = '\0';
    }
}

static bool openPort(const char *port, int32_t baud)
{
    char resp[RESP_SIZE];

    gUart = uPortUartOpen(port, baud, false);
    if (gUart == NULL) {
        return false;
    }
    gOpen = true;
    if (atCmd("AT", resp, sizeof(resp), 600, 0) != AT_OK) {
        uPortUartClose(gUart);
        gUart = NULL;
        gOpen = false;
        return false;
    }
    (void)atCmd("ATE0", resp, sizeof(resp), 500, 0);
    snprintf(gPort, sizeof(gPort), "%s", port);
    gBaud = baud;
    learnMaker();
    return true;
}

static void closePort(void)
{
    if (gOpen) {
        uPortUartClose(gUart);
    }
    gUart = NULL;
    gOpen = false;
    gMaker[0] = '\0';
}

/* ---- commands ---- */

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    static ucxapp_serial_port_t ports[32];
    int count = ucxapp_platform_list_serial_ports(ports, 32);
    int found = 0;
    (void)app;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gOpen) {
        ucxapp_print("ERROR: cellular module already open on %s (cell.close first)\n", gPort);
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 0; i < count; i++) {
        const ucxapp_serial_port_t *sp = &ports[i];
        if (argc == 2 ? strcasecmp(sp->name, argv[1]) != 0 : ucxapp_port_skip(sp)) {
            continue;
        }
        for (size_t b = 0; b < sizeof(kBauds) / sizeof(kBauds[0]); b++) {
            if (openPort(sp->name, kBauds[b])) {
                ucxapp_print("  %s%s%s%s: ", sp->name, sp->desc[0] ? " (" : "", sp->desc, sp->desc[0] ? ")" : "");
                ucxapp_print("AT module at %d baud: %s\n", (int)kBauds[b], gMaker);
                closePort();
                found++;
                break;
            }
        }
    }
    ucxapp_print("%d AT port(s) found\n", found);
    return found > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = 115200;
    (void)app;

    if (argc < 2 || argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &baud) || baud <= 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (gOpen) {
        ucxapp_print("ERROR: already open on %s (cell.close first)\n", gPort);
        return UCXAPP_ERR_FAILED;
    }
    if (!openPort(argv[1], baud)) {
        ucxapp_print("ERROR: no AT response on %s at %d baud\n", argv[1], (int)baud);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("cellular module open on %s at %d baud: %s\n", gPort, (int)gBaud, gMaker);
    return UCXAPP_OK;
}

static int cmdClose(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    closePort();
    ucxapp_print("closed\n");
    return UCXAPP_OK;
}

static const char *regText(int stat)
{
    switch (stat) {
    case 0: return "not registered";
    case 1: return "registered (home)";
    case 2: return "searching";
    case 3: return "denied";
    case 5: return "registered (roaming)";
    default: return "unknown";
    }
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char v[160];
    (void)app;
    (void)argc;
    (void)argv;

    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Port:      %s at %d baud\n", gPort, (int)gBaud);
    ucxapp_print("Module:    %s\n", gMaker[0] ? gMaker : "?");
    if (atCmd("AT+CPIN?", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+CPIN:", v, sizeof(v))) {
        ucxapp_print("SIM:       %s\n", v);
    } else {
        ucxapp_print("SIM:       not ready or missing\n");
    }
    if (atCmd("AT+CCID", resp, sizeof(resp), 2000, 0) == AT_OK &&
        (field(resp, "+CCID:", v, sizeof(v)) || field(resp, "+ICCID:", v, sizeof(v)))) {
        ucxapp_print("ICCID:     %s\n", v);
    }
    if (atCmd("AT+CSQ", resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, "+CSQ:", v, sizeof(v))) {
        int rssi = atoi(v);
        if (rssi >= 0 && rssi < 99) {
            ucxapp_print("Signal:    %d dBm (CSQ %d)\n", -113 + 2 * rssi, rssi);
        } else {
            ucxapp_print("Signal:    unknown\n");
        }
    }
    const char *regCmd[] = { "AT+CEREG?", "AT+CREG?" };
    const char *regTag[] = { "+CEREG:", "+CREG:" };
    for (int i = 0; i < 2; i++) {
        if (atCmd(regCmd[i], resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, regTag[i], v, sizeof(v))) {
            const char *comma = strchr(v, ',');
            int stat = comma ? atoi(comma + 1) : atoi(v);
            ucxapp_print("Network:   %s (%s)\n", regText(stat), i == 0 ? "LTE" : "2G/3G");
            if (stat == 1 || stat == 5) {
                break;
            }
        }
    }
    if (atCmd("AT+COPS?", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+COPS:", v, sizeof(v))) {
        ucxapp_print("Operator:  %s\n", v);
    }
    if (atCmd("AT+CGPADDR=1", resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, "+CGPADDR:", v, sizeof(v))) {
        ucxapp_print("IP:        %s\n", v);
    }
    return UCXAPP_OK;
}

/* cell.at <command...>: send a raw AT command and print the reply. */
static int cmdAt(ucxapp_t *app, int argc, char *argv[])
{
    char cmd[256];
    char resp[RESP_SIZE];
    size_t pos = 0;
    (void)app;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 1; i < argc && pos < sizeof(cmd) - 1; i++) {
        pos += (size_t)snprintf(cmd + pos, sizeof(cmd) - pos, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    int r = atCmd(cmd, resp, sizeof(resp), 10000, 0);
    printLines(resp, cmd);
    if (r == AT_TIMEOUT) {
        ucxapp_print("(no final response)\n");
    }
    return r == AT_OK ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* cell.apn [apn [ip|ipv6|ipv4v6]]: show or set the default PDP context (cid 1). */
static int cmdApn(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char cmd[160];
    (void)app;

    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (argc == 1) {
        int r = atCmd("AT+CGDCONT?", resp, sizeof(resp), 3000, 0);
        printLines(resp, "AT+CGDCONT?");
        return r == AT_OK ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    const char *type = "IP";
    if (argc == 3) {
        if (strcasecmp(argv[2], "ipv6") == 0) {
            type = "IPV6";
        } else if (strcasecmp(argv[2], "ipv4v6") == 0) {
            type = "IPV4V6";
        } else if (strcasecmp(argv[2], "ip") != 0) {
            return UCXAPP_ERR_USAGE;
        }
    }
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"%s\",\"%s\"", type, argv[1]);
    if (atCmd(cmd, resp, sizeof(resp), 3000, 0) != AT_OK) {
        ucxapp_print("ERROR: APN not accepted\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("APN set to %s (%s)\n", argv[1], type);
    return UCXAPP_OK;
}

/* cell.attach [on|off]: full functionality, packet attach and PDP context 1. Without argument shows the state. */
static int cmdAttach(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char v[160];
    (void)app;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (argc == 1) {
        if (atCmd("AT+CGATT?", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+CGATT:", v, sizeof(v))) {
            ucxapp_print("packet service: %s\n", atoi(v) == 1 ? "attached" : "detached");
            return UCXAPP_OK;
        }
        return UCXAPP_ERR_FAILED;
    }
    if (strcmp(argv[1], "off") == 0) {
        (void)atCmd("AT+CGACT=0,1", resp, sizeof(resp), 10000, 0);
        int r = atCmd("AT+CGATT=0", resp, sizeof(resp), 30000, 0);
        ucxapp_print(r == AT_OK ? "detached\n" : "ERROR: detach failed\n");
        return r == AT_OK ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    (void)atCmd("AT+CFUN=1", resp, sizeof(resp), 15000, 0);
    ucxapp_print("attaching (can take up to a minute)...\n");
    if (atCmd("AT+CGATT=1", resp, sizeof(resp), 75000, 0) != AT_OK) {
        ucxapp_print("ERROR: attach failed (check SIM, APN and signal with cell.status)\n");
        return UCXAPP_ERR_FAILED;
    }
    (void)atCmd("AT+CGACT=1,1", resp, sizeof(resp), 30000, 0);
    if (atCmd("AT+CGPADDR=1", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+CGPADDR:", v, sizeof(v))) {
        ucxapp_print("attached, IP %s\n", v);
    } else {
        ucxapp_print("attached\n");
    }
    return UCXAPP_OK;
}

/* cell.ping <host>: the only vendor specific command; needs an active data context (cell.attach on). */
static int cmdPing(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char cmd[200];
    (void)app;

    if (argc != 2 || strlen(argv[1]) > 100) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (strstr(gMaker, "Quectel") != NULL) {
        snprintf(cmd, sizeof(cmd), "AT+QPING=1,\"%s\",4,4", argv[1]);
    } else if (strstr(gMaker, "u-blox") != NULL) {
        snprintf(cmd, sizeof(cmd), "AT+UPING=\"%s\"", argv[1]);
    } else {
        ucxapp_print("ERROR: ping is not mapped for '%s'; use cell.at with the module's ping command\n", gMaker);
        return UCXAPP_ERR_FAILED;
    }
    int r = atCmd(cmd, resp, sizeof(resp), 15000, 12000);
    printLines(resp, cmd);
    return r == AT_OK ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* While bridged the serial port belongs to a stream; the remote side gets the modem's AT console. */
static ucxapp_stream_t *gStream;

/* Transparent data mode: the modem's UART carries the TCP/UDP payload, so any stream can bridge to that port. */
static bool gDataMode;
static char gLinkPort[32];
static int32_t gLinkBaud;
enum { VENDOR_UBLOX, VENDOR_QUECTEL, VENDOR_SIMCOM, VENDOR_TELIT, VENDOR_NORDIC };
static int gVendor;
static int gSock;

bool ucxapp_cell_data_link(char *port, size_t size, int32_t *baud)
{
    if (!gDataMode) {
        return false;
    }
    snprintf(port, size, "%s", gLinkPort);
    *baud = gLinkBaud;
    return true;
}

bool ucxapp_cell_dash(char *out, size_t size)
{
    char resp[RESP_SIZE];
    char v[160];
    int n = 0;

    out[0] = '\0';
    if (!gOpen) {
        return false;
    }
    if (gDataMode || gStream != NULL) {
        snprintf(out, size, "%s on %s", gDataMode ? "data link" : "AT bridge", gPort);
        return true;
    }
    if (atCmd("AT+CEREG?", resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, "+CEREG:", v, sizeof(v))) {
        const char *comma = strchr(v, ',');
        n = snprintf(out, size, "%s", regText(comma ? atoi(comma + 1) : atoi(v)));
    }
    if (n >= 0 && (size_t)n < size && atCmd("AT+COPS?", resp, sizeof(resp), 3000, 0) == AT_OK &&
        field(resp, "+COPS:", v, sizeof(v))) {
        char *q = strchr(v, '"');
        char *e = q != NULL ? strchr(q + 1, '"') : NULL;
        if (e != NULL) {
            *e = '\0';
            n += snprintf(out + n, size - (size_t)n, "  %s", q + 1);
        }
    }
    if (n >= 0 && (size_t)n < size && atCmd("AT+CSQ", resp, sizeof(resp), 2000, 0) == AT_OK &&
        field(resp, "+CSQ:", v, sizeof(v))) {
        int rssi = atoi(v);
        if (rssi >= 0 && rssi < 99) {
            (void)snprintf(out + n, size - (size_t)n, "  %d dBm", -113 + 2 * rssi);
        }
    }
    if (out[0] == '\0') {
        snprintf(out, size, "no answer on %s", gPort);
    }
    return true;
}

/* cell.connect <host> <port> [tcp|udp]: Quectel transparent socket (AT+QIOPEN, access mode 2). */
static int cmdConnect(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char cmd[200];
    int32_t port;
    const char *proto = "TCP";
    (void)app;

    if (argc < 3 || argc > 4 || strlen(argv[1]) > 100 || !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 4) {
        if (strcasecmp(argv[3], "udp") == 0) {
            proto = "UDP";
        } else if (strcasecmp(argv[3], "tcp") != 0) {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (gDataMode) {
        ucxapp_print("ERROR: already connected (cell.disconnect first)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (strstr(gMaker, "Quectel") != NULL) {
        gVendor = VENDOR_QUECTEL;
        (void)atCmd("AT+QIACT=1", resp, sizeof(resp), 30000, 0);
        snprintf(cmd, sizeof(cmd), "AT+QIOPEN=1,0,\"%s\",\"%s\",%d,0,2", proto, argv[1], (int)port);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR) {
            ucxapp_print("ERROR: modem refused the socket (attach first with cell.attach on)\n");
            return UCXAPP_ERR_FAILED;
        }
    } else if (strstr(gMaker, "u-blox") != NULL) {
        /* SARA/LARA: socket, connect, then direct link (AT+USODL) */
        int sock = -1;
        gVendor = VENDOR_UBLOX;
        snprintf(cmd, sizeof(cmd), "AT+USOCR=%d", strcmp(proto, "UDP") == 0 ? 17 : 6);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR ||
            sscanf(strstr(resp, "+USOCR:") != NULL ? strstr(resp, "+USOCR:") : "", "+USOCR: %d", &sock) != 1) {
            ucxapp_print("ERROR: modem refused the socket (attach first with cell.attach on)\n");
            return UCXAPP_ERR_FAILED;
        }
        snprintf(cmd, sizeof(cmd), "AT+USOCO=%d,\"%s\",%d", sock, argv[1], (int)port);
        if (atCmd(cmd, resp, sizeof(resp), 130000, 0) != AT_OK) {
            snprintf(cmd, sizeof(cmd), "AT+USOCL=%d", sock);
            (void)atCmd(cmd, resp, sizeof(resp), 10000, 0);
            ucxapp_print("ERROR: the connection was not established\n");
            return UCXAPP_ERR_FAILED;
        }
        gSock = sock;
        snprintf(cmd, sizeof(cmd), "AT+USODL=%d", sock);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR) {
            ucxapp_print("ERROR: modem refused direct link mode\n");
            return UCXAPP_ERR_FAILED;
        }
    } else if (strstr(gMaker, "SIM") != NULL) {
        /* SIMCom: transparent mode via AT+CIPMODE=1 */
        gVendor = VENDOR_SIMCOM;
        (void)atCmd("AT+CIPMODE=1", resp, sizeof(resp), 5000, 0);
        (void)atCmd("AT+NETOPEN", resp, sizeof(resp), 30000, 0);
        snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=0,\"%s\",\"%s\",%d", proto, argv[1], (int)port);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR) {
            ucxapp_print("ERROR: modem refused the socket (attach first with cell.attach on)\n");
            return UCXAPP_ERR_FAILED;
        }
    } else if (strstr(gMaker, "Telit") != NULL) {
        /* Telit: #SD online mode, context 1 */
        gVendor = VENDOR_TELIT;
        (void)atCmd("AT#SGACT=1,1", resp, sizeof(resp), 30000, 0);
        snprintf(cmd, sizeof(cmd), "AT#SD=1,%d,%d,\"%s\",0,0,1", strcmp(proto, "UDP") == 0 ? 1 : 0, (int)port, argv[1]);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR) {
            ucxapp_print("ERROR: modem refused the socket (attach first with cell.attach on)\n");
            return UCXAPP_ERR_FAILED;
        }
    } else if (strstr(gMaker, "Nordic") != NULL) {
        /* nRF91 Serial LTE Modem: #XSOCKET, #XCONNECT, #XSEND enters data mode (no CONNECT is printed) */
        gVendor = VENDOR_NORDIC;
        snprintf(cmd, sizeof(cmd), "AT#XSOCKET=1,%d,0", strcmp(proto, "UDP") == 0 ? 2 : 1);
        if (atCmd(cmd, resp, sizeof(resp), 10000, 0) == AT_ERROR) {
            ucxapp_print("ERROR: modem refused the socket (attach first with cell.attach on)\n");
            return UCXAPP_ERR_FAILED;
        }
        snprintf(cmd, sizeof(cmd), "AT#XCONNECT=\"%s\",%d", argv[1], (int)port);
        if (atCmd(cmd, resp, sizeof(resp), 60000, 0) != AT_OK ||
            atCmd("AT#XSEND", resp, sizeof(resp), 5000, 0) != AT_OK) {
            (void)atCmd("AT#XSOCKET=0", resp, sizeof(resp), 10000, 0);
            ucxapp_print("ERROR: the connection was not established\n");
            return UCXAPP_ERR_FAILED;
        }
        snprintf(resp, sizeof(resp), "CONNECT");
    } else {
        ucxapp_print("ERROR: cell.connect is not mapped for '%s'; use cell.at\n", gMaker);
        return UCXAPP_ERR_FAILED;
    }
    {
        uint8_t buf[64];
        size_t n = strlen(resp);
        uint32_t start = ucxapp_platform_tick_ms();

        while (strstr(resp, "CONNECT") == NULL && strstr(resp, "+QIOPEN:") == NULL &&
               ucxapp_platform_tick_ms() - start < 150000u) {
            int32_t got = uPortUartRead(gUart, buf, sizeof(buf), 100);
            for (int32_t i = 0; i < got && n < sizeof(resp) - 1; i++) {
                resp[n++] = (char)buf[i];
            }
            resp[n] = '\0';
        }
    }
    if (strstr(resp, "CONNECT") == NULL) {
        ucxapp_print("ERROR: the connection was not established\n");
        return UCXAPP_ERR_FAILED;
    }
    snprintf(gLinkPort, sizeof(gLinkPort), "%s", gPort);
    gLinkBaud = gBaud;
    closePort();
    gDataMode = true;
    ucxapp_print("connected to %s:%d; %s is now a data link, e.g. gnss.bridge cell\n", argv[1], (int)port, gLinkPort);
    return UCXAPP_OK;
}

/* cell.disconnect: guard time, +++, then close the socket. Stop any bridge using the link first. */
static int cmdDisconnect(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    uint8_t buf[32];
    (void)app;
    (void)argv;

    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gDataMode) {
        ucxapp_print("ERROR: no cellular data link\n");
        return UCXAPP_ERR_FAILED;
    }
    gUart = uPortUartOpen(gLinkPort, gLinkBaud, false);
    if (gUart == NULL) {
        ucxapp_print("ERROR: cannot open %s (stop the bridge that uses it first)\n", gLinkPort);
        return UCXAPP_ERR_FAILED;
    }
    U_CX_PORT_SLEEP_MS(1200);
    (void)uPortUartWrite(gUart, (const uint8_t *)"+++", 3);
    U_CX_PORT_SLEEP_MS(1200);
    while (uPortUartRead(gUart, buf, sizeof(buf), 50) > 0) {
        /* drop the OK */
    }
    uPortUartClose(gUart);
    gUart = NULL;
    gDataMode = false;
    if (!openPort(gLinkPort, gLinkBaud)) {
        ucxapp_print("ERROR: no AT response on %s after +++, use cell.open\n", gLinkPort);
        return UCXAPP_ERR_FAILED;
    }
    if (gVendor == VENDOR_UBLOX) {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+USOCL=%d", gSock);
        (void)atCmd(cmd, resp, sizeof(resp), 10000, 0);
    } else if (gVendor == VENDOR_SIMCOM) {
        (void)atCmd("AT+CIPCLOSE=0", resp, sizeof(resp), 10000, 0);
        (void)atCmd("AT+NETCLOSE", resp, sizeof(resp), 10000, 0);
    } else if (gVendor == VENDOR_TELIT) {
        (void)atCmd("AT#SH=1", resp, sizeof(resp), 10000, 0);
    } else if (gVendor == VENDOR_NORDIC) {
        (void)atCmd("AT#XSOCKET=0", resp, sizeof(resp), 10000, 0);
    } else {
        (void)atCmd("AT+QICLOSE=0", resp, sizeof(resp), 10000, 0);
    }
    ucxapp_print("disconnected, command mode on %s\n", gPort);
    return UCXAPP_OK;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    if (gStream == NULL && !needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (gStream != NULL && (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "status") == 0)) {
        int r = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 2323);
        if (strcasecmp(argv[1], "off") == 0) {
            char port[32];
            int32_t baud = ucxapp_stream_baud(gStream);
            snprintf(port, sizeof(port), "%s", ucxapp_stream_port(gStream));
            ucxapp_stream_close(gStream);
            gStream = NULL;
            if (!openPort(port, baud)) {
                ucxapp_print("ERROR: no AT response on %s after the bridge, use cell.open\n", port);
                return UCXAPP_ERR_FAILED;
            }
        }
        return r;
    }
    if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "status") == 0) {
        ucxapp_print("bridge: off\n");
        return UCXAPP_OK;
    }
    if (gStream != NULL) {
        ucxapp_print("ERROR: already bridged (cell.bridge off first)\n");
        return UCXAPP_ERR_FAILED;
    }
    char port[32];
    int32_t baud = gBaud;
    ucxapp_stream_cfg_t cfg = { "cell", NULL, NULL, 0, NULL, NULL };
    snprintf(port, sizeof(port), "%s", gPort);
    closePort();
    gStream = ucxapp_stream_open(port, baud, &cfg);
    if (gStream == NULL) {
        (void)openPort(port, baud);
        ucxapp_print("ERROR: cannot reopen %s for bridging\n", port);
        return UCXAPP_ERR_FAILED;
    }
    int r = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 2323);
    if (r != UCXAPP_OK || !ucxapp_stream_bridge_active(gStream)) {
        ucxapp_stream_close(gStream);
        gStream = NULL;
        (void)openPort(port, baud);
    }
    return r;
}

static const ucxapp_cmd_t kCmds[] = {
    { "cell.detect", "[port]", "Find a cellular module answering AT on the PC serial ports", 0, cmdDetect },
    { "cell.open", "<port> [baud]", "Open a cellular module on a PC serial port (default 115200)", 0, cmdOpen },
    { "cell.close", "", "Close the cellular module", 0, cmdClose },
    { "cell.status", "", "Show SIM, signal, registration, operator and IP", 0, cmdStatus },
    { "cell.at", "<command>", "Send a raw AT command to the cellular module", 0, cmdAt },
    { "cell.apn", "[apn [ip|ipv6|ipv4v6]]", "Show or set the default APN (context 1)", 0, cmdApn },
    { "cell.attach", "[on|off]", "Attach and activate data (on), detach (off) or show the state", 0, cmdAttach },
    { "cell.ping", "<host>", "Ping through the cellular link (Quectel and u-blox only)", 0, cmdPing },
    { "cell.connect", "<host> <port> [tcp|udp]", "Open a transparent TCP/UDP socket through the modem (u-blox, Quectel, SIMCom, Telit, Nordic nRF91); then use gnss.bridge cell or imu.bridge cell", 0, cmdConnect },
    { "cell.disconnect", "", "Leave transparent mode and close the socket", 0, cmdDisconnect },
    { "cell.bridge", "tcp [port] | tls [port] [cert] [clientauth] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | uart <port> [baud] | off | status", "Bridge the modem's AT console to TCP, UDP, BLE or another PC UART", 0, cmdBridge },
};

void ucxapp_register_cell_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
