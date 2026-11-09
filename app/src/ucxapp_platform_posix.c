/* POSIX backend for ucxapp_platform.h */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "ucxapp_platform.h"
#ifndef __EMSCRIPTEN__
#include "u_port_uart.h"
#endif

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

void ucxapp_platform_init(void)
{
}

void ucxapp_platform_args(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
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
#ifdef __EMSCRIPTEN__
    /* the browser has no keyboard on stdin, and its stdin always looks readable */
    return false;
#else
    fd_set fds;
    struct timeval tv = { 0, 0 };
    /* A pipe or /dev/null at EOF is always readable; only a terminal has keys (as _kbhit on Windows). */
    if (!isatty(STDIN_FILENO)) {
        return false;
    }
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
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

void ucxapp_platform_wait_key(uint32_t ms)
{
#ifdef __EMSCRIPTEN__
    usleep(ms * 1000u);
#else
    fd_set in;
    struct timeval tv = { (time_t)(ms / 1000u), (suseconds_t)((ms % 1000u) * 1000u) };

    FD_ZERO(&in);
    FD_SET(STDIN_FILENO, &in);
    (void)select(STDIN_FILENO + 1, &in, NULL, NULL, &tv);
#endif
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

#ifdef __EMSCRIPTEN__

/* The browser cannot open TCP sockets. */
intptr_t ucxapp_platform_tcp_connect(const char *host, uint16_t port, uint32_t timeoutMs)
{
    (void)host;
    (void)port;
    (void)timeoutMs;
    return -1;
}

bool ucxapp_platform_tcp_send(intptr_t sock, const void *data, size_t len)
{
    (void)sock;
    (void)data;
    (void)len;
    return false;
}

int32_t ucxapp_platform_tcp_recv(intptr_t sock, void *buf, size_t max, uint32_t timeoutMs)
{
    (void)sock;
    (void)buf;
    (void)max;
    (void)timeoutMs;
    return -1;
}

void ucxapp_platform_tcp_close(intptr_t sock)
{
    (void)sock;
}

#else

/* Non-blocking connect so an unreachable phone (app in the background) fails within the timeout. */
intptr_t ucxapp_platform_tcp_connect(const char *host, uint16_t port, uint32_t timeoutMs)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char service[8];
    int one = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wr;
        struct timeval tv = { (time_t)(timeoutMs / 1000u), (suseconds_t)(timeoutMs % 1000u) * 1000 };
        int err = 0;
        socklen_t len = sizeof(err);
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        rc = select(s + 1, NULL, &wr, NULL, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
             err == 0 ? 0 : -1;
    }
    if (rc != 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fl);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return (intptr_t)s;
}

bool ucxapp_platform_tcp_send(intptr_t sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif

    while (len > 0) {
        ssize_t n = send((int)sock, p, len, flags);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

int32_t ucxapp_platform_tcp_recv(intptr_t sock, void *buf, size_t max, uint32_t timeoutMs)
{
    fd_set rd;
    struct timeval tv = { (time_t)(timeoutMs / 1000u), (suseconds_t)(timeoutMs % 1000u) * 1000 };

    FD_ZERO(&rd);
    FD_SET((int)sock, &rd);
    int ready = select((int)sock + 1, &rd, NULL, NULL, &tv);
    if (ready == 0 || (ready < 0 && errno == EINTR)) {
        return 0;
    }
    if (ready < 0) {
        return -1;
    }
    ssize_t n = recv((int)sock, buf, max > 65536u ? 65536u : max, 0);
    return n > 0 ? (int32_t)n : -1;
}

void ucxapp_platform_tcp_close(intptr_t sock)
{
    if (sock >= 0) {
        close((int)sock);
    }
}

#endif /* __EMSCRIPTEN__ */

bool ucxapp_platform_pulse_dtr(void *uartHandle)
{
#ifdef __EMSCRIPTEN__
    (void)uartHandle;
    return false;
#else
    if (uPortUartSetDtr(uartHandle, false) != 0) {
        return false;
    }
    ucxapp_platform_sleep_ms(20);
    return uPortUartSetDtr(uartHandle, true) == 0;
#endif
}

bool ucxapp_platform_recover_dtr(void *uartHandle, uint32_t holdMs)
{
#ifdef __EMSCRIPTEN__
    (void)uartHandle;
    (void)holdMs;
    return false;
#else
    if (uPortUartSetDtr(uartHandle, false) != 0) {
        return false;
    }
    ucxapp_platform_sleep_ms(holdMs);
    for (int i = 0; i < 5; i++) {
        uPortUartSetDtr(uartHandle, true);
        ucxapp_platform_sleep_ms(10);
        uPortUartSetDtr(uartHandle, false);
        ucxapp_platform_sleep_ms(10);
    }
    return true;
#endif
}

bool ucxapp_platform_location(double *lat, double *lon, double *alt)
{
#ifdef __EMSCRIPTEN__
    (void)lat;
    (void)lon;
    (void)alt;
    return false;
#else
    /* GeoClue's demo client prints "Latitude:    55.61 deg", "Longitude: ...", "Altitude: ... meters". */
    static const char kCmd[] =
        "for p in /usr/libexec/geoclue-2.0/demos/where-am-i /usr/lib/geoclue-2.0/demos/where-am-i; do "
        "[ -x \"$p\" ] && exec timeout 8 \"$p\" -t 5; done 2>/dev/null";
    char line[128];
    bool haveLat = false;
    bool haveLon = false;
    FILE *p = popen(kCmd, "r");

    if (p == NULL) {
        return false;
    }
    *alt = 0.0;
    while (fgets(line, (int)sizeof(line), p) != NULL) {
        if (strncmp(line, "Latitude:", 9) == 0) {
            haveLat = sscanf(line + 9, "%lf", lat) == 1;
        } else if (strncmp(line, "Longitude:", 10) == 0) {
            haveLon = sscanf(line + 10, "%lf", lon) == 1;
        } else if (strncmp(line, "Altitude:", 9) == 0 && (sscanf(line + 9, "%lf", alt) != 1 ||
                                                           !(*alt > -1000.0 && *alt < 10000.0))) {
            *alt = 0.0;
        }
    }
    (void)pclose(p);
    return haveLat && haveLon && *lat >= -90.0 && *lat <= 90.0 && *lon >= -180.0 && *lon <= 180.0;
#endif
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

static void readSysfsFile(const char *path, char *out, size_t outSize)
{
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (f) {
        if (fgets(out, (int)outSize, f)) {
            out[strcspn(out, "\r\n")] = '\0';
        }
        fclose(f);
    }
}

/* device is the USB interface's tty node; the USB device holding product and idVendor is two levels up */
static void readSysfsText(const char *tty, const char *file, char *out, size_t outSize)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../../%s", tty, file);
    readSysfsFile(path, out, outSize);
}

/* FTDI channel of an EVK port: interface 0 is the AT port and 2 the log port, as on Windows (channels A and C) */
static void labelEvkChannel(const char *tty, char *label, size_t labelSize)
{
    char path[PATH_MAX];
    char ifnum[8];
    snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../bInterfaceNumber", tty);
    readSysfsFile(path, ifnum, sizeof(ifnum));
    static const char *const kChannel[] = { "AT", "B", "LOG", "D" };
    unsigned long n = strtoul(ifnum, NULL, 16);
    if (n < sizeof(kChannel) / sizeof(kChannel[0])) {
        snprintf(label, labelSize, "%s", kChannel[n]);
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
        if (strstr(port->desc, "EVK-NORA") != NULL) {
            labelEvkChannel(entry->d_name, port->label, sizeof(port->label));
        }
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

ucxapp_trust_t ucxapp_platform_root_trust(const uint8_t *der, size_t len, bool add)
{
    (void)der;
    (void)len;
    (void)add;
    return UCXAPP_TRUST_UNSUPPORTED;   /* each browser / distro has its own store */
}

bool ucxapp_platform_key_protect(bool protect, const uint8_t *in, size_t len, uint8_t *out, size_t outSize,
                                 size_t *outLen)
{
    (void)protect;
    (void)in;
    (void)len;
    (void)out;
    (void)outSize;
    (void)outLen;
    return false;   /* the key file is owner-only (ucxapp_store_make_private) */
}

int ucxapp_platform_readout_protection(void)
{
    return -1;
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
