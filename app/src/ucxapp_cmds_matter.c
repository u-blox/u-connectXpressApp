/* matter.*: a Matter node on the connected module. The node itself (the Matter SDK, C++) is the optional plug-in
 * ucxmatter.dll / libucxmatter.so next to the app, built by "ucxapp --matter". This file loads it, lends it the
 * module's AT client and hands it a copy of every URC (ucxapp_matter.h), so the other commands, the AT trace and
 * the web page keep working while the node runs. */
#include "ucxapp_cmd.h"
#include "ucxapp_matter.h"
#include "ucxapp_store.h"
#include "ucxapp_stream.h"
#include "ucxapp_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_port.h"
#include "u_cx_bluetooth.h"
#include "u_cx_mqtt.h"
#include "u_cx_socket.h"
#include "u_cx_wifi.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define PLUGIN_BASE    "ucxmatter"
#define PLUGIN_ENV     "UCX_MATTER_PLUGIN"   /* another plug-in file (the tests' stand-in); never set from a command */
#define STORE_PREFIX   "matter."
#define STORE_SUFFIX   ".key"                /* sealed by the secure store */
#define STORE_FACTORY  STORE_PREFIX "factory" STORE_SUFFIX   /* the setup codes: matter.reset keeps them */
#define STORE_NAME_MAX 16
#define STORE_FILES    16

static void *gLib;
/* A library a node has run in stays loaded until the app exits, as where the node is linked in (the browser,
   boards): every start after the first is then one in the same process, on every platform */
static void *gKeptLib;
static const ucxapp_matter_plugin_t *gPlugin;
static volatile bool gRunning;
static uCxAtClient_t *gClient;
static uUrcCallback_t gNextUrc;
static U_CX_MUTEX_HANDLE gLock;
static bool gLockMade;
static int gLogLevel = UCXAPP_MATTER_LOG_INFO;
static uint32_t gUrcCount;
static uint32_t gUrcKept;
static char gPath[260];
static char gEndpoints[UCXAPP_MAX_ARGS][24];   /* what matter.start was given: matter.reset starts the same node */
static int gEndpointCount;
/* Every call into the plug-in but urc() and card(): the command thread and a bridge's thread take turns, and
   what the plug-in prints for a bridge goes to gCapture instead of the console. */
static U_CX_MUTEX_HANDLE gCmdLock;
static char *gCapture;
static size_t gCaptureSize, gCaptureLen;

/* matter.bridge: the node's events wait here for the bridge's poll; hostEvent may come from any thread */
#define BRIDGE_EVENTS 16
#define BRIDGE_TEXT   128
static U_CX_MUTEX_HANDLE gBridgeLock;
static volatile bool gBridgeOn;
static char gEvents[BRIDGE_EVENTS][BRIDGE_TEXT];
static int gEventHead, gEventCount;

static const char *const kLevels[] = { "error", "info", "detail" };

/* ---- what the plug-in may ask of the app -------------------------------- */

static void hostPrint(const char *text)
{
    if (gCapture != NULL) {
        int n = snprintf(gCapture + gCaptureLen, gCaptureSize - gCaptureLen, "%s", text);

        gCaptureLen += n > 0 ? (size_t)n : 0;
        gCaptureLen = gCaptureLen < gCaptureSize ? gCaptureLen : gCaptureSize - 1;
        return;
    }
    ucxapp_print("%s", text);
}

static void hostLog(int level, const char *line)
{
    if (level <= gLogLevel) {
        ucxapp_print("[matter] %s\n", line);
    }
}

static void hostEvent(const char *line)
{
    if (ucxappLogEventsEnabled()) {
        ucxapp_print("[event] %s\n", line);
    }
    ucxapp_ui_event(line);
    if (gBridgeOn) {
        U_CX_MUTEX_LOCK(gBridgeLock);
        snprintf(gEvents[(gEventHead + gEventCount) % BRIDGE_EVENTS], BRIDGE_TEXT, "%s", line);
        if (gEventCount < BRIDGE_EVENTS) {
            gEventCount++;
        } else {
            gEventHead = (gEventHead + 1) % BRIDGE_EVENTS;   /* full: the oldest goes */
        }
        U_CX_MUTEX_UNLOCK(gBridgeLock);
    }
}

static void hostQr(const char *payload)
{
    ucxapp_qr_print(payload);
}

static bool storeName(const char *name, char *out, size_t size)
{
    size_t n = name != NULL ? strlen(name) : 0;

    if (n == 0 || n > STORE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9'))) {
            return false;
        }
    }
    snprintf(out, size, STORE_PREFIX "%s" STORE_SUFFIX, name);
    return true;
}

static bool hostStoreSave(const char *name, const void *data, size_t length)
{
    char file[48];

    return storeName(name, file, sizeof(file)) && ucxapp_store_save(file, data, length);
}

static int32_t hostStoreLoad(const char *name, void *buffer, size_t size)
{
    char file[48];
    size_t length = 0;
    uint8_t *data = storeName(name, file, sizeof(file)) ? ucxapp_store_load(file, &length) : NULL;

    if (data == NULL) {
        return -1;
    }
    if (size > 0) {
        memcpy(buffer, data, length < size ? length : size);
    }
    free(data);
    return (int32_t)length;
}

static bool hostStoreRemove(const char *name)
{
    char file[48];

    return storeName(name, file, sizeof(file)) && ucxapp_store_remove(file);
}

static const ucxapp_matter_at_t kAt = {
    .execSimpleCmd = uCxAtClientExecSimpleCmd,
    .execSimpleCmdF = uCxAtClientExecSimpleCmdF,
    .cmdBeginF = uCxAtClientCmdBeginF,
    .cmdGetRspParamLine = uCxAtClientCmdGetRspParamLine,
    .cmdGetRspParamsF = uCxAtClientCmdGetRspParamsF,
    .cmdEnd = uCxAtClientCmdEnd,
    .setCommandTimeout = uCxAtClientSetCommandTimeout,
    .getLastIoError = uCxAtClientGetLastIoError,
};

static ucxapp_matter_host_t gHost = {
    .abi = UCXAPP_MATTER_ABI,
    .size = sizeof(ucxapp_matter_host_t),
    .at = &kAt,
    .print = hostPrint,
    .log = hostLog,
    .event = hostEvent,
    .qr = hostQr,
    .storeSave = hostStoreSave,
    .storeLoad = hostStoreLoad,
    .storeRemove = hostStoreRemove,
    .tickMs = ucxapp_platform_tick_ms,
};

