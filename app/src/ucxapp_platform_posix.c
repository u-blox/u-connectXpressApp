/* POSIX backend for ucxapp_platform.h */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "ucxapp_platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void ucxapp_platform_init(void)
{
}

void ucxapp_platform_sleep_ms(uint32_t ms)
{
    usleep((useconds_t)ms * 1000u);
}

uint32_t ucxapp_platform_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

bool ucxapp_platform_clock_adjust(int64_t deltaSeconds)
{
    (void)deltaSeconds;
    return false;
}

bool ucxapp_platform_key_pressed(void)
{
    fd_set fds;
    struct timeval tv = { 0, 0 };
    /* A pipe or /dev/null at EOF is always readable; only a terminal has keys (as _kbhit on Windows). */
    if (!isatty(STDIN_FILENO)) {
        return false;
    }
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

int ucxapp_platform_get_key(void)
{
    return getchar();
}

static struct termios gSavedTermios;
static bool gTermActive;

bool ucxapp_platform_terminal_enter(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &gSavedTermios) != 0) {
        return false;
    }
    raw = gSavedTermios;
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return false;
    }
    gTermActive = true;
    return true;
}

void ucxapp_platform_terminal_leave(void)
{
    if (gTermActive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &gSavedTermios);
        gTermActive = false;
    }
}

void ucxapp_platform_terminal_size(int *cols, int *rows)
{
    struct winsize ws;

    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

static int readByte(void)
{
    unsigned char c;

    return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : -1;
}

int ucxapp_platform_read_key(void)
{
    int c = readByte();

    if (c == 127) {
        return 8;
    }
    if (c != 27) {
        return c;
    }
    int c1 = readByte();
    if (c1 < 0) {
        return 27;
    }
    int c2 = readByte();
    if (c1 == '[' || c1 == 'O') {
        switch (c2) {
        case 'A': return UCXAPP_KEY_UP;
        case 'B': return UCXAPP_KEY_DOWN;
        case 'C': return UCXAPP_KEY_RIGHT;
        case 'D': return UCXAPP_KEY_LEFT;
        case 'H': return UCXAPP_KEY_HOME;
        case 'F': return UCXAPP_KEY_END;
        case 'P': return UCXAPP_KEY_F1;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
            int num = c2 - '0';
            int d = readByte();
            while (d >= '0' && d <= '9') {
                num = num * 10 + (d - '0');
                d = readByte();
            }
            if (d == '~') {
                switch (num) {
                case 1: case 7: return UCXAPP_KEY_HOME;
                case 3: return UCXAPP_KEY_DEL;
                case 4: case 8: return UCXAPP_KEY_END;
                case 5: return UCXAPP_KEY_PGUP;
                case 6: return UCXAPP_KEY_PGDN;
                case 11: return UCXAPP_KEY_F1;
                case 21: return UCXAPP_KEY_F10;
                default: break;
                }
            }
            return -1;
        }
        default: break;
        }
    }
    return -1;
}

bool ucxapp_platform_local_ip(char *out, size_t size)
{
    static const char *const skip[] = { "docker", "veth", "br-", "virbr", "tun", "tap", "vmnet", "wg" };
    struct ifaddrs *all;
    bool found = false;

    out[0] = '\0';
    if (getifaddrs(&all) != 0) {
        return false;
    }
    for (struct ifaddrs *a = all; a != NULL && !found; a = a->ifa_next) {
        char ip[INET_ADDRSTRLEN];
        bool virt = false;

        if (a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_LOOPBACK) != 0 ||
            (a->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
            virt = virt || strncmp(a->ifa_name, skip[i], strlen(skip[i])) == 0;
        }
        if (!virt && inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof(ip)) != NULL &&
            strncmp(ip, "169.254.", 8) != 0) {
            snprintf(out, size, "%s", ip);
            found = true;
        }
    }
    freeifaddrs(all);
    return found;
}

void ucxapp_platform_exe_dir(char *buffer, size_t bufferSize)
{
    ssize_t n = readlink("/proc/self/exe", buffer, bufferSize - 1);
    if (n <= 0) {
        snprintf(buffer, bufferSize, "./");
        return;
    }
    buffer[n] = '\0';
    char *lastSlash = strrchr(buffer, '/');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
}

static void readSysfsText(const char *tty, const char *file, char *out, size_t outSize)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../%s", tty, file);
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (f) {
        if (fgets(out, (int)outSize, f)) {
            out[strcspn(out, "\r\n")] = '\0';
        }
        fclose(f);
    }
}

