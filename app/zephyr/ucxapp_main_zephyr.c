/* Zephyr entry: command shell on the zephyr,console UART, u-connectXpress module on the UART with the
 * devicetree alias "ucxapp-module" (USART1 on NUCLEO-H753ZI, see boards/<board>.overlay). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include "ucxapp_cmd.h"
#include "ucxapp_store.h"
#include "ucxapp_zephyr.h"
#include "u_cx_system.h"
#include "u_port.h"

#define MODULE_NODE DT_ALIAS(ucxapp_module)
#if !DT_NODE_HAS_STATUS(MODULE_NODE, okay)
#error "The board overlay must enable the module UART and name it with the ucxapp-module alias"
#endif

#define MODULE_UART DEVICE_DT_NAME(MODULE_NODE)
#define MODULE_BAUD DT_PROP_OR(MODULE_NODE, current_speed, 115200)

#ifndef UCXAPP_RX_BUFFER_SIZE
#define UCXAPP_RX_BUFFER_SIZE 8192
#endif
#ifndef UCXAPP_URC_BUFFER_SIZE
#define UCXAPP_URC_BUFFER_SIZE 4096
#endif
/* Holds a YMODEM 1 KB packet with room to spare. */
#define CONSOLE_RX_SIZE 2048

/* ---- console I/O ---------------------------------------------------------- */

static const struct device *const gConsole = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
RING_BUF_DECLARE(gConsoleRx, CONSOLE_RX_SIZE);
static K_SEM_DEFINE(gConsoleSem, 0, 1);

static void consoleIsr(const struct device *dev, void *userData)
{
    uint8_t buf[32];

    (void)userData;
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        ring_buf_put(&gConsoleRx, buf, (uint32_t)n);
        k_sem_give(&gConsoleSem);
    }
}

static bool consoleInit(void)
{
    if (!device_is_ready(gConsole) || uart_irq_callback_user_data_set(gConsole, consoleIsr, NULL) != 0) {
        return false;
    }
    uart_irq_rx_enable(gConsole);
    return true;
}

static uint32_t consoleReadMany(uint8_t *buf, uint32_t len)
{
    uint32_t n;
    unsigned int key = irq_lock();

    n = ring_buf_get(&gConsoleRx, buf, len);
    irq_unlock(key);
    return n;
}

int32_t ucxapp_zephyr_console_read(uint8_t *byte, int32_t timeoutMs)
{
    int64_t end = k_uptime_get() + (timeoutMs > 0 ? timeoutMs : 0);

    for (;;) {
        if (consoleReadMany(byte, 1) == 1) {
            return 1;
        }
        int64_t left = end - k_uptime_get();
        if (left <= 0) {
            return 0;
        }
        k_sem_take(&gConsoleSem, K_MSEC(left));
    }
}

static void consolePutRaw(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(gConsole, data[i]);
    }
}

static void consoleWrite(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            uart_poll_out(gConsole, '\r');
        }
        uart_poll_out(gConsole, (unsigned char)text[i]);
    }
}

static int consoleReadline(char *buf, size_t size)
{
    size_t len = 0;
    uint8_t c;
    bool lastWasCr = false;

    for (;;) {
        if (ucxapp_zephyr_console_read(&c, 1000) == 0) {
            continue;
        }
        if (c == '\n' && lastWasCr) {
            lastWasCr = false;
            continue;
        }
        lastWasCr = (c == '\r');
        if (c == '\r' || c == '\n') {
            consolePutRaw((const uint8_t *)"\r\n", 2);
            buf[len] = '\0';
            return (int)len;
        }
        if ((c == 0x08 || c == 0x7f) && len > 0) {
            len--;
            consolePutRaw((const uint8_t *)"\b \b", 3);
        } else if (c >= 0x20 && c < 0x7f && len + 1 < size) {
            buf[len++] = (char)c;
            consolePutRaw(&c, 1);
        }
    }
}

static int consoleGetByte(int timeoutMs)
{
    uint8_t c;

    return ucxapp_zephyr_console_read(&c, timeoutMs) == 1 ? c : -1;
}

static const ucxapp_io_t gConsoleIo = { consoleWrite, consoleReadline, consoleGetByte, consolePutRaw };

/* ---- module connection ---------------------------------------------------- */

static uCxAtClient_t gAtClient;
static uCxAtClientConfig_t gAtConfig;
static uCxHandle_t gUcxHandle;
static char gRxBuffer[UCXAPP_RX_BUFFER_SIZE];
static char gUrcBuffer[UCXAPP_URC_BUFFER_SIZE];
static char gPortName[32];
static bool gConnected;
static int32_t gModuleBaud = MODULE_BAUD;