/* ---- URCs: the plug-in first, then the app's own handlers ---------------- */

static void urcTap(struct uCxAtClient *client, void *tag, char *line, size_t length, uint8_t *binary,
                   size_t binaryLength)
{
    /* The app's parser cuts the line up in place, so the plug-in gets its own copy */
    char *copy = gRunning ? (char *)malloc(length + 1) : NULL;
    bool kept = false;

    if (copy != NULL) {
        memcpy(copy, line, length);
        copy[length] = '\0';
        U_CX_MUTEX_LOCK(gLock);
        if (gRunning) {
            gUrcCount++;
            kept = gPlugin->urc(copy, length, binary, binaryLength);
            gUrcKept += kept ? 1u : 0u;
        }
        U_CX_MUTEX_UNLOCK(gLock);
        free(copy);
    }
    if (!kept) {
        gNextUrc(client, tag, line, length, binary, binaryLength);
    }
}

/* ---- loading -------------------------------------------------------------- */

#ifdef UCXAPP_MATTER_STATIC
/* The node is linked into the app (the browser, boards): there is no file to load */
const ucxapp_matter_plugin_t *ucxmatter_plugin(void);
#endif

static bool fileExists(const char *path)
{
#ifdef UCXAPP_MATTER_STATIC
    (void)path;
    return true;
#else
    FILE *f = fopen(path, "rb");

    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
#endif
}

/* The plug-in file; false where this build cannot load one (the browser). */
static bool pluginFile(char *out, size_t size)
{
#ifdef UCXAPP_MATTER_STATIC
    snprintf(out, size, "built in");
    return true;
#else
    const char *env = getenv(PLUGIN_ENV);

    if (env != NULL && env[0] != '\0') {
        snprintf(out, size, "%s", env);
        return true;
    }
    return ucxapp_platform_lib_file(PLUGIN_BASE, out, size);
#endif
}

static bool pluginPath(void)
{
    return pluginFile(gPath, sizeof(gPath));
}

static void unload(void)
{
    /* The AT client may still point at the plug-in's last expected response prefix, a string in the library */
    if (gClient != NULL && gLib != NULL) {
        U_CX_MUTEX_LOCK(gClient->cmdMutex);
        gClient->pExpectedRsp = NULL;
        gClient->pExpectedRspLen = 0;
        U_CX_MUTEX_UNLOCK(gClient->cmdMutex);
    }
    if (gLib != NULL && gLib != gKeptLib) {
        ucxapp_platform_lib_close(gLib);
    }
    gLib = NULL;
    gPlugin = NULL;
}

static bool load(void)
{
    ucxapp_matter_entry_t entry;

    if (!pluginPath()) {
        ucxapp_print("ERROR: this build cannot load the Matter plug-in\n");
        return false;
    }
    if (!fileExists(gPath)) {
        ucxapp_print("ERROR: the Matter plug-in is not installed (%s)\n"
                     "       build it with: ucxapp --matter --build\n", gPath);
        return false;
    }
#ifdef UCXAPP_MATTER_STATIC
    entry = ucxmatter_plugin;
#else
    gLib = gKeptLib != NULL ? gKeptLib : ucxapp_platform_lib_open(gPath);
    if (gLib == NULL) {
        ucxapp_print("ERROR: %s does not load (built for another system, or one of its own libraries is missing)\n",
                     gPath);
        return false;
    }
    entry = (ucxapp_matter_entry_t)ucxapp_platform_lib_symbol(gLib, UCXAPP_MATTER_ENTRY);
#endif
    gPlugin = entry != NULL ? entry() : NULL;
    if (gPlugin == NULL) {
        ucxapp_print("ERROR: %s is not a Matter plug-in (no %s)\n", gPath, UCXAPP_MATTER_ENTRY);
    } else if (gPlugin->abi != UCXAPP_MATTER_ABI || gPlugin->size < sizeof(ucxapp_matter_plugin_t)) {
        ucxapp_print("ERROR: %s is for another version of the app (interface %u, this app has %u); rebuild it\n",
                     gPath, (unsigned)gPlugin->abi, (unsigned)UCXAPP_MATTER_ABI);
        gPlugin = NULL;
    }
    if (gPlugin == NULL) {
        unload();
        return false;
    }
    return true;
}

void ucxapp_matter_stop(void)
{
    if (!gRunning) {
        return;
    }
    U_CX_MUTEX_LOCK(gCmdLock);
    uCxAtClientSetUrcCallback(gClient, gNextUrc, gClient->pUrcCallbackTag);
    U_CX_MUTEX_LOCK(gLock);
    gRunning = false;
    U_CX_MUTEX_UNLOCK(gLock);
    gPlugin->stop();
    unload();
    U_CX_MUTEX_UNLOCK(gCmdLock);
}

bool ucxapp_matter_running(void)
{
    return gRunning;
}

/* The components of an installed plug-in, after the app's own in app.sbom; on the command thread. */
void ucxapp_matter_sbom(void)
{
    bool mine = gPlugin == NULL;

    if (mine && (!pluginPath() || !fileExists(gPath) || !load())) {
        return;
    }
    /* line by line: ucxapp_print has a bounded buffer */
    for (const char *line = gPlugin->sbom != NULL ? gPlugin->sbom : ""; *line != '\0';) {
        const char *end = strchr(line, '\n');
        int len = end != NULL ? (int)(end - line) : (int)strlen(line);

        ucxapp_print("%.*s\n", len, line);
        line += len + (end != NULL ? 1 : 0);
    }
    if (mine) {
        unload();
    }
}

/* The web page asks four times a second from its own thread: the plug-in under the lock that stop takes, the
   file looked for every few seconds only. */
bool ucxapp_matter_card(char *out, size_t size)
{
    static uint32_t checkedMs;
    static bool checked, installed;
    uint32_t now = ucxapp_platform_tick_ms();
    size_t n = 0;
    bool running = false;

    if (size == 0) {
        return false;
    }
    out[0] = '\0';
    if (gLockMade) {
        U_CX_MUTEX_LOCK(gLock);
        running = gRunning;
        if (running) {
            n = gPlugin->card(out, size);
            n = n < size ? n : size - 1;
        }
        U_CX_MUTEX_UNLOCK(gLock);
    }
    if (!running) {
        if (!checked || now - checkedMs >= 5000u) {
            char path[sizeof(gPath)];

            checked = true;
            checkedMs = now;
            installed = pluginFile(path, sizeof(path)) && fileExists(path);
        }
        if (!installed) {
            return false;
        }
        n = (size_t)snprintf(out, size, "NODE  stopped");
        n = n < size ? n : size - 1;
    }
    while (n > 0 && out[n - 1] == '\n') {
        out[--n] = '\0';
    }
    (void)snprintf(out + n, size - n, "%sLOG   %s", n > 0 ? "\n" : "", kLevels[gLogLevel]);
    return true;
}

