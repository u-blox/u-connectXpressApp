/* Bare-metal entry for the STM32 build: command shell on the ST-Link console (USART3),
 * u-connectXpress module on the UART named by U_EXAMPLE_UART (USART1 on NUCLEO-H753ZI). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ucxapp_cmd.h"
#include "ucxapp_flashfs.h"
#include "ucxapp_sdcard.h"
#include "ucxapp_store.h"
#include "u_cx_system.h"
#include "u_port.h"

#ifndef U_EXAMPLE_UART
#define U_EXAMPLE_UART "USART1"
#endif
#ifndef UCXAPP_MODULE_BAUD
#define UCXAPP_MODULE_BAUD 115200
#endif
#ifndef UCXAPP_STACK_WORDS
#define UCXAPP_STACK_WORDS 6144
#endif
/* UCXAPP_LITE (NUCLEO-F103RB, 20 KB RAM): no net, MQTT or cert packs and small AT buffers. */
#ifndef UCXAPP_RX_BUFFER_SIZE
#define UCXAPP_RX_BUFFER_SIZE 8192
#endif
#ifndef UCXAPP_URC_BUFFER_SIZE
#define UCXAPP_URC_BUFFER_SIZE 4096
#endif

/* Provided by main_stm32.c (ST-Link VCP, interrupt-fed RX ring). */
extern int32_t exampleConsoleUartRead(uint8_t *pByte, int32_t timeoutMs);
extern int32_t exampleConsoleUartWrite(const void *pData, size_t length);
extern int app_main(int argc, char *argv[]);

/* ---- console I/O ---------------------------------------------------------- */

static void consoleWrite(const char *text, size_t len)
{
    size_t start = 0;

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            exampleConsoleUartWrite(text + start, i - start);
            exampleConsoleUartWrite("\r\n", 2);
            start = i + 1;
        }
    }
    if (start < len) {
        exampleConsoleUartWrite(text + start, len - start);
    }
}

