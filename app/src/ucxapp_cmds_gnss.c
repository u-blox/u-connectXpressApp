/* GNSS receiver tools (host only): detect, monitor and configure a u-blox receiver on a serial port. */

#include "ucxapp_cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "u_port.h"
#include "u_port_uart.h"
#include "ucxapp_platform.h"
#include "ucxapp_stream.h"
#include "ucxapp_thread.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define MAX_SATS      128
#define LINE_MAX_LEN  160
#define UBX_MAX_LEN   2048
#define UNIT_MAX      (8 + UBX_MAX_LEN + 1)   /* one NMEA sentence or UBX frame of a recording, NUL-terminated */
#define USED_WINDOW_MS 3000u
#define FENCE_MAX_PTS 2000

enum { SYS_UNKNOWN, SYS_GPS, SYS_GLO, SYS_GAL, SYS_BDS, SYS_QZSS, SYS_NAVIC, SYS_SBAS, SYS_COUNT };
static const char *const kSysName[SYS_COUNT] = { "?", "GPS", "GLO", "GAL", "BDS", "QZSS", "NavIC", "SBAS" };

/* ---- Transport: where the receiver's byte stream comes from (UART now; I2C/SPI plug in here) ---- */

typedef struct {
    const char *name;
    void *(*open)(const char *port, int32_t baud);
    int32_t (*read)(void *h, uint8_t *buf, size_t size, int32_t timeoutMs);
    int32_t (*write)(void *h, const uint8_t *buf, size_t size);
    void (*close)(void *h);
} gnssTransport_t;

static void *uartOpen(const char *port, int32_t baud)
{
    return uPortUartOpen(port, baud, false);
}

static int32_t uartRead(void *h, uint8_t *buf, size_t size, int32_t timeoutMs)
{
    return uPortUartRead((uPortUartHandle_t)h, buf, size, timeoutMs);
}

static int32_t uartWrite(void *h, const uint8_t *buf, size_t size)
{
    return uPortUartWrite((uPortUartHandle_t)h, buf, size);
}

static void uartClose(void *h)
{
    uPortUartClose((uPortUartHandle_t)h);
}

static const gnssTransport_t kUart = { "UART", uartOpen, uartRead, uartWrite, uartClose };

/* ---- Receiver state ---- */

typedef struct {
    uint8_t sys;
    uint8_t id;
    int16_t elev;
    int16_t azim;
    uint8_t cno;      /* dBHz, 0 = not tracked */
    bool used;
} sat_t;

typedef struct {
    bool hasPos;
    int fixType;      /* 0 none, 1 dead reckoning, 2 2D, 3 3D */
    int quality;      /* NMEA GGA quality indicator */
    double lat, lon, alt, speed, course;   /* degrees, metres, m/s */
    double hdop, vdop, pdop;
    int satsUsed, satsView;
    int year, month, day, hour, min, sec;
    bool hasDate, hasTime;
    char sw[48], hw[16], mod[24], proto[16], ext[96];
    uint32_t nmea, ubx, bad;
    uint32_t posSeq;  /* counts position updates */
    uint32_t lastMs;
    sat_t sats[MAX_SATS];
    int nsat;
    uint32_t ubxSatMs;
    uint32_t usedStamp[SYS_COUNT][256];
} gnss_t;

enum { ST_IDLE, ST_NMEA, ST_SYNC2, ST_CLS, ST_ID, ST_LEN1, ST_LEN2, ST_PAY, ST_CKA, ST_CKB };

typedef struct {
    gnss_t st;
    int stage;
    char line[LINE_MAX_LEN];
    size_t nlen;
    uint8_t cls, id, ca, cb;
    uint32_t len, pos;
    uint8_t pay[UBX_MAX_LEN];
    uint8_t rxA;
} parser_t;

/* The live receiver (or replay); other parsers (gnss.export) neither log nor check the fence. */
static parser_t gP;

/* gnss.log <file> nmea|ubx: only valid NMEA sentences or UBX frames, files gnss.replay and u-center read. */
enum { LOG_NMEA = 1, LOG_UBX = 2 };
static FILE *gMsgLog;
static int gMsgLogKind;
static char gMsgLogName[200];

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

static int32_t s32(const uint8_t *p)
{
    return (int32_t)le32(p);
}

static int sysFromTalker(const char *t)
{
    if (strncmp(t, "GP", 2) == 0) return SYS_GPS;
    if (strncmp(t, "GL", 2) == 0) return SYS_GLO;
    if (strncmp(t, "GA", 2) == 0) return SYS_GAL;
    if (strncmp(t, "GB", 2) == 0 || strncmp(t, "BD", 2) == 0) return SYS_BDS;
    if (strncmp(t, "GQ", 2) == 0) return SYS_QZSS;
    if (strncmp(t, "GI", 2) == 0) return SYS_NAVIC;
    return SYS_UNKNOWN;
}

static int sysFromUbx(int gnssId)
{
    switch (gnssId) {
        case 0: return SYS_GPS;
        case 1: return SYS_SBAS;
        case 2: return SYS_GAL;
        case 3: return SYS_BDS;
        case 5: return SYS_QZSS;
        case 6: return SYS_GLO;
        case 7: return SYS_NAVIC;
        default: return SYS_UNKNOWN;
    }
}

static double coord(const char *v, const char *hemi)
{
    double x = atof(v);
    int deg = (int)(x / 100.0);
    double r = deg + (x - deg * 100.0) / 60.0;

    return (hemi[0] == 'S' || hemi[0] == 'W') ? -r : r;
}

static void setTime(gnss_t *s, const char *t)
{
    if (strlen(t) >= 6) {
        s->hour = (t[0] - '0') * 10 + (t[1] - '0');
        s->min = (t[2] - '0') * 10 + (t[3] - '0');
        s->sec = (t[4] - '0') * 10 + (t[5] - '0');
        s->hasTime = true;
    }
}

static void setDate(gnss_t *s, const char *d)
{
    if (strlen(d) >= 6) {
        s->day = (d[0] - '0') * 10 + (d[1] - '0');
        s->month = (d[2] - '0') * 10 + (d[3] - '0');
        s->year = 2000 + (d[4] - '0') * 10 + (d[5] - '0');
        s->hasDate = true;
    }
}

static void nmeaSentence(parser_t *p, char *body)
{
    gnss_t *s = &p->st;
    char *f[24];
    int n = 0;
    char *c = body;
    const char *type;

    f[n++] = c;
    for (; *c != '\0' && n < 24; c++) {
        if (*c == ',') {
            *c = '\0';
            f[n++] = c + 1;
        }
    }
    if (strlen(f[0]) < 5) {
        return;
    }
    type = f[0] + 2;
    for (int i = n; i < 24; i++) {
        f[i] = (char *)"";
    }

    if (strncmp(type, "GGA", 3) == 0) {
        s->quality = atoi(f[6]);
        setTime(s, f[1]);
        if (s->quality > 0) {
            s->lat = coord(f[2], f[3]);
            s->lon = coord(f[4], f[5]);
            s->alt = atof(f[9]);
            s->hasPos = true;
            s->posSeq++;
        }
        s->satsUsed = atoi(f[7]);
        s->hdop = atof(f[8]);
    } else if (strncmp(type, "RMC", 3) == 0) {
        setTime(s, f[1]);
        setDate(s, f[9]);
        if (f[2][0] == 'A') {
            s->lat = coord(f[3], f[4]);
            s->lon = coord(f[5], f[6]);
            s->speed = atof(f[7]) * 0.514444;
            s->course = atof(f[8]);
            s->hasPos = true;
            s->posSeq++;
        }
    } else if (strncmp(type, "GSA", 3) == 0) {
        int sys = (n > 18 && f[18][0] != '\0') ? atoi(f[18]) : sysFromTalker(f[0]);
        static const int kGsaSys[] = { SYS_UNKNOWN, SYS_GPS, SYS_GLO, SYS_GAL, SYS_BDS, SYS_QZSS, SYS_NAVIC };
        if (n > 18 && f[18][0] != '\0') {
            sys = (sys >= 0 && sys < 7) ? kGsaSys[sys] : SYS_UNKNOWN;
        }
        s->fixType = atoi(f[2]) >= 2 ? atoi(f[2]) : 0;   /* GSA mode 1 = no fix, 2 = 2D, 3 = 3D */
        for (int i = 3; i <= 14; i++) {
            int id = atoi(f[i]);
            if (id > 0 && id < 256) {
                s->usedStamp[sys][id] = ucxapp_platform_tick_ms();
            }
        }
        s->pdop = atof(f[15]);
        s->hdop = atof(f[16]);
        s->vdop = atof(f[17]);
    } else if (strncmp(type, "GSV", 3) == 0) {
        int sys = sysFromTalker(f[0]);
        int total = atoi(f[1]);
        int num = atoi(f[2]);
        int blocks = (n - 4) / 4;

        (void)total;
        if (s->ubxSatMs != 0 && ucxapp_platform_tick_ms() - s->ubxSatMs < USED_WINDOW_MS) {
            return;
        }
        if (num == 1) {
            int w = 0;
            for (int i = 0; i < s->nsat; i++) {
                if (s->sats[i].sys != sys) {
                    s->sats[w++] = s->sats[i];
                }
            }
            s->nsat = w;
        }
        for (int b = 0; b < blocks && s->nsat < MAX_SATS; b++) {
            sat_t *t = &s->sats[s->nsat++];
            t->sys = (uint8_t)sys;
            t->id = (uint8_t)atoi(f[4 + b * 4]);
            t->elev = (int16_t)atoi(f[5 + b * 4]);
            t->azim = (int16_t)atoi(f[6 + b * 4]);
            t->cno = (uint8_t)atoi(f[7 + b * 4]);
            t->used = false;
        }
        s->satsView = atoi(f[3]);
    }
}