static bool connectModule(const char *port)
{
    /* The AT client keeps the name pointer, so it must outlive the command line. */
    snprintf(gPortName, sizeof(gPortName), "%s", port);
    uPortInit();
    memset(&gAtConfig, 0, sizeof(gAtConfig));
    gAtConfig.pRxBuffer = gRxBuffer;
    gAtConfig.rxBufferLen = sizeof(gRxBuffer);
    gAtConfig.pUrcBuffer = gUrcBuffer;
    gAtConfig.urcBufferLen = sizeof(gUrcBuffer);
    gAtConfig.pUartDevName = gPortName;

    uCxAtClientInit(&gAtConfig, &gAtClient);
    int32_t result = uCxAtClientOpen(&gAtClient, gModuleBaud, false);
    if (result != 0) {
        ucxapp_print("ERROR: cannot open %s (%d)\n", gPortName, (int)result);
        uCxAtClientDeinit(&gAtClient);
        uPortDeinit();
        return false;
    }
    uCxInit(&gAtClient, &gUcxHandle);
    ucxapp_events_init();
    ucxapp_urc_register(&gUcxHandle);
    if (uCxSystemSetEchoOff(&gUcxHandle) != 0) {
        ucxapp_print("warning: could not disable echo (is the module on %s at %d baud?)\n", gPortName,
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
    if (!connectModule(argc >= 2 ? argv[1] : MODULE_UART)) {
        return UCXAPP_ERR_FAILED;
    }
    app->ucx = &gUcxHandle;
    ucxapp_print("connected to %s\n", gPortName);
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

static void moduleWriteAll(uPortUartHandle_t uart, const uint8_t *data, size_t len)
{
    while (len > 0) {
        int32_t n = uPortUartWrite(uart, data, len);
        if (n < 0) {
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

/* Ctrl-] exits only when surrounded by BRIDGE_GUARD_MS of console silence, so binary transfers pass through. */
static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    int32_t baud = gModuleBaud;
    uint8_t buf[256];
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
    uPortUartHandle_t uart = uPortUartOpen(MODULE_UART, baud, false);
    if (uart == NULL) {
        uPortDeinit();
        ucxapp_print("ERROR: cannot open %s\n", MODULE_UART);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("bridge: console <-> module (%s, %d baud). Ctrl-] with 1 s silence around it exits.\n",
                 MODULE_UART, (int)baud);
    lastConsoleMs = k_uptime_get_32();
    for (;;) {
        uint32_t now = k_uptime_get_32();
        int32_t n = uPortUartRead(uart, buf, sizeof(buf), 0);
        uint32_t got;

        if (n > 0) {
            consolePutRaw(buf, (size_t)n);
        }
        if (exitPending && now - exitKeyMs >= BRIDGE_GUARD_MS) {
            break;
        }
        got = consoleReadMany(buf, sizeof(buf));
        if (got > 0) {
            if (exitPending) {
                uint8_t key = BRIDGE_EXIT_KEY;
                exitPending = false;
                moduleWriteAll(uart, &key, 1);
            }
            if (got == 1 && buf[0] == BRIDGE_EXIT_KEY && now - lastConsoleMs >= BRIDGE_GUARD_MS) {
                exitPending = true;
                exitKeyMs = now;
            } else {
                moduleWriteAll(uart, buf, got);
            }
            lastConsoleMs = now;
        } else if (n <= 0) {
            /* Idle: wake on module data or after 1 ms to poll the console again. */
            uPortUartWaitForData(uart, 1);
        }
    }
    uPortUartClose(uart);
    uPortDeinit();
    ucxapp_print("\nbridge closed\n");
    if (connectModule(MODULE_UART)) {
        app->ucx = &gUcxHandle;
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gConnectCmds[] = {
    { "bridge",            "[baud]", "Transparent console<->module bridge, Ctrl-] exits", 0, cmdBridge },
    { "dev.connect",       "[uart] [baud]", "Connect to the module (default " MODULE_UART ")", 0, cmdDeviceConnect },
    { "dev.disconnect",    "",       "Disconnect from the module",                         0, cmdDeviceDisconnect },
};

int main(void)
{
    ucxapp_t app = { NULL };
    char line[] = "dev.connect";

    if (!consoleInit()) {
        printf("ERROR: console UART not ready\n");
        return 0;
    }
    ucxapp_io_set(&gConsoleIo);
    ucxapp_print("\nu-connectXpressApp (Zephyr, %s), module on %s at %d baud\n", CONFIG_BOARD, MODULE_UART,
                 (int)gModuleBaud);
    ucxapp_store_set_backend(&ucxapp_store_ram);
    ucxapp_print("files: RAM only (lost at reset)\n");

    ucxapp_cmd_register(gConnectCmds, sizeof(gConnectCmds) / sizeof(gConnectCmds[0]));
    ucxapp_register_device_cmds();
    ucxapp_register_wifi_cmds();
    ucxapp_register_sock_cmds();
    ucxapp_register_sys_cmds();
    ucxapp_register_app_cmds();
    ucxapp_register_bt_cmds();
    ucxapp_register_gatt_cmds();
    ucxapp_register_mqtt_cmds();
    ucxapp_register_net_cmds();
    ucxapp_register_time_cmds();
    ucxapp_register_cert_cmds();
    ucxapp_register_ymodem_cmds();
    ucxapp_register_examples_cmds();

    if (ucxapp_cmd_exec(&app, line) != UCXAPP_OK) {
        ucxapp_print("continuing without a device\n");
    }
    for (;;) {
        ucxapp_shell_run(&app);   /* 'exit' only re-prompts; there is nowhere to return to */
    }
    return 0;
}
