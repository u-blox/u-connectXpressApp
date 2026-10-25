/* app.time - the app's wall clock: shows UTC, syncs from SNTP (RFC 4330), an HTTP Date header (RFC 9110) or the module
 * clock (AT+USYTU), can be set by hand and pushed to the module. On STM32 it sets the clock time() reads; on a PC the
 * OS keeps the clock and the app keeps the measured offset. */

#include "ucxapp_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ucxapp_platform.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#include "u_cx_system.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define NTP_PORT        123
#define NTP_UNIX_DELTA  2208988800LL   /* seconds from 1900-01-01 to 1970-01-01 */
#define NTP_TRIES       3
#define WAIT_MS         3000
#define POLL_MS         10
#define HTTP_BUF        1024
#define DEFAULT_NTP     "pool.ntp.org"
#define DEFAULT_HTTP    "www.google.com"

static int64_t gOffset;                /* added to time() when the OS owns the clock */
static char gSource[64] = "";
static uint32_t gSyncTick;

static int64_t nowEpoch(void)
{
    return (int64_t)time(NULL) + gOffset;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil). */
static int64_t daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void formatUtc(int64_t t, char *out, size_t size)
{
    int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    int64_t sec = t - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int y = (int)(yoe + era * 400 + (m <= 2));

    snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, (int)(sec / 3600), (int)(sec / 60 % 60),
             (int)(sec % 60));
}

static bool validDate(int y, int mo, int d, int h, int mi, int s)
{
    return y >= 1970 && y <= 2105 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 &&
           mi <= 59 && s >= 0 && s <= 60;
}

/* "1726460580", "2024-09-16T04:23:00[Z]" or "2024-09-16 04:23:00". */
static bool parseTime(const char *s, int64_t *t)
{
    int y, mo, d, h, mi, sec;
    char *end;

    if (*s >= '0' && *s <= '9') {
        long long v = strtoll(s, &end, 10);
        if (*end == '\0' && v > 0) {
            *t = v;
            return true;
        }
    }
    if ((sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6 ||
         sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) &&
        validDate(y, mo, d, h, mi, sec)) {
        *t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
        return true;
    }
    return false;
}

/* The value of an HTTP "Date: Sun, 06 Nov 1994 08:49:37 GMT" header. */
static bool parseHttpDate(const char *rsp, int64_t *t)
{
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *p = rsp;
    char mon[4];
    int d, y, h, mi, s;

    for (; *p != '\0'; p++) {
        if ((p == rsp || p[-1] == '\n') && (p[0] == 'D' || p[0] == 'd') && (p[1] == 'a' || p[1] == 'A') &&
            (p[2] == 't' || p[2] == 'T') && (p[3] == 'e' || p[3] == 'E') && p[4] == ':') {
            break;
        }
    }
    if (*p == '\0' || sscanf(p + 5, " %*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) != 6) {
        return false;
    }
    const char *m = strstr(kMonths, mon);
    if (m == NULL || strlen(mon) != 3 || (m - kMonths) % 3 != 0) {
        return false;
    }
    int mo = (int)((m - kMonths) / 3) + 1;
    if (!validDate(y, mo, d, h, mi, s)) {
        return false;
    }
    *t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

/* Unix seconds from an SNTP reply (transmit timestamp at offset 40), rounded with half the round trip. */
static bool parseNtp(const uint8_t *p, size_t len, uint32_t rttMs, int64_t *t)
{
    if (len < 48 || (p[0] & 0x07) != 4 || p[1] == 0) {
        return false;                  /* not a server reply, or a kiss-of-death (stratum 0) */
    }
    uint32_t sec = ((uint32_t)p[40] << 24) | ((uint32_t)p[41] << 16) | ((uint32_t)p[42] << 8) | p[43];
    uint32_t frac = ((uint32_t)p[44] << 24) | ((uint32_t)p[45] << 16) | ((uint32_t)p[46] << 8) | p[47];
    double s = (double)frac / 4294967296.0 + (double)rttMs / 2000.0;
    /* Era 1 starts in 2036: timestamps with the top bit clear are after 2036-02-07. */
    int64_t ntp = (int64_t)sec + ((sec & 0x80000000u) == 0 ? 4294967296LL : 0);
    *t = ntp - NTP_UNIX_DELTA + (int64_t)(s + 0.5);
    return true;
}

static void apply(int64_t t, const char *source)
{
    int64_t delta = t - nowEpoch();
    char buf[32];

    if (!ucxapp_platform_clock_adjust(delta)) {
        gOffset += delta;
    }
    snprintf(gSource, sizeof(gSource), "%s", source);
    gSyncTick = ucxapp_platform_tick_ms();
    formatUtc(t, buf, sizeof(buf));
    ucxapp_print("time %s from %s (clock was off by %lld s)\n", buf, source, (long long)delta);
}

static bool syncNtp(uCxHandle_t *ucx, const char *host, int64_t *t)
{
    uint8_t pkt[64];
    int32_t sock;
    bool ok = false;

    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_UDP, &sock) != 0) {
        return false;
    }
    if (uCxSocketConnect(ucx, sock, host, NTP_PORT) == 0) {
        for (int tr = 0; tr < NTP_TRIES && !ok; tr++) {
            memset(pkt, 0, 48);
            pkt[0] = 0x23;             /* LI 0, version 4, mode 3 (client) */
            uint32_t start = ucxapp_platform_tick_ms();
            if (uCxSocketWrite(ucx, sock, pkt, 48) < 0) {
                break;
            }
            while (!ok && ucxapp_platform_tick_ms() - start < WAIT_MS) {
                uCxSocketReceiveFrom_t from;
                memset(&from, 0, sizeof(from));
                int32_t n = uCxSocketReceiveFrom(ucx, sock, (int32_t)sizeof(pkt), pkt, &from);
                if (n > 0) {
                    ok = parseNtp(pkt, (size_t)n, ucxapp_platform_tick_ms() - start, t);
                } else {
                    ucxapp_platform_sleep_ms(POLL_MS);
                }
            }
        }
    }
    uCxSocketClose(ucx, sock);
    return ok;
}