static void nmeaLine(parser_t *p)
{
    char *star = strchr(p->line, '*');
    uint8_t x = 0;

    if (star == NULL || strlen(star) < 3) {
        p->st.bad++;
        return;
    }
    for (const char *c = p->line + 1; c < star; c++) {
        x ^= (uint8_t)*c;
    }
    if (x != (uint8_t)strtoul((char[3]){ star[1], star[2], '\0' }, NULL, 16)) {
        p->st.bad++;
        return;
    }
    if (p == &gP && gMsgLog != NULL && gMsgLogKind == LOG_NMEA) {
        fprintf(gMsgLog, "%s\r\n", p->line);
        fflush(gMsgLog);
    }
    *star = '\0';
    p->st.nmea++;
    p->st.lastMs = ucxapp_platform_tick_ms();
    nmeaSentence(p, p->line + 1);
}

static void ubxFrame(parser_t *p)
{
    gnss_t *s = &p->st;
    const uint8_t *d = p->pay;

    s->ubx++;
    s->lastMs = ucxapp_platform_tick_ms();
    if (p->cls == 0x01 && p->id == 0x07 && p->len >= 92) {            /* NAV-PVT */
        s->year = (int)le16(d + 4);
        s->month = d[6];
        s->day = d[7];
        s->hour = d[8];
        s->min = d[9];
        s->sec = d[10];
        s->hasDate = (d[11] & 0x01) != 0;
        s->hasTime = (d[11] & 0x02) != 0;
        s->fixType = d[20];
        s->satsUsed = d[23];
        s->lon = s32(d + 24) * 1e-7;
        s->lat = s32(d + 28) * 1e-7;
        s->alt = s32(d + 36) * 1e-3;
        s->speed = s32(d + 60) * 1e-3;
        s->course = s32(d + 64) * 1e-5;
        s->pdop = (double)le16(d + 76) * 0.01;
        s->hasPos = (d[21] & 0x01) != 0 && s->fixType >= 2;
        s->posSeq += s->hasPos ? 1u : 0u;
    } else if (p->cls == 0x01 && p->id == 0x35 && p->len >= 8) {    /* NAV-SAT */
        uint32_t count = d[5];
        s->nsat = 0;
        for (uint32_t i = 0; i < count && 8 + 12 * (i + 1) <= p->len && s->nsat < MAX_SATS; i++) {
            const uint8_t *e = d + 8 + 12 * i;
            sat_t *t = &s->sats[s->nsat++];
            t->sys = (uint8_t)sysFromUbx(e[0]);
            t->id = e[1];
            t->cno = e[2];
            t->elev = (int16_t)(int8_t)e[3];
            t->azim = (int16_t)le16(e + 4);
            t->used = (le32(e + 8) & 0x08u) != 0;
        }
        s->satsView = s->nsat;
        s->ubxSatMs = ucxapp_platform_tick_ms();
    } else if (p->cls == 0x0A && p->id == 0x04 && p->len >= 40) {   /* MON-VER */
        snprintf(s->sw, sizeof(s->sw), "%.30s", (const char *)d);
        snprintf(s->hw, sizeof(s->hw), "%.10s", (const char *)d + 30);
        s->ext[0] = '\0';
        for (uint32_t o = 40; o + 30 <= p->len; o += 30) {
            const char *e = (const char *)d + o;
            size_t used = strlen(s->ext);
            /* u-blox 7 and older write "PROTVER 14.00", later ones "PROTVER=18.00". */
            if (strncmp(e, "MOD=", 4) == 0) {
                snprintf(s->mod, sizeof(s->mod), "%.20s", e + 4);
            } else if (strncmp(e, "PROTVER", 7) == 0 && (e[7] == '=' || e[7] == ' ')) {
                snprintf(s->proto, sizeof(s->proto), "%.12s", e + 8);
            } else if (e[0] != '\0' && used + 4 < sizeof(s->ext)) {
                snprintf(s->ext + used, sizeof(s->ext) - used, "%s%.30s", used > 0 ? ", " : "", e);
            }
        }
    }
}

#define CK(p, b) do { (p)->ca = (uint8_t)((p)->ca + (b)); (p)->cb = (uint8_t)((p)->cb + (p)->ca); } while (0)

static void feedByte(parser_t *p, uint8_t b)
{
    switch (p->stage) {
        case ST_IDLE:
            if (b == '$') {
                p->stage = ST_NMEA;
                p->nlen = 0;
                p->line[p->nlen++] = '$';
            } else if (b == 0xB5) {
                p->stage = ST_SYNC2;
            }
            break;
        case ST_NMEA:
            if (b == '\n') {
                p->line[p->nlen] = '\0';
                nmeaLine(p);
                p->stage = ST_IDLE;
            } else if (b == '\r') {
                break;
            } else if (b == '$') {
                p->nlen = 0;
                p->line[p->nlen++] = '$';
            } else if (p->nlen < sizeof(p->line) - 1) {
                p->line[p->nlen++] = (char)b;
            } else {
                p->stage = ST_IDLE;
            }
            break;
        case ST_SYNC2:
            p->ca = 0;
            p->cb = 0;
            p->stage = (b == 0x62) ? ST_CLS : ST_IDLE;
            break;
        case ST_CLS:
            p->cls = b;
            CK(p, b);
            p->stage = ST_ID;
            break;
        case ST_ID:
            p->id = b;
            CK(p, b);
            p->stage = ST_LEN1;
            break;
        case ST_LEN1:
            p->len = b;
            CK(p, b);
            p->stage = ST_LEN2;
            break;
        case ST_LEN2:
            p->len |= (uint32_t)b << 8;
            CK(p, b);
            p->pos = 0;
            if (p->len > UBX_MAX_LEN) {
                p->stage = ST_IDLE;
            } else {
                p->stage = (p->len == 0) ? ST_CKA : ST_PAY;
            }
            break;
        case ST_PAY:
            p->pay[p->pos++] = b;
            CK(p, b);
            if (p->pos == p->len) {
                p->stage = ST_CKA;
            }
            break;
        case ST_CKA:
            p->rxA = b;
            p->stage = ST_CKB;
            break;
        case ST_CKB:
            if (p->rxA == p->ca && b == p->cb) {
                if (p == &gP && gMsgLog != NULL && gMsgLogKind == LOG_UBX) {
                    const uint8_t h[6] = { 0xB5, 0x62, p->cls, p->id, (uint8_t)p->len, (uint8_t)(p->len >> 8) };
                    const uint8_t ck[2] = { p->ca, p->cb };
                    fwrite(h, 1, sizeof(h), gMsgLog);
                    fwrite(p->pay, 1, p->len, gMsgLog);
                    fwrite(ck, 1, sizeof(ck), gMsgLog);
                    fflush(gMsgLog);
                }
                ubxFrame(p);
            } else {
                p->st.bad++;
            }
            p->stage = ST_IDLE;
            break;
        default:
            p->stage = ST_IDLE;
            break;
    }
}

/* ---- Geofence: a polygon from a KML file (Google My Maps, Google Earth); alerts when the position leaves it ---- */

typedef struct {
    int n;
    double lat[FENCE_MAX_PTS];
    double lon[FENCE_MAX_PTS];
    char name[64];
    int state;        /* -1 no position yet, 0 outside, 1 inside */
    unsigned exits;
} fence_t;

static fence_t gFence;

/* Ray casting; fine for fields and gardens, where the earth is flat enough. */
static bool fenceInside(const fence_t *f, double lat, double lon)
{
    bool in = false;

    for (int i = 0, j = f->n - 1; i < f->n; j = i++) {
        if ((f->lat[i] > lat) != (f->lat[j] > lat) &&
            lon < (f->lon[j] - f->lon[i]) * (lat - f->lat[i]) / (f->lat[j] - f->lat[i]) + f->lon[i]) {
            in = !in;
        }
    }
    return in;
}

static void fenceCheck(const gnss_t *s)
{
    int in;

    if (gFence.n == 0 || !s->hasPos) {
        return;
    }
    in = fenceInside(&gFence, s->lat, s->lon) ? 1 : 0;
    if (in == gFence.state) {
        return;
    }
    if (in == 0) {
        gFence.exits++;
        ucxapp_print("FENCE: outside %s at %.7f, %.7f\n", gFence.name, s->lat, s->lon);
    } else if (gFence.state == 0) {
        ucxapp_print("FENCE: back inside %s\n", gFence.name);
    }
    gFence.state = in;
}

static void feed(parser_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint32_t seq = p->st.posSeq;
        feedByte(p, data[i]);
        if (p == &gP && p->st.posSeq != seq) {
            fenceCheck(&p->st);
        }
    }
}

/* ---- Sending ---- */

