/* Portable command-line entry point for Windows and POSIX hosts. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"
#include "ucxapp_tui.h"
#ifdef APP_VERSION_MAJOR
#include "version.h"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_system.h"
#include "u_cx_error_codes.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define MAX_EXEC 1024
#define MAX_POS 16
#define UART_BAUD 115200

static uCxAtClient_t gAtClient;
static uCxAtClientConfig_t gAtConfig;
static uCxHandle_t gUcxHandle;
static char gRxBuffer[8192];
static char gUrcBuffer[4096];
static bool gConnected;
static char gPortName[32];

const char *ucxapp_device_port(void)
{
    return gConnected ? gPortName : "";
}

static bool connectModule(const char *port)
{
    uPortInit();
    memset(&gAtConfig, 0, sizeof(gAtConfig));
    gAtConfig.pRxBuffer = gRxBuffer;
    gAtConfig.rxBufferLen = sizeof(gRxBuffer);
    gAtConfig.pUrcBuffer = gUrcBuffer;
    gAtConfig.urcBufferLen = sizeof(gUrcBuffer);
    gAtConfig.pUartDevName = port;

    uCxAtClientInit(&gAtConfig, &gAtClient);
    int32_t result = uCxAtClientOpen(&gAtClient, UART_BAUD, false);
    if (result != 0) {
        ucxapp_print("ERROR: cannot open %s (%d)\n", port, (int)result);
        uCxAtClientDeinit(&gAtClient);
        uPortDeinit();
        return false;
    }
    uCxInit(&gAtClient, &gUcxHandle);
    ucxapp_events_init();
    ucxapp_urc_register(&gUcxHandle);

    bool echoOk = uCxSystemSetEchoOff(&gUcxHandle) == 0;
    if (!echoOk) {
        ucxapp_print("warning: could not disable echo\n");
    }
    if (uCxSystemSetExtendedError(&gUcxHandle, U_SYS_EXTENDED_ERRORS_ON) != 0) {
        if (!echoOk) {
            /* Without this every later command waits for its own timeout and the run looks hung. */
            ucxapp_print("ERROR: module on %s does not answer AT commands (power-cycle it)\n", port);
            ucxapp_events_deinit();
            uCxAtClientClose(&gAtClient);
            uCxAtClientDeinit(&gAtClient);
            uPortDeinit();
            return false;
        }
        ucxapp_print("warning: could not enable extended error codes\n");
    }
    ucxapp_adopt_connections(&gUcxHandle);
    ucxapp_udp_reset_limit();
    gConnected = true;
    snprintf(gPortName, sizeof(gPortName), "%s", port);
    ucxapp_caps_reset();
    return true;
}

static void disconnectModule(void)
{
    if (!gConnected) {
        return;
    }
    ucxapp_events_deinit();
    uCxAtClientClose(&gAtClient);
    uCxAtClientDeinit(&gAtClient);
    uPortDeinit();
    gConnected = false;
}

static int cmdDeviceConnect(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gConnected) {
        ucxapp_print("already connected\n");
        return UCXAPP_OK;
    }
    if (!connectModule(argv[1])) {
        return UCXAPP_ERR_FAILED;
    }
    app->ucx = &gUcxHandle;
    ucxapp_print("connected to %s\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdDeviceDisconnect(ucxapp_t *app, int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    app->ucx = NULL;
    disconnectModule();
    return UCXAPP_OK;
}

/* dev.use <port>: make another module the active one (the current one is released). */
static int cmdDeviceUse(ucxapp_t *app, int argc, char *argv[])
{
    char previous[sizeof(gPortName)];

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gConnected && strcmp(gPortName, argv[1]) == 0) {
        ucxapp_print("%s is already the active module\n", argv[1]);
        return UCXAPP_OK;
    }
    snprintf(previous, sizeof(previous), "%s", gConnected ? gPortName : "");
    app->ucx = NULL;
    disconnectModule();
    if (!connectModule(argv[1])) {
        if (previous[0] != '\0' && connectModule(previous)) {
            app->ucx = &gUcxHandle;
            ucxapp_print("still using %s\n", previous);
        }
        return UCXAPP_ERR_FAILED;
    }
    app->ucx = &gUcxHandle;
    ucxapp_print("active module: %s\n", argv[1]);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gConnectCmds[] = {
    { "dev.connect",       "<port>", "Connect to the module on a serial port", 0, cmdDeviceConnect },
    { "dev.disconnect",    "",       "Disconnect from the module",             0, cmdDeviceDisconnect },
    { "dev.use",           "<port>", "Switch the active module to the one on another serial port (see dash for the ones found)", 0, cmdDeviceUse },
};