static bool syncHttp(uCxHandle_t *ucx, const char *host, int64_t *t)
{
    static char rsp[HTTP_BUF + 1];
    char req[200];
    int32_t sock;
    size_t got = 0;
    bool ok = false;

    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        return false;
    }
    ucxapp_events_clear(URC_FLAG_SOCK_CONNECTED);
    if (uCxSocketConnect(ucx, sock, host, 80) == 0 && ucxapp_events_wait(URC_FLAG_SOCK_CONNECTED, 10)) {
        int n = snprintf(req, sizeof(req), "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
        if (n > 0 && n < (int)sizeof(req) && uCxSocketWrite(ucx, sock, (const uint8_t *)req, n) == n) {
            uint32_t start = ucxapp_platform_tick_ms();
            while (got < HTTP_BUF && ucxapp_platform_tick_ms() - start < WAIT_MS) {
                int32_t r = uCxSocketRead(ucx, sock, (int32_t)(HTTP_BUF - got), (uint8_t *)rsp + got);
                if (r > 0) {
                    got += (size_t)r;
                    rsp[got] = '\0';
                    if ((ok = parseHttpDate(rsp, t)) || strstr(rsp, "\r\n\r\n") != NULL) {
                        break;
                    }
                } else {
                    ucxapp_platform_sleep_ms(POLL_MS);
                }
            }
        }
    }
    uCxSocketClose(ucx, sock);
    return ok;
}

static bool readModule(uCxHandle_t *ucx, int64_t *t)
{
    uByteArray_t v;
    bool ok = false;

    memset(&v, 0, sizeof(v));
    if (uCxSystemGetUnixTimeBegin(ucx, &v) && v.pData != NULL && v.length >= 1 && v.length <= 8) {
        int64_t x = 0;
        for (size_t i = 0; i < (size_t)v.length; i++) {
            x = (x << 8) | v.pData[i];
        }
        *t = x;
        ok = x > 0;
    }
    return uCxEnd(ucx) == 0 && ok;
}