/* ---- commands ------------------------------------------------------------- */

static int notRunning(void)
{
    ucxapp_print("ERROR: no Matter node is running; start one with matter.start\n");
    return UCXAPP_ERR_FAILED;
}

/* argv[0] "matter.set" reaches the plug-in as "set". */
static int forward(int argc, char *argv[])
{
    const char *words[UCXAPP_MAX_ARGS];
    int rc;

    if (!gRunning) {
        return notRunning();
    }
    words[0] = strchr(argv[0], '.') + 1;
    for (int i = 1; i < argc && i < UCXAPP_MAX_ARGS; i++) {
        words[i] = argv[i];
    }
    U_CX_MUTEX_LOCK(gCmdLock);
    rc = gRunning ? gPlugin->command(argc, words) : UCXAPP_MATTER_ERR_FAILED;
    U_CX_MUTEX_UNLOCK(gCmdLock);
    if (rc == UCXAPP_MATTER_ERR_UNSUPPORTED) {
        ucxapp_print("ERROR: %s is not in this plug-in (%s)\n", argv[0], gPlugin->name);
    }
    return rc;
}

/* The node binds UDP 5540, which a socket an earlier session left bound would hold; the module cannot say which
   port a socket has, so every bound UDP socket from before this session is closed. */
static void closeLeftoverSockets(ucxapp_t *app)
{
    uCxSocketListStatus_t status;
    uint32_t leftovers = ucxapp_sock_leftovers();
    int32_t handles[32];
    int count = 0;

    if (leftovers == 0) {
        return;
    }
    uCxSocketListStatusBegin(app->ucx);
    while (uCxSocketListStatusGetNext(app->ucx, &status) && count < 32) {
        if (status.protocol == U_SOCKET_PROTOCOL_UDP && status.socket_handle >= 0 && status.socket_handle < 32 &&
            (leftovers & (1u << status.socket_handle)) != 0) {
            handles[count++] = status.socket_handle;
        }
    }
    uCxEnd(app->ucx);
    for (int i = 0; i < count; i++) {
        if (uCxSocketClose(app->ucx, handles[i]) == 0) {
            ucxapp_print("matter: closed UDP socket %d, left bound on the module by an earlier session\n",
                         (int)handles[i]);
        }
        ucxapp_handle_remove(UCXAPP_HANDLE_SOCK, handles[i]);
    }
    ucxapp_sock_set_leftovers(0);
}

/* The module's GATT server cannot remove a service: every start defines the Matter service once more, until the
   module reboots. A module that has the service from a start in this boot is rebooted first (a second service
   with the same UUID would leave a commissioner talking to the stale one); one that refuses it (filled by an
   earlier session) is rebooted and the node started once more. */
static uint32_t gServiceBootMs;
static bool gServiceDefined;

static void rebootForService(ucxapp_t *app, const char *why)
{
    bool exact;

    ucxapp_print("matter: rebooting the module, %s (a GATT service cannot be removed)\n", why);
    if (ucxapp_module_reboot(app)) {
        gServiceBootMs = ucxapp_module_boot_ms(&exact);
        gServiceDefined = false;
    }
}

static int startNodeOnce(ucxapp_t *app, int count, const char *const words[]);

static int startNode(ucxapp_t *app, int count, const char *const words[])
{
    bool exact;
    int rc;

    if (gServiceDefined && ucxapp_module_boot_ms(&exact) == gServiceBootMs) {
        rebootForService(app, "its GATT server has the Matter service of the last start");
    }
    rc = startNodeOnce(app, count, words);
    if (rc == UCXAPP_MATTER_ERR_GATT_FULL) {
        rebootForService(app, "its GATT server has no room for the Matter service");
        rc = startNodeOnce(app, count, words);
        if (rc == UCXAPP_MATTER_ERR_GATT_FULL) {
            ucxapp_print("ERROR: the module refused the Matter GATT service after a reboot as well "
                         "(matter.log detail shows why)\n");
        }
    }
    if (rc != UCXAPP_MATTER_ERR_GATT_FULL) {
        return rc;
    }
    gServiceDefined = false;
    return UCXAPP_ERR_FAILED;
}

static int startNodeOnce(ucxapp_t *app, int count, const char *const words[])
{
    bool exact;
    int rc;

    if (!load()) {
        return UCXAPP_ERR_FAILED;
    }
    closeLeftoverSockets(app);
    if (!gLockMade) {
        U_CX_MUTEX_CREATE(gLock);
        gLockMade = true;
    }
    gClient = app->ucx->pAtClient;
    gHost.client = gClient;
    gHost.radios = UCXAPP_MATTER_RADIO_BLE | (ucxapp_module_has_wifi(app) ? UCXAPP_MATTER_RADIO_WIFI : 0u) |
                   (ucxapp_module_has_thread(app) ? UCXAPP_MATTER_RADIO_THREAD : 0u);
    gUrcCount = 0;
    gUrcKept = 0;
    /* URCs flow before start() returns: the node waits for some of them while it comes up */
    U_CX_MUTEX_LOCK(gCmdLock);   /* a bridge's command waits until the node is up */
    gNextUrc = gClient->urcCallback;
    gRunning = true;
    uCxAtClientSetUrcCallback(gClient, urcTap, gClient->pUrcCallbackTag);
    gServiceBootMs = ucxapp_module_boot_ms(&exact);
    gServiceDefined = true;
    gKeptLib = gLib;
    rc = gPlugin->start(&gHost, count, words);
    if (rc != UCXAPP_MATTER_OK) {
        uCxAtClientSetUrcCallback(gClient, gNextUrc, gClient->pUrcCallbackTag);
        U_CX_MUTEX_LOCK(gLock);
        gRunning = false;
        U_CX_MUTEX_UNLOCK(gLock);
        U_CX_MUTEX_UNLOCK(gCmdLock);
        if (rc != UCXAPP_MATTER_ERR_GATT_FULL) {
            ucxapp_print("ERROR: the Matter node did not start (%s)\n", ucxapp_err_code(rc));
        }
        unload();
        return rc == UCXAPP_MATTER_ERR_GATT_FULL ? rc : UCXAPP_ERR_FAILED;
    }
    U_CX_MUTEX_UNLOCK(gCmdLock);
    ucxapp_print("matter: node started (%s)\n", gPlugin->name);
    return UCXAPP_OK;
}

