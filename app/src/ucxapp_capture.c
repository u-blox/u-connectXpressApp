/* Packet captures by an external tool into a pcap file, with an optional live view in Wireshark: a feeder thread
 * tails the growing file into "wireshark -k -i -". See ucxapp_capture.h. */

#include "ucxapp_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_proc.h"
#include "ucxapp_thread.h"

#define LOG_SIZE  8192
#define FEED_SIZE 16384

struct ucxapp_capture {
    ucxapp_proc_t *tool;
    ucxapp_proc_t *shark;
    ucxapp_thread_t *feeder;
    volatile bool toolDone;
    volatile bool feedFailed;
    char pcap[512];
    char log[LOG_SIZE];
    size_t logLen;
    uint32_t startMs;
};

/* The command that starts Wireshark: UCXAPP_WIRESHARK is used as given (tests put a fake one there). */
static bool wiresharkCmd(char *out, size_t size)
{
    const char *env = getenv("UCXAPP_WIRESHARK");
    char path[512];

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, size, "%s", env);
        return true;
    }
    return ucxapp_capture_wireshark(path, sizeof(path)) && ucxapp_proc_quote(path, out, size);
}

bool ucxapp_capture_wireshark(char *out, size_t size)
{
    const char *env = getenv("UCXAPP_WIRESHARK");
#ifdef _WIN32
    static const char *const kDirs[] = { "ProgramFiles", "ProgramW6432", "ProgramFiles(x86)" };
    size_t i;
#endif

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, size, "%s", env);
        return true;
    }
    if (ucxapp_proc_which("wireshark", out, size) || ucxapp_proc_which("Wireshark", out, size)) {
        return true;
    }
#ifdef _WIN32
    for (i = 0; i < sizeof(kDirs) / sizeof(kDirs[0]); i++) {
        const char *dir = getenv(kDirs[i]);
        FILE *f;

        if (dir == NULL) {
            continue;
        }
        (void)snprintf(out, size, "%s\\Wireshark\\Wireshark.exe", dir);
        f = fopen(out, "rb");
        if (f != NULL) {
            fclose(f);
            return true;
        }
    }
#endif
    return false;
}

bool ucxapp_capture_view(const char *path)
{
    char cmd[1200], ws[600], arg[600];
    ucxapp_proc_t *p;

    if (!wiresharkCmd(ws, sizeof(ws))) {
        ucxapp_print("ERROR: Wireshark not found (install it or set UCXAPP_WIRESHARK)\n");
        return false;
    }
    if (!ucxapp_proc_quote(path, arg, sizeof(arg))) {
        ucxapp_print("ERROR: the file name cannot be passed to Wireshark\n");
        return false;
    }
    (void)snprintf(cmd, sizeof(cmd), "%s -r %s", ws, arg);
    p = ucxapp_proc_start(cmd, NULL, UCXAPP_PROC_DETACHED);
    if (p == NULL) {
        ucxapp_print("ERROR: Wireshark did not start\n");
        return false;
    }
    ucxapp_proc_release(p);
    return true;
}

void ucxapp_capture_default_name(const char *prefix, char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tmv;

#ifdef _WIN32
    (void)localtime_s(&tmv, &now);
#else
    (void)localtime_r(&now, &tmv);
#endif
    (void)snprintf(out, size, "%s-%04d%02d%02d-%02d%02d%02d.pcap", prefix, tmv.tm_year + 1900, tmv.tm_mon + 1,
                   tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static uint32_t rd32(const uint8_t *b, bool swap)
{
    return swap ? ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3])
                : ((uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]);
}

bool ucxapp_capture_count(const char *path, uint32_t *packets, uint64_t *bytes)
{
    FILE *f = fopen(path, "rb");
    uint8_t h[24];
    bool swap = false, ok = false;

    *packets = 0;
    *bytes = 0;
    if (f == NULL) {
        return false;
    }
    if (fread(h, 1, sizeof(h), f) == sizeof(h)) {
        uint32_t magic = rd32(h, false);

        if (magic == 0xA1B2C3D4u || magic == 0xA1B23C4Du || magic == 0xD4C3B2A1u || magic == 0x4D3CB2A1u) {
            uint8_t r[16];

            swap = magic == 0xD4C3B2A1u || magic == 0x4D3CB2A1u;
            *bytes = sizeof(h);
            ok = true;
            while (fread(r, 1, sizeof(r), f) == sizeof(r)) {
                uint32_t len = rd32(r + 8, swap);

                if (len > 0x4000000u || fseek(f, (long)len, SEEK_CUR) != 0) {
                    break;
                }
                *bytes += sizeof(r) + len;
                (*packets)++;
            }
        } else if (magic == 0x0A0D0D0Au) {
            uint32_t type = 0, len = 0;
            uint8_t b[8];

            swap = rd32(h + 8, false) != 0x1A2B3C4Du;
            len = rd32(h + 4, swap);
            ok = len >= 12 && fseek(f, (long)len, SEEK_SET) == 0;
            *bytes = ok ? len : 0;
            while (ok && fread(b, 1, sizeof(b), f) == sizeof(b)) {
                type = rd32(b, swap);
                len = rd32(b + 4, swap);
                if (len < 12 || len > 0x4000000u || fseek(f, (long)(len - 8), SEEK_CUR) != 0) {
                    break;
                }
                *bytes += len;
                if (type == 6 || type == 3 || type == 2) {
                    (*packets)++;
                }
            }
        }
    }
    fclose(f);
    return ok;
}

