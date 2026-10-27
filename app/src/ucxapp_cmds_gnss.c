/* GNSS receiver tools (host only): detect, monitor and configure a u-blox receiver on a serial port. */

#include "ucxapp_cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define USED_WINDOW_MS 3000u

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

static void feed(parser_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        feedByte(p, data[i]);
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
static parser_t gP;
static gnss_t gSnap;
static uint32_t gOpenMs;
static uint32_t gFixMs;   /* first 2D/3D fix since gnss.open, 0 = none yet */

static void onData(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    feed(&gP, data, n);
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
    ucxapp_print("closed\n");
    return UCXAPP_OK;
}

static bool satUsed(const gnss_t *s, const sat_t *t, uint32_t now)
{
    return t->used || (t->sys < SYS_COUNT && s->usedStamp[t->sys][t->id] != 0 &&
                       now - s->usedStamp[t->sys][t->id] < USED_WINDOW_MS);
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
    ucxapp_print("Port:      %s at %d baud, %u bytes received\n", ucxapp_stream_port(gStream),
                 (int)ucxapp_stream_baud(gStream), (unsigned)ucxapp_stream_rx_bytes(gStream));
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
    ucxapp_print("Log:       %s\n", ucxapp_stream_log_name(gStream)[0] ? ucxapp_stream_log_name(gStream) : "off");
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
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    bool off = strcasecmp(argv[1], "off") == 0;
    if (!ucxapp_stream_log(gStream, off ? NULL : argv[1])) {
        ucxapp_print("ERROR: cannot create %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print(off ? "logging stopped\n" : "logging raw receiver data to %s\n", argv[1]);
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
        bool ble = argc > 5 && strcasecmp(argv[argc - 1], "ble") == 0;
        int n = argc - (ble ? 1 : 0);
        if ((n != 5 && n != 7) || !ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535) {
            return UCXAPP_ERR_USAGE;
        }
        if (!needOpen()) {
            return UCXAPP_ERR_FAILED;
        }
        return ucxapp_stream_bridge_ntrip(gStream, app, argv[2], port, argv[4], n == 7 ? argv[5] : "",
                                          n == 7 ? argv[6] : "", 10, ble) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
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
    { "gnss.log", "<file|off>", "Record the raw receiver stream (NMEA/UBX) to a file", 0, cmdLog },
    { "gnss.send", "<hex..|$NMEA|ubx cls id [hex]>", "Send raw bytes, an NMEA sentence or a UBX frame", 0, cmdSend },
    { "gnss.restart", "<hot|warm|cold>", "Restart the receiver (UBX-CFG-RST)", 0, cmdRestart },
    { "gnss.rate", "<hz>", "Set the navigation rate, 1 to 25 Hz (UBX-CFG-RATE)", 0, cmdRate },
    { "gnss.bridge", "<tcp [port]|tls [port] [cert] [clientauth]|tcpc <host> <port>|ntrip <host> <port> <mount> [user pass] [ble]|udp <host> <port> [local]|udps <port>|ble [nus|sps|custom <uuid> [rx tx]]|uart <port> [baud]|cell|off|status>", "Two-way bridge of the receiver stream (TCP server/client, NTRIP corrections, UDP, BLE NUS/SPS, PC UART)", 0, cmdBridge },
};

void ucxapp_register_gnss_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