static int cmdStart(ucxapp_t *app, int argc, char *argv[])
{
    const char *words[UCXAPP_MAX_ARGS];

    if (gRunning) {
        ucxapp_print("ERROR: a Matter node is already running; matter.stop ends it\n");
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 1; i < argc; i++) {
        snprintf(gEndpoints[i - 1], sizeof(gEndpoints[0]), "%s", argv[i]);
        words[i - 1] = gEndpoints[i - 1];
    }
    gEndpointCount = argc == 2 && strcmp(argv[1], "all") == 0 ? 0 : argc - 1;
    return startNode(app, gEndpointCount, words);
}

static int cmdStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        return notRunning();
    }
    ucxapp_matter_stop();
    ucxapp_print("matter: node stopped\n");
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        if (!pluginPath()) {
            ucxapp_print("matter: stopped (this build cannot load the plug-in)\n");
        } else {
            ucxapp_print("matter: stopped\nplug-in: %s%s\n", gPath, fileExists(gPath) ? "" : " (not installed)");
        }
        return UCXAPP_OK;
    }
    ucxapp_print("matter: running\nplug-in: %s (%s)\nradios: BLE%s%s\nURCs passed on: %u, kept by the node: %u\nlog: %s\n",
                 gPlugin->name, gPath, (gHost.radios & UCXAPP_MATTER_RADIO_WIFI) ? ", Wi-Fi" : "",
                 (gHost.radios & UCXAPP_MATTER_RADIO_THREAD) ? ", Thread" : "", (unsigned)gUrcCount,
                 (unsigned)gUrcKept, kLevels[gLogLevel]);
    return forward(argc, argv);
}

static int cmdCard(ucxapp_t *app, int argc, char *argv[])
{
    static char card[UCXAPP_CONN_SNAPSHOT];

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_matter_card(card, sizeof(card))) {
        ucxapp_print("matter: no card, the plug-in is not installed (ucxapp --matter --build)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s\n", card);
    return UCXAPP_OK;
}

static int cmdForward(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    return forward(argc, argv);
}

static int cmdSet(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc < 3) {
        return UCXAPP_ERR_USAGE;
    }
    return forward(argc, argv);
}

static void collectFile(const char *name, uint64_t size, void *ctx)
{
    char (*files)[48] = (char (*)[48])ctx;
    size_t n = strlen(name), suffix = sizeof(STORE_SUFFIX) - 1;

    (void)size;
    if (strncmp(name, STORE_PREFIX, sizeof(STORE_PREFIX) - 1) != 0 || n < suffix || n >= 48 ||
        strcmp(name + n - suffix, STORE_SUFFIX) != 0) {
        return;
    }
    for (int i = 0; i < STORE_FILES; i++) {
        if (files[i][0] == '\0') {
            snprintf(files[i], 48, "%s", name);
            return;
        }
    }
}

/* Up to 10 s for the link down a disconnect ends with: a reboot sent while the module still leaves the network
   goes unanswered. False when the module restarted instead (+STARTUP). */
static bool waitWifiDown(void)
{
    for (int i = 0; i < 100; i++) {
        if (ucxapp_events_take(URC_FLAG_WIFI_LINK_DOWN)) {
            return true;
        }
        if (ucxapp_events_take(URC_FLAG_STARTUP)) {
            return false;
        }
        U_CX_PORT_SLEEP_MS(100);
    }
    return true;
}

/* The module as a commissioner must find it: off the Wi-Fi network the node was given (the access point sees it
   leave), rebooted, and with "all" back on its factory settings. A module that restarts by itself on the way
   (seen leaving Wi-Fi) has done the reboot: its echo goes off again and the reset goes on. */
static bool resetModule(ucxapp_t *app, bool factory)
{
    char ssid[64], now[64], ip[48];
    bool restarted = false;

    ucxapp_events_clear(URC_FLAG_STARTUP);
    if (ucxapp_module_has_wifi(app) && ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_events_clear(URC_FLAG_WIFI_LINK_DOWN);
        restarted = ucxapp_events_take(URC_FLAG_STARTUP);
        if (!restarted && uCxWifiStationDisconnect(app->ucx) != 0) {
            restarted = ucxapp_events_take(URC_FLAG_STARTUP);
            if (!restarted) {
                ucxapp_print("ERROR: the module did not leave Wi-Fi network %s\n", ssid);
                return false;
            }
        }
        if (!restarted) {
            restarted = !waitWifiDown();
            if (!restarted && ucxapp_wifi_get_status(app->ucx, now, sizeof(now), ip, sizeof(ip))) {
                ucxapp_print("ERROR: the module is still on Wi-Fi network %s\n", ssid);
                return false;
            }
        }
        ucxapp_print("matter: left Wi-Fi network %s%s\n", ssid, restarted ? " (the module restarted)" : "");
    }
    restarted = restarted || ucxapp_events_take(URC_FLAG_STARTUP);
    if (restarted) {
        U_CX_PORT_SLEEP_MS(200);
        (void)ucxapp_session_restore(app);
    }
    if (factory ? !ucxapp_module_factory_reset(app) : !restarted && !ucxapp_module_reboot(app)) {
        return false;
    }
    ucxapp_print("matter: module %s\n", factory ? "reset to factory settings and rebooted" : "rebooted");
    return true;
}

/* Forgets every fabric: the node stops and what it stored goes. Its setup codes stay, as the label on a product
   does; "all" draws new ones and resets the module to factory settings. Then the module leaves its network and
   reboots, and a node that ran starts again with the same endpoints, ready to be commissioned. */
