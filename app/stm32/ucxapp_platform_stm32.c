/* Platform backend for bare-metal STM32: no OS shell, no network of its own. */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ucxapp_platform.h"

#include <sys/time.h>

#include "ucxapp_cmd.h"

extern int32_t exampleConsoleUartRead(uint8_t *pByte, int32_t timeoutMs);

static int gPendingKey = -1;

/* Features not built for STM32. */
void ucxapp_web_on_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
void ucxapp_web_on_data(int32_t sock, int32_t bytes) { (void)sock; (void)bytes; }
void ucxapp_web_on_closed(int32_t sock) { (void)sock; }
void ucxapp_stream_on_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
void ucxapp_stream_on_closed(int32_t sock) { (void)sock; }
void ucxapp_stream_on_data(int32_t sock) { (void)sock; }
void ucxapp_stream_on_sps_data(void) { }
void ucxapp_stream_on_ble_rx(const uint8_t *data, size_t len) { (void)data; (void)len; }
size_t ucxapp_gnss_summary(char *out, size_t size) { (void)size; out[0] = '\0'; return 0; }
size_t ucxapp_imu_summary(char *out, size_t size) { (void)size; out[0] = '\0'; return 0; }
void ucxapp_ui_event(const char *text) { (void)text; }
void ucxapp_ui_wifi_set(bool up) { (void)up; }
void ucxapp_ui_prompt(const char *text, const char *prefill, const char *cancel) { (void)text; (void)prefill; (void)cancel; }
bool ucxapp_hid_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_hid_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ft_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ft_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_ancs_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
bool ucxapp_notify_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    (void)conn; (void)handle; (void)data; (void)len;
    return false;
}
void ucxapp_ft_on_sock_data(int32_t sock) { (void)sock; }
void ucxapp_ft_on_sock_closed(int32_t sock) { (void)sock; }
void ucxapp_ft_on_sock_incoming(int32_t sock, int32_t listenSock) { (void)sock; (void)listenSock; }
bool ucxapp_qr_print_wifi(const char *ssid, const char *password, const char *security, bool hidden)
{
    (void)ssid; (void)password; (void)security; (void)hidden;
    return false;
}

/* newlib syscall used by time(); seconds since boot plus the offset set by app.time. */
static volatile int64_t gEpochOffset;

bool ucxapp_platform_clock_adjust(int64_t deltaSeconds)
{
    gEpochOffset += deltaSeconds;
    return true;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv != NULL) {
        uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        tv->tv_sec = (time_t)(ms / 1000 + gEpochOffset);
        tv->tv_usec = (ms % 1000) * 1000;
    }
    return 0;
}

void ucxapp_platform_init(void)
{
}

/* Only the board's own family CMSIS header is on the include path. */
#if __has_include("stm32h7xx.h")
#include "stm32h7xx.h"
#elif __has_include("stm32f7xx.h")
#include "stm32f7xx.h"
#elif __has_include("stm32f4xx.h")
#include "stm32f4xx.h"
#elif __has_include("stm32f1xx.h")
#include "stm32f1xx.h"
#elif __has_include("stm32g0xx.h")
#include "stm32g0xx.h"
#endif

extern uint8_t _sidata[], _sdata[], _edata[], _ebss[], _estack[];

static int memRow(ucxapp_mem_t *rows, int n, int max, const char *name, uint64_t size, uint64_t used)
{
    if (n < max && size > 0) {
        strncpy(rows[n].name, name, sizeof(rows[n].name) - 1);
        rows[n].name[sizeof(rows[n].name) - 1] = '\0';
        rows[n].size = size;
        rows[n].used = used;
        n++;
    }
    return n;
}