int ucxapp_platform_list_serial_ports(ucxapp_serial_port_t *ports, int maxPorts)
{
    int count = 0;
    DIR *dir = opendir("/dev");
    if (!dir) {
        return 0;
    }
    struct dirent *entry;
    while (count < maxPorts && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "ttyUSB", 6) != 0 && strncmp(entry->d_name, "ttyACM", 6) != 0) {
            continue;
        }
        ucxapp_serial_port_t *port = &ports[count];
        memset(port, 0, sizeof(*port));
        int n = snprintf(port->name, sizeof(port->name), "/dev/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(port->name)) {
            continue;
        }
        count++;
        readSysfsText(entry->d_name, "product", port->desc, sizeof(port->desc));
        char id[8];
        readSysfsText(entry->d_name, "idVendor", id, sizeof(id));
        port->vid = (uint16_t)strtoul(id, NULL, 16);
        readSysfsText(entry->d_name, "idProduct", id, sizeof(id));
        port->pid = (uint16_t)strtoul(id, NULL, 16);
        port->available = access(port->name, R_OK | W_OK) == 0;
    }
    closedir(dir);
    return count;
}

bool ucxapp_platform_extract_zip(const char *zipPath, const char *destDir)
{
    if (strpbrk(zipPath, "\"'`$\\") != NULL || strpbrk(destDir, "\"'`$\\") != NULL) {
        return false;
    }
    char cmd[2 * PATH_MAX];
    snprintf(cmd, sizeof(cmd), "unzip -o -q \"%s\" -d \"%s\"", zipPath, destDir);
    return system(cmd) == 0;
}

int ucxapp_platform_list_files(const char *dir, void (*cb)(const char *name, uint64_t size, void *ctx), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int count = 0;

    if (d == NULL) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            cb(e->d_name, (uint64_t)st.st_size, ctx);
            count++;
        }
    }
    closedir(d);
    return count;
}

/* Value in kB of "Key:   1234 kB" in a /proc file; 0 if missing. */
static uint64_t procKb(const char *path, const char *key)
{
    char line[128];
    size_t keyLen = strlen(key);
    uint64_t kb = 0;
    FILE *f = fopen(path, "r");

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, keyLen) == 0 && line[keyLen] == ':') {
            kb = strtoull(line + keyLen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static int memRow(ucxapp_mem_t *rows, int n, int max, const char *name, uint64_t size, uint64_t used)
{
    if (n < max && size > 0) {
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
        rows[n].size = size;
        rows[n].used = used;
        n++;
    }
    return n;
}

int ucxapp_platform_memory(ucxapp_mem_t *areas, int max)
{
    uint64_t total = procKb("/proc/meminfo", "MemTotal") * 1024u;
    uint64_t avail = procKb("/proc/meminfo", "MemAvailable") * 1024u;
    struct statvfs fs;
    int n = 0;

    n = memRow(areas, n, max, "App RAM", total, procKb("/proc/self/status", "VmRSS") * 1024u);
    n = memRow(areas, n, max, "App RAM peak", total, procKb("/proc/self/status", "VmHWM") * 1024u);
    n = memRow(areas, n, max, "System RAM", total, total - avail);
    if (statvfs(".", &fs) == 0) {
        uint64_t size = (uint64_t)fs.f_blocks * fs.f_frsize;
        n = memRow(areas, n, max, "Disk", size, size - (uint64_t)fs.f_bavail * fs.f_frsize);
    }
    return n;
}

int ucxapp_platform_tasks(ucxapp_mem_t *tasks, int max)
{
    (void)tasks;
    (void)max;
    return 0;
}

bool ucxapp_platform_find_file(const char *dir, const char *ext, char *out, size_t outSize)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    bool found = false;
    size_t extLen = strlen(ext);
    struct dirent *entry;
    while (!found && (entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        size_t n = strlen(entry->d_name);
        if (n >= extLen && strcasecmp(entry->d_name + n - extLen, ext) == 0) {
            snprintf(out, outSize, "%s", path);
            found = true;
        } else if (ucxapp_platform_find_file(path, ext, out, outSize)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* No HTTPS client on POSIX; only the Windows menu's GitHub firmware download uses these. */
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
    if (outSize) {
        *outSize = 0;
    }
    return NULL;
}