/* Builds a UBX frame in f (room for 8 + 64 bytes); returns its size, 0 if the payload is too long. */
static size_t buildFrame(uint8_t *f, uint8_t cls, uint8_t id, const uint8_t *pl, size_t len)
{
    uint8_t a = 0;
    uint8_t b = 0;

    if (len > 64) {
        return 0;
    }
    f[0] = 0xB5;
    f[1] = 0x62;
    f[2] = cls;
    f[3] = id;
    f[4] = (uint8_t)len;
    f[5] = (uint8_t)(len >> 8);
    if (len > 0) {
        memcpy(f + 6, pl, len);
    }
    for (size_t i = 2; i < 6 + len; i++) {
        a = (uint8_t)(a + f[i]);
        b = (uint8_t)(b + a);
    }
    f[6 + len] = a;
    f[7 + len] = b;
    return 8 + len;
}

static bool sendFrame(const gnssTransport_t *tr, void *h, uint8_t cls, uint8_t id, const uint8_t *pl, size_t len)
{
    uint8_t f[8 + 64];
    size_t n = buildFrame(f, cls, id, pl, len);

    return n > 0 && tr->write(h, f, n) == (int32_t)n;
}

/* ---- Detection ---- */

static const int32_t kBauds[] = { 9600, 38400, 115200, 460800, 921600 };

/* Opens the port at baud, polls MON-VER and listens briefly; true if valid NMEA or UBX arrived. */
static bool probe(const char *port, int32_t baud, parser_t *p)
{
    void *h = kUart.open(port, baud);
    uint8_t buf[256];
    uint32_t start;

    if (h == NULL) {
        return false;
    }
    memset(p, 0, sizeof(*p));
    (void)sendFrame(&kUart, h, 0x0A, 0x04, NULL, 0);
    start = ucxapp_platform_tick_ms();
    while (ucxapp_platform_tick_ms() - start < 300u) {
        int32_t n = kUart.read(h, buf, sizeof(buf), 10);
        if (n > 0) {
            feed(p, buf, (size_t)n);
        }
        if (p->st.sw[0] != '\0' || (p->st.nmea + p->st.ubx > 0 && ucxapp_platform_tick_ms() - start > 150u)) {
            break;
        }
    }
    kUart.close(h);
    return p->st.nmea + p->st.ubx > 0;
}

/* Moves a receiver that runs at a slow UART rate to 115200 baud (RAM only, so a power cycle restores it). */
static void raiseBaud(const char *port, int32_t *baud, parser_t *p)
{
    static const int32_t kFast = 115200;
    const uint8_t pl[12] = { 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x52, 0x40,
                             (uint8_t)kFast, (uint8_t)(kFast >> 8), (uint8_t)(kFast >> 16), 0x00 };
    void *h;

    if (*baud >= kFast || strtol(p->st.proto, NULL, 10) < 23) {
        return;
    }
    h = kUart.open(port, *baud);
    if (h == NULL) {
        return;
    }
    (void)sendFrame(&kUart, h, 0x06, 0x8A, pl, sizeof(pl));
    ucxapp_platform_sleep_ms(100);
    kUart.close(h);
    if (probe(port, kFast, p)) {
        *baud = kFast;
    }
}

/* Receiver generation from the MON-VER hardware version. */
static const char *generation(const char *hw)
{
    static const struct {
        const char *hw;
        const char *name;
    } kGen[] = {
        { "00040005", "u-blox 5" }, { "00040006", "u-blox 6" }, { "00040007", "u-blox 6" },
        { "00070000", "u-blox 7" }, { "00080000", "u-blox M8" }, { "00190000", "u-blox F9" },
        { "000A0000", "u-blox M10" }, { "00200000", "u-blox F10" },
    };

    for (size_t i = 0; i < sizeof(kGen) / sizeof(kGen[0]); i++) {
        if (strcmp(hw, kGen[i].hw) == 0) {
            return kGen[i].name;
        }
    }
    return "";
}

static void describe(const parser_t *p)
{
    const gnss_t *s = &p->st;
    const char *gen = generation(s->hw);

    ucxapp_print("%s%s%s", s->nmea > 0 ? "NMEA" : "", (s->nmea > 0 && s->ubx > 0) ? " + " : "", s->ubx > 0 ? "UBX" : "");
    if (gen[0] != '\0' || s->mod[0] != '\0') {
        ucxapp_print(", %s%s%s", gen, gen[0] != '\0' && s->mod[0] != '\0' ? " " : "", s->mod);
    }
    if (s->hw[0] != '\0') {
        ucxapp_print(", HW %s", s->hw);
    }
    if (s->sw[0] != '\0') {
        ucxapp_print(", FW %s", s->sw);
    }
    if (s->proto[0] != '\0') {
        ucxapp_print(", PROTVER %s", s->proto);
    }
    if (s->ext[0] != '\0') {
        ucxapp_print(", %s", s->ext);
    }
}

typedef struct {
    parser_t p;
    int32_t baud;
} portProbe_t;

static bool probeAllBauds(const char *port, void *result)
{
    portProbe_t *r = (portProbe_t *)result;

    for (size_t b = 0; b < sizeof(kBauds) / sizeof(kBauds[0]); b++) {
        if (probe(port, kBauds[b], &r->p)) {
            r->baud = kBauds[b];
            return true;
        }
    }
    return false;
}