int ucxapp_platform_memory(ucxapp_mem_t *areas, int max)
{
    uint64_t heap = configTOTAL_HEAP_SIZE;
    uint64_t image = (uint64_t)((uintptr_t)_sidata + (uintptr_t)(_edata - _sdata) - FLASH_BASE);
    int n = 0;

    n = memRow(areas, n, max, "RAM static", (uint64_t)(_estack - _sdata), (uint64_t)(_ebss - _sdata));
    n = memRow(areas, n, max, "Heap", heap, heap - xPortGetFreeHeapSize());
    n = memRow(areas, n, max, "Heap peak", heap, heap - xPortGetMinimumEverFreeHeapSize());
#ifdef FLASHSIZE_BASE
    n = memRow(areas, n, max, "Flash", (uint64_t)(*(const uint16_t *)FLASHSIZE_BASE) * 1024u, image);
#else
    n = memRow(areas, n, max, "Flash image", image, image);
#endif
    return n;
}

int ucxapp_platform_tasks(ucxapp_mem_t *tasks, int max)
{
#if configUSE_TRACE_FACILITY == 1
    TaskStatus_t status[16];
    UBaseType_t count = uxTaskGetSystemState(status, 16, NULL);
    int n = 0;

    for (UBaseType_t i = 0; i < count && n < max; i++) {
        /* heap_4 block header in front of the stack: size with the top bit as "allocated" flag. */
        size_t block = ((const size_t *)status[i].pxStackBase)[-1] & ~((size_t)1 << (sizeof(size_t) * 8 - 1));
        uint64_t free = (uint64_t)status[i].usStackHighWaterMark * sizeof(StackType_t);
        uint64_t size = (block > 8 && block <= configTOTAL_HEAP_SIZE) ? block - 8 : 0;
        strncpy(tasks[n].name, status[i].pcTaskName, sizeof(tasks[n].name) - 1);
        tasks[n].name[sizeof(tasks[n].name) - 1] = '\0';
        tasks[n].size = size;
        tasks[n].used = size > free ? size - free : 0;
        n++;
    }
    return n;
#else
    (void)tasks;
    (void)max;
    return 0;
#endif
}

bool ucxapp_platform_local_ip(char *out, size_t size)
{
    (void)out;
    (void)size;
    return false;
}

uint32_t ucxapp_platform_tick_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ucxapp_platform_sleep_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);

    vTaskDelay(ticks > 0 ? ticks : 1);
}

bool ucxapp_platform_key_pressed(void)
{
    uint8_t c;

    if (gPendingKey < 0 && exampleConsoleUartRead(&c, 0) == 1) {
        gPendingKey = c;
    }
    return gPendingKey >= 0;
}

int ucxapp_platform_get_key(void)
{
    int key;

    while (!ucxapp_platform_key_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    key = gPendingKey;
    gPendingKey = -1;
    return key;
}

void ucxapp_platform_exe_dir(char *buffer, size_t bufferSize)
{
    if (bufferSize > 0) {
        buffer[0] = '\0';
    }
}

int ucxapp_platform_list_serial_ports(ucxapp_serial_port_t *ports, int maxPorts)
{
    (void)ports;
    (void)maxPorts;
    return 0;
}

char *ucxapp_platform_https_get_text(const char *host, const char *path)
{
    (void)host;
    (void)path;
    return NULL;
}

char *ucxapp_platform_https_get_binary(const char *host, const char *path, size_t *outSize)
{
    (void)host;
    (void)path;
    (void)outSize;
    return NULL;
}

bool ucxapp_platform_extract_zip(const char *zipPath, const char *destDir)
{
    (void)zipPath;
    (void)destDir;
    return false;
}

bool ucxapp_platform_find_file(const char *dir, const char *ext, char *out, size_t outSize)
{
    (void)dir;
    (void)ext;
    (void)out;
    (void)outSize;
    return false;
}

int ucxapp_platform_list_files(const char *dir, void (*cb)(const char *name, uint64_t size, void *ctx), void *ctx)
{
    (void)dir;
    (void)cb;
    (void)ctx;
    return -1;
}
