#include "ucxapp_cmd.h"
#include "ucxapp_hash.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
#include "u_cx_system.h"
#include "u_cx_xmodem.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define FW_DEFAULT_BAUD   921600
#define AT_BAUD           115200
#define FW_SWITCH_MS      2000      /* time for the module to enter XMODEM mode */
#define FW_STARTUP_WAIT_S 10

typedef struct {
    const uint8_t *data;
    size_t len;
} image_t;

static int32_t readImage(uint8_t *buf, size_t offset, size_t maxLen, void *user)
{
    const image_t *img = (const image_t *)user;
    if (offset >= img->len) {
        return 0;
    }
    size_t n = img->len - offset;
    if (n > maxLen) {
        n = maxLen;
    }
    memcpy(buf, img->data + offset, n);
    return (int32_t)n;
}

static void showProgress(size_t total, size_t done, void *user)
{
    static int last = -1;
    (void)user;
    int percent = total > 0 ? (int)((done * 100) / total) : 0;
    if (percent != last) {
        last = percent;
        ucxapp_print("\rtransferred %3d%% (%u/%u bytes)%s", percent, (unsigned)done, (unsigned)total,
                     percent >= 100 ? "\n" : "");
    }
}

static bool endsWithNoCase(const char *s, const char *suffix)
{
    size_t n = strlen(s);
    size_t m = strlen(suffix);
    if (n < m) {
        return false;
    }
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) {
            return false;
        }
    }
    return true;
}

/* A zip is only extracted; flashing anything but a .bin hangs the module until a HW reset. */
static bool resolveImage(const char *arg, char *binPath, size_t binSize)
{
    if (endsWithNoCase(arg, ".zip")) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%.*s_extracted", (int)(strlen(arg) - 4), arg);
        ucxapp_print("extracting %s\n", arg);
        if (!ucxapp_unzip(arg, dir)) {
            ucxapp_print("ERROR: cannot extract %s\n", arg);
            return false;
        }
        if (!ucxapp_platform_find_file(dir, ".bin", binPath, binSize)) {
            ucxapp_print("ERROR: no .bin file found in %s\n", dir);
            return false;
        }
        ucxapp_print("firmware image: %s\n", binPath);
    } else {
        snprintf(binPath, binSize, "%s", arg);
    }
    if (!endsWithNoCase(binPath, ".bin")) {
        ucxapp_print("ERROR: refusing to flash %s: not a .bin file\n", binPath);
        return false;
    }
    return true;
}

static void showVersion(ucxapp_t *app)
{
    const char *text = NULL;
    if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
        ucxapp_print("firmware version: %s\n", text);
    }
    uCxEnd(app->ucx);
}

static int cmdFwFlash(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = FW_DEFAULT_BAUD;
    char binPath[512];
    size_t len = 0;

    if (argc < 2 || argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &baud) || baud <= 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!resolveImage(argv[1], binPath, sizeof(binPath))) {
        return UCXAPP_ERR_FAILED;
    }
    uint8_t *data = ucxapp_store_load(binPath, &len);
    if (data == NULL || len == 0) {
        ucxapp_print("ERROR: cannot read %s\n", binPath);
        free(data);
        return UCXAPP_ERR_FAILED;
    }

    uCxAtClient_t *client = app->ucx->pAtClient;
    const char *portName = client->pConfig->pUartDevName;
    int rc = UCXAPP_ERR_FAILED;

    ucxapp_print("flashing %u bytes via %s at %d baud; the module reboots when done\n",
                 (unsigned)len, portName, (int)baud);
    ucxapp_events_clear(URC_FLAG_STARTUP);

    /* The module switches to XMODEM without a normal AT response, so the result is ignored. */
    (void)uCxSystemStartSerialFirmwareUpdate2(app->ucx, baud, 0);
    uCxAtClientClose(client);
    U_CX_PORT_SLEEP_MS(FW_SWITCH_MS);

    uCxXmodemConfig_t xm;
    uCxXmodemInit(portName, &xm);
    xm.use1K = true;
    int32_t err = uCxXmodemOpen(&xm, baud, false);
    if (err != 0) {
        ucxapp_print("ERROR: cannot open %s for XMODEM (%d)\n", portName, (int)err);
    } else {
        image_t img = { data, len };
        err = uCxXmodemSend(&xm, len, readImage, showProgress, &img);
        uCxXmodemClose(&xm);
        if (err != 0) {
            ucxapp_print("\nERROR: transfer failed (%d)\n", (int)err);
        } else {
            ucxapp_print("transfer complete\n");
            rc = UCXAPP_OK;
        }
    }
    free(data);

    err = uCxAtClientOpen(client, AT_BAUD, false);
    if (err != 0) {
        ucxapp_print("ERROR: cannot reopen %s (%d); reconnect manually\n", portName, (int)err);
        app->ucx = NULL;
        return UCXAPP_ERR_FAILED;
    }
    if (rc == UCXAPP_OK && !ucxapp_events_wait(URC_FLAG_STARTUP, FW_STARTUP_WAIT_S)) {
        ucxapp_print("warning: no +STARTUP received, continuing\n");
    }
    if (uCxSystemSetEchoOff(app->ucx) != 0) {
        ucxapp_print("warning: could not disable echo\n");
    }
    if (uCxSystemSetExtendedError(app->ucx, U_SYS_EXTENDED_ERRORS_ON) != 0) {
        ucxapp_print("warning: could not enable extended error codes\n");
    }
    showVersion(app);
    return rc;
}

