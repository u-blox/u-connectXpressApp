/* matter.*: a Matter node on the connected module. The node itself (the Matter SDK, C++) is the optional plug-in
 * ucxmatter.dll / libucxmatter.so next to the app, built by "ucxapp --matter". This file loads it, lends it the
 * module's AT client and hands it a copy of every URC (ucxapp_matter.h), so the other commands, the AT trace and
 * the web page keep working while the node runs. */
#include "ucxapp_cmd.h"
#include "ucxapp_matter.h"
#include "ucxapp_store.h"
#include "ucxapp_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define PLUGIN_BASE    "ucxmatter"
#define PLUGIN_ENV     "UCX_MATTER_PLUGIN"   /* another plug-in file (the tests' stand-in); never set from a command */
#define STORE_PREFIX   "matter."
#define STORE_SUFFIX   ".key"                /* sealed by the secure store */
#define STORE_NAME_MAX 16
#define STORE_FILES    16

static void *gLib;
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

static const char *const kLevels[] = { "error", "info", "detail" };

/* ---- what the plug-in may ask of the app -------------------------------- */

static void hostPrint(const char *text)
{
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

static bool fileExists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

/* The plug-in file; false where this build cannot load one (the browser). */
static bool pluginFile(char *out, size_t size)
{
    const char *env = getenv(PLUGIN_ENV);

    if (env != NULL && env[0] != '\0') {
        snprintf(out, size, "%s", env);
        return true;
    }
    return ucxapp_platform_lib_file(PLUGIN_BASE, out, size);
}

static bool pluginPath(void)
{
    return pluginFile(gPath, sizeof(gPath));
}

static void unload(void)
{
    if (gLib != NULL) {
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
    gLib = ucxapp_platform_lib_open(gPath);
    if (gLib == NULL) {
        ucxapp_print("ERROR: %s does not load (built for another system, or one of its own libraries is missing)\n",
                     gPath);
        return false;
    }
    entry = (ucxapp_matter_entry_t)ucxapp_platform_lib_symbol(gLib, UCXAPP_MATTER_ENTRY);
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
    uCxAtClientSetUrcCallback(gClient, gNextUrc, gClient->pUrcCallbackTag);
    U_CX_MUTEX_LOCK(gLock);
    gRunning = false;
    U_CX_MUTEX_UNLOCK(gLock);
    gPlugin->stop();
    unload();
}

bool ucxapp_matter_running(void)
{
    return gRunning;
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
    rc = gPlugin->command(argc, words);
    if (rc == UCXAPP_MATTER_ERR_UNSUPPORTED) {
        ucxapp_print("ERROR: %s is not in this plug-in (%s)\n", argv[0], gPlugin->name);
    }
    return rc;
}

static int cmdStart(ucxapp_t *app, int argc, char *argv[])
{
    const char *words[UCXAPP_MAX_ARGS];
    int rc;

    if (gRunning) {
        ucxapp_print("ERROR: a Matter node is already running; matter.stop ends it\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!load()) {
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 1; i < argc; i++) {
        words[i - 1] = argv[i];
    }
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
    gNextUrc = gClient->urcCallback;
    gRunning = true;
    uCxAtClientSetUrcCallback(gClient, urcTap, gClient->pUrcCallbackTag);
    rc = gPlugin->start(&gHost, argc - 1, words);
    if (rc != UCXAPP_MATTER_OK) {
        uCxAtClientSetUrcCallback(gClient, gNextUrc, gClient->pUrcCallbackTag);
        U_CX_MUTEX_LOCK(gLock);
        gRunning = false;
        U_CX_MUTEX_UNLOCK(gLock);
        ucxapp_print("ERROR: the Matter node did not start (%s)\n", ucxapp_err_code(rc));
        unload();
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("matter: node started (%s)\n", gPlugin->name);
    return UCXAPP_OK;
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

/* Forgets every fabric: the node drops what it holds, then what it stored goes too. */
static int cmdReset(ucxapp_t *app, int argc, char *argv[])
{
    char files[STORE_FILES][48];
    int removed = 0;

    (void)app;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gRunning) {
        int rc = forward(argc, argv);
        if (rc != UCXAPP_OK) {
            return rc;
        }
    }
    memset(files, 0, sizeof(files));
    ucxapp_store_list(collectFile, files);
    for (int i = 0; i < STORE_FILES && files[i][0] != '\0'; i++) {
        removed += ucxapp_store_remove(files[i]) ? 1 : 0;
    }
    ucxapp_print("matter: reset, %d stored file(s) removed; commission the node again\n", removed);
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

static const ucxapp_cmd_t gMatterCmds[] = {
    { "matter.start", "[endpoint...]", "Start a Matter node on the module with the ucxmatter plug-in; endpoints as matter.endpoints lists them, none for the default set", UCXAPP_CMD_NEEDS_DEVICE, cmdStart },
    { "matter.stop", "", "Stop the Matter node and unload the plug-in", 0, cmdStop },
    { "matter.status", "", "Is a node running, with which plug-in, and what the node says about itself", 0, cmdStatus },
    { "matter.card", "", "The node at a glance: state, commissioning step, network, setup codes and endpoints (the web page shows the same card, with buttons, under What's happening)", 0, cmdCard },
    { "matter.endpoints", "", "Endpoints of the running node and the kinds the plug-in can build", 0, cmdForward },
    { "matter.set", "<endpoint> <value...>", "Change an endpoint from this side, as a button or sensor would (matter.endpoints shows the values each takes)", 0, cmdSet },
    { "matter.qr", "", "Setup QR code and manual pairing code of the node", 0, cmdForward },
    { "matter.open", "[seconds]", "Open the commissioning window again", 0, cmdForward },
    { "matter.reset", "", "Forget every fabric and remove what the node stored", UCXAPP_CMD_DANGER, cmdReset },
    { "matter.log", "[error|info|detail]", "How much of the Matter SDK's log is printed", 0, cmdLog },
};

void ucxapp_register_matter_cmds(void)
{
    ucxapp_cmd_register(gMatterCmds, sizeof(gMatterCmds) / sizeof(gMatterCmds[0]));
}
