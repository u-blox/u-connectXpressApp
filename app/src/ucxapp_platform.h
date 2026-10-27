#ifndef UCXAPP_PLATFORM_H
#define UCXAPP_PLATFORM_H

/* Host OS services used by the app. Each target provides one backend (ucxapp_platform_<os>.c). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UCXAPP_PLATFORM_PORT_NAME_LEN   32
#define UCXAPP_PLATFORM_PORT_DESC_LEN   256
#define UCXAPP_PLATFORM_PORT_LABEL_LEN  64

typedef struct {
    char name[UCXAPP_PLATFORM_PORT_NAME_LEN];     /* e.g. "COM13" or "/dev/ttyUSB0" */
    char desc[UCXAPP_PLATFORM_PORT_DESC_LEN];     /* device description, empty if unknown */
    char label[UCXAPP_PLATFORM_PORT_LABEL_LEN];   /* port function such as "AT" or "LOG", may be empty */
    bool available;                               /* false when another process has the port open */
    uint16_t vid, pid;                            /* USB vendor/product ID, 0 if unknown */
} ucxapp_serial_port_t;

/** One-time setup (UTF-8 console output). */
void ucxapp_platform_init(void);

/** Milliseconds since an arbitrary start point; wraps after 49 days. */
uint32_t ucxapp_platform_tick_ms(void);

/** Blocks the calling thread (not available on the STM32 backend). */
void ucxapp_platform_sleep_ms(uint32_t ms);

/** Shifts the clock that time() reads by deltaSeconds; false if the OS owns the clock (PC builds). */
bool ucxapp_platform_clock_adjust(int64_t deltaSeconds);

/** True if a key press is waiting. */
bool ucxapp_platform_key_pressed(void);

/** Blocks for one key press and returns it. */
int ucxapp_platform_get_key(void);

/* Text UI: raw keyboard and an ANSI-capable terminal. Not available on the STM32 backend. */
enum {
    UCXAPP_KEY_UP = 0x100, UCXAPP_KEY_DOWN, UCXAPP_KEY_LEFT, UCXAPP_KEY_RIGHT,
    UCXAPP_KEY_HOME, UCXAPP_KEY_END, UCXAPP_KEY_PGUP, UCXAPP_KEY_PGDN,
    UCXAPP_KEY_DEL, UCXAPP_KEY_F1, UCXAPP_KEY_F10,
    UCXAPP_KEY_CPGUP, UCXAPP_KEY_CPGDN, UCXAPP_KEY_WHEELUP, UCXAPP_KEY_WHEELDN
};

/** Raw mode and VT output on; call leave to restore. Returns false if not a terminal. */
bool ucxapp_platform_terminal_enter(void);
void ucxapp_platform_terminal_leave(void);
void ucxapp_platform_terminal_size(int *cols, int *rows);

/** Next key without blocking, or -1. Enter is 13, Backspace 8, Esc 27, Ctrl-C 3, others as above or ASCII. */
int ucxapp_platform_read_key(void);

/** IPv4 address of the host's main LAN/Wi-Fi interface (virtual and link-local ones skipped); false if none. */
bool ucxapp_platform_local_ip(char *out, size_t size);

/** Directory of the running executable, with a trailing path separator. */
void ucxapp_platform_exe_dir(char *buffer, size_t bufferSize);

/**
 * Lists serial ports, sorted as the OS reports them.
 * @return number of entries written (at most maxPorts).
 */
int ucxapp_platform_list_serial_ports(ucxapp_serial_port_t *ports, int maxPorts);

/**
 * HTTPS GET with redirects followed. The body is returned in a malloc'ed buffer (caller frees),
 * NULL on failure or empty body. The text variant is NUL terminated; the binary variant
 * reports the length in outSize and prints download progress.
 */
char *ucxapp_platform_https_get_text(const char *host, const char *path);
char *ucxapp_platform_https_get_binary(const char *host, const char *path, size_t *outSize);

/** Extracts a ZIP archive into destDir (created if missing) using the OS tools. */
bool ucxapp_platform_extract_zip(const char *zipPath, const char *destDir);

/** Finds the first file below dir whose name ends with ext (case-insensitive, e.g. ".bin"). */
bool ucxapp_platform_find_file(const char *dir, const char *ext, char *out, size_t outSize);

/** Calls cb(name, size, ctx) for every file directly in dir (folders skipped); returns the count, -1 on error. */
int ucxapp_platform_list_files(const char *dir, void (*cb)(const char *name, uint64_t size, void *ctx), void *ctx);

/** A memory area or task stack: size and used, in bytes. */
typedef struct {
    char name[24];
    uint64_t size;
    uint64_t used;
} ucxapp_mem_t;

/** RAM, heap, flash/disk areas of this program; returns the count. */
int ucxapp_platform_memory(ucxapp_mem_t *areas, int max);

/** Stack of each task (RTOS targets; 0 on hosts); returns the count. */
int ucxapp_platform_tasks(ucxapp_mem_t *tasks, int max);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_PLATFORM_H */
