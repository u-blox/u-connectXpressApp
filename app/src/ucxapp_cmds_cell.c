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
#include "ucxapp_vport.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
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

/* Sends one command and collects the reply until OK/ERROR; keeps reading extraMs after OK for URC lines, or until
 * "until" has been seen untilCount times. */
static int atCmdUntil(const char *cmd, char *resp, size_t size, int32_t timeoutMs, int32_t extraMs, const char *until,
                      int untilCount)
{
    uint8_t scratch[64];
    size_t n = 0;
    int result = AT_TIMEOUT;
    uint32_t start;

    while (ucxapp_uart_read(gUart, scratch, sizeof(scratch), 10) > 0) {
        /* drop stale bytes */
    }
    resp[0] = '\0';
    char line[320];
    int len = snprintf(line, sizeof(line), "%s\r", cmd);
    if (len <= 0 || ucxapp_uart_write(gUart, (const uint8_t *)line, (size_t)len) < 0) {
        return AT_TIMEOUT;
    }
    start = ucxapp_platform_tick_ms();
    uint32_t doneMs = 0;
    while (ucxapp_platform_tick_ms() - start < (uint32_t)timeoutMs) {
        int32_t got = ucxapp_uart_read(gUart, scratch, sizeof(scratch), 50);
        for (int32_t i = 0; i < got && n < size - 1; i++) {
            resp[n++] = (char)scratch[i];
        }
        resp[n] = '\0';
        if (result == AT_TIMEOUT) {
            const char *connect = strstr(resp, "CONNECT");
            if (strstr(resp, "\r\nOK\r\n") != NULL || (n >= 4 && strncmp(resp, "OK\r\n", 4) == 0)) {
                result = AT_OK;
            } else if (strstr(resp, "ERROR") != NULL) {
                result = AT_ERROR;
            } else if (connect != NULL && strstr(connect, "\r\n") != NULL) {
                result = AT_OK;   /* the modem is in data mode now: no OK follows */
            }
            if (result != AT_TIMEOUT) {
                if (extraMs <= 0) {
                    break;
                }
                doneMs = ucxapp_platform_tick_ms();
            }
        } else if (ucxapp_platform_tick_ms() - doneMs >= (uint32_t)extraMs) {
            break;
        } else if (until != NULL) {
            int seen = 0;
            const char *last = NULL;
            for (const char *p = strstr(resp, until); p != NULL; p = strstr(p + 1, until)) {
                seen++;
                last = p;
            }
            if (seen >= untilCount && strstr(last, "\r\n") != NULL) {   /* the last line is complete too */
                break;
            }
        }
    }
    return result;
}

static int atCmd(const char *cmd, char *resp, size_t size, int32_t timeoutMs, int32_t extraMs)
{
    return atCmdUntil(cmd, resp, size, timeoutMs, extraMs, NULL, 0);
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
    /* a SARA-R5 says only "SARA-R510S-01B-00" to ATI: put the manufacturer in front for the vendor checks */
    if (atCmd("AT+CGMI", resp, sizeof(resp), 1000, 0) == AT_OK) {
        const char *p = resp;
        for (;;) {
            p += strspn(p, "\r\n");
            size_t len = strcspn(p, "\r\n");
            if (len == 0 || (len >= 2 && strncmp(p, "OK", 2) == 0)) {
                break;
            }
            if (strncmp(p, "AT", 2) != 0) {   /* skip the echo */
                char maker[32];
                snprintf(maker, sizeof(maker), "%.*s", (int)len, p);
                if (strstr(gMaker, maker) == NULL) {
                    char both[sizeof(maker) + sizeof(gMaker) + 1];
                    int n = snprintf(both, sizeof(both), "%s%s%s", maker, gMaker[0] ? " " : "", gMaker);
                    size_t keep = n < 0 ? 0 : (size_t)n < sizeof(gMaker) ? (size_t)n : sizeof(gMaker) - 1;
                    memcpy(gMaker, both, keep);
                    gMaker[keep] = '\0';
                }
                break;
            }
            p += len;
        }
    }
}

