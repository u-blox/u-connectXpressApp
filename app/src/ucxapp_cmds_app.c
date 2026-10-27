#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STR2(x) #x
#define STR(x)  STR2(x)

#ifdef APP_VERSION_MAJOR
#define APP_VERSION STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR) "." STR(APP_VERSION_PATCH) "." STR(APP_VERSION_BUILD)
#define UCX_API_VERSION STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR) "." STR(APP_VERSION_PATCH)
#else
#define APP_VERSION "dev"
#define UCX_API_VERSION "-"
#endif

#if defined(_WIN32)
#define APP_PLATFORM "Windows"
#elif defined(U_PORT_POSIX)
#define APP_PLATFORM "Linux"
#elif defined(__ZEPHYR__)
#define APP_PLATFORM "Zephyr"
#else
#define APP_PLATFORM "STM32 FreeRTOS"
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define APP_ARCH "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define APP_ARCH "arm64"
#elif defined(__arm__)
#define APP_ARCH "arm"
#else
#define APP_ARCH "unknown"
#endif

#if defined(_MSC_VER)
#define APP_COMPILER "MSVC " STR(_MSC_VER)
#elif defined(__GNUC__)
#define APP_COMPILER "gcc " __VERSION__
#else
#define APP_COMPILER "unknown"
#endif

#if defined(_WIN32) || defined(U_PORT_POSIX)
#define HOSTED 1
#else
#define HOSTED 0
#endif

typedef struct {
    const char *name;
    const char *version;
    const char *license;
    const char *use;
    const char *source;
    const char *notice;   /* full text for app.license, NULL if the name says it all */
} component_t;

#if HOSTED || !defined(__ZEPHYR__)
static const char kMit[] =
    "Permission is hereby granted, free of charge, to any person obtaining a copy of\n"
    "this software and associated documentation files (the \"Software\"), to deal in\n"
    "the Software without restriction, including without limitation the rights to\n"
    "use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of\n"
    "the Software, and to permit persons to whom the Software is furnished to do so,\n"
    "subject to the following conditions:\n"
    "- The above copyright notice and this permission notice shall be included in\n"
    "  all copies or substantial portions of the Software.\n"
    "- The Software is provided \"as is\", without warranty of any kind, express or\n"
    "  implied, including but not limited to the warranties of merchantability,\n"
    "  fitness for a particular purpose and noninfringement. In no event shall the\n"
    "  authors or copyright holders be liable for any claim, damages or other\n"
    "  liability, whether in an action of contract, tort or otherwise, arising from,\n"
    "  out of or in connection with the Software or the use or other dealings in the\n"
    "  Software.\n";
#endif

#if HOSTED
static const char kZlib[] =
    "This software is provided 'as-is', without any express or implied warranty. In\n"
    "no event will the authors be held liable for any damages arising from the use\n"
    "of this software.\n"
    "Permission is granted to anyone to use this software for any purpose, including\n"
    "commercial applications, and to alter it and redistribute it freely, subject to\n"
    "the following restrictions:\n"
    "1. The origin of this software must not be misrepresented; you must not claim\n"
    "   that you wrote the original software. If you use this software in a product,\n"
    "   an acknowledgment in the product documentation would be appreciated but is\n"
    "   not required.\n"
    "2. Altered source versions must be plainly marked as such, and must not be\n"
    "   misrepresented as being the original software.\n"
    "3. This notice may not be removed or altered from any source distribution.\n";
#endif

static const char kApache[] =
    "Licensed under the Apache License, Version 2.0 (the \"License\"); you may not use\n"
    "this file except in compliance with the License. You may obtain a copy of the\n"
    "License at http://www.apache.org/licenses/LICENSE-2.0\n"
    "Unless required by applicable law or agreed to in writing, software distributed\n"
    "under the License is distributed on an \"AS IS\" BASIS, WITHOUT WARRANTIES OR\n"
    "CONDITIONS OF ANY KIND, either express or implied.\n";

static const component_t kComponents[] = {
    { "u-connectXpressApp", APP_VERSION, "Copyright (C) u-blox AG", "this application",
      "https://github.com/u-blox/u-connectXpressApp", NULL },
    { "u-connectClient", UCX_API_VERSION, "Apache-2.0", "AT command client for u-connectXpress modules",
      "https://github.com/u-blox/u-connectClient", kApache },
#if HOSTED
    { "qrcodegen", "-", "MIT", "QR codes (qr.*)",
      "https://github.com/nayuki/QR-Code-generator", kMit },
    { "puff", "-", "Zlib", "inflate in utils.unzip, adapted from zlib's contrib/puff",
      "https://github.com/madler/zlib", kZlib },
    { "Bluetooth SIG Assigned Numbers", "-", "Bluetooth SIG", "UUID, company and appearance names",
      "https://www.bluetooth.com/specifications/assigned-numbers/", NULL },
#endif
#ifdef _WIN32
    { "FTDI D2XX", "-", "FTDI driver licence", "ftd2xx64.dll, FTDI port names and reset",
      "https://ftdichip.com/drivers/d2xx-drivers/", NULL },
#endif
#if !HOSTED && defined(__ZEPHYR__)
    { "Zephyr RTOS", "-", "Apache-2.0", "kernel and drivers", "https://www.zephyrproject.org", kApache },
#elif !HOSTED
    { "STM32Cube HAL", "-", "BSD-3-Clause", "board support", "https://github.com/STMicroelectronics", NULL },
    { "FreeRTOS", "-", "MIT", "kernel", "https://www.freertos.org", kMit },
#endif
};

