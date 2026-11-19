/* sniff.*: Bluetooth LE capture with the nRF Sniffer for Bluetooth LE (an nRF52840 dongle or DK with Nordic's
 * sniffer firmware) through "nrfutil ble-sniffer sniff", into a pcap file and optionally live in Wireshark.
 * nrfutil comes from UCXAPP_NRFUTIL (a command, used as given), PATH or ~/.nrfutil/bin. PC builds only. */

#include "ucxapp_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_cx_system.h"
#include "ucxapp_capture.h"
#include "ucxapp_platform.h"
#include "ucxapp_proc.h"

#ifndef __EMSCRIPTEN__

#define STOP_GRACE_MS 5000

static struct {
    ucxapp_capture_t *cap;
    char last[512];
    char port[64];
    char follow[64];
} gSniff;

static void saveLast(void);

/* nrfutil as a command prefix; false if it is not installed. */
static bool nrfutilCmd(char *out, size_t size)
{
    const char *env = getenv("UCXAPP_NRFUTIL");
    char path[512];
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    const char *exe = "\\.nrfutil\\bin\\nrfutil.exe";
#else
    const char *home = getenv("HOME");
    const char *exe = "/.nrfutil/bin/nrfutil";
#endif
    FILE *f;

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, size, "%s", env);
        return true;
    }
    if (!ucxapp_proc_which("nrfutil", path, sizeof(path))) {
        if (home == NULL) {
            return false;
        }
        (void)snprintf(path, sizeof(path), "%s%s", home, exe);
        f = fopen(path, "rb");
        if (f == NULL) {
            return false;
        }
        fclose(f);
    }
    return ucxapp_proc_quote(path, out, size);
}

/* Fills ports with the sniffers found; returns the count. */
static int findSniffers(char ports[][64], int max)
{
    ucxapp_serial_port_t *sp = (ucxapp_serial_port_t *)calloc(32, sizeof(*sp));
    int n, i, found = 0;

    if (sp == NULL) {
        return 0;
    }
    n = ucxapp_platform_list_serial_ports(sp, 32);
    for (i = 0; i < n && found < max; i++) {
        if (ucxapp_port_is_sniffer(&sp[i])) {
            (void)snprintf(ports[found++], 64, "%s", sp[i].name);
        }
    }
    free(sp);
    return found;
}

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    char ports[8][64], tool[600];
    int n, i;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    n = findSniffers(ports, 8);
    for (i = 0; i < n; i++) {
        ucxapp_print("nRF Sniffer: %s\n", ports[i]);
    }
    if (n == 0) {
        ucxapp_print("No nRF Sniffer found (an nRF52840 dongle or DK with the nRF Sniffer for Bluetooth LE firmware)\n");
    }
    ucxapp_print("nrfutil:   %s\n", nrfutilCmd(tool, sizeof(tool)) ? tool
                 : "not found (install nrfutil, then: nrfutil install ble-sniffer)");
    ucxapp_print("Wireshark: %s\n", ucxapp_capture_wireshark(tool, sizeof(tool)) ? tool : "not found");
    return n > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* "module": the connected module's Bluetooth address. Anything else is passed on as given. */
static bool followTarget(ucxapp_t *app, const char *spec, char *out, size_t size)
{
    uMacAddress_t mac;
    int32_t r;

    if (strcmp(spec, "module") != 0) {
        (void)snprintf(out, size, "%s", spec);
        return true;
    }
    if (app->ucx == NULL) {
        ucxapp_print("ERROR: follow=module needs a connected module\n");
        return false;
    }
    r = uCxSystemGetLocalAddress(app->ucx, U_INTERFACE_ID_BLUETOOTH, &mac);
    if (r != 0) {
        ucxapp_print("ERROR: read the Bluetooth address failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return false;
    }
    (void)snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac.address[0], mac.address[1], mac.address[2],
                   mac.address[3], mac.address[4], mac.address[5]);
    return true;
}

static bool append(char *cmd, size_t size, const char *opt, const char *value)
{
    char q[300];
    size_t len = strlen(cmd);

    if (value != NULL && !ucxapp_proc_quote(value, q, sizeof(q))) {
        ucxapp_print("ERROR: %s cannot be passed to nrfutil: %s\n", opt, value);
        return false;
    }
    (void)snprintf(cmd + len, size - len, " %s%s%s", opt, value != NULL ? " " : "", value != NULL ? q : "");
    return true;
}