static void printUsage(const char *prog)
{
    ucxapp_print("usage: %s [port] [--exec \"<command>\"]... [--json] [--shell] [--tui] [--list-ports] [--list-modules] [--version]\n", prog);
    ucxapp_print("  a command can also be given directly: %s [port] dev.info (no spaces inside the command)\n", prog);
    ucxapp_print("  without a port, --exec/--shell use the first NORA EVK found\n");
    ucxapp_print("  with no --exec/--shell the text UI starts (command shell if not on a terminal); 'help' lists the commands\n");
    ucxapp_print("  --tui forces the three-pane text UI (commands, log, events), --shell the plain command shell\n");
    ucxapp_print("  --offline skips the module auto-detection (commands that need no device)\n");
    ucxapp_print("  --json prints one JSON object per command: command, success, code, error, output lines, data\n");
    ucxapp_print("  ${NAME} in --exec and shell commands is replaced by the environment variable (keeps secrets off the command line)\n");
}

/* --json: command output is captured here and printed as one JSON line per command. */
static char gCapture[16384];
static size_t gCaptureLen;

static void captureWrite(const char *text, size_t len)
{
    if (len > sizeof(gCapture) - 1 - gCaptureLen) {
        len = sizeof(gCapture) - 1 - gCaptureLen;
    }
    memcpy(gCapture + gCaptureLen, text, len);
    gCaptureLen += len;
    gCapture[gCaptureLen] = '\0';
}

static int captureReadline(char *buf, size_t size)
{
    return ucxapp_io_stdio.readline(buf, size);
}

static const ucxapp_io_t gCaptureIo = { captureWrite, captureReadline, NULL, NULL };

/* "Key name:   value" -> "key_name"; false for lines that are not a key/value pair. */
static bool jsonKey(const char *line, char *key, size_t size, const char **value)
{
    const char *colon = strstr(line, ": ");
    size_t n = 0;

    if (colon == NULL || colon == line || colon - line > 32 || !isalpha((unsigned char)line[0]) ||
        strncmp(line, "ERROR: ", 7) == 0 || strncmp(line, "usage: ", 7) == 0) {
        return false;
    }
    for (const char *p = line; p < colon && n + 1 < size; p++) {
        if (isalnum((unsigned char)*p)) {
            key[n++] = (char)tolower((unsigned char)*p);
        } else if (*p == ' ' || *p == '-' || *p == '_') {
            if (n > 0 && key[n - 1] != '_') {
                key[n++] = '_';
            }
        } else {
            return false;
        }
    }
    while (n > 0 && key[n - 1] == '_') {
        n--;
    }
    key[n] = '\0';
    *value = colon + 1;
    while (**value == ' ') {
        (*value)++;
    }
    return n > 0;
}

static void printJsonResult(const char *command, int rc)
{
    const char *error = NULL;
    char *lines[256];
    int count = 0;
    bool anyData = false;

    for (char *p = gCapture; *p != '\0' && count < 256;) {
        char *nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') {
            p[len - 1] = '\0';
        }
        if (error == NULL && strncmp(p, "ERROR: ", 7) == 0) {
            error = p + 7;
        }
        lines[count++] = p;
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    ucxapp_io_set(&ucxapp_io_stdio);
    ucxapp_print("{\"command\":");
    ucxapp_print_json_string(command);
    ucxapp_print(",\"success\":%s,\"code\":\"%s\"", rc == UCXAPP_OK ? "true" : "false", ucxapp_err_code(rc));
    if (rc != UCXAPP_OK) {
        int32_t mod = ucxapp_last_module_error();
        const char *modName = mod != 0 ? uCxGetErrorName(mod) : NULL;

        ucxapp_print(",\"error\":");
        ucxapp_print_json_string(error != NULL ? error : ucxapp_err_code(rc));
        if (modName != NULL) {
            ucxapp_print(",\"moduleError\":{\"code\":%d,\"name\":\"%s\"}", (int)mod, modName);
        }
    }
    ucxapp_print(",\"output\":[");
    for (int i = 0; i < count; i++) {
        ucxapp_print("%s", i > 0 ? "," : "");
        ucxapp_print_json_string(lines[i]);
    }
    ucxapp_print("]");
    for (int i = 0; i < count; i++) {
        char key[40];
        const char *value;
        if (jsonKey(lines[i], key, sizeof(key), &value)) {
            ucxapp_print("%s\"%s\":", anyData ? "," : ",\"data\":{", key);
            ucxapp_print_json_string(value);
            anyData = true;
        }
    }
    ucxapp_print("%s}\n", anyData ? "}" : "");
    gCaptureLen = 0;
    gCapture[0] = '\0';
    ucxapp_io_set(&gCaptureIo);
}

