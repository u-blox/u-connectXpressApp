/* app.time - the app's wall clock: shows UTC, syncs from SNTP (RFC 4330), an HTTP Date header (RFC 9110) or the module
 * clock (AT+USYTU), can be set by hand and pushed to the module. On STM32 it sets the clock time() reads; on a PC the
 * OS keeps the clock and the app keeps the measured offset. app.time geo asks ip-api.com for the public IP, the place
 * and the time zone with its current UTC offset (daylight saving included); app.time sync refreshes it after that. */

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
#define GEO_HOST        "ip-api.com"
#define GEO_PATH        "/json/?fields=status,message,country,city,lat,lon,timezone,offset,query"
#define ZONE_MAX_S      (14 * 3600)

static int64_t gOffset;                /* added to time() when the OS owns the clock */
static char gSource[64] = "";
static uint32_t gSyncTick;
static bool gZoneSet, gZoneAuto;       /* auto: from app.time geo, refreshed by app.time sync */
static int32_t gZoneOffset;            /* seconds east of UTC, daylight saving included */
static char gZoneName[48];
static char gLocText[72];              /* last known place: "Malmo, Sweden (ip)", "55.60500, 13.00380 (wifi, 30 m)" */
static char gRsp[HTTP_BUF + 1];        /* HTTP answer for sync http and geo */

typedef struct {
    char ip[48], country[40], city[40], zone[48], message[40];
    double lat, lon;
    long offset;
} geo_t;

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

/* "2024-09-16 06:23:00": the wall clock in the time zone. */
static void formatLocal(int64_t t, char *out, size_t size)
{
    formatUtc(t + gZoneOffset, out, size);
    if (strlen(out) == 20) {
        out[10] = ' ';
        out[19] = '\0';
    }
}

/* "UTC+02:00", "UTC-05:30". */
static void formatZone(int32_t offset, char *out, size_t size)
{
    int32_t a = offset < 0 ? -offset : offset;

    snprintf(out, size, "UTC%c%02d:%02d", offset < 0 ? '-' : '+', (int)(a / 3600), (int)(a / 60 % 60));
}

/* "+2", "+02:00", "-5:30", "0" to seconds east of UTC. */
static bool parseZone(const char *s, int32_t *offset)
{
    char sign = *s;
    char *end;
    long m = 0;

    if (sign == '+' || sign == '-') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return false;
    }
    long h = strtol(s, &end, 10);
    if (*end == ':') {
        const char *ms = end + 1;
        if (*ms < '0' || *ms > '9') {
            return false;
        }
        m = strtol(ms, &end, 10);
        if (end - ms != 2 || m > 59 || m % 15 != 0) {
            return false;
        }
    }
    if (*end != '\0' || h > 14 || h * 3600 + m * 60 > ZONE_MAX_S) {
        return false;
    }
    int32_t v = (int32_t)(h * 3600 + m * 60);
    *offset = sign == '-' ? -v : v;
    return true;
}

/* The value after "key": in a flat JSON object, or NULL. */
static const char *jsonField(const char *json, const char *key)
{
    char pat[24];

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p != NULL ? p + strlen(pat) : NULL;
}

