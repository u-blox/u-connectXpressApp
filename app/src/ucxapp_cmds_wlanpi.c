/* wlanpi.*: Wi-Fi capture on a WLAN Pi (or any Linux box with a monitor-mode adapter) over the system OpenSSH
 * client with key login: the Pi puts its adapter in monitor mode on a channel and runs "tcpdump -w -", ssh brings
 * the packets back into a local pcap file, optionally live in Wireshark. wlanpi.setup checks the key login and
 * prints the one-time recipe. ssh comes from UCXAPP_SSH (a command, used as given) or PATH. PC builds only. */

#include "ucxapp_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_cx_wifi.h"
#include "ucxapp_capture.h"
#include "ucxapp_platform.h"
#include "ucxapp_proc.h"
#include "ucxapp_store.h"

#ifndef __EMSCRIPTEN__

#define CFG_FILE        "ucxapp.wlanpi"
#define START_WAIT_MS   15000
#define STOP_WAIT_MS    8000
#define SSH_OPTS        " -T -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new "

static struct {
    char host[128];
    char user[64];
    char iface[32];
} gCfg;

static struct {
    ucxapp_capture_t *cap;
    long pid;
    char what[96];
} gCap;

static bool validName(const char *s, const char *extra)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!isalnum((unsigned char)*s) && strchr(extra, *s) == NULL) {
            return false;
        }
    }
    return true;
}

static void loadCfg(void)
{
    size_t len;
    char *data = (char *)ucxapp_store_load(CFG_FILE, &len);

    (void)snprintf(gCfg.user, sizeof(gCfg.user), "wlanpi");
    (void)snprintf(gCfg.iface, sizeof(gCfg.iface), "wlan0");
    gCfg.host[0] = '\0';
    if (data != NULL) {
        char host[128] = "", user[64] = "", iface[32] = "";

        if (sscanf(data, "%127s %63s %31s", host, user, iface) == 3 && validName(host, ".-:_") &&
            validName(user, ".-_") && validName(iface, "-_.")) {
            (void)snprintf(gCfg.host, sizeof(gCfg.host), "%s", host);
            (void)snprintf(gCfg.user, sizeof(gCfg.user), "%s", user);
            (void)snprintf(gCfg.iface, sizeof(gCfg.iface), "%s", iface);
        }
        free(data);
    }
}

static bool needHost(void)
{
    loadCfg();
    if (gCfg.host[0] == '\0') {
        ucxapp_print("ERROR: no WLAN Pi set; first: wlanpi.host <host> [user=wlanpi] [iface=wlan0]\n");
        return false;
    }
    return true;
}

/* "ssh <options> user@host <script>"; false if ssh is missing or the script cannot be passed. */
static bool sshCmd(const char *script, char *out, size_t size)
{
    const char *env = getenv("UCXAPP_SSH");
    char path[512], q[1200];
    size_t len;

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, size, "%s", env);
    } else if (!ucxapp_proc_which("ssh", path, sizeof(path)) || !ucxapp_proc_quote(path, out, size)) {
        ucxapp_print("ERROR: ssh not found (Windows: Settings > Optional features > OpenSSH Client; Linux: openssh-client)\n");
        return false;
    }
    if (!ucxapp_proc_quote(script, q, sizeof(q))) {
        ucxapp_print("ERROR: the remote command cannot be passed to ssh (no \", %% or ! on Windows)\n");
        return false;
    }
    len = strlen(out);
    (void)snprintf(out + len, size - len, SSH_OPTS "%s@%s %s", gCfg.user, gCfg.host, q);
    return true;
}

