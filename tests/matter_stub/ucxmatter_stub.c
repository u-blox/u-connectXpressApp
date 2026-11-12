/* Stand-in for the Matter plug-in (app/src/ucxapp_matter.h) for the offline tests: no Matter SDK, but every part
 * of the interface is used, so the app's side of it is tested on any build. Loaded through UCX_MATTER_PLUGIN. */
#include "ucxapp_matter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#define MAX_ENDPOINTS 8

typedef struct {
    char name[16];
    char value[16];
} endpoint_t;

static const char *const kKinds[] = { "light", "temp", "contact" };
static const char *const kDefaults[] = { "off", "21.5", "closed" };

static const ucxapp_matter_host_t *gHost;
static endpoint_t gEndpoints[MAX_ENDPOINTS];
static int gEndpointCount;
static char gModel[32];
static char gLastUrc[32];
static unsigned gUrcs;
static uint32_t gStarts;

static void say(const char *fmt, const char *a, const char *b)
{
    char line[160];

    snprintf(line, sizeof(line), fmt, a, b);
    gHost->print(line);
}

static int kindOf(const char *name)
{
    for (int i = 0; i < (int)(sizeof(kKinds) / sizeof(kKinds[0])); i++) {
        if (strcmp(name, kKinds[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int start(const ucxapp_matter_host_t *host, int argc, const char *const argv[])
{
    char *model = NULL;
    char line[96];

    gHost = host;
    gEndpointCount = 0;
    gUrcs = 0;
    gLastUrc[0] = '\0';
    if (argc > MAX_ENDPOINTS) {
        return UCXAPP_MATTER_ERR_USAGE;
    }
    for (int i = 0; i < (argc > 0 ? argc : 3); i++) {
        int kind = argc > 0 ? kindOf(argv[i]) : i;
        if (kind < 0) {
            say("ERROR: unknown endpoint %s%s\n", argv[i], "");
            return UCXAPP_MATTER_ERR_USAGE;
        }
        snprintf(gEndpoints[gEndpointCount].name, sizeof(gEndpoints[0].name), "%s", kKinds[kind]);
        snprintf(gEndpoints[gEndpointCount].value, sizeof(gEndpoints[0].value), "%s", kDefaults[kind]);
        gEndpointCount++;
    }
    /* One plain and one variadic call with a parsed answer, as the generated u-connectClient API makes them */
    if (host->at->execSimpleCmd(host->client, "AT") != 0) {
        host->log(UCXAPP_MATTER_LOG_ERROR, "the module does not answer AT");
        return UCXAPP_MATTER_ERR_FAILED;
    }
    host->at->cmdBeginF(host->client, "AT+GMM", "", NULL);
    if (host->at->cmdGetRspParamsF(host->client, "", NULL, NULL, "s", &model, NULL) == 1 && model != NULL) {
        snprintf(gModel, sizeof(gModel), "%s", model);
    }
    if (host->at->cmdEnd(host->client) != 0) {
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (host->storeLoad("stub", &gStarts, sizeof(gStarts)) != (int32_t)sizeof(gStarts)) {
        gStarts = 0;
    }
    gStarts++;
    if (!host->storeSave("stub", &gStarts, sizeof(gStarts))) {
        return UCXAPP_MATTER_ERR_FAILED;
    }
    snprintf(line, sizeof(line), "stub node on %s, %d endpoint(s)", gModel, gEndpointCount);
    host->log(UCXAPP_MATTER_LOG_INFO, line);
    host->log(UCXAPP_MATTER_LOG_DETAIL, "stub detail line");
    host->event("matter: stub node started");
    return UCXAPP_MATTER_OK;
}

static void stop(void)
{
    gHost->log(UCXAPP_MATTER_LOG_INFO, "stub node stopped");
}

static bool urc(char *line, size_t length, const uint8_t *binary, size_t binaryLength)
{
    /* UCX_STUB_KEEP: a URC name the stub claims for itself, as the node does for its own GATT writes */
    const char *keep = getenv("UCX_STUB_KEEP");

    (void)binary;
    (void)binaryLength;
    /* cut at the ':' as the real parser does: the app must still see the whole line */
    for (size_t i = 0; i < length; i++) {
        if (line[i] == ':') {
            line[i] = '\0';
        }
    }
    snprintf(gLastUrc, sizeof(gLastUrc), "%s", line);
    gUrcs++;
    return keep != NULL && strcmp(keep, line) == 0;
}

static int command(int argc, const char *const argv[])
{
    char number[16];

    if (strcmp(argv[0], "status") == 0) {
        snprintf(number, sizeof(number), "%u", (unsigned)gStarts);
        say("node: stub on %s, started %s time(s)\n", gModel, number);
        snprintf(number, sizeof(number), "%u", gUrcs);
        say("stub URCs: %s, last %s\n", number, gLastUrc[0] != '\0' ? gLastUrc : "none");
        return UCXAPP_MATTER_OK;
    }
    if (strcmp(argv[0], "endpoints") == 0) {
        for (int i = 0; i < gEndpointCount; i++) {
            snprintf(number, sizeof(number), "%d", i + 1);
            say("%s  %s", number, gEndpoints[i].name);
            say("  %s%s\n", gEndpoints[i].value, "");
        }
        gHost->print("kinds: light temp contact\n");
        return UCXAPP_MATTER_OK;
    }
    if (strcmp(argv[0], "set") == 0 && argc >= 3) {
        for (int i = 0; i < gEndpointCount; i++) {
            if (strcmp(argv[1], gEndpoints[i].name) == 0) {
                char line[64];
                snprintf(gEndpoints[i].value, sizeof(gEndpoints[i].value), "%s", argv[2]);
                snprintf(line, sizeof(line), "matter: %s = %s", argv[1], argv[2]);
                gHost->event(line);
                return UCXAPP_MATTER_OK;
            }
        }
        say("ERROR: no endpoint %s%s\n", argv[1], "");
        return UCXAPP_MATTER_ERR_FAILED;
    }
    if (strcmp(argv[0], "qr") == 0) {
        gHost->qr("MT:STUB0000000000000");
        gHost->print("manual code: 0000-000-0000\n");
        return UCXAPP_MATTER_OK;
    }
    if (strcmp(argv[0], "reset") == 0) {
        gStarts = 0;
        return UCXAPP_MATTER_OK;
    }
    return UCXAPP_MATTER_ERR_UNSUPPORTED;   /* "open": a command this plug-in does not have */
}

static ucxapp_matter_plugin_t gPlugin = {
    .abi = UCXAPP_MATTER_ABI,
    .size = sizeof(ucxapp_matter_plugin_t),
    .name = "stub",
    .start = start,
    .stop = stop,
    .urc = urc,
    .command = command,
};

EXPORT const ucxapp_matter_plugin_t *ucxmatter_plugin(void)
{
    /* UCX_STUB_ABI: pose as a plug-in built for another version of the app */
    const char *abi = getenv("UCX_STUB_ABI");

    if (abi != NULL) {
        gPlugin.abi = (uint32_t)atoi(abi);
    }
    return &gPlugin;
}
