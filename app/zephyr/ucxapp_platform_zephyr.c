/* Platform backend for Zephyr targets: kernel time and threads, console keys, no OS shell or host network. */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <time.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/mem_stats.h>

#include "ucxapp_platform.h"
#include "ucxapp_zephyr.h"

#if defined(CONFIG_COMMON_LIBC_MALLOC) && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
int malloc_runtime_stats_get(struct sys_memory_stats *stats);
#define HAVE_HEAP_STATS 1
#endif

static int gPendingKey = -1;

void ucxapp_platform_init(void)
{
}

uint32_t ucxapp_platform_tick_ms(void)
{
    return k_uptime_get_32();
}

void ucxapp_platform_sleep_ms(uint32_t ms)
{
    k_msleep((int32_t)(ms > 0 ? ms : 1));
}

bool ucxapp_platform_clock_adjust(int64_t deltaSeconds)
{
    struct timespec ts;

    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return false;
    }
    ts.tv_sec += (time_t)deltaSeconds;
    return sys_clock_settime(SYS_CLOCK_REALTIME, &ts) == 0;
}

bool ucxapp_platform_key_pressed(void)
{
    uint8_t c;

    if (gPendingKey < 0 && ucxapp_zephyr_console_read(&c, 0) == 1) {
        gPendingKey = c;
    }
    return gPendingKey >= 0;
}

int ucxapp_platform_get_key(void)
{
    uint8_t c;
    int key;

    if (gPendingKey < 0) {
        while (ucxapp_zephyr_console_read(&c, 1000) == 0) {
        }
        gPendingKey = c;
    }
    key = gPendingKey;
    gPendingKey = -1;
    return key;
}

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
    uint64_t ramStatic = (uint64_t)(_image_ram_end - _image_ram_start);
    uint64_t image = (uint64_t)(__rom_region_end - __rom_region_start);
    int n = 0;

    n = memRow(areas, n, max, "RAM static", DT_REG_SIZE(DT_CHOSEN(zephyr_sram)), ramStatic);
#ifdef HAVE_HEAP_STATS
    struct sys_memory_stats stats;
    if (malloc_runtime_stats_get(&stats) == 0) {
        uint64_t heap = (uint64_t)stats.allocated_bytes + stats.free_bytes;
        n = memRow(areas, n, max, "Heap", heap, stats.allocated_bytes);
        n = memRow(areas, n, max, "Heap peak", heap, stats.max_allocated_bytes);
    }
#endif
    n = memRow(areas, n, max, "Flash", DT_REG_SIZE(DT_CHOSEN(zephyr_flash)), image);
    return n;
}

typedef struct {
    ucxapp_mem_t *rows;
    int max;
    int n;
} threadWalk_t;

static void threadRow(const struct k_thread *thread, void *userData)
{
    threadWalk_t *walk = (threadWalk_t *)userData;
    ucxapp_mem_t *row;
    const char *name = k_thread_name_get((k_tid_t)thread);
    size_t unused = 0;

    if (walk->n >= walk->max) {
        return;
    }
    row = &walk->rows[walk->n++];
    strncpy(row->name, (name != NULL && name[0] != '\0') ? name : "?", sizeof(row->name) - 1);
    row->name[sizeof(row->name) - 1] = '\0';
    row->size = thread->stack_info.size;
    row->used = k_thread_stack_space_get(thread, &unused) == 0 && unused <= row->size ? row->size - unused : 0;
}

int ucxapp_platform_tasks(ucxapp_mem_t *tasks, int max)
{
    threadWalk_t walk = { tasks, max, 0 };

    k_thread_foreach(threadRow, &walk);
    return walk.n;
}

bool ucxapp_platform_local_ip(char *out, size_t size)
{
    (void)out;
    (void)size;
    return false;
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