static int cmdStart(ucxapp_t *app, int argc, char *argv[])
{
    char file[512] = "", port[64] = "", follow[64] = "", cmd[1600], q[600];
    const char *followSpec = NULL, *rssi = NULL;
    bool adv = false, coded = false, live = false, byName = false;
    int32_t v;
    int i;

    if (gSniff.cap != NULL) {
        ucxapp_print("ERROR: a capture is running (sniff.stop)\n");
        return UCXAPP_ERR_FAILED;
    }
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "follow=", 7) == 0) {
            followSpec = argv[i] + 7;
        } else if (strncmp(argv[i], "port=", 5) == 0) {
            (void)snprintf(port, sizeof(port), "%s", argv[i] + 5);
        } else if (strncmp(argv[i], "rssi=", 5) == 0 && ucxapp_parse_int(argv[i] + 5, &v) && v < 0 && v >= -127) {
            rssi = argv[i] + 5;
        } else if (strcmp(argv[i], "adv") == 0) {
            adv = true;
        } else if (strcmp(argv[i], "coded") == 0) {
            coded = true;
        } else if (strcmp(argv[i], "live") == 0) {
            live = true;
        } else if (strchr(argv[i], '=') == NULL && file[0] == '\0') {
            (void)snprintf(file, sizeof(file), "%s", argv[i]);
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (port[0] == '\0') {
        char ports[2][64];
        int n = findSniffers(ports, 2);

        if (n == 0) {
            ucxapp_print("ERROR: no nRF Sniffer found; plug one in or give port=<port>\n");
            return UCXAPP_ERR_FAILED;
        }
        (void)snprintf(port, sizeof(port), "%s", ports[0]);
    }
    if (followSpec != NULL) {
        byName = strncmp(followSpec, "name:", 5) == 0;
        if (!followTarget(app, byName ? followSpec + 5 : followSpec, follow, sizeof(follow))) {
            return UCXAPP_ERR_FAILED;
        }
    }
    if (file[0] == '\0') {
        ucxapp_capture_default_name("ble", file, sizeof(file));
    }
    if (!nrfutilCmd(cmd, sizeof(cmd))) {
        ucxapp_print("ERROR: nrfutil not found (install nrfutil, then: nrfutil install ble-sniffer)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_proc_quote(file, q, sizeof(q))) {
        ucxapp_print("ERROR: the file name cannot be passed to nrfutil\n");
        return UCXAPP_ERR_FAILED;
    }
    (void)snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), " ble-sniffer sniff --output-pcap-file %s", q);
    if (!append(cmd, sizeof(cmd), "--port", port) ||
        (follow[0] != '\0' && !append(cmd, sizeof(cmd), byName ? "--follow-by-name" : "--follow", follow)) ||
        (adv && !append(cmd, sizeof(cmd), "--only-advertising", NULL)) ||
        (coded && !append(cmd, sizeof(cmd), "--coded", NULL)) ||
        (rssi != NULL && !append(cmd, sizeof(cmd), "--rssi-cut-off", rssi))) {
        return UCXAPP_ERR_FAILED;
    }
    gSniff.cap = ucxapp_capture_start(cmd, file, false, live);
    if (gSniff.cap == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    (void)snprintf(gSniff.port, sizeof(gSniff.port), "%s", port);
    (void)snprintf(gSniff.follow, sizeof(gSniff.follow), "%s%s", byName ? "name:" : "", follow);
    ucxapp_print("Sniffing on %s into %s%s%s%s (sniff.stop to end)\n", port, file, follow[0] ? ", following " : "",
                 gSniff.follow, live ? ", live in Wireshark" : "");
    return UCXAPP_OK;
}

static void printTail(const char *log)
{
    size_t len = strlen(log);
    const char *p = len > 600 ? log + len - 600 : log;

    if (*p != '\0') {
        ucxapp_print("%s%s", p, log[len - 1] == '\n' ? "" : "\n");
    }
}

static int cmdStop(ucxapp_t *app, int argc, char *argv[])
{
    int code;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gSniff.cap == NULL) {
        ucxapp_print("No capture running\n");
        return UCXAPP_OK;
    }
    saveLast();
    code = ucxapp_capture_stop(gSniff.cap, STOP_GRACE_MS);
    gSniff.cap = NULL;
    (void)code;
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    uint32_t packets;
    uint64_t bytes;
    char dur[32];
    int code = 0;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gSniff.cap == NULL) {
        ucxapp_print("No capture running\n");
        return UCXAPP_OK;
    }
    if (!ucxapp_capture_running(gSniff.cap, &code)) {
        ucxapp_print("nrfutil ended (exit code %d):\n", code);
        printTail(ucxapp_capture_log(gSniff.cap));
        saveLast();
        (void)ucxapp_capture_stop(gSniff.cap, 0);
        gSniff.cap = NULL;
        return UCXAPP_ERR_FAILED;
    }
    (void)ucxapp_capture_count(ucxapp_capture_file(gSniff.cap), &packets, &bytes);
    ucxapp_print("Sniffing on %s for %s: %s, %u packets, %llu bytes%s%s\n", gSniff.port,
                 ucxapp_fmt_duration(ucxapp_capture_elapsed_ms(gSniff.cap), dur, sizeof(dur)),
                 ucxapp_capture_file(gSniff.cap), (unsigned)packets, (unsigned long long)bytes,
                 gSniff.follow[0] ? ", following " : "", gSniff.follow);
    return UCXAPP_OK;
}

static int cmdView(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    return ucxapp_capture_view(argv[1]) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static void saveLast(void)
{
    (void)snprintf(gSniff.last, sizeof(gSniff.last), "%s", ucxapp_capture_file(gSniff.cap));
}

static int cmdShow(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    return ucxapp_capture_show(argc, argv, gSniff.cap != NULL ? ucxapp_capture_file(gSniff.cap) : gSniff.last);
}

static const ucxapp_cmd_t kCmds[] = {
    { "sniff.detect", "", "Find an nRF Sniffer for Bluetooth LE on USB, nrfutil and Wireshark", 0, cmdDetect },
    { "sniff.start", "[file.pcap] [follow=module|<addr>|name:<name>] [port=<port>] [adv] [coded] [rssi=<-dBm>] [live]",
      "Capture BLE with nrfutil ble-sniffer into a pcap file; follow=module follows this module, live shows it in Wireshark",
      0, cmdStart },
    { "sniff.show", "[file.pcap] [count=50] [skip=0] [filter=<text>] [summary]",
      "Print a capture as text, one line per packet or per device with summary (default: the last capture)", 0,
      cmdShow },
    { "sniff.stop", "", "Stop the capture and print the packet count", 0, cmdStop },
    { "sniff.status", "", "Running capture: port, time, file, packets", 0, cmdStatus },
    { "sniff.view", "<file.pcap>", "Open a capture in Wireshark", 0, cmdView },
};

void ucxapp_register_sniff_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}

#else

void ucxapp_register_sniff_cmds(void)
{
}

#endif