/* A module in bootloader mode has no AT interface: wait for its '>' prompt, send 'x', then XMODEM. */
static int cmdFwBootloader(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = AT_BAUD;
    char binPath[512];
    size_t len = 0;
    uint8_t rx[128];
    bool prompt = false;
    uCxXmodemConfig_t xm;

    if (argc < 3 || argc > 4 || (argc == 4 && (!ucxapp_parse_int(argv[3], &baud) || baud <= 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx != NULL) {
        ucxapp_print("ERROR: disconnect first (device.disconnect); the port is in use\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!resolveImage(argv[2], binPath, sizeof(binPath))) {
        return UCXAPP_ERR_FAILED;
    }
    uint8_t *data = ucxapp_store_load(binPath, &len);
    if (data == NULL || len == 0) {
        ucxapp_print("ERROR: cannot read %s\n", binPath);
        free(data);
        return UCXAPP_ERR_FAILED;
    }
    uCxXmodemInit(argv[1], &xm);
    xm.use1K = true;
    int32_t err = uCxXmodemOpen(&xm, baud, false);
    if (err != 0) {
        ucxapp_print("ERROR: cannot open %s (%d)\n", argv[1], (int)err);
        free(data);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("waiting up to 30 s for the bootloader prompt '>' on %s at %d baud (put the module in bootloader mode)\n",
                 argv[1], (int)baud);
    uPortUartWrite(xm.uartHandle, "\r", 1);
    for (int32_t start = U_CX_PORT_GET_TIME_MS(); !prompt && U_CX_PORT_GET_TIME_MS() - start < 30000;) {
        int32_t n = uPortUartRead(xm.uartHandle, rx, sizeof(rx), 500);

        for (int32_t i = 0; i < n; i++) {
            prompt = prompt || rx[i] == '>';
        }
    }
    int rc = UCXAPP_ERR_FAILED;

    if (!prompt) {
        ucxapp_print("ERROR: no bootloader prompt\n");
    } else {
        image_t img = { data, len };

        uPortUartWrite(xm.uartHandle, "x\r\n", 3);
        U_CX_PORT_SLEEP_MS(500);
        err = uCxXmodemSend(&xm, len, readImage, showProgress, &img);
        if (err != 0) {
            ucxapp_print("\nERROR: transfer failed (%d)\n", (int)err);
        } else {
            ucxapp_print("transfer complete; the module reboots with the new firmware\n");
            rc = UCXAPP_OK;
        }
    }
    uCxXmodemClose(&xm);
    free(data);
    return rc;
}

#define FW_GITHUB_HOST    "github.com"
#define FW_API_HOST       "api.github.com"
#define FW_RELEASES_PATH  "/repos/u-blox/u-connectXpress/releases?per_page=30"
#define FW_URL_KEY        "\"browser_download_url\""
#define FW_URL_PREFIX     "https://github.com"

/* Newest release asset "<family>-x.y.z.zip" (no EVAL or special builds); the API lists newest first. */
static bool findLatestAsset(const char *json, const char *family, char *path, size_t pathSize, char *name, size_t nameSize)
{
    const char *p = json;
    size_t famLen = strlen(family);

    while ((p = strstr(p, FW_URL_KEY)) != NULL) {
        p += strlen(FW_URL_KEY);
        /* GitHub pretty-prints with a space (and sometimes a newline) around the colon. */
        while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        if (*p != '"') {
            continue;
        }
        p++;
        const char *end = strchr(p, '"');
        if (end == NULL) {
            break;
        }
        size_t n = (size_t)(end - p);
        const char *base = p;
        for (const char *c = p; c < end; c++) {
            if (*c == '/') {
                base = c + 1;
            }
        }
        size_t baseLen = (size_t)(end - base);
        if (n > strlen(FW_URL_PREFIX) && strncmp(p, FW_URL_PREFIX, strlen(FW_URL_PREFIX)) == 0 &&
            baseLen > famLen + 1 && strncmp(base, family, famLen) == 0 && base[famLen] == '-' &&
            base[famLen + 1] >= '0' && base[famLen + 1] <= '9' && baseLen < nameSize &&
            (memcpy(name, base, baseLen), name[baseLen] = '\0', endsWithNoCase(name, ".zip"))) {
            size_t pLen = n - strlen(FW_URL_PREFIX);
            if (pLen < pathSize && baseLen < nameSize) {
                memcpy(path, p + strlen(FW_URL_PREFIX), pLen);
                path[pLen] = '\0';
                memcpy(name, base, baseLen);
                name[baseLen] = '\0';
                return true;
            }
        }
        p = end;
    }
    return false;
}

/* Finds the newest GitHub firmware zip for the connected module family. */
static bool fwLookupLatest(ucxapp_t *app, char *path, size_t pathSize, char *name, size_t nameSize)
{
    char model[64] = "";
    char family[16];
    const char *text = NULL;

    if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
        snprintf(model, sizeof(model), "%s", text);
    }
    uCxEnd(app->ucx);
    if (strncmp(model, "NORA-", 5) != 0 || strlen(model) < 8) {
        ucxapp_print("ERROR: unknown module model '%s'\n", model);
        return false;
    }
    snprintf(family, sizeof(family), "%.8s", model);

    ucxapp_print("looking for the latest %s firmware on GitHub\n", family);
    char *json = ucxapp_platform_https_get_text(FW_API_HOST, FW_RELEASES_PATH);
    if (json == NULL) {
        ucxapp_print("ERROR: cannot query GitHub (needs the Windows build and internet access)\n");
        return false;
    }
    bool found = findLatestAsset(json, family, path, pathSize, name, nameSize);
    free(json);
    if (!found) {
        ucxapp_print("ERROR: no release found for %s\n", family);
    }
    return found;
}

static bool fwFetch(const char *path, const char *name, const char *file)
{
    size_t size = 0;
    char hex[UCXAPP_SHA256_HEX_LEN];

    ucxapp_print("downloading %s\n", name);
    char *data = ucxapp_platform_https_get_binary(FW_GITHUB_HOST, path, &size);
    if (data == NULL || !ucxapp_store_save(file, data, size)) {
        ucxapp_print("ERROR: download of %s failed\n", name);
        free(data);
        return false;
    }
    ucxapp_sha256_hex((const uint8_t *)data, size, hex);
    free(data);
    ucxapp_print("saved %s (%u bytes)\nsha256: %s\n", file, (unsigned)size, hex);
    return true;
}

/* Downloads the newest firmware for the connected module family from the u-connectXpress GitHub releases. */
static int fwDownloadLatest(ucxapp_t *app, int argc, char *argv[])
{
    char path[512];
    char name[128];

    if (!fwLookupLatest(app, path, sizeof(path), name, sizeof(name))) {
        return UCXAPP_ERR_FAILED;
    }
    const char *file = argc == 3 ? argv[2] : name;
    if (!fwFetch(path, name, file)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("flash it with: fw.flash %s\n", file);
    return UCXAPP_OK;
}

/* "3.5.0-156" or "NORA-W36-3.5.0.zip" -> major/minor/patch; false if no x.y.z found. */
static bool parseVersion(const char *s, int v[3])
{
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9' && sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3) {
            return true;
        }
    }
    return false;
}

static void currentVersion(ucxapp_t *app, char *out, size_t size)
{
    const char *text = NULL;

    out[0] = '\0';
    if (uCxGeneralGetSoftwareVersionBegin(app->ucx, &text) && text != NULL) {
        snprintf(out, size, "%s", text);
    }
    uCxEnd(app->ucx);
}

/* Current vs newest GitHub release; downloads (unless already in the store), flashes and verifies. */
static int cmdFwUpdate(ucxapp_t *app, int argc, char *argv[])
{
    char path[512];
    char name[128];
    char current[64];
    int cur[3];
    int latest[3];
    bool force = false;
    char *flashArgv[3];
    int flashArgc = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "force") == 0) {
            force = true;
        } else if (flashArgc == 2) {
            flashArgv[2] = argv[i];
            flashArgc = 3;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    currentVersion(app, current, sizeof(current));
    ucxapp_print("current firmware: %s\n", current[0] != '\0' ? current : "unknown");
    if (!fwLookupLatest(app, path, sizeof(path), name, sizeof(name))) {
        return UCXAPP_ERR_FAILED;
    }
    if (!parseVersion(name, latest)) {
        ucxapp_print("ERROR: cannot read the version of %s\n", name);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("latest firmware:  %d.%d.%d (%s)\n", latest[0], latest[1], latest[2], name);
    if (!force && parseVersion(current, cur) &&
        (cur[0] > latest[0] || (cur[0] == latest[0] && (cur[1] > latest[1] || (cur[1] == latest[1] && cur[2] >= latest[2]))))) {
        ucxapp_print("already up to date; use 'fw.update force' to flash anyway\n");
        return UCXAPP_OK;
    }

    size_t len = 0;
    uint8_t *existing = ucxapp_store_load(name, &len);
    free(existing);
    if (existing != NULL && len > 0) {
        ucxapp_print("using %s already in the store\n", name);
    } else if (!fwFetch(path, name, name)) {
        return UCXAPP_ERR_FAILED;
    }

    flashArgv[0] = "fw.flash";
    flashArgv[1] = name;
    int rc = cmdFwFlash(app, flashArgc, flashArgv);
    if (rc != UCXAPP_OK || app->ucx == NULL) {
        return rc != UCXAPP_OK ? rc : UCXAPP_ERR_FAILED;
    }
    currentVersion(app, current, sizeof(current));
    if (!parseVersion(current, cur) || cur[0] != latest[0] || cur[1] != latest[1] || cur[2] != latest[2]) {
        ucxapp_print("ERROR: module reports %s, expected %d.%d.%d\n", current, latest[0], latest[1], latest[2]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("updated to %s\n", current);
    return UCXAPP_OK;
}

static int cmdFwDownload(ucxapp_t *app, int argc, char *argv[])
{
    static char line[2300];

    if (argc == 1 || (argc >= 2 && argc <= 3 && strcmp(argv[1], "latest") == 0)) {
        return fwDownloadLatest(app, argc, argv);
    }
    if (argc < 3 || argc > 4 || strchr(argv[1], '"') != NULL || strchr(argv[2], '"') != NULL ||
        strlen(argv[1]) > 2048) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(line, sizeof(line), "http.download \"%s\" \"%s\"", argv[1], argv[2]);
    int rc = ucxapp_cmd_exec(app, line);
    if (rc != UCXAPP_OK) {
        return rc;
    }

    size_t len = 0;
    uint8_t *data = ucxapp_store_load(argv[2], &len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read back %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    char hex[UCXAPP_SHA256_HEX_LEN];
    ucxapp_sha256_hex(data, len, hex);
    free(data);
    ucxapp_print("sha256: %s\n", hex);
    if (argc == 4 && !ucxapp_hex_equal(hex, argv[3])) {
        ucxapp_print("ERROR: sha256 mismatch (expected %s); file removed\n", argv[3]);
        ucxapp_store_remove(argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gFwCmds[] = {
    { "fw.download", "[latest [file]] | <url> <file> [sha256]", "Download the newest GitHub firmware for this module (default), or a file via the module with SHA-256 check (latest needs Windows)", UCXAPP_CMD_NEEDS_DEVICE, cmdFwDownload },
    { "fw.bootloader", "<port> <file.bin|file.zip> [baud]", "Flash a module that is in bootloader mode (no AT interface); disconnect first", UCXAPP_CMD_DANGER, cmdFwBootloader },
    { "fw.flash",    "<file.bin|file.zip> [baud]", "Update the module firmware over XMODEM (default 921600 baud)", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdFwFlash },
    { "fw.update",   "[force] [baud]", "Compare with the newest GitHub firmware; if newer, download to the store, flash and verify the version (needs Windows)", UCXAPP_CMD_NEEDS_DEVICE | UCXAPP_CMD_DANGER, cmdFwUpdate },
};

void ucxapp_register_fw_cmds(void)
{
    ucxapp_cmd_register(gFwCmds, sizeof(gFwCmds) / sizeof(gFwCmds[0]));
}