/* Scans one port (or all candidate ports at once). Writes the best hit to outPort/outBaud when given. */
static int detect(const char *only, char *outPort, size_t outPortSize, int32_t *outBaud)
{
    static ucxapp_serial_port_t ports[32];
    static parser_t p;
    static portProbe_t results[32];
    static char names[32][32];
    int map[32];
    bool hits[32];
    int count = ucxapp_platform_list_serial_ports(ports, 32);
    int n = 0;
    int found = 0;
    int best = -1;
    char lastPort[32];
    int32_t lastBaud = 0;

    if (only == NULL && ucxapp_last_port_load("gnss", lastPort, sizeof(lastPort), &lastBaud)) {
        for (int i = 0; i < count; i++) {
            if (strcasecmp(ports[i].name, lastPort) == 0 && ports[i].available) {
                ucxapp_print("  %s (last known): ", lastPort);
                if (probe(lastPort, lastBaud, &p)) {
                    ucxapp_print("GNSS at %d baud: ", (int)lastBaud);
                    describe(&p);
                    ucxapp_print("\n");
                    if (outPort != NULL) {
                        snprintf(outPort, outPortSize, "%s", lastPort);
                        *outBaud = lastBaud;
                    }
                    return 1;
                }
                ucxapp_print("no GNSS data\n");
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (only != NULL ? strcasecmp(ports[i].name, only) == 0 : !ucxapp_port_skip(&ports[i])) {
            snprintf(names[n], sizeof(names[n]), "%.31s", ports[i].name);
            map[n++] = i;
        }
    }
    ucxapp_stream_probe_ports((const char (*)[32])names, n, probeAllBauds, results, sizeof(results[0]), hits);

    for (int k = 0; k < n; k++) {
        const ucxapp_serial_port_t *sp = &ports[map[k]];
        if (!hits[k]) {
            continue;
        }
        ucxapp_print("  %s%s%s%s: ", sp->name, sp->desc[0] ? " (" : "", sp->desc, sp->desc[0] ? ")" : "");
        ucxapp_print("GNSS at %d baud: ", (int)results[k].baud);
        describe(&results[k].p);
        ucxapp_print("\n");
        if (best < 0 || (ucxapp_port_is_gnss(sp) && !ucxapp_port_is_gnss(&ports[map[best]]))) {
            best = k;
        }
        found++;
    }
    if (best >= 0) {
        int32_t was = results[best].baud;
        raiseBaud(names[best], &results[best].baud, &results[best].p);
        if (results[best].baud != was) {
            ucxapp_print("  %s switched to %d baud\n", names[best], (int)results[best].baud);
        }
        ucxapp_last_port_save("gnss", names[best], results[best].baud);
        if (outPort != NULL) {
            snprintf(outPort, outPortSize, "%s", names[best]);
            *outBaud = results[best].baud;
        }
    }
    return found;
}

/* ---- Commands ---- */

static ucxapp_stream_t *gStream;
static gnss_t gSnap;
static uint32_t gOpenMs;
static uint32_t gFixMs;   /* first 2D/3D fix since gnss.open, 0 = none yet */

static void onData(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    feed(&gP, data, n);
}

/* ---- Replay: a virtual receiver that plays an NMEA or UBX recording (gnss.log, u-center, nmeagen.org) in real time ---- */

enum { CLOCK_NONE, CLOCK_NMEA, CLOCK_UBX };

typedef struct {
    FILE *f;
    char name[200];
    double speed;
    uint8_t unit[UNIT_MAX];   /* next sentence or frame to send; len 0 = none read yet */
    size_t len;
    double epoch;        /* NMEA: UTC seconds of day, UBX: NAV-PVT time of week in seconds */
    bool haveEpoch;
    int clock;           /* the first time source seen paces the replay */
    uint32_t due;
    uint32_t loops;
} replay_t;

static replay_t gReplay;

/* UTC seconds of day of an RMC, GGA, GNS or ZDA sentence; -1 for other lines. */
static double sentenceTime(const char *line)
{
    static const char *const kTimed[] = { "RMC", "GGA", "GNS", "ZDA" };
    const char *v = line + 7;
    bool timed = false;

    if (line[0] != '$' || strlen(line) < 13 || line[6] != ',') {
        return -1.0;
    }
    for (size_t i = 0; i < sizeof(kTimed) / sizeof(kTimed[0]); i++) {
        timed = timed || strncmp(line + 3, kTimed[i], 3) == 0;
    }
    for (int i = 0; i < 6 && timed; i++) {
        timed = v[i] >= '0' && v[i] <= '9';
    }
    if (!timed) {
        return -1.0;
    }
    return ((v[0] - '0') * 10 + (v[1] - '0')) * 3600.0 + ((v[2] - '0') * 10 + (v[3] - '0')) * 60.0 + atof(v + 4);
}

/* Next NMEA sentence (ending in \r\n) or UBX frame of a recording, NUL-terminated; other bytes are skipped. */
static bool readUnit(FILE *f, uint8_t *u, size_t *len)
{
    int c;

    while ((c = getc(f)) != EOF) {
        if (c == '$') {
            size_t n = 0;
            u[n++] = '$';
            while ((c = getc(f)) != EOF && c != '\n') {
                if (c != '\r' && n < 510) {
                    u[n++] = (uint8_t)c;
                }
            }
            u[n++] = '\r';
            u[n++] = '\n';
            u[n] = '\0';
            *len = n;
            return true;
        }
        if (c == 0xB5) {
            size_t pl;
            c = getc(f);
            if (c != 0x62) {
                if (c != EOF) {
                    (void)ungetc(c, f);
                }
                continue;
            }
            u[0] = 0xB5;
            u[1] = 0x62;
            if (fread(u + 2, 1, 4, f) != 4) {
                return false;
            }
            pl = le16(u + 4);
            if (pl > UBX_MAX_LEN) {
                continue;
            }
            if (fread(u + 6, 1, pl + 2, f) != pl + 2) {
                return false;
            }
            *len = pl + 8;
            u[*len] = '\0';
            return true;
        }
    }
    return false;
}

/* Epoch time of a unit in seconds (see replay_t.epoch), -1 when it carries none; clock tells which kind. */
static double unitTime(const uint8_t *u, size_t len, int *clock)
{
    if (u[0] == '$') {
        *clock = CLOCK_NMEA;
        return sentenceTime((const char *)u);
    }
    *clock = CLOCK_UBX;
    return (u[2] == 0x01 && u[3] == 0x07 && len >= 12) ? le32(u + 6) / 1000.0 : -1.0;
}

/* Next unit of the file, from the start again at the end; false when the file has none. */
static bool replayRead(replay_t *r)
{
    for (int tries = 0; tries < 2; tries++) {
        if (readUnit(r->f, r->unit, &r->len)) {
            return true;
        }
        rewind(r->f);
        r->loops++;
    }
    r->len = 0;
    return false;
}

/* Sends every unit of one epoch at once, then waits for the time the next epoch carries. */
static void replayPoll(void *ctx, ucxapp_stream_t *s)
{
    replay_t *r = ctx;
    uint32_t now = ucxapp_platform_tick_ms();

    for (int i = 0; i < 200 && (int32_t)(now - r->due) >= 0; i++) {
        int clock;
        if (r->len == 0 && !replayRead(r)) {
            return;
        }
        double t = unitTime(r->unit, r->len, &clock);
        if (t >= 0.0 && r->clock == CLOCK_NONE) {
            r->clock = clock;
        }
        if (clock != r->clock) {
            t = -1.0;
        }
        if (t >= 0.0 && r->haveEpoch && t != r->epoch) {
            double dt = t - r->epoch;
            if (dt < 0.0) {
                dt += r->clock == CLOCK_UBX ? 604800.0 : 86400.0;
            }
            if (dt > 10.0) {   /* next lap of the file, or a gap in the recording */
                dt = 1.0;
            }
            r->epoch = t;
            r->due = now + (uint32_t)(dt * 1000.0 / r->speed);
            continue;
        }
        if (t >= 0.0) {
            r->epoch = t;
            r->haveEpoch = true;
        }
        ucxapp_stream_push(s, r->unit, r->len);
        r->len = 0;
    }
}

/* Commands to the replayed receiver (gnss.send, gnss.rate, ...) are accepted and dropped. */
static bool replayWrite(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    (void)data;
    (void)n;
    return true;
}

/* ---- Virtual receiver: a fixed position (the host's location, or u-blox Malmo) as a normal NMEA stream ---- */

#define MALMO_LAT 55.6125   /* u-blox AB, Malmo office */
#define MALMO_LON 12.9925
#define MALMO_ALT 10.0

typedef struct {
    bool on;
    double lat, lon, alt;
    char source[40];
    uint32_t due;
} virt_t;

static virt_t gVirt;

static void pushNmea(ucxapp_stream_t *s, const char *body)
{
    char line[128];
    uint8_t x = 0;

    for (const char *c = body; *c != '\0'; c++) {
        x ^= (uint8_t)*c;
    }
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, x);
    if (n > 0 && (size_t)n < sizeof(line)) {
        ucxapp_stream_push(s, (const uint8_t *)line, (size_t)n);
    }
}

/* ddmm.mmmmm,N or dddmm.mmmmm,E */
static void nmeaCoord(char *out, size_t size, double v, bool isLat)
{
    double a = fabs(v);
    int deg = (int)a;
    double min = (a - deg) * 60.0;

    if (min >= 59.999995) {
        deg++;
        min = 0.0;
    }
    snprintf(out, size, isLat ? "%02d%08.5f,%c" : "%03d%08.5f,%c", deg, min,
             isLat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E'));
}

/* One epoch a second: RMC, GGA, GSA and GSV with ten GPS satellites, stamped with the host's UTC time. */
static void virtPoll(void *ctx, ucxapp_stream_t *s)
{
    static const int kSv[10][4] = {   /* PRN, elevation, azimuth, C/N0 */
        { 2, 48, 62, 44 }, { 5, 30, 145, 40 }, { 12, 67, 250, 47 }, { 15, 22, 305, 36 }, { 18, 55, 80, 45 },
        { 20, 12, 190, 31 }, { 24, 40, 10, 42 }, { 25, 75, 120, 48 }, { 29, 35, 220, 41 }, { 31, 18, 340, 33 }
    };
    virt_t *v = ctx;
    uint32_t now = ucxapp_platform_tick_ms();
    char body[100];
    char lat[24];
    char lon[24];
    char hms[16];
    char dmy[8];
    time_t t = time(NULL);
    const struct tm *tm = gmtime(&t);

    if ((int32_t)(now - v->due) < 0 || tm == NULL) {
        return;
    }
    v->due = now + 1000u;
    nmeaCoord(lat, sizeof(lat), v->lat, true);
    nmeaCoord(lon, sizeof(lon), v->lon, false);
    strftime(hms, sizeof(hms), "%H%M%S.00", tm);
    strftime(dmy, sizeof(dmy), "%d%m%y", tm);
    snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%s,0.0,0.0,%s,,,A", hms, lat, lon, dmy);
    pushNmea(s, body);
    snprintf(body, sizeof(body), "GPGGA,%s,%s,%s,1,10,0.8,%.1f,M,0.0,M,,", hms, lat, lon, v->alt);
    pushNmea(s, body);
    snprintf(body, sizeof(body), "GPGSA,A,3,%02d,%02d,%02d,%02d,%02d,%02d,%02d,%02d,%02d,%02d,,,1.4,0.8,1.1",
             kSv[0][0], kSv[1][0], kSv[2][0], kSv[3][0], kSv[4][0], kSv[5][0], kSv[6][0], kSv[7][0], kSv[8][0],
             kSv[9][0]);
    pushNmea(s, body);
    for (int m = 0; m < 3; m++) {
        int n = snprintf(body, sizeof(body), "GPGSV,3,%d,10", m + 1);
        for (int i = m * 4; i < m * 4 + 4 && i < 10 && n > 0 && (size_t)n < sizeof(body); i++) {
            n += snprintf(body + n, sizeof(body) - (size_t)n, ",%02d,%02d,%03d,%02d", kSv[i][0], kSv[i][1],
                          kSv[i][2], kSv[i][3]);
        }
        pushNmea(s, body);
    }
}

static bool needOpen(void)
{
    if (gStream == NULL) {
        ucxapp_print("ERROR: no receiver open (use gnss.open)\n");
        return false;
    }
    return true;
}

/* Dashboard text such as "GNSS 3D 12sv"; empty when no receiver is open. */
size_t ucxapp_gnss_summary(char *out, size_t size)
{
    static const char *const kFix[] = { "no fix", "DR", "2D", "3D" };
    ucxapp_stream_t *s = gStream;

    out[0] = '\0';
    if (s == NULL) {
        return 0;
    }
    ucxapp_stream_lock(s);
    int fix = gP.st.fixType & 3;
    int used = gP.st.satsUsed;
    uint32_t last = gP.st.lastMs;
    ucxapp_stream_unlock(s);
    bool stalled = last == 0 || ucxapp_platform_tick_ms() - last > 3000u;
    int n = snprintf(out, size, "GNSS %s", stalled ? "no data" : kFix[fix]);
    if (!stalled && fix > 0 && n > 0 && (size_t)n < size) {
        n += snprintf(out + n, size - (size_t)n, " %dsv", used);
    }
    if (ucxapp_stream_bridge_active(s) && n > 0 && (size_t)n < size) {
        n += snprintf(out + n, size - (size_t)n, " bridged");
    }
    return n > 0 ? (size_t)n : 0;
}

static void snapshot(void)
{
    ucxapp_stream_lock(gStream);
    memcpy(&gSnap, &gP.st, sizeof(gSnap));
    ucxapp_stream_unlock(gStream);
    if (gFixMs == 0 && gSnap.fixType >= 2 && gSnap.hasPos) {
        gFixMs = ucxapp_platform_tick_ms();
        gFixMs += gFixMs == 0 ? 1u : 0u;
    }
}

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("Scanning serial ports for a GNSS receiver (all ports at once)...\n");
    int n = detect(argc == 2 ? argv[1] : NULL, NULL, 0, NULL);
    ucxapp_print("%d receiver(s) found\n", n);
    return n > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    char port[32];
    int32_t baud = 0;
    uint8_t frame[8];
    size_t n;
    ucxapp_stream_cfg_t cfg = { "gnss", onData, NULL, 0, NULL, NULL };
    (void)app;

    if (argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &baud) || baud < 300))) {
        return UCXAPP_ERR_USAGE;
    }
    if (gStream != NULL) {
        if (argc == 1 || strcmp(argv[1], ucxapp_stream_port(gStream)) == 0) {
            ucxapp_print("GNSS receiver already open on %s\n", ucxapp_stream_port(gStream));
            return UCXAPP_OK;
        }
        ucxapp_print("ERROR: %s is already open (gnss.close first)\n", ucxapp_stream_port(gStream));
        return UCXAPP_ERR_FAILED;
    }
    if (argc >= 2) {
        snprintf(port, sizeof(port), "%s", argv[1]);
    }
    if (argc < 3) {
        ucxapp_print("Detecting...\n");
        if (detect(argc == 2 ? argv[1] : NULL, port, sizeof(port), &baud) == 0) {
            ucxapp_print("ERROR: no GNSS receiver found\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    memset(&gP, 0, sizeof(gP));
    gFence.state = -1;
    gOpenMs = ucxapp_platform_tick_ms();
    gFixMs = 0;
    gStream = ucxapp_stream_open(port, baud, &cfg);
    if (gStream == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", port);
        return UCXAPP_ERR_FAILED;
    }
    n = buildFrame(frame, 0x0A, 0x04, NULL, 0);
    (void)ucxapp_stream_write(gStream, frame, n);
    ucxapp_print("GNSS receiver open on %s at %d baud\n", port, (int)baud);
    return UCXAPP_OK;
}

static int cmdClose(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stream_close(gStream);
    gStream = NULL;
    if (gReplay.f != NULL) {
        fclose(gReplay.f);
        gReplay.f = NULL;
    }
    gVirt.on = false;
    if (gMsgLog != NULL) {
        fclose(gMsgLog);
        gMsgLog = NULL;
    }
    ucxapp_print("closed\n");
    return UCXAPP_OK;
}

static bool satUsed(const gnss_t *s, const sat_t *t, uint32_t now)
{
    return t->used || (t->sys < SYS_COUNT && s->usedStamp[t->sys][t->id] != 0 &&
                       now - s->usedStamp[t->sys][t->id] < USED_WINDOW_MS);
}

static void fenceStatus(void)
{
    static const char *const kState[] = { "no position yet", "outside", "inside" };

    if (gFence.n == 0) {
        ucxapp_print("Fence:     off\n");
        return;
    }
    ucxapp_print("Fence:     %s, %d points, %s, left it %u time(s)\n", gFence.name, gFence.n,
                 kState[gFence.state + 1], gFence.exits);
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kFix[] = { "no fix", "dead reckoning", "2D", "3D" };
    uint32_t now = ucxapp_platform_tick_ms();
    int used = 0;
    (void)app;
    (void)argc;
    (void)argv;

    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    snapshot();
    for (int i = 0; i < gSnap.nsat; i++) {
        used += satUsed(&gSnap, &gSnap.sats[i], now) ? 1 : 0;
    }
    if (gReplay.f != NULL) {
        ucxapp_print("Replay:    %s at %gx, lap %u, %u bytes sent\n", gReplay.name, gReplay.speed,
                     (unsigned)gReplay.loops + 1, (unsigned)ucxapp_stream_rx_bytes(gStream));
    } else if (gVirt.on) {
        ucxapp_print("Virtual:   %s at %.7f, %.7f, %u bytes sent\n", gVirt.source, gVirt.lat, gVirt.lon,
                     (unsigned)ucxapp_stream_rx_bytes(gStream));
    } else {
        ucxapp_print("Port:      %s at %d baud, %u bytes received\n", ucxapp_stream_port(gStream),
                     (int)ucxapp_stream_baud(gStream), (unsigned)ucxapp_stream_rx_bytes(gStream));
    }
    ucxapp_print("Messages:  %u NMEA, %u UBX, %u bad%s\n", (unsigned)gSnap.nmea, (unsigned)gSnap.ubx, (unsigned)gSnap.bad,
                 gSnap.lastMs == 0 ? " (no data yet)" : (now - gSnap.lastMs > 3000u ? " (stalled)" : ""));
    if (gSnap.mod[0] != '\0' || gSnap.sw[0] != '\0') {
        ucxapp_print("Receiver:  %s %s, FW %s, HW %s, PROTVER %s\n", generation(gSnap.hw), gSnap.mod,
                     gSnap.sw, gSnap.hw, gSnap.proto);
        if (gSnap.ext[0] != '\0') {
            ucxapp_print("           %s\n", gSnap.ext);
        }
    }
    ucxapp_print("Fix:       %s%s\n", kFix[gSnap.fixType & 3], gSnap.hasPos ? "" : " (no position)");
    if (gSnap.hasPos) {
        ucxapp_print("Position:  %.7f, %.7f  alt %.1f m\n", gSnap.lat, gSnap.lon, gSnap.alt);
        ucxapp_print("Motion:    %.2f m/s, course %.1f deg\n", gSnap.speed, gSnap.course);
    }
    if (gSnap.hasTime) {
        ucxapp_print("Time:      %04d-%02d-%02d %02d:%02d:%02d UTC\n", gSnap.hasDate ? gSnap.year : 0,
                     gSnap.month, gSnap.day, gSnap.hour, gSnap.min, gSnap.sec);
    }
    ucxapp_print("Satellites: %d used of %d tracked; HDOP %.1f VDOP %.1f PDOP %.1f\n",
                 gSnap.satsUsed > 0 ? gSnap.satsUsed : used, gSnap.nsat, gSnap.hdop, gSnap.vdop, gSnap.pdop);
    ucxapp_print("Log:       %s\n", gMsgLog != NULL ? gMsgLogName :
                 (ucxapp_stream_log_name(gStream)[0] ? ucxapp_stream_log_name(gStream) : "off"));
    fenceStatus();
    ucxapp_print("Bridge:    ");
    ucxapp_stream_bridge_status(gStream);
    return UCXAPP_OK;
}

static int cmdSats(ucxapp_t *app, int argc, char *argv[])
{
    uint32_t now = ucxapp_platform_tick_ms();
    int shown = 0;
    (void)app;
    (void)argc;
    (void)argv;

    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    snapshot();
    ucxapp_print("Sys   PRN  Elev  Azim  C/N0\n");
    for (int sys = 1; sys < SYS_COUNT; sys++) {
        for (int i = 0; i < gSnap.nsat; i++) {
            const sat_t *t = &gSnap.sats[i];
            char bar[24];
            int len = t->cno / 3;

            if (t->sys != sys) {
                continue;
            }
            len = len > 20 ? 20 : len;
            memset(bar, '#', (size_t)len);
            bar[len] = '\0';
            ucxapp_print("%-5s %3d  %4d  %4d  %3d  %-20s %s\n", kSysName[sys], t->id, t->elev, t->azim, t->cno, bar,
                         satUsed(&gSnap, t, now) ? "used" : "");
            shown++;
        }
    }
    ucxapp_print("%d satellite(s)\n", shown);
    return UCXAPP_OK;
}

#define U8_FULL  "\xE2\x96\x88"
#define U8_LIGHT "\xE2\x96\x91"
#define U8_DOT   "\xE2\x97\x8F"
#define U8_RING  "\xE2\x97\x8B"
#define U8_HLINE "\xE2\x95\x90"
#define U8_VLINE "\xE2\x95\x91"
#define CARD_SATS 16

/* HDOP 1..5 bars: <1 ideal, <2 excellent, <5 good, <10 moderate, else poor. */
static int hdopLevel(double h)
{
    return h <= 0.0 ? 0 : h < 1.0 ? 5 : h < 2.0 ? 4 : h < 5.0 ? 3 : h < 10.0 ? 2 : 1;
}

static void countSats(uint32_t now, int *used, int *seen)
{
    *used = 0;
    *seen = 0;
    for (int i = 0; i < gSnap.nsat; i++) {
        if (gSnap.sats[i].cno > 0) {
            (*seen)++;
            *used += satUsed(&gSnap, &gSnap.sats[i], now) ? 1 : 0;
        }
    }
    if (gSnap.satsUsed > *used) {
        *used = gSnap.satsUsed;
    }
    if (*seen < *used) {
        *seen = *used;
    }
}

bool ucxapp_gnss_dash(char *out, size_t size)
{
    static const char *const kFix[] = { "no fix", "DR", "2D", "3D" };
    uint32_t now = ucxapp_platform_tick_ms();
    int used;
    int seen;

    out[0] = '\0';
    if (gStream == NULL) {
        return false;
    }
    snapshot();
    if (gSnap.lastMs == 0 || now - gSnap.lastMs > 3000u) {
        snprintf(out, size, "no data on %s", ucxapp_stream_port(gStream));
        return true;
    }
    countSats(now, &used, &seen);
    snprintf(out, size, "%s  %d/%d sats  HDOP %.1f", kFix[gSnap.fixType & 3], used, seen, gSnap.hdop);
    return true;
}

static void printCard(void)
{
    static const char *const kFix[] = { "no fix", "dead reckoning", "2D", "3D" };
    static const char *const kLevel[] = { "?", "poor", "moderate", "good", "excellent", "ideal" };
    static const char *const kBar[] = { "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x85",
                                        "\xE2\x96\x87" };
    uint32_t now = ucxapp_platform_tick_ms();
    int fix = gSnap.fixType & 3;
    int used;
    int seen;
    int level = hdopLevel(gSnap.hdop);

    countSats(now, &used, &seen);
    ucxapp_print(U8_HLINE U8_HLINE U8_HLINE " GNSS ");
    for (int i = 0; i < 30; i++) {
        ucxapp_print(U8_HLINE);
    }
    ucxapp_print("\n");
    if (gSnap.lastMs == 0 || now - gSnap.lastMs > 3000u) {
        ucxapp_print(U8_VLINE " no data from the receiver on %s\n", ucxapp_stream_port(gStream));
    } else {
        ucxapp_print(U8_VLINE " FIX   %s%s%s %-14s", fix >= 1 ? U8_FULL : U8_LIGHT, fix >= 2 ? U8_FULL : U8_LIGHT,
                     fix >= 3 ? U8_FULL : U8_LIGHT, kFix[fix]);
        if (gFixMs != 0) {
            ucxapp_print(" TTFF %u s\n", (unsigned)((gFixMs - gOpenMs) / 1000u));
        } else {
            ucxapp_print(" searching %u s\n", (unsigned)((now - gOpenMs) / 1000u));
        }
        ucxapp_print(U8_VLINE " SATS  ");
        for (int i = 0; i < seen && i < CARD_SATS; i++) {
            ucxapp_print(i < used ? U8_DOT : U8_RING);
        }
        ucxapp_print("%s %d used / %d seen\n", seen > CARD_SATS ? "+" : "", used, seen);
        ucxapp_print(U8_VLINE " HDOP  %.1f ", gSnap.hdop);
        for (int i = 0; i < 5; i++) {
            ucxapp_print(i < level ? kBar[i] : " ");
        }
        ucxapp_print(" %s\n", kLevel[level]);
        if (gSnap.hasPos) {
            ucxapp_print(U8_VLINE " POS   %.7f %c  %.7f %c  alt %.1f m\n", fabs(gSnap.lat), gSnap.lat < 0 ? 'S' : 'N',
                         fabs(gSnap.lon), gSnap.lon < 0 ? 'W' : 'E', gSnap.alt);
        }
        if (gSnap.hasTime) {
            ucxapp_print(U8_VLINE " TIME  %04d-%02d-%02d %02d:%02d:%02d UTC\n", gSnap.hasDate ? gSnap.year : 0,
                         gSnap.month, gSnap.day, gSnap.hour, gSnap.min, gSnap.sec);
        }
    }
    for (int i = 0; i < 39; i++) {
        ucxapp_print(U8_HLINE);
    }
    ucxapp_print("\n");
}

/* gnss.watch [seconds]: the card once a second; any key stops it early. */
static int cmdWatch(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 10;
    (void)app;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &seconds) || seconds < 1 || seconds > 3600))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    for (int32_t s = 0; s < seconds; s++) {
        snapshot();
        printCard();
        if (s + 1 == seconds) {
            break;
        }
        for (int t = 0; t < 20; t++) {
            if (ucxapp_platform_key_pressed()) {
                (void)ucxapp_platform_get_key();
                return UCXAPP_OK;
            }
            ucxapp_platform_sleep_ms(50);
        }
    }
    return UCXAPP_OK;
}