/* Runs one local command line: ${NAME} expansion, then the command; in --json mode one JSON result line. */
static int runLine(ucxapp_t *app, const char *command, bool json)
{
    char line[512];
    int rc = UCXAPP_ERR_USAGE;

    if (json && strncmp(command, "help --json", 11) == 0) {
        /* Already JSON: printed as is. */
        snprintf(line, sizeof(line), "%s", command);
        ucxapp_io_set(&ucxapp_io_stdio);
        rc = ucxapp_cmd_exec(app, line);
        ucxapp_io_set(&gCaptureIo);
        return rc;
    }
    if (ucxapp_expand_env(command, line, sizeof(line))) {
        rc = ucxapp_cmd_exec(app, line);
    }
    if (json) {
        printJsonResult(command, rc);
    }
    return rc;
}

static bool looksLikePort(const char *name)
{
    if (toupper((unsigned char)name[0]) == 'C' && toupper((unsigned char)name[1]) == 'O' &&
        toupper((unsigned char)name[2]) == 'M' && name[3] != '\0') {
        const char *d = name + 3;
        while (*d >= '0' && *d <= '9') {
            d++;
        }
        return *d == '\0';
    }
    return strncmp(name, "/dev/", 5) == 0;
}

static bool isPort(const char *name)
{
    if (looksLikePort(name)) {
        return true;
    }
    ucxapp_serial_port_t ports[32];
    int count = ucxapp_platform_list_serial_ports(ports, 32);
    for (int i = 0; i < count; i++) {
        const char *a = ports[i].name;
        const char *b = name;
        while (*a != '\0' && *b != '\0' && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return true;
        }
    }
    return false;
}

static void listPorts(void)
{
    ucxapp_serial_port_t ports[32];
    int count = ucxapp_platform_list_serial_ports(ports, (int)(sizeof(ports) / sizeof(ports[0])));
    for (int i = 0; i < count; i++) {
        ucxapp_print("%-16s %-10s %s\n", ports[i].name, ports[i].available ? "available" : "no access",
                     ports[i].desc);
    }
    if (count == 0) {
        ucxapp_print("no serial ports found\n");
    }
}