static bool openPort(const char *port, int32_t baud)
{
    char resp[RESP_SIZE];

    gUart = ucxapp_uart_open(port, baud);
    if (gUart == NULL) {
        return false;
    }
    gOpen = true;
    if (atCmd("AT", resp, sizeof(resp), 600, 0) != AT_OK) {
        ucxapp_uart_close(gUart);
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
        ucxapp_uart_close(gUart);
    }
    gUart = NULL;
    gOpen = false;
    gMaker[0] = '\0';
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
static char gLinkHost[104];   /* what cell.connect opened, so a dropped link can be opened again */
static int32_t gLinkRemote;
static bool gLinkUdp;
static bool gRelinking;

/* The modem is busy as a data link or with its own bridge: say so instead of probing its port. */
static bool cellBusy(void)
{
    if (gDataMode) {
        ucxapp_print("  %s: data link to %s:%d now (cell.disconnect or cell.open for AT commands)\n",
                     gLinkPort, gLinkHost, (int)gLinkRemote);
        return true;
    }
    if (gStream != NULL) {
        ucxapp_print("  %s: bridged now (cell.bridge off or cell.open for AT commands)\n", ucxapp_stream_port(gStream));
        return true;
    }
    return false;
}

static int cmdDisconnect(ucxapp_t *app, int argc, char *argv[]);

/* XPLR-IOT-1: an FT4232 gives the NORA-B106 (Zephyr shell, interface A) and the SARA-R510S (interface B). The
 * NORA-B106 drives the SARA power pin, so the SARA port stays silent until "modules SARAR5 power_on" is sent. */
#define XPLR_SARA_BOOT_MS 15000

static bool isFt4232(const ucxapp_serial_port_t *sp)
{
    return sp->vid == 0x0403 && sp->pid == 0x6011;
}

/* Powers the SARA-R5 on when an XPLR-IOT-1 shell answers on one of the FT4232 ports; returns that shell port. */
static const char *xplrPowerOn(const ucxapp_serial_port_t *ports, int count)
{
    const char *dev = ucxapp_device_port();

    for (int i = 0; i < count; i++) {
        const ucxapp_serial_port_t *sp = &ports[i];
        if (!isFt4232(sp) || !sp->available || (dev != NULL && strcasecmp(dev, sp->name) == 0)) {
            continue;
        }
        uPortUartHandle_t h = ucxapp_uart_open(sp->name, 115200);
        if (h == NULL) {
            continue;
        }
        char resp[256] = "";
        size_t n = 0;
        uint8_t scratch[64];
        uint32_t start = ucxapp_platform_tick_ms();

        (void)ucxapp_uart_write(h, (const uint8_t *)"\r", 1);
        while (ucxapp_platform_tick_ms() - start < 400 && strstr(resp, "uart:~$") == NULL) {
            int32_t got = ucxapp_uart_read(h, scratch, sizeof(scratch), 50);
            for (int32_t k = 0; k < got; k++) {
                if (n < sizeof(resp) - 1) {
                    resp[n++] = (char)scratch[k];
                } else {   /* keep the tail, where the prompt is */
                    memmove(resp, resp + 1, n - 1);
                    resp[n - 1] = (char)scratch[k];
                }
            }
            resp[n] = '\0';
        }
        bool shell = strstr(resp, "uart:~$") != NULL;
        if (shell) {
            static const char kCmd[] = "modules SARAR5 power_on\r";
            ucxapp_print("  %s: XPLR-IOT-1 shell, powering the SARA-R5 on\n", sp->name);
            (void)ucxapp_uart_write(h, (const uint8_t *)kCmd, sizeof(kCmd) - 1);
            while (ucxapp_uart_read(h, scratch, sizeof(scratch), 300) > 0) {
                /* let the shell finish its reply */
            }
        }
        ucxapp_uart_close(h);
        if (shell) {
            return sp->name;
        }
    }
    return NULL;
}

/* After xplrPowerOn: waits for one of the silent FT4232 ports (or only "want") to answer AT and leaves it open. */
static bool xplrOpenSara(const ucxapp_serial_port_t *ports, int count, const char *shell, const char *want)
{
    uint32_t start = ucxapp_platform_tick_ms();

    while (ucxapp_platform_tick_ms() - start < XPLR_SARA_BOOT_MS) {
        for (int i = 0; i < count; i++) {
            const ucxapp_serial_port_t *sp = &ports[i];
            if (want != NULL ? strcasecmp(sp->name, want) != 0
                             : !isFt4232(sp) || !sp->available || strcasecmp(sp->name, shell) == 0) {
                continue;
            }
            if (openPort(sp->name, 115200)) {
                char resp[RESP_SIZE];
                /* a NORA-W36 EVK is an FT4232 too: only a 3GPP modem identifies itself */
                if (atCmd("AT+CGMI", resp, sizeof(resp), 1000, 0) == AT_OK) {
                    return true;
                }
                closePort();
            }
        }
        ucxapp_platform_sleep_ms(500);
    }
    return false;
}

/* ---- commands ---- */

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    static ucxapp_serial_port_t ports[32];
    bool onlyVirt = argc == 2 && ucxapp_vport_is(argv[1]);
    int count = onlyVirt ? 0 : ucxapp_platform_list_serial_ports(ports, 32);
    int found = 0;
    bool cellFound = false;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gOpen) {   /* the scan shares the open modem's UART: list that one only */
        ucxapp_print("  %s: AT module at %d baud, open now: %s\n1 AT port(s) found\n", gPort, (int)gBaud, gMaker);
        return UCXAPP_OK;
    }
    if ((argc == 1 || strcasecmp(argv[1], gDataMode ? gLinkPort : gStream != NULL ? ucxapp_stream_port(gStream) : "") == 0) &&
        cellBusy()) {
        ucxapp_print("1 AT port(s) found\n");
        return UCXAPP_OK;
    }
    for (int i = 0; i < count; i++) {
        const ucxapp_serial_port_t *sp = &ports[i];
        if (argc == 2 ? strcasecmp(sp->name, argv[1]) != 0 : ucxapp_port_skip(sp)) {
            continue;
        }
        for (size_t b = 0; b < sizeof(kBauds) / sizeof(kBauds[0]); b++) {
            if (openPort(sp->name, kBauds[b])) {
                char resp[RESP_SIZE];
                ucxapp_print("  %s%s%s%s: ", sp->name, sp->desc[0] ? " (" : "", sp->desc, sp->desc[0] ? ")" : "");
                ucxapp_print("AT module at %d baud: %s\n", (int)kBauds[b], gMaker);
                cellFound = cellFound || atCmd("AT+CGMI", resp, sizeof(resp), 1000, 0) == AT_OK;
                closePort();
                found++;
                break;
            }
        }
    }
    if (!cellFound && !onlyVirt) {
        const char *shell = xplrPowerOn(ports, count);
        if (shell != NULL && xplrOpenSara(ports, count, shell, argc == 2 ? argv[1] : NULL)) {
            ucxapp_print("  %s: AT module at 115200 baud: %s\n", gPort, gMaker);
            closePort();
            found++;
        }
    }
    /* a virtual module gets the simulated SARA-R5 (virt:cell) */
    if ((onlyVirt || (argc == 1 && ucxapp_module_is_virtual(app))) && openPort(UCXAPP_VPORT_CELL, 115200)) {
        ucxapp_print("  %s (%s): AT module at 115200 baud: %s\n", UCXAPP_VPORT_CELL,
                     ucxapp_vport_desc(UCXAPP_VPORT_CELL), gMaker);
        closePort();
        found++;
    }
    ucxapp_print("%d AT port(s) found\n", found);
    return found > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = 115200;

    if (argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &baud) || baud <= 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (gOpen) {
        if (argc == 1 || strcasecmp(argv[1], gPort) == 0) {
            ucxapp_print("cellular module already open on %s: %s\n", gPort, gMaker);
            return UCXAPP_OK;
        }
        ucxapp_print("ERROR: already open on %s (cell.close first)\n", gPort);
        return UCXAPP_ERR_FAILED;
    }
    if (gDataMode || gStream != NULL) {
        const char *busy = gDataMode ? gLinkPort : ucxapp_stream_port(gStream);
        if (argc == 2 && strcasecmp(argv[1], busy) != 0) {
            ucxapp_print("ERROR: the cellular module is in use:\n");
            (void)cellBusy();
            return UCXAPP_ERR_FAILED;
        }
        /* cell.open is for AT commands: end the data link or the bridge that holds the port */
        if (gDataMode) {
            ucxapp_print("ending the data link to %s:%d\n", gLinkHost, (int)gLinkRemote);
            int r = cmdDisconnect(app, 1, argv);
            if (r != UCXAPP_OK) {
                return r;
            }
        } else {
            char port[32];
            int32_t sbaud = ucxapp_stream_baud(gStream);
            snprintf(port, sizeof(port), "%s", busy);
            ucxapp_print("stopping the cell bridge on %s\n", port);
            ucxapp_stream_close(gStream);
            gStream = NULL;
            if (!openPort(port, sbaud)) {
                ucxapp_print("ERROR: no AT response on %s after the bridge\n", port);
                return UCXAPP_ERR_FAILED;
            }
        }
        ucxapp_print("cellular module open on %s at %d baud: %s\n", gPort, (int)gBaud, gMaker);
        return UCXAPP_OK;
    }
    if (argc == 1) {
        static ucxapp_serial_port_t ports[32];
        int count = ucxapp_platform_list_serial_ports(ports, 32);
        char resp[RESP_SIZE];

        for (int i = 0; i < count && !gOpen; i++) {
            const char *dev = ucxapp_device_port();

            if (ucxapp_port_skip(&ports[i]) || (dev != NULL && strcasecmp(dev, ports[i].name) == 0)) {
                continue;
            }
            for (size_t b = 0; b < sizeof(kBauds) / sizeof(kBauds[0]) && !openPort(ports[i].name, kBauds[b]); b++) {
            }
            /* a NORA module answers AT too, but has no 3GPP identification */
            if (gOpen && atCmd("AT+CGMI", resp, sizeof(resp), 1000, 0) != AT_OK) {
                closePort();
            }
        }
        if (!gOpen && ucxapp_module_is_virtual(app)) {
            (void)openPort(UCXAPP_VPORT_CELL, baud);
        }
        if (!gOpen) {
            const char *shell = xplrPowerOn(ports, count);
            if (shell != NULL) {
                (void)xplrOpenSara(ports, count, shell, NULL);
            }
        }
        if (!gOpen) {
            ucxapp_print("ERROR: no cellular module found (cell.open <port>)\n");
            return UCXAPP_ERR_FAILED;
        }
    } else if (!openPort(argv[1], baud)) {
        static ucxapp_serial_port_t ports[32];
        int count = ucxapp_vport_is(argv[1]) ? 0 : ucxapp_platform_list_serial_ports(ports, 32);
        const char *shell = xplrPowerOn(ports, count);

        if (shell == NULL || !xplrOpenSara(ports, count, shell, argv[1])) {
            ucxapp_print("ERROR: no AT response on %s at %d baud\n", argv[1], (int)baud);
            return UCXAPP_ERR_FAILED;
        }
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

/* Registration state from +CEREG (LTE), or +CREG when LTE is not registered; -1 when the modem does not say. */
static int regState(void)
{
    static const char *const kCmd[] = { "AT+CEREG?", "AT+CREG?" };
    static const char *const kTag[] = { "+CEREG:", "+CREG:" };
    char resp[RESP_SIZE];
    char v[160];
    int state = -1;

    for (int i = 0; i < 2 && state != 1 && state != 5; i++) {
        if (atCmd(kCmd[i], resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, kTag[i], v, sizeof(v))) {
            const char *comma = strchr(v, ',');
            int stat = comma ? atoi(comma + 1) : atoi(v);
            state = state < 0 || stat == 1 || stat == 5 || stat == 3 ? stat : state;
        }
    }
    return state;
}

/* Network name from +COPS: <mode>[,<format>,"<oper>"[,<act>]]; false when not on a network. */
static bool operatorName(const char *cops, char *out, size_t size)
{
    const char *q = strchr(cops, '"');
    const char *e = q != NULL ? strchr(q + 1, '"') : NULL;

    if (e == NULL || e == q + 1) {
        return false;
    }
    snprintf(out, size, "%.*s", (int)(e - q - 1), q + 1);
    return true;
}

/* First non-zero address of context 1: +CGPADDR: 1,"10.0.0.5" or 1,"0.0.0.0 <IPv6>" as a SARA-R5 gives it. */
static bool pdpAddress(char *out, size_t size)
{
    char resp[RESP_SIZE];
    char v[160];

    out[0] = '\0';
    if (atCmd("AT+CGPADDR=1", resp, sizeof(resp), 3000, 0) != AT_OK || !field(resp, "+CGPADDR:", v, sizeof(v))) {
        return false;
    }
    for (const char *p = strchr(v, ','); p != NULL && *p != '\0';) {
        p += strspn(p, ", \"");
        size_t len = strcspn(p, ", \"");
        if (len == 0) {
            break;
        }
        if (strspn(p, "0.:") < len) {   /* 0.0.0.0 and :: are no address */
            snprintf(out, size, "%.*s", (int)len, p);
            return true;
        }
        p += len;
    }
    return false;
}

/* Why the network refused: +CEER: "EMM cause",15,"No suitable cells in tracking area" without the quotes. */
static void printRejectCause(void)
{
    char resp[RESP_SIZE];
    char v[160];

    if (atCmd("AT+CEER", resp, sizeof(resp), 2000, 0) == AT_OK && field(resp, "+CEER:", v, sizeof(v))) {
        char clean[160];
        size_t n = 0;
        for (const char *p = v; *p != '\0' && n < sizeof(clean) - 1; p++) {
            if (*p == ',') {
                clean[n++] = ' ';
            } else if (*p != '"') {
                clean[n++] = *p;
            }
        }
        clean[n] = '\0';
        ucxapp_print("Reason:    %s\n", clean);
    }
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    char resp[RESP_SIZE];
    char v[160];
    char name[64];
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
    int stat = regState();
    if (stat >= 0) {
        ucxapp_print("Network:   %s\n", regText(stat));
        if (stat == 3) {
            printRejectCause();
        }
    }
    if (atCmd("AT+COPS?", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+COPS:", v, sizeof(v))) {
        ucxapp_print("Operator:  %s\n", operatorName(v, name, sizeof(name)) ? name : "none");
    }
    ucxapp_print("IP:        %s\n", pdpAddress(v, sizeof(v)) ? v : "none");
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
    /* network selection, scan and attach may take up to 3 minutes (TS 27.007) */
    bool slow = strncasecmp(cmd, "AT+COPS=", 8) == 0 || strncasecmp(cmd, "AT+CGATT=", 9) == 0 ||
                strncasecmp(cmd, "AT+CGACT=", 9) == 0;
    int r = atCmd(cmd, resp, sizeof(resp), slow ? 180000 : 10000, 0);
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
    ucxapp_print("attaching (can take a few minutes)...\n");
    if (atCmd("AT+CGATT=1", resp, sizeof(resp), 180000, 0) != AT_OK) {
        ucxapp_print("ERROR: attach failed (check SIM, APN and signal with cell.status)\n");
        return UCXAPP_ERR_FAILED;
    }
    /* many modems (SARA-R5 among them) say OK at once and attach in the background */
    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t shown = start;
    int stat = -1;
    bool attached = false;
    while (!attached && ucxapp_platform_tick_ms() - start < 180000) {
        attached = atCmd("AT+CGATT?", resp, sizeof(resp), 3000, 0) == AT_OK && field(resp, "+CGATT:", v, sizeof(v)) &&
                   atoi(v) == 1;
        if (!attached) {
            stat = regState();
            if (stat == 3) {
                break;
            }
            if (ucxapp_platform_tick_ms() - shown >= 10000) {
                shown = ucxapp_platform_tick_ms();
                ucxapp_print("  %s (%u s)\n", regText(stat), (unsigned)((shown - start) / 1000));
            }
            ucxapp_platform_sleep_ms(1000);
        }
    }
    if (!attached) {
        ucxapp_print("ERROR: not attached: %s\n", regText(stat));
        printRejectCause();
        return UCXAPP_ERR_FAILED;
    }
    (void)atCmd("AT+CGACT=1,1", resp, sizeof(resp), 180000, 0);   /* already active on modems that do it at attach */
    if (!pdpAddress(v, sizeof(v))) {
        ucxapp_print("ERROR: attached, but context 1 has no IP address (check the APN with cell.apn)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("attached, IP %s\n", v);
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
    /* 4 replies (u-blox), or 4 replies and the summary (Quectel) */
    int r = atCmdUntil(cmd, resp, sizeof(resp), 15000, 12000, strstr(gMaker, "Quectel") != NULL ? "+QPING:" : "+UUPING",
                       strstr(gMaker, "Quectel") != NULL ? 5 : 4);
    printLines(resp, cmd);
    return r == AT_OK ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

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
        char name[64];
        if (operatorName(v, name, sizeof(name))) {
            n += snprintf(out + n, size - (size_t)n, "  %s", name);
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
            int32_t got = ucxapp_uart_read(gUart, buf, sizeof(buf), 100);
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
    snprintf(gLinkHost, sizeof(gLinkHost), "%s", argv[1]);
    gLinkRemote = port;
    gLinkUdp = strcmp(proto, "UDP") == 0;
    closePort();
    gDataMode = true;
    if (!gRelinking) {
        ucxapp_print("connected to %s:%d; %s is now a data link, e.g. gnss.bridge cell\n", argv[1], (int)port, gLinkPort);
    }
    return UCXAPP_OK;
}

int ucxapp_cell_connect(ucxapp_t *app, const char *host, int32_t port)
{
    char name[] = "cell.connect";
    char h[104];
    char p[8];
    char *argv[] = { name, h, p };

    snprintf(h, sizeof(h), "%s", host);
    snprintf(p, sizeof(p), "%d", (int)port);
    return cmdConnect(app, 3, argv);
}

/* Closes the transparent socket on the modem (command mode on gUart). */
static void closeLinkSocket(void)
{
    char resp[RESP_SIZE];

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
}

bool ucxapp_cell_relink(void)
{
    char name[] = "cell.connect";
    char host[sizeof(gLinkHost)];
    char p[8];
    char proto[] = "udp";
    char *argv[] = { name, host, p, proto };
    int r;

    if (!gDataMode) {
        return false;
    }
    if (!openPort(gLinkPort, gLinkBaud)) {
        return false;   /* still in data mode, or the modem is gone: try again later */
    }
    gDataMode = false;
    closeLinkSocket();
    snprintf(host, sizeof(host), "%s", gLinkHost);
    snprintf(p, sizeof(p), "%d", (int)gLinkRemote);
    gRelinking = true;
    r = cmdConnect(NULL, gLinkUdp ? 4 : 3, argv);
    gRelinking = false;
    if (r != UCXAPP_OK) {
        closePort();
        gDataMode = true;   /* keep the link settings so the next try connects the same way */
        return false;
    }
    return true;
}

/* cell.disconnect: guard time, +++, then close the socket. Stop any bridge using the link first. */
static int cmdDisconnect(ucxapp_t *app, int argc, char *argv[])
{
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
    (void)ucxapp_stream_bridge_stop_port(gLinkPort);
    gUart = ucxapp_uart_open(gLinkPort, gLinkBaud);
    if (gUart == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", gLinkPort);
        return UCXAPP_ERR_FAILED;
    }
    U_CX_PORT_SLEEP_MS(1200);
    (void)ucxapp_uart_write(gUart, (const uint8_t *)"+++", 3);
    U_CX_PORT_SLEEP_MS(1200);
    while (ucxapp_uart_read(gUart, buf, sizeof(buf), 50) > 0) {
        /* drop the OK */
    }
    ucxapp_uart_close(gUart);
    gUart = NULL;
    gDataMode = false;
    if (!openPort(gLinkPort, gLinkBaud)) {
        ucxapp_print("ERROR: no AT response on %s after +++, use cell.open\n", gLinkPort);
        return UCXAPP_ERR_FAILED;
    }
    closeLinkSocket();
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
    if (gStream != NULL) {
        int r = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 2323);
        if (!ucxapp_stream_bridge_active(gStream)) {   /* last bridge gone: back to command mode */
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
    { "cell.detect", "[port]", "Find a cellular modem answering AT on the PC serial ports (powers on the SARA-R5 of an XPLR-IOT-1 through its NORA-B106 shell; a virtual module also offers virt:cell, a simulated SARA-R5)", 0, cmdDetect },
    { "cell.open", "[port] [baud=115200]", "Open a cellular modem on a PC serial port (finds it when omitted; virt:cell is the simulated one); ends a data link or cell bridge on it", 0, cmdOpen },
    { "cell.close", "", "Close the cellular modem", 0, cmdClose },
    { "cell.status", "", "SIM, signal, registration, operator and IP", 0, cmdStatus },
    { "cell.at", "<command>", "Send a raw AT command to the cellular modem", 0, cmdAt },
    { "cell.apn", "[<apn> [ip|ipv6|ipv4v6]]", "Show or set the default APN (context 1)", 0, cmdApn },
    { "cell.attach", "[on|off]", "Attach and activate data (on), detach (off) or show the state", 0, cmdAttach },
    { "cell.ping", "<host>", "Ping through the cellular link (Quectel and u-blox only)", 0, cmdPing },
    { "cell.connect", "<host> <port> [tcp|udp]", "Transparent TCP/UDP socket through the modem (u-blox, Quectel, SIMCom, Telit, Nordic nRF91) for gnss.bridge cell, imu.bridge cell, can.bridge cell", 0, cmdConnect },
    { "cell.disconnect", "", "Leave transparent mode and close the socket (stops the bridges that use the link)", 0, cmdDisconnect },
    { "cell.bridge", "tcp [port] | tls [port] [cert] [clientauth] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | uart <port> [baud] | off [n] | status", "Bridge the modem's AT console to TCP, UDP, BLE or another PC UART; up to 4 at once", 0, cmdBridge },
};

void ucxapp_register_cell_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