static int cmdLog(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    bool off = argc >= 2 && strcasecmp(argv[1], "off") == 0;
    int kind = argc != 3 ? 0 : strcasecmp(argv[2], "nmea") == 0 ? LOG_NMEA : strcasecmp(argv[2], "ubx") == 0 ? LOG_UBX : 0;

    if (argc < 2 || argc > 3 || (argc == 3 && (kind == 0 || off))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    FILE *f = (off || kind == 0) ? NULL : fopen(argv[1], "wb");
    if ((kind != 0 && f == NULL) || !ucxapp_stream_log(gStream, (off || kind != 0) ? NULL : argv[1])) {
        ucxapp_print("ERROR: cannot create %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stream_lock(gStream);
    if (gMsgLog != NULL) {
        fclose(gMsgLog);
    }
    gMsgLog = f;
    gMsgLogKind = kind;
    snprintf(gMsgLogName, sizeof(gMsgLogName), "%s (%s)", argv[1], kind == LOG_UBX ? "UBX" : "NMEA");
    ucxapp_stream_unlock(gStream);
    if (off) {
        ucxapp_print("logging stopped\n");
    } else if (kind == LOG_NMEA) {
        ucxapp_print("logging NMEA sentences to %s (gnss.replay plays it back)\n", argv[1]);
    } else if (kind == LOG_UBX) {
        ucxapp_print("logging UBX frames to %s (gnss.replay and u-center play it back)\n", argv[1]);
    } else {
        ucxapp_print("logging raw receiver data to %s\n", argv[1]);
    }
    return UCXAPP_OK;
}

static int cmdReplay(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_cfg_t cfg = { "gnss", onData, replayPoll, 10, &gReplay, replayWrite };
    unsigned sentences = 0;
    unsigned frames = 0;
    unsigned epochs = 0;
    double last = -1.0;
    double speed = 1.0;
    char *end = NULL;
    char ubx[32] = "";
    int clock = CLOCK_NONE;
    size_t len;
    (void)app;

    if (argc < 2 || argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3) {
        speed = strtod(argv[2], &end);
        if (end == argv[2] || *end != '\0' || !(speed >= 0.1 && speed <= 100.0)) {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (gStream != NULL) {
        ucxapp_print("ERROR: %s is already open (gnss.close first)\n", ucxapp_stream_port(gStream));
        return UCXAPP_ERR_FAILED;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    while (readUnit(f, gReplay.unit, &len)) {
        int c;
        double t = unitTime(gReplay.unit, len, &c);
        if (gReplay.unit[0] == '$') {
            sentences++;
        } else {
            frames++;
        }
        if (t >= 0.0 && clock == CLOCK_NONE) {
            clock = c;
        }
        if (t >= 0.0 && c == clock && t != last) {
            epochs++;
            last = t;
        }
    }
    if (sentences == 0 && frames == 0) {
        fclose(f);
        ucxapp_print("ERROR: no NMEA sentences or UBX frames in %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    rewind(f);
    memset(&gReplay, 0, sizeof(gReplay));
    gReplay.f = f;
    gReplay.speed = speed;
    snprintf(gReplay.name, sizeof(gReplay.name), "%s", argv[1]);
    memset(&gP, 0, sizeof(gP));
    gFence.state = -1;
    gOpenMs = ucxapp_platform_tick_ms();
    gFixMs = 0;
    gStream = ucxapp_stream_open("virt:replay", 0, &cfg);
    if (gStream == NULL) {
        fclose(f);
        gReplay.f = NULL;
        ucxapp_print("ERROR: cannot start the replay\n");
        return UCXAPP_ERR_FAILED;
    }
    if (frames > 0) {
        snprintf(ubx, sizeof(ubx), "%u UBX frames, ", frames);
    }
    ucxapp_print("Replaying %s: %s%u sentences, %u epochs at %gx, from the start again at the end (gnss.close stops)\n",
                 argv[1], ubx, sentences, epochs, speed);
    return UCXAPP_OK;
}

static bool parseDouble(const char *s, double *out, double lo, double hi)
{
    char *end = NULL;
    *out = strtod(s, &end);
    return end != s && *end == '\0' && *out >= lo && *out <= hi;
}

static int cmdVirtual(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_cfg_t cfg = { "gnss", onData, virtPoll, 50, &gVirt, replayWrite };
    double lat = MALMO_LAT;
    double lon = MALMO_LON;
    double alt = MALMO_ALT;
    const char *source = "u-blox Malmo office";
    bool host = argc == 1 || (argc == 2 && strcasecmp(argv[1], "host") == 0);

    if (argc == 2 && strcasecmp(argv[1], "off") == 0) {
        if (!gVirt.on) {
            ucxapp_print("ERROR: no virtual receiver is running\n");
            return UCXAPP_ERR_FAILED;
        }
        return cmdClose(app, 1, argv);
    }
    if (argc == 3 || argc == 4) {
        alt = 0.0;
        if (!parseDouble(argv[1], &lat, -90.0, 90.0) || !parseDouble(argv[2], &lon, -180.0, 180.0) ||
            (argc == 4 && !parseDouble(argv[3], &alt, -1000.0, 10000.0))) {
            return UCXAPP_ERR_USAGE;
        }
        source = "fixed position";
    } else if (!host && !(argc == 2 && strcasecmp(argv[1], "malmo") == 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (gStream != NULL) {
        ucxapp_print("ERROR: %s is already open (gnss.close first)\n", ucxapp_stream_port(gStream));
        return UCXAPP_ERR_FAILED;
    }
    if (host) {
        ucxapp_print("Asking the host for its location...\n");
        if (ucxapp_platform_location(&lat, &lon, &alt)) {
            source = "host location";
        } else {
            ucxapp_print("note: no location from the host (location service off or missing), using u-blox in Malmo\n");
        }
    }
    memset(&gP, 0, sizeof(gP));
    gFence.state = -1;
    gOpenMs = ucxapp_platform_tick_ms();
    gFixMs = 0;
    memset(&gVirt, 0, sizeof(gVirt));
    gVirt.lat = lat;
    gVirt.lon = lon;
    gVirt.alt = alt;
    snprintf(gVirt.source, sizeof(gVirt.source), "%s", source);
    gVirt.due = ucxapp_platform_tick_ms();
    gVirt.on = true;
    gStream = ucxapp_stream_open("virt:gnss", 0, &cfg);
    if (gStream == NULL) {
        gVirt.on = false;
        ucxapp_print("ERROR: cannot start the virtual receiver\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Virtual receiver at %.7f, %.7f (%s): NMEA once a second; every gnss.* command and bridge works "
                 "on it (gnss.close stops)\n", lat, lon, source);
    return UCXAPP_OK;
}

/* The text between "<tag>" and "</tag>" (CDATA unwrapped, trimmed) into out. */
static void xmlTagText(const char *start, const char *tag, char *out, size_t size)
{
    char open[24];
    char close[24];
    const char *b;
    const char *e;

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    out[0] = '\0';
    b = strstr(start, open);
    e = b != NULL ? strstr(b, close) : NULL;
    if (e == NULL) {
        return;
    }
    b += strlen(open);
    if (strncmp(b, "<![CDATA[", 9) == 0) {
        b += 9;
        e = (e - b >= 3 && strncmp(e - 3, "]]>", 3) == 0) ? e - 3 : e;
    }
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) {
        b++;
    }
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        e--;
    }
    snprintf(out, size, "%.*s", (int)(e - b), b);
}

/* First polygon of a KML file (the first line string when there is none); false with the reason printed. */
static bool fenceLoad(const char *file, fence_t *f)
{
    FILE *in = fopen(file, "rb");
    char *text = NULL;
    char *shape;
    char *coords;
    char *end;
    char *c;
    long size;

    if (in == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", file);
        return false;
    }
    if (fseek(in, 0, SEEK_END) != 0 || (size = ftell(in)) <= 0 || size > 8L * 1024 * 1024 ||
        fseek(in, 0, SEEK_SET) != 0 || (text = malloc((size_t)size + 1)) == NULL) {
        fclose(in);
        ucxapp_print("ERROR: %s is empty or too big\n", file);
        return false;
    }
    size = (long)fread(text, 1, (size_t)size, in);
    fclose(in);
    text[size] = '\0';
    shape = strstr(text, "<Polygon");
    shape = shape != NULL ? shape : strstr(text, "<LineString");
    coords = strstr(shape != NULL ? shape : text, "<coordinates>");
    end = coords != NULL ? strstr(coords, "</coordinates>") : NULL;
    if (end == NULL) {
        free(text);
        ucxapp_print("ERROR: no polygon in %s (draw one in Google My Maps and export it as KML)\n", file);
        return false;
    }
    *end = '\0';
    /* the area's name is the last <name> before its coordinates (the Placemark's) */
    f->name[0] = '\0';
    for (char *n = strstr(text, "<name>"); n != NULL && n < coords; n = strstr(n + 6, "<name>")) {
        char save = *coords;
        *coords = '\0';
        xmlTagText(n, "name", f->name, sizeof(f->name));
        *coords = save;
    }
    if (f->name[0] == '\0') {
        snprintf(f->name, sizeof(f->name), "fence");
    }
    f->n = 0;
    c = coords + 13;
    while (f->n < FENCE_MAX_PTS) {
        char *e;
        double lon = strtod(c, &e);
        if (e == c || *e != ',') {
            break;
        }
        c = e + 1;
        double lat = strtod(c, &e);
        if (e == c || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            break;
        }
        c = e;
        if (*c == ',') {
            (void)strtod(c + 1, &e);
            c = e;
        }
        f->lat[f->n] = lat;
        f->lon[f->n] = lon;
        f->n++;
    }
    free(text);
    if (f->n > 1 && f->lat[0] == f->lat[f->n - 1] && f->lon[0] == f->lon[f->n - 1]) {
        f->n--;   /* KML repeats the first point to close the ring */
    }
    if (f->n < 3) {
        ucxapp_print("ERROR: the area in %s needs at least 3 points\n", file);
        return false;
    }
    return true;
}

static int cmdFence(ucxapp_t *app, int argc, char *argv[])
{
    fence_t *f;
    (void)app;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "status") == 0) {
        fenceStatus();
        return UCXAPP_OK;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        if (gStream != NULL) {
            ucxapp_stream_lock(gStream);
        }
        gFence.n = 0;
        if (gStream != NULL) {
            ucxapp_stream_unlock(gStream);
        }
        ucxapp_print("fence off\n");
        return UCXAPP_OK;
    }
    f = malloc(sizeof(*f));
    if (f == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    if (!fenceLoad(argv[1], f)) {
        free(f);
        return UCXAPP_ERR_FAILED;
    }
    f->state = -1;
    f->exits = 0;
    if (gStream != NULL) {
        ucxapp_stream_lock(gStream);
    }
    memcpy(&gFence, f, sizeof(gFence));
    if (gStream != NULL) {
        ucxapp_stream_unlock(gStream);
    }
    free(f);
    ucxapp_print("fence '%s': %d points from %s; FENCE: lines report leaving and coming back\n", gFence.name, gFence.n,
                 argv[1]);
    return UCXAPP_OK;
}

static void xmlEscaped(FILE *out, const char *s)
{
    for (; *s != '\0'; s++) {
        switch (*s) {
            case '&': fputs("&amp;", out); break;
            case '<': fputs("&lt;", out); break;
            case '>': fputs("&gt;", out); break;
            case '"': fputs("&quot;", out); break;
            default: fputc(*s, out); break;
        }
    }
}

static unsigned exportPoint(FILE *out, const gnss_t *s, bool gpx)
{
    if (!s->hasPos) {
        return 0;
    }
    if (!gpx) {
        fprintf(out, "%.7f,%.7f,%.1f\n", s->lon, s->lat, s->alt);
        return 1;
    }
    fprintf(out, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", s->lat, s->lon, s->alt);
    if (s->hasDate && s->hasTime) {
        fprintf(out, "<time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>", s->year, s->month, s->day, s->hour, s->min,
                s->sec);
    }
    fprintf(out, "</trkpt>\n");
    return 1;
}

/* gnss.export <recording> <out.kml|out.gpx>: the track of an NMEA or UBX recording, one point per epoch. */
static int cmdExport(ucxapp_t *app, int argc, char *argv[])
{
    size_t olen = argc == 3 ? strlen(argv[2]) : 0;
    bool gpx = olen > 4 && strcasecmp(argv[2] + olen - 4, ".gpx") == 0;
    bool kml = olen > 4 && strcasecmp(argv[2] + olen - 4, ".kml") == 0;
    parser_t *p;
    uint8_t *u;
    FILE *in;
    FILE *out;
    size_t len;
    double epoch = -1.0;
    int clock = CLOCK_NONE;
    unsigned points = 0;
    (void)app;

    if (argc != 3 || (!gpx && !kml)) {
        return UCXAPP_ERR_USAGE;
    }
    in = fopen(argv[1], "rb");
    if (in == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    out = fopen(argv[2], "wb");
    if (out == NULL) {
        fclose(in);
        ucxapp_print("ERROR: cannot create %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    p = calloc(1, sizeof(*p));
    u = malloc(UNIT_MAX);
    if (p == NULL || u == NULL) {
        free(p);
        free(u);
        fclose(in);
        fclose(out);
        return UCXAPP_ERR_FAILED;
    }
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", out);
    if (gpx) {
        fputs("<gpx version=\"1.1\" creator=\"u-connectXpressApp\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
              "<trk><name>", out);
        xmlEscaped(out, argv[1]);
        fputs("</name><trkseg>\n", out);
    } else {
        fputs("<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark><name>", out);
        xmlEscaped(out, argv[1]);
        fputs("</name>\n<LineString><tessellate>1</tessellate><coordinates>\n", out);
    }
    while (readUnit(in, u, &len)) {
        int c;
        double t = unitTime(u, len, &c);
        if (t >= 0.0 && clock == CLOCK_NONE) {
            clock = c;
        }
        if (t >= 0.0 && c == clock && t != epoch) {
            if (epoch >= 0.0) {
                points += exportPoint(out, &p->st, gpx);
            }
            epoch = t;
            p->st.hasPos = false;
        }
        feed(p, u, len);
    }
    if (epoch >= 0.0) {
        points += exportPoint(out, &p->st, gpx);
    }
    fputs(gpx ? "</trkseg></trk></gpx>\n" : "</coordinates></LineString></Placemark></Document></kml>\n", out);
    fclose(out);
    fclose(in);
    free(p);
    free(u);
    if (points == 0) {
        (void)remove(argv[2]);
        ucxapp_print("ERROR: no positions in %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%u points written to %s (open it in Google Earth, Google My Maps or any GPX tool)\n", points, argv[2]);
    return UCXAPP_OK;
}

static size_t parseHexBytes(char *const *parts, int count, uint8_t *out, size_t max)
{
    size_t n = 0;
    int hi = -1;

    for (int i = 0; i < count; i++) {
        for (const char *c = parts[i]; *c != '\0'; c++) {
            int v;
            if (*c >= '0' && *c <= '9') {
                v = *c - '0';
            } else if ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'f') {
                v = (*c | 0x20) - 'a' + 10;
            } else {
                return 0;
            }
            if (hi < 0) {
                hi = v;
            } else {
                if (n == max) {
                    return 0;
                }
                out[n++] = (uint8_t)(hi << 4 | v);
                hi = -1;
            }
        }
    }
    return hi < 0 ? n : 0;
}

static int cmdSend(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t buf[8 + 64];
    size_t n = 0;
    (void)app;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (argv[1][0] == '$') {
        char body[120];
        char line[132];
        uint8_t x = 0;
        char *star;

        snprintf(body, sizeof(body), "%s", argv[1] + 1);
        star = strchr(body, '*');
        if (star != NULL) {
            *star = '\0';
        }
        for (const char *c = body; *c != '\0'; c++) {
            x ^= (uint8_t)*c;
        }
        n = (size_t)snprintf(line, sizeof(line), "$%s*%02X\r\n", body, x);
        ucxapp_print("sending %.*s\n", (int)n - 2, line);
        return ucxapp_stream_write(gStream, (const uint8_t *)line, n) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcasecmp(argv[1], "ubx") == 0) {
        uint8_t pl[64];
        size_t plen = 0;

        if (argc < 4 || argc > 5) {
            return UCXAPP_ERR_USAGE;
        }
        if (argc == 5 && (plen = parseHexBytes(&argv[4], 1, pl, sizeof(pl))) == 0) {
            return UCXAPP_ERR_USAGE;
        }
        n = buildFrame(buf, (uint8_t)strtoul(argv[2], NULL, 16), (uint8_t)strtoul(argv[3], NULL, 16), pl, plen);
    } else {
        n = parseHexBytes(&argv[1], argc - 1, buf, sizeof(buf));
    }
    if (n == 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!ucxapp_stream_write(gStream, buf, n)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent %u byte(s)\n", (unsigned)n);
    return UCXAPP_OK;
}

static int cmdRestart(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t pl[4] = { 0, 0, 0x02, 0 };   /* navBbrMask, resetMode: controlled GNSS-only reset */
    uint8_t buf[8 + 4];
    (void)app;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[1], "hot") == 0) {
        pl[0] = 0x00;
    } else if (strcasecmp(argv[1], "warm") == 0) {
        pl[0] = 0x01;
    } else if (strcasecmp(argv[1], "cold") == 0) {
        pl[0] = 0xFF;
        pl[1] = 0xFF;
    } else {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    size_t n = buildFrame(buf, 0x06, 0x04, pl, sizeof(pl));
    ucxapp_print("%s start requested\n", argv[1]);
    gOpenMs = ucxapp_platform_tick_ms();
    gFixMs = 0;
    ucxapp_stream_lock(gStream);
    gP.st.fixType = 0;
    gP.st.hasPos = false;
    ucxapp_stream_unlock(gStream);
    return ucxapp_stream_write(gStream, buf, n) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdRate(ucxapp_t *app, int argc, char *argv[])
{
    int32_t hz;
    uint8_t pl[6];
    uint8_t buf[8 + 6];
    (void)app;

    if (argc != 2 || !ucxapp_parse_int(argv[1], &hz) || hz < 1 || hz > 25) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    pl[0] = (uint8_t)(1000 / hz);
    pl[1] = (uint8_t)((1000 / hz) >> 8);
    pl[2] = 1;
    pl[3] = 0;
    pl[4] = 1;   /* GPS time */
    pl[5] = 0;
    size_t n = buildFrame(buf, 0x06, 0x08, pl, sizeof(pl));
    ucxapp_print("navigation rate set to %d Hz (not saved to flash)\n", (int)hz);
    return ucxapp_stream_write(gStream, buf, n) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port;

    if (argc >= 2 && strcasecmp(argv[1], "ntrip") == 0) {
        bool ble = false;
        bool cell = false;
        int n = argc;
        for (int i = 0; i < 2 && n > 5; i++) {
            if (!ble && strcasecmp(argv[n - 1], "ble") == 0) {
                ble = true;
                n--;
            } else if (!cell && strcasecmp(argv[n - 1], "cell") == 0) {
                cell = true;
                n--;
            }
        }
        if ((n != 5 && n != 7) || !ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535) {
            return UCXAPP_ERR_USAGE;
        }
        if (!needOpen()) {
            return UCXAPP_ERR_FAILED;
        }
        const char *user = n == 7 ? argv[5] : "";
        const char *pass = n == 7 ? argv[6] : "";
        if (cell) {
            char cport[32];
            int32_t cbaud;
            if (!ucxapp_cell_data_link(cport, sizeof(cport), &cbaud) &&
                (ucxapp_cell_connect(app, argv[2], port) != UCXAPP_OK ||
                 !ucxapp_cell_data_link(cport, sizeof(cport), &cbaud))) {
                ucxapp_print("ERROR: no cellular data link (open the modem with cell.open and attach with cell.attach on)\n");
                return UCXAPP_ERR_FAILED;
            }
            return ucxapp_stream_bridge_ntrip_uart(gStream, app, cport, cbaud, argv[4], user, pass, 10, ble)
                       ? UCXAPP_OK : UCXAPP_ERR_FAILED;
        }
        return ucxapp_stream_bridge_ntrip(gStream, app, argv[2], port, argv[4], user, pass, 10, ble)
                   ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    return ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 2947);   /* gpsd's port */
}

static const ucxapp_cmd_t kCmds[] = {
    { "gnss.detect", "[port]", "Find a GNSS receiver on the serial ports and report its baud rate", 0, cmdDetect },
    { "gnss.open", "[port] [baud]", "Open a receiver (detects port and baud when omitted)", 0, cmdOpen },
    { "gnss.close", "", "Close the receiver", 0, cmdClose },
    { "gnss.status", "", "Fix, position, time, satellites and counters", 0, cmdStatus },
    { "gnss.sats", "", "Satellites in view with elevation, azimuth and signal strength", 0, cmdSats },
    { "gnss.watch", "[seconds]", "Live card once a second (fix, satellites, HDOP, position, time to first fix); default 10 s, any key stops", 0, cmdWatch },
    { "gnss.log", "<file|off> [nmea|ubx]", "Record the raw receiver stream (NMEA/UBX) to a file; nmea or ubx keeps only valid NMEA sentences or UBX frames, for gnss.replay and u-center", 0, cmdLog },
    { "gnss.replay", "<file.nmea|file.ubx> [speed]", "Virtual receiver: play an NMEA or UBX recording (gnss.log, u-center, nmeagen.org) in real time or at speed 0.1 to 100, looping; every gnss.* command and bridge works on it", 0, cmdReplay },
    { "gnss.virtual", "[host|malmo|<lat> <lon> [alt]|off]", "Virtual receiver at the host's location (Windows Location, GeoClue on Linux; u-blox Malmo when unknown), u-blox Malmo or a given position: NMEA once a second for bridges and tests", 0, cmdVirtual },
    { "gnss.fence", "<file.kml|off|status>", "Geofence from the first polygon of a KML file (Google My Maps, Google Earth); prints FENCE: lines when the position leaves or comes back", 0, cmdFence },
    { "gnss.export", "<recording> <out.kml|out.gpx>", "Track of an NMEA or UBX recording as KML (Google Earth, My Maps) or GPX, one point per epoch", 0, cmdExport },
    { "gnss.send", "<hex..|$NMEA|ubx cls id [hex]>", "Send raw bytes, an NMEA sentence or a UBX frame", 0, cmdSend },
    { "gnss.restart", "<hot|warm|cold>", "Restart the receiver (UBX-CFG-RST)", 0, cmdRestart },
    { "gnss.rate", "<hz>", "Set the navigation rate, 1 to 25 Hz (UBX-CFG-RATE)", 0, cmdRate },
    { "gnss.bridge", "<tcp [port]|tls [port] [cert] [clientauth]|tcpc <host> <port>|ntrip <host> <port> <mount> [user pass] [ble] [cell]|udp <host> <port> [local]|udps <port>|ble [nus|sps|custom <uuid> [rx tx]]|uart <port> [baud]|cell|off|status>", "Two-way bridge of the receiver stream (TCP server/client, NTRIP corrections, UDP, BLE NUS/SPS, PC UART)", 0, cmdBridge },
};

void ucxapp_register_gnss_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