int main(int argc, char *argv[])
{
    const char *port = NULL;
    static const char *execCmds[MAX_EXEC];
    int execCount = 0;
    bool shellMode = false;
    bool tuiMode = false;
    bool offline = false;
    bool json = false;
    const char *pos[MAX_POS];
    int posCount = 0;

    ucxapp_platform_init();
    ucxapp_io_set(&ucxapp_io_stdio);
    ucxapp_store_set_dir("storage/");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
            if (execCount >= MAX_EXEC) {
                ucxapp_print("ERROR: more than %d --exec commands; use --shell with the commands on stdin\n", MAX_EXEC);
                return 2;
            }
            execCmds[execCount++] = argv[++i];
        } else if (strcmp(argv[i], "--shell") == 0) {
            shellMode = true;
        } else if (strcmp(argv[i], "--tui") == 0) {
            tuiMode = true;
        } else if (strcmp(argv[i], "--offline") == 0) {
            offline = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
#ifdef APP_VERSION_STRING
            ucxapp_print("%s\n", ucxapp_version_text());
#else
            ucxapp_print("unknown\n");
#endif
            return 0;
        } else if (strcmp(argv[i], "--ini") == 0 && i + 1 < argc) {
            i++;   /* accepted for compatibility; the app keeps no settings file */
        } else if (strcmp(argv[i], "--list-modules") == 0) {
            char names[8][32];
            int n = ucxapp_find_modules(names, 8);
            for (int k = 0; k < n; k++) {
                ucxapp_print("%s\n", names[k]);
            }
            return 0;
        } else if (strcmp(argv[i], "--list-ports") == 0) {
            listPorts();
            return 0;
        } else if (argv[i][0] != '-' && posCount < MAX_POS) {
            pos[posCount++] = argv[i];
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    static char posCmd[256];
    posCmd[0] = '\0';
    for (int k = 0; k < posCount; k++) {
        if (port == NULL && isPort(pos[k])) {
            port = pos[k];
        } else {
            size_t used = strlen(posCmd);
            const char *q = strchr(pos[k], ' ') != NULL ? "\"" : "";
            snprintf(posCmd + used, sizeof(posCmd) - used, "%s%s%s%s", used > 0 ? " " : "", q, pos[k], q);
        }
    }
    if (posCmd[0] != '\0' && execCount < MAX_EXEC) {
        execCmds[execCount++] = posCmd;
    }

    ucxapp_cmd_register(gConnectCmds, sizeof(gConnectCmds) / sizeof(gConnectCmds[0]));
    ucxapp_register_device_cmds();
    ucxapp_register_wifi_cmds();
    ucxapp_register_sock_cmds();
    ucxapp_register_net_cmds();
    ucxapp_register_sys_cmds();
    ucxapp_register_app_cmds();
    ucxapp_register_mqtt_cmds();
    ucxapp_register_bt_cmds();
    ucxapp_register_cert_cmds();
    ucxapp_register_tls_cmds();
    ucxapp_register_dtls_cmds();
    ucxapp_register_coap_cmds();
    ucxapp_register_tftp_cmds();
    ucxapp_register_ft_cmds();
    ucxapp_register_ancs_cmds();
    ucxapp_register_notify_cmds();
    ucxapp_register_session_cmds();
    ucxapp_register_time_cmds();
    ucxapp_register_can_cmds();
    ucxapp_register_fw_cmds();
    ucxapp_register_ymodem_cmds();
    ucxapp_register_gatt_cmds();
    ucxapp_register_qr_cmds();
    ucxapp_register_utils_cmds();
    ucxapp_register_gnss_cmds();
    ucxapp_register_cell_cmds();
    ucxapp_register_imu_cmds();
    ucxapp_register_bridge_cmds();
    ucxapp_register_dash_cmds();
    ucxapp_register_hw_cmds();
    ucxapp_register_hid_cmds();
    ucxapp_register_web_cmds();
    ucxapp_register_examples_cmds();

    ucxapp_t app = { NULL };
    int rc = 0;
    char detected[8][32];
    bool needDevice = execCount == 0 || shellMode || tuiMode;
    json = json && execCount > 0 && !shellMode && !tuiMode;
    if (json) {
        ucxapp_io_set(&gCaptureIo);
    }
    for (int i = 0; i < execCount && !needDevice; i++) {
        char name[64];
        size_t len = strcspn(execCmds[i], " \t");
        snprintf(name, sizeof(name), "%.*s", (int)(len < sizeof(name) ? len : sizeof(name) - 1), execCmds[i]);
        const ucxapp_cmd_t *cmd = ucxapp_cmd_find(name);
        needDevice = cmd == NULL || (cmd->flags & UCXAPP_CMD_NEEDS_DEVICE) != 0;
    }
    if (port == NULL && !offline && needDevice && ucxapp_find_modules(detected, 8) > 0) {
        port = detected[0];
        ucxapp_print("auto-detected module on %s\n", port);
    }
    if (port != NULL) {
        char line[128];
        snprintf(line, sizeof(line), "dev.connect %s", port);
        if (runLine(&app, line, json) != UCXAPP_OK && !json) {
            ucxapp_print("continuing without a device\n");
        }
    }
    for (int i = 0; i < execCount && rc == 0; i++) {
        if (runLine(&app, execCmds[i], json) != UCXAPP_OK) {
            rc = 1;
        }
    }
    if (rc == 0 && (tuiMode || (execCount == 0 && !shellMode))) {
        rc = ucxapp_tui_run(&app);
        if (rc != 0 && !tuiMode) {
            ucxapp_shell_run(&app);   /* not a terminal (piped input): plain shell */
            rc = 0;
        }
    } else if (rc == 0 && shellMode) {
        ucxapp_shell_run(&app);
    }
    ucxapp_session_end();
    {
        char stopLine[] = "web.stop";
        ucxapp_cmd_exec(&app, stopLine);
    }
    disconnectModule();
    if (json) {
        ucxapp_io_set(&ucxapp_io_stdio);
    }
    return rc;
}