static int cmdReset(ucxapp_t *app, int argc, char *argv[])
{
    char files[STORE_FILES][48];
    const char *words[UCXAPP_MAX_ARGS];
    bool all = argc == 2 && strcmp(argv[1], "all") == 0;
    bool restart = gRunning;
    int removed = 0;

    if (argc > 2 || (argc == 2 && !all)) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_matter_stop();
    memset(files, 0, sizeof(files));
    ucxapp_store_list(collectFile, files);
    for (int i = 0; i < STORE_FILES && files[i][0] != '\0'; i++) {
        if (!all && strcmp(files[i], STORE_FACTORY) == 0) {
            continue;
        }
        removed += ucxapp_store_remove(files[i]) ? 1 : 0;
    }
    ucxapp_print("matter: reset, %d stored file(s) removed\n", removed);
    if (!resetModule(app, all)) {
        return UCXAPP_ERR_FAILED;
    }
    if (restart) {
        for (int i = 0; i < gEndpointCount; i++) {
            words[i] = gEndpoints[i];
        }
        if (startNode(app, gEndpointCount, words) != UCXAPP_OK) {
            return UCXAPP_ERR_FAILED;
        }
    }
    ucxapp_print("matter: %scommission the node again%s\n", restart ? "" : "matter.start, then ",
                 all ? " with its new setup codes" : "");
    return UCXAPP_OK;
}

static void selfCheck(int *failed, bool ok, const char *name, const char *detail)
{
    ucxapp_print("  %-12s %s  %s\n", name, ok ? "ok    " : "FAILED", detail);
    *failed += ok ? 0 : 1;
}

/* What a node needs, checked from the app's side, then by the node itself; a node that is not running is
   started for it and stopped again. */
static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const char kProbe[] = STORE_PREFIX "selftest" STORE_SUFFIX;
    static const uint8_t kData[] = "ucxapp matter selftest";
    const char *words[1] = { "selftest" };
    char text[160];
    uint8_t *back;
    size_t length = 0;
    uBtMode_t mode = U_BT_MODE_DISABLED;
    bool mine = !gRunning, ok;
    int failed = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("matter selftest\n");
    ok = gRunning || load();
    snprintf(text, sizeof(text), "%s, interface %u", ok ? gPlugin->name : "not loaded", (unsigned)UCXAPP_MATTER_ABI);
    selfCheck(&failed, ok, "plug-in", text);
    if (ok && mine) {
        unload();
    }

    ok = uCxBluetoothGetMode(app->ucx, &mode) == 0 && (mode == U_BT_MODE_PERIPHERAL ||
                                                        mode == U_BT_MODE_CENTRAL_PERIPHERAL);
    snprintf(text, sizeof(text), "Bluetooth LE mode %d%s, %s", (int)mode, ok ? " (peripheral)" : ", needs 2 or 3 (AT+UBTM)",
             ucxapp_module_has_wifi(app) ? "Wi-Fi" : ucxapp_module_has_thread(app) ? "Thread" : "no Wi-Fi or Thread");
    selfCheck(&failed, ok && (ucxapp_module_has_wifi(app) || ucxapp_module_has_thread(app)), "module", text);

    back = ucxapp_store_save(kProbe, kData, sizeof(kData)) ? ucxapp_store_load_raw(kProbe, &length) : NULL;
    ok = back != NULL && length > 4 && memcmp(back, "UCXS", 4) == 0;
    free(back);
    back = ok ? ucxapp_store_load(kProbe, &length) : NULL;
    ok = back != NULL && length == sizeof(kData) && memcmp(back, kData, length) == 0;
    free(back);
    (void)ucxapp_store_remove(kProbe);
    selfCheck(&failed, ok, "store", ok ? "sealed (UCXS) and read back" : "not sealed, or not read back");

    if (mine && failed == 0 && startNode(app, 0, NULL) != UCXAPP_OK) {
        failed++;
    }
    if (gRunning) {
        int rc;

        U_CX_MUTEX_LOCK(gCmdLock);
        rc = gPlugin->command(1, words);
        U_CX_MUTEX_UNLOCK(gCmdLock);

        if (rc == UCXAPP_MATTER_ERR_UNSUPPORTED) {
            ucxapp_print("  node         has no selftest (%s)\n", gPlugin->name);
        } else {
            failed += rc == UCXAPP_MATTER_OK ? 0 : 1;
        }
        if (mine) {
            ucxapp_matter_stop();
        }
    }
    if (failed > 0) {
        ucxapp_print("ERROR: matter selftest: %d check(s) failed\n", failed);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("matter selftest: ok\n");
    return UCXAPP_OK;
}

static int cmdLog(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc == 1) {
        ucxapp_print("matter log: %s\n", kLevels[gLogLevel]);
        return UCXAPP_OK;
    }
    for (int i = 0; argc == 2 && i < (int)(sizeof(kLevels) / sizeof(kLevels[0])); i++) {
        if (strcmp(argv[1], kLevels[i]) == 0) {
            gLogLevel = i;
            return UCXAPP_OK;
        }
    }
    return UCXAPP_ERR_USAGE;
}

/* ---- matter.bridge: the node as lines over TCP, UDP, BLE or a UART, and as topics over MQTT -------------------
 * Out: what changed on the card, as "EP light = on", "STATE commissioned, 1 fabric(s)", "NET ...", and the node's
 * other events as "EVT matter: commissioning window open". In: "set <endpoint> <value...>" and "get" (the whole
 * card), each answered with OK or ERR. MQTT, through the module's client (mqtt.connect first): <prefix>/<endpoint>
 * holds the value and <prefix>/node/<state> the node's state (both retained), <prefix>/event the events, and a
 * message to <prefix>/<endpoint>/set sets the endpoint. */

#define BRIDGE_PORT     7540   /* not 5540, the node's own */
#define BRIDGE_POLL_MS  100
#define BRIDGE_LINES    24
#define BRIDGE_PREFIX   "ucxapp/matter"
#define MQTT_ID         0
#define MQTT_BUSY       (-15)   /* the module's previous MQTT operation is still in flight */

static ucxapp_stream_t *gStream;
static char gLast[BRIDGE_LINES][BRIDGE_TEXT];   /* the card as last sent; the poll's own */
static int gLastCount;
static char gRx[200];
static size_t gRxLen;
static ucxapp_t *gMqttApp;
static char gMqttPrefix[48];
static volatile bool gMqttOn;
static bool gMqttFresh;   /* the next poll publishes the whole card */
static uint32_t gMqttErrors;