static int consoleReadline(char *buf, size_t size)
{
    size_t len = 0;
    uint8_t c;
    bool lastWasCr = false;

    for (;;) {
        if (exampleConsoleUartRead(&c, 0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (c == '\n' && lastWasCr) {
            lastWasCr = false;
            continue;
        }
        lastWasCr = (c == '\r');
        if (c == '\r' || c == '\n') {
            exampleConsoleUartWrite("\r\n", 2);
            buf[len] = '\0';
            return (int)len;
        }
        if ((c == 0x08 || c == 0x7f) && len > 0) {
            len--;
            exampleConsoleUartWrite("\b \b", 3);
        } else if (c >= 0x20 && c < 0x7f && len + 1 < size) {
            buf[len++] = (char)c;
            exampleConsoleUartWrite(&c, 1);
        }
    }
}

static int consoleGetByte(int timeoutMs)
{
    uint8_t c;
    TickType_t start = xTaskGetTickCount();

    for (;;) {
        if (exampleConsoleUartRead(&c, 0) == 1) {
            return c;
        }
        if ((int)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS) >= timeoutMs) {
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void consolePutRaw(const uint8_t *data, size_t len)
{
    exampleConsoleUartWrite(data, len);
}

static const ucxapp_io_t gConsoleIo = { consoleWrite, consoleReadline, consoleGetByte, consolePutRaw };

/* ---- RAM-backed store (profiles are lost at reset) ------------------------ */

#define RAM_FILES     8
#define RAM_NAME_LEN  40

typedef struct {
    char name[RAM_NAME_LEN];
    uint8_t *data;
    size_t len;
    bool used;
} ramFile_t;

typedef struct {
    ramFile_t *file;
    size_t pos;
} ramHandle_t;

static ramFile_t gRamFiles[RAM_FILES];

static ramFile_t *ramFind(const char *name)
{
    for (int i = 0; i < RAM_FILES; i++) {
        if (gRamFiles[i].used && strcmp(gRamFiles[i].name, name) == 0) {
            return &gRamFiles[i];
        }
    }
    return NULL;
}

static void ramFree(ramFile_t *f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

static void *ramOpen(const char *name, bool forWrite)
{
    ramFile_t *f = ramFind(name);
    ramHandle_t *h;

    if (strlen(name) >= RAM_NAME_LEN) {
        return NULL;
    }
    if (forWrite) {
        if (f != NULL) {
            free(f->data);
            f->data = NULL;
            f->len = 0;
        } else {
            for (int i = 0; i < RAM_FILES && f == NULL; i++) {
                f = gRamFiles[i].used ? NULL : &gRamFiles[i];
            }
            if (f == NULL) {
                return NULL;
            }
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->used = true;
        }
    } else if (f == NULL) {
        return NULL;
    }
    h = (ramHandle_t *)calloc(1, sizeof(*h));
    if (h != NULL) {
        h->file = f;
    }
    return h;
}

static int32_t ramRead(void *handle, uint8_t *buf, size_t len)
{
    ramHandle_t *h = (ramHandle_t *)handle;
    size_t n = h->file->len - h->pos;

    if (n > len) {
        n = len;
    }
    memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (int32_t)n;
}

static int32_t ramWrite(void *handle, const uint8_t *data, size_t len)
{
    ramHandle_t *h = (ramHandle_t *)handle;
    uint8_t *grown = (uint8_t *)realloc(h->file->data, h->file->len + len + 1);

    if (grown == NULL) {
        return -1;
    }
    memcpy(grown + h->file->len, data, len);
    h->file->len += len;
    grown[h->file->len] = '\0';
    h->file->data = grown;
    return (int32_t)len;
}

static void ramClose(void *handle)
{
    free(handle);
}

static bool ramRemove(const char *name)
{
    ramFile_t *f = ramFind(name);

    if (f == NULL) {
        return false;
    }
    ramFree(f);
    return true;
}

static bool ramRename(const char *from, const char *to)
{
    ramFile_t *src = ramFind(from);
    ramFile_t *dst = ramFind(to);

    if (src == NULL || strlen(to) >= RAM_NAME_LEN) {
        return false;
    }
    if (dst != NULL && dst != src) {
        ramFree(dst);
    }
    snprintf(src->name, sizeof(src->name), "%s", to);
    return true;
}

static int ramList(ucxapp_store_list_cb_t cb, void *ctx)
{
    int count = 0;

    for (int i = 0; i < RAM_FILES; i++) {
        if (gRamFiles[i].used) {
            cb(gRamFiles[i].name, gRamFiles[i].len, ctx);
            count++;
        }
    }
    return count;
}

static const ucxapp_store_ops_t gRamStore = { ramOpen, ramRead, ramWrite, ramClose, ramRemove, ramRename, ramList };

/* ---- module connection ---------------------------------------------------- */

static uCxAtClient_t gAtClient;
static uCxAtClientConfig_t gAtConfig;
static uCxHandle_t gUcxHandle;
static char gRxBuffer[UCXAPP_RX_BUFFER_SIZE];
static char gUrcBuffer[UCXAPP_URC_BUFFER_SIZE];
static bool gConnected;
static int32_t gModuleBaud = UCXAPP_MODULE_BAUD;

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
    int32_t result = uCxAtClientOpen(&gAtClient, gModuleBaud, false);
    if (result != 0) {
        ucxapp_print("ERROR: cannot open %s (%d)\n", port, (int)result);
        uCxAtClientDeinit(&gAtClient);
        uPortDeinit();
        return false;
    }
    uCxInit(&gAtClient, &gUcxHandle);
    ucxapp_events_init();
    ucxapp_urc_register(&gUcxHandle);
    if (uCxSystemSetEchoOff(&gUcxHandle) != 0) {
        ucxapp_print("warning: could not disable echo (is the module on %s at %d baud?)\n", port,
                     (int)gModuleBaud);
    }
    if (uCxSystemSetExtendedError(&gUcxHandle, U_SYS_EXTENDED_ERRORS_ON) != 0) {
        ucxapp_print("warning: could not enable extended error codes\n");
    }
    ucxapp_adopt_connections(&gUcxHandle);
    ucxapp_udp_reset_limit();
    gConnected = true;
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
    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3) {
        gModuleBaud = atoi(argv[2]);
    }
    if (gConnected) {
        ucxapp_print("already connected\n");
        return UCXAPP_OK;
    }
    if (!connectModule(argc == 2 ? argv[1] : U_EXAMPLE_UART)) {
        return UCXAPP_ERR_FAILED;
    }
    app->ucx = &gUcxHandle;
    ucxapp_print("connected to %s\n", argc == 2 ? argv[1] : U_EXAMPLE_UART);
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

#define BRIDGE_EXIT_KEY  0x1D   /* Ctrl-] */
#define BRIDGE_GUARD_MS  1000

/* Ctrl-] exits only when surrounded by BRIDGE_GUARD_MS of console silence, so binary transfers pass through. */
static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = gModuleBaud;
    uint8_t b;
    uint32_t lastConsoleMs;
    uint32_t exitKeyMs = 0;
    bool exitPending = false;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        baud = atoi(argv[1]);
    }
    app->ucx = NULL;
    disconnectModule();
    uPortInit();
    uPortUartHandle_t uart = uPortUartOpen(U_EXAMPLE_UART, baud, false);
    if (uart == NULL) {
        uPortDeinit();
        ucxapp_print("ERROR: cannot open %s\n", U_EXAMPLE_UART);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("bridge: console <-> module (%s, %d baud). Ctrl-] with 1 s silence around it exits.\n",
                 U_EXAMPLE_UART, (int)baud);
    lastConsoleMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (uPortUartRead(uart, &b, 1, 0) == 1) {
            exampleConsoleUartWrite(&b, 1);
        }
        if (exitPending && now - exitKeyMs >= BRIDGE_GUARD_MS) {
            break;
        }
        if (exampleConsoleUartRead(&b, 0) == 1) {
            if (exitPending) {
                uint8_t key = BRIDGE_EXIT_KEY;
                exitPending = false;
                uPortUartWrite(uart, &key, 1);
            }
            if (b == BRIDGE_EXIT_KEY && now - lastConsoleMs >= BRIDGE_GUARD_MS) {
                exitPending = true;
                exitKeyMs = now;
            } else {
                uPortUartWrite(uart, &b, 1);
            }
            lastConsoleMs = now;
        } else if (!exitPending) {
            taskYIELD();
        }
    }
    uPortUartClose(uart);
    uPortDeinit();
    ucxapp_print("\nbridge closed\n");
    if (connectModule(U_EXAMPLE_UART)) {
        app->ucx = &gUcxHandle;
    }
    return UCXAPP_OK;
}

typedef enum { STORE_RAM, STORE_FLASH, STORE_SD } storeKind_t;

static storeKind_t gStoreKind = STORE_RAM;

/* SD card first, then the flash area, then RAM. */
static void mountStore(void)
{
    const ucxapp_store_ops_t *ops;
    uint32_t size = 0;

    if ((ops = ucxapp_sdcard_mount(&size)) != NULL) {
        gStoreKind = STORE_SD;
        ucxapp_print("files: SD card, %u MB\n", (unsigned)size);
    } else if ((ops = ucxapp_flashfs_mount(&size)) != NULL) {
        gStoreKind = STORE_FLASH;
        ucxapp_print("files: %u KB in flash\n", (unsigned)size);
    } else {
        ops = &gRamStore;
        gStoreKind = STORE_RAM;
        ucxapp_print("files: RAM only (lost at reset)\n");
    }
    ucxapp_store_set_backend(ops);
}

static int cmdFileMount(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    mountStore();
    return UCXAPP_OK;
}

static int cmdFileFormat(ucxapp_t *app, int argc, char *argv[])
{
    bool ok;

    (void)app;
    if (argc != 2 || (strcmp(argv[1], "yes") != 0 && strcmp(argv[1], "sd") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "sd") == 0 || gStoreKind == STORE_SD) {
        ok = ucxapp_sdcard_format();
        if (ok) {
            mountStore();
        }
    } else {
        ok = ucxapp_flashfs_format();
    }
    if (!ok) {
        ucxapp_print("format failed (no SD card or flash file area)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("all files erased\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gConnectCmds[] = {
    { "bridge",            "[baud]", "Transparent console<->module bridge, Ctrl-] exits", 0, cmdBridge },
    { "device.connect",    "[uart] [baud]", "Connect to the module (default " U_EXAMPLE_UART ")", 0, cmdDeviceConnect },
    { "device.disconnect", "",       "Disconnect from the module",                         0, cmdDeviceDisconnect },
    { "file.format",       "yes|sd", "Erase all files (sd: create a FAT volume on the card)", 0, cmdFileFormat },
    { "file.mount",        "",       "Use the SD card if inserted, else flash or RAM",     0, cmdFileMount },
};

static void appTask(void *arg)
{
    ucxapp_t app = { NULL };
    char line[] = "device.connect";

    (void)arg;
    ucxapp_io_set(&gConsoleIo);
    ucxapp_print("\nu-connectXpressApp (STM32), module on %s at %d baud\n", U_EXAMPLE_UART, (int)UCXAPP_MODULE_BAUD);
    mountStore();

    ucxapp_cmd_register(gConnectCmds, sizeof(gConnectCmds) / sizeof(gConnectCmds[0]));
    ucxapp_register_device_cmds();
    ucxapp_register_wifi_cmds();
    ucxapp_register_sock_cmds();
    ucxapp_register_sys_cmds();
    ucxapp_register_app_cmds();
    ucxapp_register_bt_cmds();
    ucxapp_register_gatt_cmds();
#ifndef UCXAPP_LITE
    ucxapp_register_mqtt_cmds();
    ucxapp_register_net_cmds();
    ucxapp_register_time_cmds();
    ucxapp_register_cert_cmds();
    ucxapp_register_ymodem_cmds();
    ucxapp_register_examples_cmds();
#endif

    if (ucxapp_cmd_exec(&app, line) != UCXAPP_OK) {
        ucxapp_print("continuing without a device\n");
    }
    for (;;) {
        ucxapp_shell_run(&app);   /* 'exit' only re-prompts; there is nowhere to return to */
    }
}

int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* The caller's task stack is too small for the command layer. */
    if (xTaskCreate(appTask, "ucxapp", UCXAPP_STACK_WORDS, NULL, 4, NULL) != pdPASS) {
        printf("ERROR: cannot create the application task\r\n");
    }
    vTaskDelete(NULL);
    return 0;
}