/* Runs a short remote command; its output goes to out. Returns the exit code, -1 on timeout or start failure. */
static int sshRun(const char *script, char *out, size_t size, uint32_t ms)
{
    char cmd[1800];
    ucxapp_proc_t *p;
    size_t len = 0;
    uint32_t start = ucxapp_platform_tick_ms();
    int code = -1;

    out[0] = '\0';
    if (!sshCmd(script, cmd, sizeof(cmd))) {
        return -1;
    }
    p = ucxapp_proc_start(cmd, NULL, 0);
    if (p == NULL) {
        return -1;
    }
    for (;;) {
        int32_t n = ucxapp_proc_read(p, out + len, size - len - 1);

        if (n > 0) {
            len += (size_t)n;
            out[len] = '\0';
            if (len + 1 >= size) {
                len = 0;   /* keep reading so the child does not block; the tail is enough */
            }
            continue;
        }
        if (n < 0 && !ucxapp_proc_running(p, &code)) {
            return ucxapp_proc_wait(p, 0);
        }
        if (ucxapp_platform_tick_ms() - start > ms) {
            (void)ucxapp_proc_wait(p, 0);
            return -1;
        }
        ucxapp_platform_sleep_ms(10);
    }
}

static int cmdHost(ucxapp_t *app, int argc, char *argv[])
{
    char data[256];
    int i;
    (void)app;

    loadCfg();
    if (argc == 1) {
        if (gCfg.host[0] == '\0') {
            ucxapp_print("No WLAN Pi set (wlanpi.host <host> [user=wlanpi] [iface=wlan0])\n");
        } else {
            ucxapp_print("WLAN Pi: %s@%s, capture interface %s\n", gCfg.user, gCfg.host, gCfg.iface);
        }
        return UCXAPP_OK;
    }
    if (strchr(argv[1], '=') != NULL || !validName(argv[1], ".-:_")) {
        return UCXAPP_ERR_USAGE;
    }
    (void)snprintf(gCfg.host, sizeof(gCfg.host), "%s", argv[1]);
    for (i = 2; i < argc; i++) {
        if (strncmp(argv[i], "user=", 5) == 0 && validName(argv[i] + 5, ".-_")) {
            (void)snprintf(gCfg.user, sizeof(gCfg.user), "%s", argv[i] + 5);
        } else if (strncmp(argv[i], "iface=", 6) == 0 && validName(argv[i] + 6, "-_.")) {
            (void)snprintf(gCfg.iface, sizeof(gCfg.iface), "%s", argv[i] + 6);
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    (void)snprintf(data, sizeof(data), "%s %s %s\n", gCfg.host, gCfg.user, gCfg.iface);
    if (!ucxapp_store_save(CFG_FILE, data, strlen(data))) {
        ucxapp_print("ERROR: cannot save %s\n", CFG_FILE);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("WLAN Pi: %s@%s, capture interface %s (wlanpi.setup checks the login)\n", gCfg.user, gCfg.host,
                 gCfg.iface);
    return UCXAPP_OK;
}

static bool findKey(char *out, size_t size)
{
    static const char *const kKeys[] = { "id_ed25519.pub", "id_ecdsa.pub", "id_rsa.pub" };
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    const char sep = '\\';
#else
    const char *home = getenv("HOME");
    const char sep = '/';
#endif
    size_t i;

    for (i = 0; home != NULL && i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
        FILE *f;

        (void)snprintf(out, size, "%s%c.ssh%c%s", home, sep, sep, kKeys[i]);
        f = fopen(out, "rb");
        if (f != NULL) {
            fclose(f);
            return true;
        }
    }
    return false;
}

static int cmdSetup(ucxapp_t *app, int argc, char *argv[])
{
    char key[512], out[2048];
    bool hasKey, login = false, sudo = false;
    int code;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needHost()) {
        return UCXAPP_ERR_FAILED;
    }
    hasKey = findKey(key, sizeof(key));
    ucxapp_print("[%s] SSH key %s\n", hasKey ? "ok" : "--", hasKey ? key : "missing");
    if (hasKey) {
        code = sshRun("echo UCXLOGIN; sudo -n true && echo UCXSUDO", out, sizeof(out), 15000);
        login = strstr(out, "UCXLOGIN") != NULL;
        sudo = strstr(out, "UCXSUDO") != NULL;
        ucxapp_print("[%s] key login to %s@%s\n", login ? "ok" : "--", gCfg.user, gCfg.host);
        if (!login && out[0] != '\0') {
            ucxapp_print("     %s%s", out, out[strlen(out) - 1] == '\n' ? "" : "\n");
        }
        if (login) {
            ucxapp_print("[%s] sudo without a password\n", sudo ? "ok" : "--");
        }
        (void)code;
    }
    if (hasKey && login && sudo) {
        ucxapp_print("Ready: wlanpi.start [file.pcap] [channel=<n>|module] [live]\n");
        return UCXAPP_OK;
    }
    ucxapp_print("\nOne-time setup (type the WLAN Pi password when ssh asks for it):\n");
    if (!hasKey) {
        ucxapp_print("  1. Make a key:            ssh-keygen -t ed25519\n");
    }
    if (!login) {
#ifdef _WIN32
        ucxapp_print("  2. Copy it to the Pi:     Get-Content $env:USERPROFILE\\.ssh\\id_ed25519.pub | ssh %s@%s "
                     "\"mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys\"\n", gCfg.user, gCfg.host);
#else
        ucxapp_print("  2. Copy it to the Pi:     ssh-copy-id %s@%s\n", gCfg.user, gCfg.host);
#endif
    }
    ucxapp_print("  3. Let tcpdump, iw, ip and kill run without a password:\n"
                 "     ssh -t %s@%s \"echo '%s ALL=(root) NOPASSWD: /usr/bin/tcpdump, /usr/sbin/tcpdump, /usr/sbin/iw, "
                 "/sbin/iw, /usr/sbin/ip, /sbin/ip, /bin/kill, /usr/bin/kill' | sudo tee /etc/sudoers.d/ucxapp && "
                 "sudo chmod 440 /etc/sudoers.d/ucxapp\"\n", gCfg.user, gCfg.host, gCfg.user);
    ucxapp_print("Then run wlanpi.setup again.\n");
    return UCXAPP_ERR_FAILED;
}

/* The module's current Wi-Fi channel, 0 if it has none. */
static int32_t moduleChannel(ucxapp_t *app)
{
    uCxWifiStationStatus_t st;
    int32_t ch = 0;

    if (app->ucx == NULL) {
        return 0;
    }
    if (uCxWifiStationStatusBegin(app->ucx, U_WIFI_STATUS_ID_CHANNEL, &st)) {
        ch = st.rsp.StatusIdInt.int_val;
    }
    (void)uCxEnd(app->ucx);
    return ch;
}

/* Remote filter in single quotes ('\'' for a quote). */
static bool quoteFilter(const char *f, char *out, size_t size)
{
    size_t n = 0;

    if (n + 1 >= size) {
        return false;
    }
    out[n++] = '\'';
    for (; *f != '\0'; f++) {
        const char *part = *f == '\'' ? "'\\''" : NULL;
        size_t plen = part != NULL ? 4 : 1;

        if ((unsigned char)*f < 0x20 || n + plen + 2 >= size) {
            return false;
        }
        if (part != NULL) {
            memcpy(out + n, part, plen);
        } else {
            out[n] = *f;
        }
        n += plen;
    }
    out[n++] = '\'';
    out[n] = '\0';
    return true;
}

static int cmdStart(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kWidths[][2] = { { "20", "HT20" }, { "40+", "HT40+" }, { "40-", "HT40-" },
                                              { "80", "80MHz" } };
    char file[512] = "", filt[600] = "", script[1100], cmd[1800], chSet[96] = "", dur[8];
    const char *width = NULL, *log;
    int32_t ch = 0;
    bool live = false, chModule = false;
    uint32_t start;
    char *pid;
    size_t w;
    int i, code = 0;

    if (gCap.cap != NULL) {
        ucxapp_print("ERROR: a capture is running (wlanpi.stop)\n");
        return UCXAPP_ERR_FAILED;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "channel=module") == 0) {
            chModule = true;
        } else if (strncmp(argv[i], "channel=", 8) == 0) {
            if (!ucxapp_parse_int(argv[i] + 8, &ch) || ch < 1 || ch > 233) {
                return UCXAPP_ERR_USAGE;
            }
        } else if (strncmp(argv[i], "width=", 6) == 0) {
            for (w = 0; w < sizeof(kWidths) / sizeof(kWidths[0]); w++) {
                if (strcmp(argv[i] + 6, kWidths[w][0]) == 0) {
                    width = kWidths[w][1];
                }
            }
            if (width == NULL) {
                return UCXAPP_ERR_USAGE;
            }
        } else if (strncmp(argv[i], "filter=", 7) == 0) {
            if (!quoteFilter(argv[i] + 7, filt, sizeof(filt))) {
                return UCXAPP_ERR_USAGE;
            }
        } else if (strcmp(argv[i], "live") == 0) {
            live = true;
        } else if (strchr(argv[i], '=') == NULL && file[0] == '\0') {
            (void)snprintf(file, sizeof(file), "%s", argv[i]);
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (!needHost()) {
        return UCXAPP_ERR_FAILED;
    }
    if (chModule) {
        ch = moduleChannel(app);
        if (ch <= 0) {
            ucxapp_print("ERROR: the module is on no Wi-Fi channel (connect it first or give channel=<n>)\n");
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("Module is on channel %d\n", (int)ch);
    }
    if (ch > 0) {
        (void)snprintf(chSet, sizeof(chSet), "sudo -n iw dev %s set channel %d%s%s || exit 4; ", gCfg.iface, (int)ch,
                       width != NULL ? " " : "", width != NULL ? width : "");
    }
    if (file[0] == '\0') {
        ucxapp_capture_default_name("wifi", file, sizeof(file));
    }
    (void)snprintf(script, sizeof(script),
                   "if [ $(cat /sys/class/net/%s/type) -ne 803 ]; then sudo -n ip link set %s down && "
                   "sudo -n iw dev %s set monitor none && sudo -n ip link set %s up || exit 3; fi; %s"
                   "echo UCXPID=$$ >&2; exec sudo -n tcpdump -i %s -U -w - %s",
                   gCfg.iface, gCfg.iface, gCfg.iface, gCfg.iface, chSet, gCfg.iface, filt);
    if (!sshCmd(script, cmd, sizeof(cmd))) {
        return UCXAPP_ERR_FAILED;
    }
    gCap.cap = ucxapp_capture_start(cmd, file, true, live);
    if (gCap.cap == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    /* Wait for tcpdump to listen (or the login, sudo or iw to fail) so errors show here, not at wlanpi.stop. */
    start = ucxapp_platform_tick_ms();
    gCap.pid = 0;
    while (ucxapp_capture_running(gCap.cap, &code) && ucxapp_platform_tick_ms() - start < START_WAIT_MS) {
        log = ucxapp_capture_log(gCap.cap);
        if (strstr(log, "listening on") != NULL) {
            break;
        }
        ucxapp_platform_sleep_ms(20);
    }
    log = ucxapp_capture_log(gCap.cap);
    pid = strstr(log, "UCXPID=");
    if (pid != NULL) {
        gCap.pid = strtol(pid + 7, NULL, 10);
    }
    if (!ucxapp_capture_running(gCap.cap, &code) || gCap.pid <= 0) {
        ucxapp_print("ERROR: the capture on %s did not start%s\n", gCfg.host,
                     code == 3 ? " (monitor mode failed)" : code == 4 ? " (channel not set)" : "");
        if (log[0] != '\0') {
            ucxapp_print("%s%s", log, log[strlen(log) - 1] == '\n' ? "" : "\n");
        }
        if (strstr(log, "password") != NULL || strstr(log, "Permission denied") != NULL ||
            strstr(log, "terminal is required") != NULL) {
            ucxapp_print("Run wlanpi.setup for the key login and sudo rules.\n");
        }
        (void)ucxapp_capture_stop(gCap.cap, 0);
        gCap.cap = NULL;
        return UCXAPP_ERR_FAILED;
    }
    (void)snprintf(dur, sizeof(dur), "%d", (int)ch);
    (void)snprintf(gCap.what, sizeof(gCap.what), "%s %s%s%s%s%s", gCfg.host, gCfg.iface, ch > 0 ? " channel " : "",
                   ch > 0 ? dur : "", width != NULL ? " " : "", width != NULL ? width : "");
    ucxapp_print("Capturing on %s into %s%s (wlanpi.stop to end)\n", gCap.what, file,
                 live ? ", live in Wireshark" : "");
    return UCXAPP_OK;
}

static int cmdStop(ucxapp_t *app, int argc, char *argv[])
{
    char script[64], out[512];
    uint32_t start;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gCap.cap == NULL) {
        ucxapp_print("No capture running\n");
        return UCXAPP_OK;
    }
    /* tcpdump ends cleanly on SIGINT and closes the stream; ssh then exits on its own. */
    (void)snprintf(script, sizeof(script), "sudo -n kill -INT %ld", gCap.pid);
    (void)sshRun(script, out, sizeof(out), STOP_WAIT_MS);
    start = ucxapp_platform_tick_ms();
    while (ucxapp_capture_running(gCap.cap, NULL) && ucxapp_platform_tick_ms() - start < STOP_WAIT_MS) {
        ucxapp_platform_sleep_ms(20);
    }
    (void)ucxapp_capture_stop(gCap.cap, 2000);
    gCap.cap = NULL;
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    char out[2048], script[96], dur[32];
    uint32_t packets;
    uint64_t bytes;
    int code = 0;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gCap.cap != NULL) {
        if (!ucxapp_capture_running(gCap.cap, &code)) {
            const char *log = ucxapp_capture_log(gCap.cap);

            ucxapp_print("The capture on %s ended (exit code %d)\n%s", gCap.what, code, log);
            (void)ucxapp_capture_stop(gCap.cap, 0);
            gCap.cap = NULL;
            return UCXAPP_ERR_FAILED;
        }
        (void)ucxapp_capture_count(ucxapp_capture_file(gCap.cap), &packets, &bytes);
        ucxapp_print("Capturing on %s for %s: %s, %u packets, %llu bytes\n", gCap.what,
                     ucxapp_fmt_duration(ucxapp_capture_elapsed_ms(gCap.cap), dur, sizeof(dur)),
                     ucxapp_capture_file(gCap.cap), (unsigned)packets, (unsigned long long)bytes);
        return UCXAPP_OK;
    }
    if (!needHost()) {
        return UCXAPP_ERR_FAILED;
    }
    (void)snprintf(script, sizeof(script), "iw dev %s info", gCfg.iface);
    code = sshRun(script, out, sizeof(out), 15000);
    ucxapp_print("%s@%s %s:\n%s%s", gCfg.user, gCfg.host, gCfg.iface, out,
                 out[0] != '\0' && out[strlen(out) - 1] == '\n' ? "" : "\n");
    return code == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t kCmds[] = {
    { "wlanpi.host", "[<host> [user=wlanpi] [iface=wlan0]]", "Show or set the WLAN Pi to capture with (saved)", 0, cmdHost },
    { "wlanpi.setup", "", "Check the ssh key login and sudo rights on the WLAN Pi; prints the one-time recipe", 0,
      cmdSetup },
    { "wlanpi.start", "[file.pcap] [channel=<n>|module] [width=20|40+|40-|80] [filter=<bpf>] [live]",
      "Capture Wi-Fi in monitor mode on the WLAN Pi into a pcap file; channel=module takes the module's channel", 0,
      cmdStart },
    { "wlanpi.stop", "", "Stop the capture and print the packet count", 0, cmdStop },
    { "wlanpi.status", "", "Running capture, else the WLAN Pi adapter (iw dev info)", 0, cmdStatus },
};

void ucxapp_register_wlanpi_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}

#else

void ucxapp_register_wlanpi_cmds(void)
{
}

#endif