static int splitWords(char *text, char *words[], int max)
{
    int n = 0;

    for (char *p = text; n < max;) {
        p += strspn(p, " \t\r\n");
        if (*p == '\0') {
            break;
        }
        words[n++] = p;
        p += strcspn(p, " \t\r\n");
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    return n;
}

/* A card line as the bridge sends it: "EP    1 light = on  [on | off]" is "EP light = on"; LOG is the app's own. */
static bool bridgeLine(char *line, char *out, size_t size)
{
    char *rest = line + strcspn(line, " ");

    if (rest == line) {
        return false;
    }
    if (*rest != '\0') {
        *rest++ = '\0';
    }
    rest += strspn(rest, " ");
    if (strcmp(line, "LOG") == 0) {
        return false;
    }
    if (strcmp(line, "EP") == 0) {
        char *hint = strstr(rest, "  [");

        if (hint != NULL) {
            *hint = '\0';
        }
        rest += strcspn(rest, " ");   /* the endpoint number */
        rest += strspn(rest, " ");
    }
    snprintf(out, size, "%s %s", line, rest);
    return true;
}

static int bridgeCard(char lines[][BRIDGE_TEXT], int max)
{
    char card[UCXAPP_CONN_SNAPSHOT];
    char *next;
    int n = 0;

    if (!ucxapp_matter_card(card, sizeof(card))) {
        return 0;
    }
    for (char *p = card; p != NULL && *p != '\0' && n < max; p = next) {
        next = strchr(p, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        n += bridgeLine(p, lines[n], BRIDGE_TEXT) ? 1 : 0;
    }
    return n;
}

static void bridgePush(const char *text)
{
    if (gStream != NULL) {
        ucxapp_stream_push(gStream, (const uint8_t *)text, strlen(text));
    }
}

/* A plug-in command for a bridge; what the plug-in prints goes to out. */
static int bridgeCommand(int argc, const char *words[], char *out, size_t size)
{
    int rc = UCXAPP_ERR_FAILED;

    out[0] = '\0';
    U_CX_MUTEX_LOCK(gCmdLock);
    if (gRunning) {
        gCaptureSize = size;
        gCaptureLen = 0;
        gCapture = out;
        rc = gPlugin->command(argc, words);
        gCapture = NULL;
    } else {
        snprintf(out, size, "no Matter node is running");
    }
    U_CX_MUTEX_UNLOCK(gCmdLock);
    return rc;
}

/* "set <endpoint> <value...>": "OK", or "ERR" and why. */
static void bridgeSet(int argc, char *argv[], char *reply, size_t size)
{
    const char *words[UCXAPP_MAX_ARGS];
    char said[160];
    const char *why = said;
    int rc;

    if (argc < 3) {
        snprintf(reply, size, "ERR usage: set <endpoint> <value...>");
        return;
    }
    words[0] = "set";
    for (int i = 1; i < argc && i < UCXAPP_MAX_ARGS; i++) {
        words[i] = argv[i];
    }
    rc = bridgeCommand(argc < UCXAPP_MAX_ARGS ? argc : UCXAPP_MAX_ARGS, words, said, sizeof(said));
    if (rc == UCXAPP_MATTER_OK) {
        snprintf(reply, size, "OK");
        return;
    }
    if (strncmp(why, "ERROR: ", 7) == 0) {
        why += 7;
    }
    said[strcspn(said, "\r\n")] = '\0';
    snprintf(reply, size, "ERR %s", rc == UCXAPP_MATTER_ERR_USAGE ? "usage: set <endpoint> <value...>"
                                    : why[0] != '\0' ? why : ucxapp_err_code(rc));
}

static void bridgeExec(char *line)
{
    char *argv[UCXAPP_MAX_ARGS];
    char reply[200];
    int argc = splitWords(line, argv, UCXAPP_MAX_ARGS);

    if (argc == 0) {
        return;
    }
    if (strcasecmp(argv[0], "set") == 0) {
        bridgeSet(argc, argv, reply, sizeof(reply) - 1);
    } else if (strcasecmp(argv[0], "get") == 0 && argc == 1) {
        char lines[BRIDGE_LINES][BRIDGE_TEXT];
        char all[BRIDGE_LINES * BRIDGE_TEXT + 4];
        size_t o = 0;
        int n = bridgeCard(lines, BRIDGE_LINES);

        /* one push: a poll's lines must not land in the middle */
        for (int i = 0; i < n && o + strlen(lines[i]) + 2 < sizeof(all); i++) {
            o += (size_t)snprintf(all + o, sizeof(all) - o, "%s\n", lines[i]);
        }
        snprintf(all + o, sizeof(all) - o, "OK\n");
        bridgePush(all);
        return;
    } else {
        snprintf(reply, sizeof(reply) - 1, "ERR unknown command '%.40s' (set <endpoint> <value...> | get)", argv[0]);
    }
    strcat(reply, "\n");
    bridgePush(reply);
}

/* Bytes from the far side, from any bridge's thread: whole lines are run. */
static bool bridgeWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        char line[sizeof(gRx)];
        bool whole = false;

        U_CX_MUTEX_LOCK(gBridgeLock);
        if (data[i] == '\n' || data[i] == '\r') {
            whole = gRxLen > 0;
            memcpy(line, gRx, gRxLen);
            line[gRxLen] = '\0';
            gRxLen = 0;
        } else if (gRxLen < sizeof(gRx) - 1) {
            gRx[gRxLen++] = (char)data[i];
        } else {
            gRxLen = 0;   /* overlong line: drop it */
        }
        U_CX_MUTEX_UNLOCK(gBridgeLock);
        if (whole) {
            bridgeExec(line);   /* the plug-in's event for it takes gBridgeLock */
        }
    }
    return true;
}

static void mqttPublish(const char *sub, const char *text, bool retain)
{
    char topic[96];
    int32_t r = MQTT_BUSY;

    snprintf(topic, sizeof(topic), "%s/%s", gMqttPrefix, sub);
    for (int i = 0; r == MQTT_BUSY && i < 10; i++) {
        if (i > 0) {
            ucxapp_platform_sleep_ms(100);
        }
        r = uCxMqttPublish(gMqttApp->ucx, MQTT_ID, U_MQTT_QOS_AT_MOST_ONCE, retain ? U_MQTT_RETAIN_YES : U_MQTT_RETAIN_NO,
                           topic, (const uint8_t *)text, (int32_t)strlen(text));
    }
    gMqttErrors += r < 0 ? 1u : 0u;
}