/* Copies the growing capture file into Wireshark's stdin until the tool is done and the file is drained. */
static void feedTask(void *arg)
{
    ucxapp_capture_t *c = (ucxapp_capture_t *)arg;
    char *buf = (char *)malloc(FEED_SIZE);
    FILE *f = NULL;

    while (buf != NULL) {
        size_t n = 0;
        bool done = c->toolDone;

        if (f == NULL) {
            f = fopen(c->pcap, "rb");
        }
        if (f != NULL) {
            n = fread(buf, 1, FEED_SIZE, f);
            clearerr(f);
        }
        if (n > 0) {
            if (!ucxapp_proc_write(c->shark, buf, n)) {
                c->feedFailed = true;
                break;
            }
        } else if (done) {
            break;
        } else {
            ucxapp_platform_sleep_ms(50);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    free(buf);
}

ucxapp_capture_t *ucxapp_capture_start(const char *cmdline, const char *pcap, bool stdoutToPcap, bool live)
{
    ucxapp_capture_t *c = (ucxapp_capture_t *)calloc(1, sizeof(*c));
    char ws[600], cmd[700];

    if (c == NULL) {
        return NULL;
    }
    (void)snprintf(c->pcap, sizeof(c->pcap), "%s", pcap);
    if (!stdoutToPcap) {
        (void)remove(pcap);   /* the live view must not show an old capture */
    }
    c->tool = ucxapp_proc_start(cmdline, stdoutToPcap ? pcap : NULL, 0);
    if (c->tool == NULL) {
        ucxapp_print("ERROR: the capture tool did not start\n");
        free(c);
        return NULL;
    }
    c->startMs = ucxapp_platform_tick_ms();
    if (live) {
        if (!wiresharkCmd(ws, sizeof(ws))) {
            ucxapp_print("WARNING: Wireshark not found (install it or set UCXAPP_WIRESHARK); capturing to the file only\n");
        } else {
            (void)snprintf(cmd, sizeof(cmd), "%s -k -i -", ws);
            c->shark = ucxapp_proc_start(cmd, NULL, UCXAPP_PROC_STDIN | UCXAPP_PROC_DETACHED);
            if (c->shark == NULL) {
                ucxapp_print("WARNING: Wireshark did not start; capturing to the file only\n");
            } else {
                c->feeder = ucxapp_thread_start(feedTask, c);
            }
        }
    }
    return c;
}

const char *ucxapp_capture_log(ucxapp_capture_t *c)
{
    char buf[1024];
    int32_t n;

    while ((n = ucxapp_proc_read(c->tool, buf, sizeof(buf))) > 0) {
        if (c->logLen + (size_t)n >= LOG_SIZE) {
            size_t drop = c->logLen + (size_t)n - (LOG_SIZE / 2);

            if (drop > c->logLen) {
                drop = c->logLen;
            }
            memmove(c->log, c->log + drop, c->logLen - drop);
            c->logLen -= drop;
            if ((size_t)n >= LOG_SIZE - c->logLen) {
                n = (int32_t)(LOG_SIZE - c->logLen - 1);
            }
        }
        memcpy(c->log + c->logLen, buf, (size_t)n);
        c->logLen += (size_t)n;
        c->log[c->logLen] = '\0';
    }
    return c->log;
}

bool ucxapp_capture_running(ucxapp_capture_t *c, int *code)
{
    (void)ucxapp_capture_log(c);
    return ucxapp_proc_running(c->tool, code);
}

const char *ucxapp_capture_file(const ucxapp_capture_t *c)
{
    return c->pcap;
}

uint32_t ucxapp_capture_elapsed_ms(const ucxapp_capture_t *c)
{
    return ucxapp_platform_tick_ms() - c->startMs;
}

int ucxapp_capture_stop(ucxapp_capture_t *c, uint32_t graceMs)
{
    uint32_t packets = 0;
    uint64_t bytes = 0;
    uint32_t ms = ucxapp_capture_elapsed_ms(c);
    int code;
    char dur[32];

    (void)ucxapp_capture_log(c);
    if (ucxapp_proc_running(c->tool, &code)) {
        code = ucxapp_proc_stop(c->tool, graceMs);
    } else {
        code = ucxapp_proc_wait(c->tool, 0);
    }
    c->toolDone = true;
    if (c->feeder != NULL) {
        ucxapp_thread_join(c->feeder);
    }
    if (c->shark != NULL) {
        ucxapp_proc_release(c->shark);
    }
    if (ucxapp_capture_count(c->pcap, &packets, &bytes)) {
        ucxapp_print("%s: %u packets, %llu bytes in %s\n", c->pcap, (unsigned)packets, (unsigned long long)bytes,
                     ucxapp_fmt_duration(ms, dur, sizeof(dur)));
    } else {
        ucxapp_print("WARNING: no capture in %s\n", c->pcap);
    }
    free(c);
    return code;
}
