#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef UCXAPP_CRYPTO
#include "mbedtls/build_info.h"
#include "tf-psa-crypto/build_info.h"
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/version.h>
#endif

#define STR2(x) #x
#define STR(x)  STR2(x)

#ifdef APP_VERSION_MAJOR
#define APP_VERSION STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR) "." STR(APP_VERSION_PATCH) "." STR(APP_VERSION_BUILD)
#define UCX_API_VERSION STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR) "." STR(APP_VERSION_PATCH)
#else
#define APP_VERSION "dev"
#define UCX_API_VERSION "-"
#endif
#ifdef UCXAPP_UCC_COMMIT
#define UCC_VERSION UCX_API_VERSION " (commit " UCXAPP_UCC_COMMIT ")"
#else
#define UCC_VERSION UCX_API_VERSION
#endif
#ifndef UCXAPP_UCX_MODULE
#define UCXAPP_UCX_MODULE "MERGED"
#endif

#if defined(_WIN32)
#define APP_PLATFORM "Windows"
#elif defined(__EMSCRIPTEN__)
#define APP_PLATFORM "WebAssembly"
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

#if !HOSTED && defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#elif !HOSTED
#include "FreeRTOS.h"
#include "task.h"
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
    { "u-connectClient", UCC_VERSION, "Apache-2.0", "AT command client for u-connectXpress modules",
      "https://github.com/u-blox/u-connectClient", kApache },
#ifdef UCXAPP_CRYPTO
    { "mbedTLS", MBEDTLS_VERSION_STRING, "Apache-2.0", "TLS and X.509 (tls.*, cert.import, utils.cert.*, web.start https)",
      "https://github.com/Mbed-TLS/mbedtls", kApache },
    { "TF-PSA-Crypto", TF_PSA_CRYPTO_VERSION_STRING, "Apache-2.0",
      "PSA crypto: AES-256-GCM, PBKDF2, hashes (utils.encrypt, Wi-Fi passwords)",
      "https://github.com/Mbed-TLS/TF-PSA-Crypto", kApache },
#endif
#ifdef __EMSCRIPTEN__
#ifdef __EMSCRIPTEN_MAJOR__
    { "Emscripten", STR(__EMSCRIPTEN_MAJOR__) "." STR(__EMSCRIPTEN_MINOR__) "." STR(__EMSCRIPTEN_TINY__), "MIT",
#else
    { "Emscripten", STR(__EMSCRIPTEN_major__) "." STR(__EMSCRIPTEN_minor__) "." STR(__EMSCRIPTEN_tiny__), "MIT",
#endif
      "C runtime and JavaScript glue for the browser build", "https://emscripten.org", kMit },
#endif
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
    { "FreeRTOS", tskKERNEL_VERSION_NUMBER, "MIT", "kernel", "https://www.freertos.org", kMit },
#if defined(STM32H753xx) || defined(STM32F767xx) || defined(STM32F439xx) || defined(STM32F407xx)
    { "littlefs", "-", "BSD-3-Clause", "file store in the internal flash", "https://github.com/littlefs-project/littlefs",
      NULL },
#endif
#if defined(STM32H753xx) || defined(STM32F767xx) || defined(STM32F439xx) || defined(STM32F407xx) || \
    defined(STM32F401xE) || defined(STM32F446xx)
    { "FatFs", "-", "FatFs licence (BSD-1-Clause)", "FAT file system on an SD card (STM32Cube middleware)",
      "http://elm-chan.org/fsw/ff/", NULL },
#endif
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
    ucxapp_print("build     u-connectXpressApp %s, %s %s, %s\n", APP_VERSION, APP_PLATFORM, APP_ARCH, APP_COMPILER);
    ucxapp_print("AT API    %s %s\n\n",
                 strcmp(UCXAPP_UCX_MODULE, "MERGED") == 0 ? "NORA-W36X + NORA-B26X" : UCXAPP_UCX_MODULE,
                 UCX_API_VERSION);
    for (size_t i = 0; i < COMPONENT_COUNT; i++) {
        const component_t *c = &kComponents[i];
        bool hasVersion = strcmp(c->version, "-") != 0;
#if !HOSTED && defined(__ZEPHYR__)
        char zv[16];

        if (strcmp(c->name, "Zephyr RTOS") == 0) {
            uint32_t v = sys_kernel_version_get();

            snprintf(zv, sizeof(zv), "%u.%u.%u", (unsigned)SYS_KERNEL_VER_MAJOR(v), (unsigned)SYS_KERNEL_VER_MINOR(v),
                     (unsigned)SYS_KERNEL_VER_PATCHLEVEL(v));
            ucxapp_print("%s %s  [%s]\n", c->name, zv, c->license);
            ucxapp_print("  %s\n  %s\n\n", c->use, c->source);
            continue;
        }
#endif
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

/* Same completion as Tab in the TUI and the web page; scriptable and testable. */
static int cmdComplete(ucxapp_t *app, int argc, char *argv[])
{
    char line[200], out[200], list[512];
    size_t n = 0;
    int count;

    (void)app;
    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    line[0] = '\0';
    for (int i = 1; i < argc && n < sizeof(line); i++) {
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    count = ucxapp_cmd_complete_line(line, out, sizeof(out), list, sizeof(list));
    ucxapp_print("%d match%s: %s\n", count, count == 1 ? "" : "es", out);
    if (count > 1) {
        ucxapp_print("  %s\n", list);
    }
    return count > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static const ucxapp_cmd_t gAppCmds[] = {
    { "app.complete", "<text...>", "What Tab completes a typed line to: the command, then its keywords (quote a trailing space)", 0, cmdComplete },
    { "app.info", "", "About this app: maker, platform and links", 0, cmdInfo },
    { "app.memory", "", "App RAM, heap and flash (disk on a PC); task stacks on STM32, in the dev.stack layout", 0, cmdMemory },
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