/* "EP light = on" to <prefix>/light, "STATE ..." to <prefix>/node/state. */
static void mqttState(const char *line)
{
    char sub[48];
    const char *eq = strstr(line, " = ");

    if (strncmp(line, "EP ", 3) == 0 && eq != NULL) {
        snprintf(sub, sizeof(sub), "%.*s", (int)(eq - line - 3), line + 3);
        mqttPublish(sub, eq + 3, true);
        return;
    }
    {
        size_t tag = strcspn(line, " ");
        size_t n = (size_t)snprintf(sub, sizeof(sub), "node/");

        for (size_t i = 0; i < tag && n + 1 < sizeof(sub); i++) {
            char c = line[i];
            sub[n++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        sub[n] = '\0';
        mqttPublish(sub, line[tag] == ' ' ? line + tag + 1 : "", true);
    }
}

/* Messages to <prefix>/<endpoint>/set; the module tells with +UEMQDA, read here. */
static void mqttReceive(void)
{
    uint8_t buf[160];
    size_t prefix = strlen(gMqttPrefix);

    if (!ucxapp_events_take(URC_FLAG_MQTT_DATA)) {
        return;
    }
    for (int m = 0; m < 10; m++) {
        const char *topic = NULL;
        char text[200] = "";
        int32_t n = uCxMqttReadBegin(gMqttApp->ucx, MQTT_ID, buf, (uint16_t)(sizeof(buf) - 1), &topic);
        bool mine = false;

        if (n >= 0 && topic != NULL && strncmp(topic, gMqttPrefix, prefix) == 0 && topic[prefix] == '/') {
            const char *ep = topic + prefix + 1;
            const char *slash = strchr(ep, '/');

            buf[n] = '\0';
            mine = slash != NULL && slash > ep && strcmp(slash, "/set") == 0;
            snprintf(text, sizeof(text), "set %.*s %s", mine ? (int)(slash - ep) : 0, ep, (const char *)buf);
        }
        (void)uCxEnd(gMqttApp->ucx);
        if (n <= 0) {
            break;   /* none left: the module answers an error */
        }
        if (mine) {
            char *argv[UCXAPP_MAX_ARGS];
            char reply[200];

            bridgeSet(splitWords(text, argv, UCXAPP_MAX_ARGS), argv, reply, sizeof(reply));
            if (strncmp(reply, "OK", 2) != 0) {
                mqttPublish("event", reply, false);
            }
        }
    }
}

/* Every BRIDGE_POLL_MS on the stream's thread: the node's events and what changed on its card go out. */
static void bridgePoll(void *ctx, ucxapp_stream_t *s)
{
    char now[BRIDGE_LINES][BRIDGE_TEXT];
    char events[BRIDGE_EVENTS][BRIDGE_TEXT];
    char out[1024];
    size_t o = 0;
    int ne = 0, n;
    bool fresh;

    (void)ctx;
    (void)s;
    n = bridgeCard(now, BRIDGE_LINES);
    U_CX_MUTEX_LOCK(gBridgeLock);
    for (; gEventCount > 0; gEventCount--, gEventHead = (gEventHead + 1) % BRIDGE_EVENTS) {
        memcpy(events[ne++], gEvents[gEventHead], BRIDGE_TEXT);
    }
    fresh = gMqttFresh;
    gMqttFresh = false;
    U_CX_MUTEX_UNLOCK(gBridgeLock);

    for (int i = 0; i < ne; i++) {
        /* "matter: light = on" is on the card, as an EP line */
        if (strncmp(events[i], "matter: ", 8) == 0 && strstr(events[i], " = ") != NULL) {
            continue;
        }
        if (o + strlen(events[i]) + 6 < sizeof(out)) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "EVT %s\n", events[i]);
        }
        if (gMqttOn) {
            mqttPublish("event", events[i], false);
        }
    }
    for (int i = 0; i < n; i++) {
        bool same = false;

        for (int j = 0; j < gLastCount && !same; j++) {
            same = strcmp(now[i], gLast[j]) == 0;
        }
        if (!same && o + strlen(now[i]) + 2 < sizeof(out)) {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "%s\n", now[i]);
        }
        if (gMqttOn && (!same || fresh)) {
            mqttState(now[i]);
        }
    }
    memcpy(gLast, now, sizeof(gLast));
    gLastCount = n;
    if (o > 0) {
        bridgePush(out);
    }
    if (gMqttOn) {
        mqttReceive();
    }
}

static bool bridgeOpen(void)
{
    ucxapp_stream_cfg_t cfg = { "matter", NULL, bridgePoll, BRIDGE_POLL_MS, NULL, bridgeWrite };

    if (gStream != NULL) {
        return true;
    }
    U_CX_MUTEX_LOCK(gBridgeLock);
    gEventHead = gEventCount = 0;
    gRxLen = 0;
    U_CX_MUTEX_UNLOCK(gBridgeLock);
    gLastCount = 0;   /* the first poll sends the whole card */
    gBridgeOn = true;
    gStream = ucxapp_stream_open("virt:matter", 0, &cfg);
    gBridgeOn = gStream != NULL;
    return gStream != NULL;
}

static void bridgeCloseIfIdle(void)
{
    if (gStream != NULL && !gMqttOn && !ucxapp_stream_bridge_active(gStream)) {
        gBridgeOn = false;
        ucxapp_stream_close(gStream);
        gStream = NULL;
    }
}

static void mqttStop(void)
{
    char topic[64];

    if (!gMqttOn) {
        return;
    }
    gMqttOn = false;
    snprintf(topic, sizeof(topic), "%s/+/set", gMqttPrefix);
    (void)uCxMqttSubscribe3(gMqttApp->ucx, MQTT_ID, U_MQTT_SUBSCRIBE_ACTION_UNSUBSCRIBE, topic);
    ucxapp_print("matter.bridge mqtt stopped\n");
}