#define COMPONENT_COUNT (sizeof(kComponents) / sizeof(kComponents[0]))

static uint32_t gStartMs;
static uint32_t gLastMs;
static uint64_t gWrapMs;

static int cmdVersion(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("u-connectXpressApp %s\n", APP_VERSION);
    ucxapp_print("built     %s %s\n", __DATE__, __TIME__);
    ucxapp_print("platform  %s %s\n", APP_PLATFORM, APP_ARCH);
    ucxapp_print("compiler  %s\n", APP_COMPILER);
    return UCXAPP_OK;
}

static int cmdUptime(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uint32_t now = ucxapp_platform_tick_ms();
    if (now < gLastMs) {
        gWrapMs += (uint64_t)UINT32_MAX + 1;   /* the 32-bit tick wraps after 49.7 days */
    }
    gLastMs = now;
    uint64_t s = (gWrapMs + now - gStartMs) / 1000;
    ucxapp_print("%u days %02u:%02u:%02u\n", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24),
                 (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    return UCXAPP_OK;
}

static int cmdSbom(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < COMPONENT_COUNT; i++) {
        const component_t *c = &kComponents[i];
        bool hasVersion = strcmp(c->version, "-") != 0;
        ucxapp_print("%s%s%s  [%s]\n", c->name, hasVersion ? " " : "", hasVersion ? c->version : "", c->license);
        ucxapp_print("  %s\n  %s\n\n", c->use, c->source);
    }
    return UCXAPP_OK;
}

static int cmdLicense(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < COMPONENT_COUNT; i++) {
        const component_t *c = &kComponents[i];
        if (argc == 1) {
            ucxapp_print("%-31s %s\n", c->name, c->license);
        } else if (strcmp(argv[1], c->name) == 0) {
            ucxapp_print("%s: %s\n%s", c->name, c->license, c->notice != NULL ? c->notice : "");
            return UCXAPP_OK;
        }
    }
    if (argc == 2) {
        ucxapp_print("ERROR: no component %s; see app.license\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("app.license <name> prints the full notice\n");
    return UCXAPP_OK;
}

static int cmdInfo(ucxapp_t *app, int argc, char *argv[])
{
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("u-connectXpressApp %s by u-blox AG\n", APP_VERSION);
    ucxapp_print("Command shell for u-connectXpress modules (NORA-W36, NORA-B26)\n");
    ucxapp_print("platform  %s %s\n", APP_PLATFORM, APP_ARCH);
    ucxapp_print("module    %s\n", app->ucx != NULL ? "connected" : "not connected");
    ucxapp_print("home      https://github.com/u-blox/u-connectXpressApp\n");
    ucxapp_print("releases  https://github.com/u-blox/u-connectXpressApp/releases\n");
    ucxapp_print("firmware  https://github.com/u-blox/u-connectXpress/releases\n");
    ucxapp_print("web       https://www.u-blox.com\n");
    return UCXAPP_OK;
}

/* Same row look as dev.stack: app RAM/heap/flash (disk on hosts), then task stacks on the RTOS. */
static int cmdMemory(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_mem_t rows[16];
    int n;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    n = ucxapp_platform_memory(rows, 16);
    for (int i = 0; i < n; i++) {
        ucxapp_print_usage_row("Memory", rows[i].name, "size", rows[i].size, rows[i].used);
    }
    n = ucxapp_platform_tasks(rows, 16);
    for (int i = 0; i < n; i++) {
        ucxapp_print_usage_row("Task", rows[i].name, "stack", rows[i].size, rows[i].used);
    }
    return UCXAPP_OK;
}

/* Same completion as Tab in the TUI; scriptable and testable. */
static int cmdComplete(ucxapp_t *app, int argc, char *argv[])
{
    char common[64];
    int count;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = ucxapp_cmd_complete(argv[1], common, sizeof(common));
    ucxapp_print("%d match%s: %s\n", count, count == 1 ? "" : "es", count > 0 ? common : argv[1]);
    return count > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gAppCmds[] = {
    { "app.complete", "<prefix>", "Show what Tab would complete a typed start of a command to (for scripts and tests)", 0, cmdComplete },
    { "app.info", "", "About this app: maker, platform and links", 0, cmdInfo },
    { "app.memory", "", "App RAM, heap and flash (disk on PC) use; task stacks on STM32. Same look as dev.stack", 0, cmdMemory },
    { "app.version", "", "App version, build time, platform and compiler", 0, cmdVersion },
    { "app.uptime", "", "Time since the app started (days hh:mm:ss)", 0, cmdUptime },
    { "app.sbom", "", "Software bill of materials: components, versions and licences", 0, cmdSbom },
    { "app.license", "[name]", "Licence of each component; with a name, the full notice", 0, cmdLicense },
};

void ucxapp_register_app_cmds(void)
{
    gStartMs = ucxapp_platform_tick_ms();
    gLastMs = gStartMs;
    ucxapp_cmd_register(gAppCmds, sizeof(gAppCmds) / sizeof(gAppCmds[0]));
}
