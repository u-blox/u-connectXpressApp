/* xplorer.* - drive the u-connectXplorer phone app over its remote control (JSON-RPC over WebSocket), so the phone
 * can be the other end of a test: an echo peer for perf.latency / perf.throughput, or the BLE central for perf.phone. */

#include "ucxapp_cmd.h"
#include "ucxapp_xplorer.h"

#if UCXAPP_XPLORER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALL_MS 10000u

bool ucxapp_xplorer_has(const char *feature)
{
    char path[64];
    bool on = false;
    char *caps = ucxapp_xplorer_call("app.getCapabilities", NULL, CALL_MS);

    snprintf(path, sizeof(path), "features.%s", feature);
    (void)ucxapp_json_bool(caps, path, &on);
    free(caps);
    return on;
}

void ucxapp_xplorer_scoped(void)
{
    if (ucxapp_xplorer_has("stopOnDisconnect")) {
        free(ucxapp_xplorer_call("app.configure", "{\"stopOnDisconnect\":true}", CALL_MS));
    }
}

bool ucxapp_xplorer_echo_start(bool udp, int32_t port)
{
    char params[96];

    if (!ucxapp_xplorer_has("socketEcho")) {
        ucxapp_print("ERROR: this u-connectXplorer cannot echo (needs a version with socketEcho); update the app\n");
        return false;
    }
    snprintf(params, sizeof(params), "{\"protocol\":\"%s\",\"port\":%d,\"echo\":true,\"force\":true}",
             udp ? "udp" : "tcp", (int)port);
    char *r = ucxapp_xplorer_call("socket.startServer", params, CALL_MS);
    free(r);
    return r != NULL;
}

bool ucxapp_xplorer_echo_report(int64_t *breaks)
{
    int64_t rx = 0;
    int64_t tx = 0;
    int64_t rxp = 0;
    int64_t txp = 0;
    bool active = false;
    char *s = ucxapp_xplorer_call("socket.getState", NULL, CALL_MS);

    *breaks = -1;
    if (s == NULL) {
        return false;
    }
    (void)ucxapp_json_bool(s, "active", &active);
    if (!active) {
        ucxapp_print("phone: no socket session\n");
        free(s);
        return true;
    }
    char proto[8] = "?";
    char state[24] = "?";
    int64_t port = 0;
    bool echo = false;
    (void)ucxapp_json_str(s, "protocol", proto, sizeof(proto));
    (void)ucxapp_json_str(s, "state", state, sizeof(state));
    (void)ucxapp_json_int(s, "port", &port);
    (void)ucxapp_json_bool(s, "echo", &echo);
    (void)ucxapp_json_int(s, "bytesReceived", &rx);
    (void)ucxapp_json_int(s, "bytesSent", &tx);
    (void)ucxapp_json_int(s, "packetsReceived", &rxp);
    (void)ucxapp_json_int(s, "packetsSent", &txp);
    bool haveBreaks = ucxapp_json_int(s, "patternBreaks", breaks);
    ucxapp_print("phone: %s %s port %d%s, received %u bytes (%u packets), sent %u bytes (%u packets)", proto, state,
                 (int)port, echo ? " echo" : "", (unsigned)rx, (unsigned)rxp, (unsigned)tx, (unsigned)txp);
    if (haveBreaks) {
        ucxapp_print(", pattern 0x00-0xFF: %u break%s", (unsigned)*breaks, *breaks == 1 ? "" : "s");
    }
    ucxapp_print("\n");
    free(s);
    return true;
}

void ucxapp_xplorer_echo_stop(void)
{
    free(ucxapp_xplorer_call("socket.stop", NULL, CALL_MS));
}

/* "host" or "host:port". */
static bool parseHost(const char *arg, char *host, size_t size, uint16_t *port)
{
    const char *colon = strchr(arg, ':');
    int32_t p = UCXAPP_XPLORER_PORT;

    if (colon != NULL && (!ucxapp_parse_int(colon + 1, &p) || p < 1 || p > 65535)) {
        return false;
    }
    size_t len = colon != NULL ? (size_t)(colon - arg) : strlen(arg);
    if (len == 0 || len >= size) {
        return false;
    }
    memcpy(host, arg, len);
    host[len] = '\0';
    *port = (uint16_t)p;
    return true;
}

static void showPhone(void)
{
    char version[48] = "?";
    char os[24] = "?";
    char osVersion[80] = "";
    char bt[24] = "?";
    char *v = ucxapp_xplorer_call("app.version", NULL, CALL_MS);
    char *p = ucxapp_xplorer_call("app.getPlatform", NULL, CALL_MS);

    /* fullVersion has the build number too */
    if (!ucxapp_json_str(v, "fullVersion", version, sizeof(version))) {
        (void)ucxapp_json_str(v, "version", version, sizeof(version));
    }
    (void)ucxapp_json_str(p, "platform", os, sizeof(os));
    (void)ucxapp_json_str(p, "osVersion", osVersion, sizeof(osVersion));
    (void)ucxapp_json_str(p, "bluetoothState", bt, sizeof(bt));
    free(v);
    free(p);
    ucxapp_print("u-connectXplorer %s on %s %s at %s\n", version, os, osVersion, ucxapp_xplorer_host());
    ucxapp_print("phone Bluetooth: %s\n", bt);
    if (!ucxapp_xplorer_has("socketEcho")) {
        ucxapp_print("note: update u-connectXplorer to use the phone as echo peer (perf.latency/perf.throughput ... xplorer)\n");
    }
    if (!ucxapp_xplorer_has("perfSps")) {
        ucxapp_print("note: update u-connectXplorer for SPS throughput against the phone (perf.phone)\n");
    }
}