static int bridgeMqtt(ucxapp_t *app, int argc, char *argv[])
{
    const char *prefix = argc == 3 ? argv[2] : BRIDGE_PREFIX;
    size_t len = strlen(prefix);
    char topic[64];
    int32_t r = MQTT_BUSY;

    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3 && strcasecmp(prefix, "off") == 0) {
        if (!gMqttOn) {
            ucxapp_print("matter.bridge mqtt is not running\n");
        }
        mqttStop();
        bridgeCloseIfIdle();
        return UCXAPP_OK;
    }
    if (len == 0 || len >= sizeof(gMqttPrefix) || prefix[len - 1] == '/' || strpbrk(prefix, "+# ") != NULL) {
        ucxapp_print("ERROR: the topic prefix takes at most %d characters, without + # or a closing /\n",
                     (int)sizeof(gMqttPrefix) - 1);
        return UCXAPP_ERR_USAGE;
    }
    if (gMqttOn && strcmp(prefix, gMqttPrefix) == 0) {
        ucxapp_print("matter.bridge mqtt %s is already running\n", prefix);
        return UCXAPP_OK;
    }
    if (!ucxapp_mqtt_up()) {
        ucxapp_print("ERROR: not connected to an MQTT broker (mqtt.connect first)\n");
        return UCXAPP_ERR_FAILED;
    }
    mqttStop();
    snprintf(topic, sizeof(topic), "%s/+/set", prefix);
    ucxapp_events_clear(URC_FLAG_MQTT_SUBSCRIBED);
    for (int i = 0; r == MQTT_BUSY && i < 10; i++) {
        if (i > 0) {
            ucxapp_platform_sleep_ms(100);
        }
        r = uCxMqttSubscribe4(app->ucx, MQTT_ID, U_MQTT_SUBSCRIBE_ACTION_SUBSCRIBE, topic, U_MQTT_QOS_AT_MOST_ONCE);
    }
    if (r != 0) {
        ucxapp_print("ERROR: subscribe to %s failed (%d%s)\n", topic, (int)r, ucxapp_errname(r));
        bridgeCloseIfIdle();
        return UCXAPP_ERR_FAILED;
    }
    (void)ucxapp_events_wait(URC_FLAG_MQTT_SUBSCRIBED, 5);
    snprintf(gMqttPrefix, sizeof(gMqttPrefix), "%s", prefix);
    gMqttApp = app;
    gMqttErrors = 0;
    U_CX_MUTEX_LOCK(gBridgeLock);
    gMqttFresh = true;
    U_CX_MUTEX_UNLOCK(gBridgeLock);
    gMqttOn = true;
    if (!bridgeOpen()) {
        gMqttOn = false;
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("matter.bridge mqtt: endpoints on %s/<endpoint> (set with %s/<endpoint>/set), node state on "
                 "%s/node/..., events on %s/event\n", prefix, prefix, prefix, prefix);
    return UCXAPP_OK;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    int rc = UCXAPP_OK;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "mqtt") == 0) {
        return bridgeMqtt(app, argc, argv);
    }
    if (strcasecmp(argv[1], "status") == 0 && argc == 2) {
        bool streams = gStream != NULL && ucxapp_stream_bridge_active(gStream);

        if (streams) {
            ucxapp_stream_bridge_status(gStream, "");
        }
        if (gMqttOn) {
            ucxapp_print("mqtt: %s/# on the module's broker connection%s\n", gMqttPrefix,
                         gMqttErrors > 0 ? " (some publishes failed)" : "");
        }
        if (!streams && !gMqttOn) {
            ucxapp_print("bridge: off\n");
        }
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0 && argc == 2) {
        bool any = gMqttOn || gStream != NULL;

        mqttStop();
        if (gStream != NULL && ucxapp_stream_bridge_active(gStream)) {
            rc = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, BRIDGE_PORT);
        }
        if (!any) {
            ucxapp_print("bridge: off\n");
        }
    } else if (gStream == NULL && strcasecmp(argv[1], "off") == 0) {
        ucxapp_print("bridge: off\n");
        return argc == 3 ? UCXAPP_OK : UCXAPP_ERR_USAGE;
    } else {
        if (!bridgeOpen()) {
            return UCXAPP_ERR_FAILED;
        }
        rc = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, BRIDGE_PORT);
    }
    bridgeCloseIfIdle();
    return rc;
}

static const ucxapp_cmd_t gMatterCmds[] = {
    { "matter.start", "[all|light|temp|contact|<endpoints>=all]", "Start a Matter node on the module with the ucxmatter plug-in; all (the default): light temp contact, or one kind, or a list of kinds as matter.endpoints shows them (light light contact)", UCXAPP_CMD_NEEDS_DEVICE, cmdStart },
    { "matter.stop", "", "Stop the Matter node", 0, cmdStop },
    { "matter.status", "", "Is a node running, with which plug-in, and what the node says about itself", 0, cmdStatus },
    { "matter.log", "<info|detail|error=info>", "How much of the Matter log is printed: info (the default), detail (the SDK's own), error", 0, cmdLog },
    { "matter.reset", "[all]", "Forget every fabric and remove what the node stored, take the module off its Wi-Fi network and reboot it, then start the node again; all: new setup codes and a module factory reset as well", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdReset },
    { "matter.card", "", "The node at a glance: state, commissioning step, network, setup codes and endpoints (the web page shows the same card, with buttons, under What's happening)", 0, cmdCard },
    { "matter.endpoints", "", "Endpoints of the running node and the kinds the plug-in can build", 0, cmdForward },
    { "matter.set", "<endpoint> <value...>", "Change an endpoint from this side, as a button or sensor would (matter.endpoints shows the values each takes)", 0, cmdSet },
    { "matter.demo", "[on|off]", "The sensors change by themselves, for a demo: the temperature goes round 19 to 24 C in 2 minutes and the contact opens or closes every 30 s; controllers see it", 0, cmdForward },
    { "matter.qr", "", "Show the setup QR code and manual pairing code of the node (it does not advertise: matter.open does)", 0, cmdForward },
    { "matter.open", "[seconds]", "Open the commissioning window again and advertise over Bluetooth LE (180 to 900 s, default 900)", 0, cmdForward },
    { "matter.selftest", "", "Check the plug-in, the module, the sealed store, and on the node its random numbers, crypto known answers, setup codes, Bluetooth LE and network (starts and stops a node when none runs)", UCXAPP_CMD_NEEDS_DEVICE, cmdSelftest },
    { "matter.bridge", "tcp [port] | tls [port] [cert] [clientauth] | tcpc <host> <port> | udp <host> <port> [local] | "
      "udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | uart <port> [baud] | cell | mqtt [prefix|off] | off [n] | status",
      "The node over a bridge: endpoint changes, node state and events go out as lines (EP light = on, STATE ..., "
      "EVT ...), 'set <endpoint> <value>' and 'get' come back (default port 7540); mqtt: <prefix>/<endpoint> and "
      "<prefix>/<endpoint>/set on the module's broker (mqtt.connect first, default prefix " BRIDGE_PREFIX "); up to 4 bridges at once",
      0, cmdBridge },
};

void ucxapp_register_matter_cmds(void)
{
    static bool made;

    if (!made) {
        U_CX_MUTEX_CREATE(gCmdLock);
        U_CX_MUTEX_CREATE(gBridgeLock);
        made = true;
    }
    ucxapp_cmd_register(gMatterCmds, sizeof(gMatterCmds) / sizeof(gMatterCmds[0]));
}