static int cmdSync(ucxapp_t *app, int argc, char *argv[])
{
    const char *how = argc >= 3 ? argv[2] : "auto";
    const char *host = argc >= 4 ? argv[3] : NULL;
    char source[64];
    int64_t t;

    if (argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx == NULL) {
        ucxapp_print("ERROR: not connected to a module\n");
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (strcmp(how, "ntp") == 0 || strcmp(how, "auto") == 0) {
        const char *h = host != NULL ? host : DEFAULT_NTP;
        if (syncNtp(app->ucx, h, &t)) {
            snprintf(source, sizeof(source), "ntp %s", h);
            apply(t, source);
            return UCXAPP_OK;
        }
        ucxapp_print("%s: no SNTP reply from %s\n", strcmp(how, "ntp") == 0 ? "ERROR" : "note", h);
        if (strcmp(how, "ntp") == 0) {
            return UCXAPP_ERR_FAILED;
        }
        host = NULL;
    }
    if (strcmp(how, "http") == 0 || strcmp(how, "auto") == 0) {
        const char *h = host != NULL ? host : DEFAULT_HTTP;
        if (syncHttp(app->ucx, h, &t)) {
            snprintf(source, sizeof(source), "http %s", h);
            apply(t, source);
            return UCXAPP_OK;
        }
        ucxapp_print("ERROR: no HTTP Date header from %s (connect Wi-Fi first: wifi.connect)\n", h);
        return UCXAPP_ERR_FAILED;
    }
    if (strcmp(how, "module") == 0 && host == NULL) {
        if (!readModule(app->ucx, &t)) {
            ucxapp_print("ERROR: cannot read the module clock (AT+USYTU?)\n");
            return UCXAPP_ERR_FAILED;
        }
        apply(t, "module clock");
        return UCXAPP_OK;
    }
    return UCXAPP_ERR_USAGE;
}

static int cmdPush(ucxapp_t *app)
{
    uint8_t b[4];
    int64_t t = nowEpoch();
    char buf[32];

    if (app->ucx == NULL) {
        ucxapp_print("ERROR: not connected to a module\n");
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(t >> (24 - 8 * i));
    }
    int32_t r = uCxSystemSetUnixTime(app->ucx, b, (int32_t)sizeof(b));
    if (r != 0) {
        ucxapp_print("ERROR: cannot set the module clock (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    formatUtc(t, buf, sizeof(buf));
    ucxapp_print("module clock set to %s\n", buf);
    return UCXAPP_OK;
}

static int cmdSelftest(void)
{
    static const char kRsp[] = "HTTP/1.1 200 OK\r\nServer: x\r\ndate: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n";
    uint8_t ntp[48] = { 0x24, 2 };
    char buf[32];
    int64_t t = 0;

    ntp[40] = 0xEA;                    /* NTP 0xEA923124 = Unix 0x66E7B2A4 = 2024-09-16T04:23:00Z */
    ntp[41] = 0x92;
    ntp[42] = 0x31;
    ntp[43] = 0x24;
    formatUtc(0x66E7B2A4, buf, sizeof(buf));
    if (strcmp(buf, "2024-09-16T04:23:00Z") != 0) {
        ucxapp_print("ERROR: app.time selftest: format %s\n", buf);
        return UCXAPP_ERR_FAILED;
    }
    if (!parseTime("2024-09-16T04:23:00Z", &t) || t != 0x66E7B2A4 || !parseTime("1726460580", &t) ||
        t != 1726460580 || parseTime("2024-13-01T00:00:00", &t)) {
        ucxapp_print("ERROR: app.time selftest: parse\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!parseHttpDate(kRsp, &t) || t != 784111777) {
        ucxapp_print("ERROR: app.time selftest: HTTP date\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!parseNtp(ntp, sizeof(ntp), 0, &t) || t != 0x66E7B2A4) {
        ucxapp_print("ERROR: app.time selftest: SNTP\n");
        return UCXAPP_ERR_FAILED;
    }
    ntp[1] = 0;
    if (parseNtp(ntp, sizeof(ntp), 0, &t)) {
        ucxapp_print("ERROR: app.time selftest: kiss-of-death accepted\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("app.time selftest: format, parse, HTTP date and SNTP ok\n");
    return UCXAPP_OK;
}

/* app.time [sync [ntp|http|module] [host] | set <time> | push | selftest] */
static int cmdTime(ucxapp_t *app, int argc, char *argv[])
{
    char buf[32];
    int64_t t;

    if (argc >= 2 && strcmp(argv[1], "sync") == 0) {
        return cmdSync(app, argc, argv);
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        if (!parseTime(argv[2], &t)) {
            return UCXAPP_ERR_USAGE;
        }
        apply(t, "manual");
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "push") == 0) {
        return cmdPush(app);
    }
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        return cmdSelftest();
    }
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    formatUtc(nowEpoch(), buf, sizeof(buf));
    ucxapp_print("time    %s\n", buf);
    if (gSource[0] == '\0') {
        bool own = ucxapp_platform_clock_adjust(0);
        ucxapp_print("source  %s\n", own ? "none (time since boot; run app.time sync)" : "host clock");
    } else {
        ucxapp_print("source  %s, %u s ago\n", gSource, (unsigned)((ucxapp_platform_tick_ms() - gSyncTick) / 1000u));
        if (gOffset != 0) {
            ucxapp_print("offset  %lld s from the host clock\n", (long long)gOffset);
        }
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gTimeCmds[] = {
    { "app.time", "[sync [ntp|http|module] [host] | set <unix|YYYY-MM-DDTHH:MM:SS> | push | selftest]",
      "App wall clock in UTC: sync from SNTP (default pool.ntp.org, falls back to an HTTP Date header), HTTP or the module clock, set by hand, or push it to the module (AT+USYTU)",
      0, cmdTime },
};

void ucxapp_register_time_cmds(void)
{
    ucxapp_cmd_register(gTimeCmds, sizeof(gTimeCmds) / sizeof(gTimeCmds[0]));
}