static int cmdConnect(ucxapp_t *app, int argc, char *argv[])
{
    char host[64];
    uint16_t port = 0;

    (void)app;
    if (argc != 3 || !parseHost(argv[1], host, sizeof(host), &port) || argv[2][0] == '\0') {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_xplorer_open_to(host, port, argv[2])) {
        return UCXAPP_ERR_FAILED;
    }
    bool saved = ucxapp_xplorer_save(host, port, argv[2]);
    showPhone();
    ucxapp_xplorer_close();
    if (saved) {
        ucxapp_print("saved; xplorer.* and perf ... xplorer use this phone from now on\n");
    }
    return saved ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdInfo(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_xplorer_open()) {
        return UCXAPP_ERR_FAILED;
    }
    showPhone();
    ucxapp_xplorer_close();
    return UCXAPP_OK;
}

/* key=value pairs to a JSON object: numbers, true/false/null and {..}/[..] stay as they are, the rest is a string. */
static bool buildParams(int argc, char *argv[], char *out, size_t size)
{
    size_t n = 0;

    out[n++] = '{';
    for (int i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char key[80];
        char val[512];
        if (eq == NULL || eq == argv[i]) {
            return false;
        }
        *eq = '\0';
        const char *v = eq + 1;
        char *stop = NULL;
        (void)strtod(v, &stop);
        bool raw = (v[0] != '\0' && stop != NULL && *stop == '\0') || strcmp(v, "true") == 0 ||
                   strcmp(v, "false") == 0 || strcmp(v, "null") == 0 || v[0] == '{' || v[0] == '[';
        if (!ucxapp_json_quote(argv[i], key, sizeof(key)) ||
            (!raw && !ucxapp_json_quote(v, val, sizeof(val)))) {
            return false;
        }
        int w = snprintf(out + n, size - n, "%s%s:%s", i > 0 ? "," : "", key, raw ? v : val);
        if (w < 0 || (size_t)w >= size - n) {
            return false;
        }
        n += (size_t)w;
    }
    if (n + 2 > size) {
        return false;
    }
    out[n++] = '}';
    out[n] = '\0';
    return true;
}

static int cmdCall(ucxapp_t *app, int argc, char *argv[])
{
    char params[2048];

    (void)app;
    if (argc < 2 || strchr(argv[1], '.') == NULL || !buildParams(argc - 2, argv + 2, params, sizeof(params))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_xplorer_open()) {
        return UCXAPP_ERR_FAILED;
    }
    char *r = ucxapp_xplorer_call(argv[1], params, 30000);
    ucxapp_xplorer_close();
    if (r == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s\n", r);
    free(r);
    return UCXAPP_OK;
}

static int cmdEcho(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = 0;
    int64_t breaks;
    int rc = UCXAPP_OK;

    (void)app;
    bool start = argc == 3 && (strcmp(argv[1], "tcp") == 0 || strcmp(argv[1], "udp") == 0) &&
                 ucxapp_parse_int(argv[2], &port) && port >= 1 && port <= 65535;
    bool stop = argc == 2 && strcmp(argv[1], "stop") == 0;
    bool state = argc == 2 && strcmp(argv[1], "state") == 0;
    if (!start && !stop && !state) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_xplorer_open()) {
        return UCXAPP_ERR_FAILED;
    }
    if (start) {
        if (ucxapp_xplorer_echo_start(argv[1][0] == 'u', port)) {
            ucxapp_print("the phone echoes %s on %s:%d; xplorer.echo state shows what it got, xplorer.echo stop ends\n",
                         argv[1], ucxapp_xplorer_host(), (int)port);
        } else {
            rc = UCXAPP_ERR_FAILED;
        }
    } else if (state) {
        rc = ucxapp_xplorer_echo_report(&breaks) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    } else {
        (void)ucxapp_xplorer_echo_report(&breaks);
        ucxapp_xplorer_echo_stop();
    }
    ucxapp_xplorer_close();
    return rc;
}

static const ucxapp_cmd_t gXplorerCmds[] = {
    { "xplorer.connect", "<phone-ip[:port]> <token>",
      "Connect to the u-connectXplorer phone app (Settings > Remote control shows address and token, port 8765), "
      "show its app, OS and Bluetooth state and save it for the other xplorer and perf commands", 0,
      cmdConnect },
    { "xplorer.info", "", "Version, OS and Bluetooth state of the saved phone, and what its app version supports", 0,
      cmdInfo },
    { "xplorer.call", "<method> [key=value]...",
      "Call any remote-control method (e.g. ble.getState, app.getNetworkInfo) and print the JSON result; numbers, "
      "true/false and {..}/[..] values go as they are, others as strings", 0, cmdCall },
    { "xplorer.echo", "<tcp|udp> <port> | state | stop",
      "Make the phone an echo server for perf.latency and perf.throughput (perf ... xplorer does this itself); state shows "
      "the bytes and 0x00-0xFF pattern breaks it received", 0, cmdEcho },
};

void ucxapp_register_xplorer_cmds(void)
{
    ucxapp_cmd_register(gXplorerCmds, sizeof(gXplorerCmds) / sizeof(gXplorerCmds[0]));
}

#else /* !UCXAPP_XPLORER */

void ucxapp_register_xplorer_cmds(void)
{
}

#endif