/* A JSON string value without quotes, backslashes, '|' (the state line separator) and control characters. */
static bool jsonText(const char *json, const char *key, char *out, size_t size)
{
    const char *p = jsonField(json, key);
    size_t n = 0;

    out[0] = '\0';
    if (p == NULL || *p != '"') {
        return false;
    }
    for (p++; *p != '\0' && *p != '"' && n + 1 < size; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        if ((unsigned char)*p >= 0x20 && *p != '\\' && *p != '|' && *p != '"') {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return true;
}

static bool jsonNum(const char *json, const char *key, double *v)
{
    const char *p = jsonField(json, key);
    char *end;

    if (p == NULL) {
        return false;
    }
    *v = strtod(p, &end);
    return end != p;
}

/* An ip-api.com answer: {"status":"success","country":..,"city":..,"lat":..,"lon":..,"timezone":..,"offset":..,
   "query":"<public ip>"} or {"status":"fail","message":..}. */
static bool parseGeo(const char *json, geo_t *g)
{
    char status[16];
    double offset;

    memset(g, 0, sizeof(*g));
    (void)jsonText(json, "message", g->message, sizeof(g->message));
    if (!jsonText(json, "status", status, sizeof(status)) || strcmp(status, "success") != 0 ||
        !jsonNum(json, "offset", &offset) || offset < -ZONE_MAX_S || offset > ZONE_MAX_S ||
        !jsonNum(json, "lat", &g->lat) || !jsonNum(json, "lon", &g->lon) ||
        g->lat < -90 || g->lat > 90 || g->lon < -180 || g->lon > 180) {
        return false;
    }
    g->offset = (long)offset;
    (void)jsonText(json, "query", g->ip, sizeof(g->ip));
    (void)jsonText(json, "country", g->country, sizeof(g->country));
    (void)jsonText(json, "city", g->city, sizeof(g->city));
    (void)jsonText(json, "timezone", g->zone, sizeof(g->zone));
    return true;
}

size_t ucxapp_time_summary(char *out, size_t size)
{
    char utc[32];
    int n;

    formatUtc(nowEpoch(), utc, sizeof(utc));
    if (gSource[0] != '\0') {
        n = snprintf(out, size, "Time %s | %s, %u s ago", utc, gSource,
                     (unsigned)((ucxapp_platform_tick_ms() - gSyncTick) / 1000u));
    } else {
        n = snprintf(out, size, "Time %s | %s", utc,
                     ucxapp_platform_clock_adjust(0) ? "unsynced (since boot)" : "host clock");
    }
    if (n > 0 && (size_t)n < size && gZoneSet) {
        char local[32], zone[16];
        formatLocal(nowEpoch(), local, sizeof(local));
        formatZone(gZoneOffset, zone, sizeof(zone));
        n += snprintf(out + n, size - (size_t)n, " | Local %s %s", local, zone);
    }
    if (n > 0 && (size_t)n < size) {
        char loc[80];
        ucxapp_time_location(loc, sizeof(loc));
        n += snprintf(out + n, size - (size_t)n, " | Loc %s", loc);
    }
    return n > 0 ? (size_t)n : 0;
}

void ucxapp_time_location(char *out, size_t size)
{
    double lat, lon;

    if (ucxapp_gnss_position(&lat, &lon)) {
        snprintf(out, size, "%.5f, %.5f (gnss)", lat, lon);
    } else {
        snprintf(out, size, "%s", gLocText[0] != '\0' ? gLocText : "unknown");
    }
}

void ucxapp_time_set_location(double lat, double lon, double accuracyM, const char *source)
{
    if (accuracyM > 0) {
        snprintf(gLocText, sizeof(gLocText), "%.5f, %.5f (%s, %.0f m)", lat, lon, source, accuracyM);
    } else {
        snprintf(gLocText, sizeof(gLocText), "%.5f, %.5f (%s)", lat, lon, source);
    }
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
                int32_t n = ucxapp_udp_recvfrom(ucx, sock, (int32_t)sizeof(pkt), pkt, &from);
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

/* Sends req to host port 80 and reads the reply into rsp until done(rsp) or WAIT_MS; the length read. */
static size_t httpRequest(uCxHandle_t *ucx, const char *host, const char *req, char *rsp, size_t size,
                          bool (*done)(const char *rsp))
{
    int32_t sock;
    size_t got = 0;
    int n = (int)strlen(req);

    rsp[0] = '\0';
    if (uCxSocketCreate1(ucx, U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        return 0;
    }
    ucxapp_events_clear(URC_FLAG_SOCK_CONNECTED);
    if (uCxSocketConnect(ucx, sock, host, 80) == 0 && ucxapp_events_wait(URC_FLAG_SOCK_CONNECTED, 10) &&
        uCxSocketWrite(ucx, sock, (const uint8_t *)req, n) == n) {
        uint32_t start = ucxapp_platform_tick_ms();
        while (got + 1 < size && ucxapp_platform_tick_ms() - start < WAIT_MS) {
            int32_t r = uCxSocketRead(ucx, sock, (int32_t)(size - 1 - got), (uint8_t *)rsp + got);
            if (r > 0) {
                got += (size_t)r;
                rsp[got] = '\0';
                if (done(rsp)) {
                    break;
                }
            } else {
                ucxapp_platform_sleep_ms(POLL_MS);
            }
        }
    }
    uCxSocketClose(ucx, sock);
    return got;
}

static bool headersRead(const char *rsp)
{
    return strstr(rsp, "\r\n\r\n") != NULL;
}

static bool jsonRead(const char *rsp)
{
    const char *body = strstr(rsp, "\r\n\r\n");
    return body != NULL && strchr(body, '}') != NULL;
}

static bool syncHttp(uCxHandle_t *ucx, const char *host, int64_t *t)
{
    char req[200];

    int n = snprintf(req, sizeof(req), "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (n <= 0 || n >= (int)sizeof(req)) {
        return false;
    }
    return httpRequest(ucx, host, req, gRsp, sizeof(gRsp), headersRead) > 0 && parseHttpDate(gRsp, t);
}

/* Public IP, place and time zone from ip-api.com (plain HTTP; the free service has no TLS). Sets the zone and the
   place for the clock. */
static bool geoLookup(uCxHandle_t *ucx, bool quiet)
{
    static const char kReq[] = "GET " GEO_PATH " HTTP/1.1\r\nHost: " GEO_HOST "\r\nConnection: close\r\n\r\n";
    geo_t g;

    memset(&g, 0, sizeof(g));
    if (httpRequest(ucx, GEO_HOST, kReq, gRsp, sizeof(gRsp), jsonRead) == 0) {
        if (!quiet) {
            ucxapp_print("ERROR: no answer from " GEO_HOST " (connect Wi-Fi first: wifi.connect)\n");
        }
        return false;
    }
    const char *body = strstr(gRsp, "\r\n\r\n");
    if (body == NULL || !parseGeo(body, &g)) {
        if (!quiet) {
            ucxapp_print("ERROR: " GEO_HOST " gave no place%s%s\n", g.message[0] != '\0' ? ": " : "", g.message);
        }
        return false;
    }
    gZoneSet = gZoneAuto = true;
    gZoneOffset = (int32_t)g.offset;
    snprintf(gZoneName, sizeof(gZoneName), "%s", g.zone);
    if (g.city[0] != '\0') {
        snprintf(gLocText, sizeof(gLocText), "%.40s%s%.20s (ip)", g.city, g.country[0] != '\0' ? ", " : "", g.country);
    } else {
        ucxapp_time_set_location(g.lat, g.lon, 0, "ip");
    }
    char zone[16], local[32];
    formatZone(gZoneOffset, zone, sizeof(zone));
    formatLocal(nowEpoch(), local, sizeof(local));
    if (!quiet) {
        ucxapp_print("public IP %s\n", g.ip[0] != '\0' ? g.ip : "?");
        ucxapp_print("place     %s, %s (%.4f, %.4f, city level)\n", g.city, g.country, g.lat, g.lon);
    }
    ucxapp_print("zone      %s, %s now, daylight saving included (" GEO_HOST ")\n", gZoneName, zone);
    if (!quiet) {
        ucxapp_print("local     %s\n", local);
    }
    return true;
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

static int cmdSyncOnce(ucxapp_t *app, int argc, char *argv[])
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

/* After a sync from the network the zone from app.time geo is asked again, so a daylight saving change shows up. */
static int cmdSync(ucxapp_t *app, int argc, char *argv[])
{
    int rc = cmdSyncOnce(app, argc, argv);

    if (rc == UCXAPP_OK && gZoneAuto && !(argc >= 3 && strcmp(argv[2], "module") == 0)) {
        (void)geoLookup(app->ucx, true);
    }
    return rc;
}

/* app.time zone [<+HH:MM>|off]: a fixed zone by hand (no daylight saving), or none. */
static int cmdZone(int argc, char *argv[])
{
    char zone[16], local[32];
    int32_t offset;

    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        gZoneSet = gZoneAuto = false;
        gZoneOffset = 0;
        gZoneName[0] = '\0';
        ucxapp_print("zone off: the clock shows UTC only\n");
        return UCXAPP_OK;
    }
    if (argc == 3) {
        if (!parseZone(argv[2], &offset)) {
            ucxapp_print("ERROR: zone %s: use +HH:MM, -HH:MM or off (app.time geo finds it)\n", argv[2]);
            return UCXAPP_ERR_USAGE;
        }
        gZoneSet = true;
        gZoneAuto = false;
        gZoneOffset = offset;
        snprintf(gZoneName, sizeof(gZoneName), "manual");
    } else if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gZoneSet) {
        ucxapp_print("zone none (app.time geo finds it from the public IP, or app.time zone +01:00)\n");
        return UCXAPP_OK;
    }
    formatZone(gZoneOffset, zone, sizeof(zone));
    formatLocal(nowEpoch(), local, sizeof(local));
    ucxapp_print("zone    %s %s%s\nlocal   %s\n", gZoneName, zone,
                 gZoneAuto ? ", refreshed by app.time sync" : ", fixed (no daylight saving)", local);
    return UCXAPP_OK;
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
    char summary[256];
    if (ucxapp_time_summary(summary, sizeof(summary)) >= sizeof(summary) ||
        strncmp(summary, "Time ", 5) != 0 || strchr(summary, '|') == NULL || strstr(summary, " | Loc ") == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    static const char kGeo[] = "{\"status\":\"success\",\"country\":\"Sweden\",\"city\":\"Malm\\\"o|x\",\"lat\":55.6059,"
                               "\"lon\":13.0007,\"timezone\":\"Europe/Stockholm\",\"offset\":7200,\"query\":\"192.0.2.7\"}";
    geo_t g;
    int32_t z = 0;
    formatZone(-19800, buf, sizeof(buf));
    if (!parseGeo(kGeo, &g) || g.offset != 7200 || strcmp(g.city, "Malmox") != 0 || strcmp(g.ip, "192.0.2.7") != 0 ||
        strcmp(g.zone, "Europe/Stockholm") != 0 || g.lat < 55.6 || g.lon > 13.01 ||
        parseGeo("{\"status\":\"fail\",\"message\":\"private range\"}", &g) || strcmp(g.message, "private range") != 0 ||
        strcmp(buf, "UTC-05:30") != 0) {
        ucxapp_print("ERROR: app.time selftest: geo\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!parseZone("+2", &z) || z != 7200 || !parseZone("-05:30", &z) || z != -19800 || !parseZone("5:45", &z) ||
        z != 20700 || parseZone("+2x", &z) || parseZone("15", &z) || parseZone("+1:5", &z) || parseZone("", &z) ||
        parseZone("+01:20", &z)) {
        ucxapp_print("ERROR: app.time selftest: zone\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("app.time selftest: format, parse, HTTP date, SNTP, geo and zone ok\n");
    return UCXAPP_OK;
}

/* app.time [sync [ntp|http|module] [host] | set <time> | push | geo | zone [<+HH:MM>|off] | selftest] */
static int cmdTime(ucxapp_t *app, int argc, char *argv[])
{
    char buf[32];
    int64_t t;

    if (argc >= 2 && strcmp(argv[1], "sync") == 0) {
        return cmdSync(app, argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "zone") == 0) {
        return cmdZone(argc, argv);
    }
    if (argc == 2 && strcmp(argv[1], "geo") == 0) {
        if (app->ucx == NULL) {
            ucxapp_print("ERROR: not connected to a module\n");
            return UCXAPP_ERR_NOT_CONNECTED;
        }
        return geoLookup(app->ucx, false) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
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
    if (gZoneSet) {
        char zone[16];
        formatZone(gZoneOffset, zone, sizeof(zone));
        formatLocal(nowEpoch(), buf, sizeof(buf));
        ucxapp_print("local   %s %s %s\n", buf, zone, gZoneName);
    }
    char loc[80];
    ucxapp_time_location(loc, sizeof(loc));
    ucxapp_print("place   %s\n", loc);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gTimeCmds[] = {
    { "app.time", "[sync [ntp|http|module] [host] | set <unix|YYYY-MM-DDTHH:MM:SS> | push | geo | zone [<+HH:MM>|off] | selftest]",
      "App wall clock (UTC): sync from SNTP (default pool.ntp.org, HTTP Date as fallback), HTTP or the module, set by hand, or push it to the module (AT+USYTU); geo: public IP, place and time zone with daylight saving from ip-api.com",
      0, cmdTime },
};

void ucxapp_register_time_cmds(void)
{
    ucxapp_cmd_register(gTimeCmds, sizeof(gTimeCmds) / sizeof(gTimeCmds[0]));
}
