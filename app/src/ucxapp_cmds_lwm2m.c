/* lwm2m.* - a small OMA LwM2M 1.1 client on a module UDP socket, plain CoAP or CoAP over DTLS 1.2 (pre-shared key
 * or certificate, run on the host). It registers with a server such as Eclipse Leshan and serves the Server (1),
 * Device (3), Connectivity Monitoring (4) and Location (6) objects with the module's identity, Wi-Fi signal and
 * address, and the GNSS or wifi.locate position: Read, Discover, Observe (notifications on change), Write of the
 * lifetime and Execute of Registration Update Trigger and Reboot. It runs in the foreground (lwm2m.run) or in the
 * background (lwm2m.start). Host builds only. */

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_dtls.h"
#include "ucxapp_store.h"
#include "ucxapp_thread.h"
#include "ucxapp_tls.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
#include "u_cx_socket.h"
#include "u_cx_system.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define LW_MAX           512
#define LW_POLL_MS       10
#define LW_TRIES         3
#define LW_WAIT_MS       2000
#define LW_LIFETIME      120      /* s; an update goes out every half lifetime */
#define LW_SSID          1        /* short server id: no bootstrap, so the one server is 1 */
#define LW_EP_MAX        64
#define LW_FIRST_MID     0x1D00   /* fixed start, so a recorded exchange can be replayed */
#define LW_OBS_MAX       8
#define LW_PMIN_MS       2000     /* at most one notification per observation this often */
#define LW_PMAX_MS       60000    /* and at least one this often, changed or not */
#define LW_LIVE_MS       5000     /* Wi-Fi signal, address and position are read again after this */
#define LW_HS_MS         20000    /* DTLS handshake */
#define LW_PSK_FILE      "lwm2m-psk.key"   /* "<identity>\n<hex key>", sealed by the secure store */
#define LW_PSK_MAX       64
#define LW_BEARER_WLAN   21       /* Connectivity Monitoring network bearer: WLAN */

#define CO_CON           0
#define CO_NON           1
#define CO_ACK           2
#define CO_RST           3
#define CO_GET           0x01
#define CO_POST          0x02
#define CO_PUT           0x03
#define CO_DELETE        0x04
#define CO_CREATED       0x41
#define CO_DELETED       0x42
#define CO_CHANGED       0x44
#define CO_CONTENT       0x45
#define CO_BAD_REQUEST   0x80
#define CO_NOT_FOUND     0x84
#define CO_NOT_ALLOWED   0x85
#define CO_NOT_ACCEPT    0x86
#define OPT_OBSERVE      6
#define OPT_LOCATION     8
#define OPT_URI_PATH     11
#define OPT_FORMAT       12
#define OPT_URI_QUERY    15
#define OPT_ACCEPT       17
#define LW_CF_TEXT          0
#define LW_CF_LINK          40
#define LW_CF_TLV           11542

typedef struct {
    unsigned type;
    unsigned code;
    uint16_t mid;
    uint8_t token[8];
    size_t tkl;
    char path[64];        /* Uri-Path joined: "/3/0/1" */
    char location[64];    /* Location-Path joined: "/rd/5a3f" */
    long accept;          /* -1 when absent */
    long observe;         /* -1 when absent */
    bool query;           /* Uri-Query present (Write-Attributes) */
    const uint8_t *payload;
    size_t plen;
} coapMsg_t;

typedef struct {
    char path[24];
    uint8_t token[8];
    size_t tkl;
    long accept;
    uint32_t seq;
    uint32_t sentAt;
    uint16_t mid;         /* of the last notification: a reset with it cancels */
    uint8_t last[LW_MAX];
    size_t lastLen;
    bool used;
} lwObs_t;

typedef struct {
    uCxHandle_t *ucx;
    int32_t sock;
    ucxapp_dtls_conn_t *dtls;
    ucxapp_dtls_xport_t xport;
    char security[64];    /* "none", "PSK <cipher>" or "certificate <cipher>" */
    uint16_t mid;
    uint8_t tok;
    char host[64];
    int32_t port;
    char ep[LW_EP_MAX + 1];
    char location[64];
    char maker[48];
    char model[48];
    char serial[48];
    char fw[48];
    int32_t lifetime;
    bool updateNow;
    bool sendLifetime;    /* the next update carries lt= (the server wrote a new lifetime) */
    bool rebootNow;
    unsigned served;
    unsigned updates;
    unsigned notifies;
    lwObs_t obs[LW_OBS_MAX];
    uint32_t liveAt;
    bool liveRead;
    bool wifiUp;
    int rssi;
    char ip[48];
    bool haveLoc;
    double lat;
    double lon;
} lw_t;

typedef struct {
    uint16_t obj;
    uint16_t res;
    char kind;            /* 's' string, 'i' integer, 'b' boolean, 't' time, 'f' float, 'x' executable */
    bool multi;           /* a multiple resource: one instance, 0 */
} lwRes_t;

static const uint16_t kObjects[] = { 1, 3, 4, 6 };

static const lwRes_t kRes[] = {
    { 1, 0, 'i', false }, { 1, 1, 'i', false }, { 1, 6, 'b', false }, { 1, 7, 's', false }, { 1, 8, 'x', false },
    { 3, 0, 's', false }, { 3, 1, 's', false }, { 3, 2, 's', false }, { 3, 3, 's', false }, { 3, 4, 'x', false },
    { 3, 13, 't', false }, { 3, 16, 's', false },
    { 4, 0, 'i', false }, { 4, 1, 'i', true }, { 4, 2, 'i', false }, { 4, 4, 's', true },
    { 6, 0, 'f', false }, { 6, 1, 'f', false }, { 6, 5, 't', false },
};

/* ---- CoAP encoding ---- */

/* Option delta or length: under 13 in the nibble, else 13 + one byte or 269 + two bytes (RFC 7252, 3.1). */
static unsigned nibble(unsigned v, uint8_t *ext, size_t *e)
{
    if (v < 13) {
        return v;
    }
    if (v < 269) {
        ext[(*e)++] = (uint8_t)(v - 13);
        return 13;
    }
    v -= 269;
    ext[(*e)++] = (uint8_t)(v >> 8);
    ext[(*e)++] = (uint8_t)(v & 0xFF);
    return 14;
}

static bool putOpt(uint8_t *out, size_t *n, size_t max, unsigned *prev, unsigned num, const void *val, size_t len)
{
    uint8_t ext[4];
    size_t e = 0;
    unsigned d = nibble(num - *prev, ext, &e);
    unsigned l = nibble((unsigned)len, ext, &e);

    if (*n + 1 + e + len > max) {
        return false;
    }
    out[(*n)++] = (uint8_t)((d << 4) | l);
    memcpy(out + *n, ext, e);
    *n += e;
    if (len > 0) {
        memcpy(out + *n, val, len);
    }
    *n += len;
    *prev = num;
    return true;
}

static bool putUint(uint8_t *out, size_t *n, size_t max, unsigned *prev, unsigned num, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    size_t len = v > 0xFFFFFF ? 4 : v > 0xFFFF ? 3 : v > 0xFF ? 2 : v > 0 ? 1 : 0;
    return putOpt(out, n, max, prev, num, b + 4 - len, len);
}

/* "/rd/5a3f" as Uri-Path options. */
static bool putPath(uint8_t *out, size_t *n, size_t max, unsigned *prev, const char *path)
{
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char *s = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        if (p > s && !putOpt(out, n, max, prev, OPT_URI_PATH, s, (size_t)(p - s))) {
            return false;
        }
    }
    return true;
}

static size_t putHeader(uint8_t *out, unsigned type, unsigned code, uint16_t mid, const uint8_t *token, size_t tkl)
{
    out[0] = (uint8_t)(0x40 | (type << 4) | tkl);
    out[1] = (uint8_t)code;
    out[2] = (uint8_t)(mid >> 8);
    out[3] = (uint8_t)(mid & 0xFF);
    memcpy(out + 4, token, tkl);
    return 4 + tkl;
}

static bool putPayload(uint8_t *out, size_t *n, size_t max, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (*n + 1 + len > max) {
        return false;
    }
    out[(*n)++] = 0xFF;
    memcpy(out + *n, data, len);
    *n += len;
    return true;
}

/* ---- CoAP parsing ---- */

static bool extended(const uint8_t *b, size_t len, size_t *p, unsigned *v)
{
    if (*v == 15) {
        return false;
    }
    if (*v == 13) {
        if (*p >= len) {
            return false;
        }
        *v = 13u + b[(*p)++];
    } else if (*v == 14) {
        if (*p + 1 >= len) {
            return false;
        }
        *v = 269u + (((unsigned)b[*p] << 8) | b[*p + 1]);
        *p += 2;
    }
    return true;
}

static bool appendSegment(char *to, size_t size, const uint8_t *seg, size_t len)
{
    size_t used = strlen(to);
    if (used + 1 + len >= size) {
        return false;
    }
    to[used] = '/';
    memcpy(to + used + 1, seg, len);
    to[used + 1 + len] = '\0';
    return true;
}

static bool parse(const uint8_t *b, size_t len, coapMsg_t *m)
{
    unsigned num = 0;

    memset(m, 0, sizeof(*m));
    m->accept = -1;
    m->observe = -1;
    if (len < 4 || (b[0] >> 6) != 1) {
        return false;
    }
    m->type = (b[0] >> 4) & 0x03;
    m->tkl = b[0] & 0x0F;
    if (m->tkl > 8 || 4 + m->tkl > len) {
        return false;
    }
    m->code = b[1];
    m->mid = (uint16_t)((b[2] << 8) | b[3]);
    memcpy(m->token, b + 4, m->tkl);
    size_t p = 4 + m->tkl;
    while (p < len && b[p] != 0xFF) {
        unsigned d = b[p] >> 4;
        unsigned l = b[p] & 0x0F;
        p++;
        if (!extended(b, len, &p, &d) || !extended(b, len, &p, &l) || l > len - p) {
            return false;
        }
        num += d;
        const uint8_t *v = b + p;
        p += l;
        if (num == OPT_URI_PATH && !appendSegment(m->path, sizeof(m->path), v, l)) {
            return false;
        }
        if (num == OPT_LOCATION && !appendSegment(m->location, sizeof(m->location), v, l)) {
            return false;
        }
        if (num == OPT_URI_QUERY) {
            m->query = true;
        }
        if (num == OPT_ACCEPT || num == OPT_OBSERVE) {
            long value = 0;
            for (unsigned i = 0; i < l && i < 4; i++) {
                value = (value << 8) | v[i];
            }
            *(num == OPT_ACCEPT ? &m->accept : &m->observe) = value;
        }
    }
    if (p < len) {
        m->payload = b + p + 1;
        m->plen = len - p - 1;
    }
    return true;
}

/* ---- transport: the module socket, or DTLS on top of it ---- */

static int xSend(void *ctx, const unsigned char *buf, size_t len)
{
    lw_t *lw = (lw_t *)ctx;
    return uCxSocketWrite(lw->ucx, lw->sock, buf, (int32_t)len) >= 0 ? (int)len : -1;
}

static int xRecv(void *ctx, unsigned char *buf, size_t len)
{
    lw_t *lw = (lw_t *)ctx;
    uCxSocketReceiveFrom_t from;
    memset(&from, 0, sizeof(from));
    int32_t rc = ucxapp_udp_recvfrom(lw->ucx, lw->sock, (int32_t)len, buf, &from);
    return rc < 0 ? 0 : (int)rc;
}

static bool sendRaw(lw_t *lw, const uint8_t *buf, size_t len)
{
    if (lw->dtls != NULL) {
        return ucxapp_dtls_write(lw->dtls, buf, len) >= 0;
    }
    return uCxSocketWrite(lw->ucx, lw->sock, buf, (int32_t)len) >= 0;
}

static int32_t recvRaw(lw_t *lw, uint8_t *buf, size_t size)
{
    if (lw->dtls != NULL) {
        return ucxapp_dtls_read(lw->dtls, buf, size);
    }
    uCxSocketReceiveFrom_t from;
    memset(&from, 0, sizeof(from));
    return ucxapp_udp_recvfrom(lw->ucx, lw->sock, (int32_t)size, buf, &from);
}

/* ---- LwM2M objects ---- */

/* Wi-Fi signal and address, and the position, read again at most every LW_LIVE_MS (only when 4 or 6 is asked). */
static void refreshLive(lw_t *lw)
{
    char text[160];

    if (lw->liveRead && ucxapp_platform_tick_ms() - lw->liveAt < LW_LIVE_MS) {
        return;
    }
    lw->liveRead = true;
    lw->liveAt = ucxapp_platform_tick_ms();
    lw->wifiUp = ucxapp_wifi_dash(lw->ucx, text, sizeof(text));
    lw->rssi = 0;
    lw->ip[0] = '\0';
    if (lw->wifiUp) {
        const char *dbm = strstr(text, " dBm");
        const char *nl = strchr(text, '\n');
        if (dbm != NULL) {
            const char *s = dbm;
            while (s > text && s[-1] != ' ') {
                s--;
            }
            lw->rssi = atoi(s);
        }
        if (nl != NULL && strcmp(nl + 1, "no IP") != 0) {
            (void)snprintf(lw->ip, sizeof(lw->ip), "%s", nl + 1);
        }
    }
    ucxapp_time_location(text, sizeof(text));
    lw->haveLoc = sscanf(text, "%lf, %lf", &lw->lat, &lw->lon) == 2;
}

/* The resource's value as text (strings) or a number (the other kinds; floats in *f). */
static const char *resValue(const lw_t *lw, const lwRes_t *r, long long *num, double *f)
{
    *num = 0;
    *f = 0;
    switch (r->obj * 100 + r->res) {
    case 100: *num = LW_SSID; return NULL;
    case 101: *num = lw->lifetime; return NULL;
    case 106: return NULL;
    case 107: return "U";
    case 300: return lw->maker;
    case 301: return lw->model;
    case 302: return lw->serial;
    case 303: return lw->fw;
    case 313: *num = (long long)time(NULL); return NULL;
    case 316: return "U";   /* supported binding */
    case 400: case 401: *num = LW_BEARER_WLAN; return NULL;
    case 402: *num = lw->rssi; return NULL;
    case 404: return lw->ip;
    case 600: *f = lw->lat; return NULL;
    case 601: *f = lw->lon; return NULL;
    default:  *num = (long long)time(NULL); return NULL;   /* 6/5 timestamp */
    }
}

/* One resource as TLV (OMA LwM2M 1.1, 7.4.3): type byte, id, length, value; integers in 1, 2, 4 or 8 bytes. */
static bool putTlv(uint8_t *out, size_t *n, size_t max, unsigned typeBits, uint16_t id, const uint8_t *v, size_t len)
{
    uint8_t hdr[6];
    size_t h = 1;
    hdr[0] = (uint8_t)typeBits;
    if (id > 0xFF) {
        hdr[0] |= 0x20;
        hdr[h++] = (uint8_t)(id >> 8);
    }
    hdr[h++] = (uint8_t)(id & 0xFF);
    if (len < 8) {
        hdr[0] |= (uint8_t)len;
    } else if (len < 0x100) {
        hdr[0] |= 0x08;
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[0] |= 0x10;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len & 0xFF);
    }
    if (*n + h + len > max) {
        return false;
    }
    memcpy(out + *n, hdr, h);
    memcpy(out + *n + h, v, len);
    *n += h + len;
    return true;
}

static bool resTlv(const lw_t *lw, const lwRes_t *r, uint8_t *out, size_t *n, size_t max)
{
    long long num;
    double f;
    uint8_t v[LW_MAX / 2];
    size_t len;
    const char *s = resValue(lw, r, &num, &f);

    if (s != NULL) {
        len = strlen(s) < sizeof(v) ? strlen(s) : sizeof(v);
        memcpy(v, s, len);
    } else if (r->kind == 'f') {
        uint64_t bits;
        memcpy(&bits, &f, sizeof(bits));
        len = 8;
        for (size_t i = 0; i < len; i++) {
            v[i] = (uint8_t)(bits >> (8 * (len - 1 - i)));
        }
    } else {
        len = r->kind == 'b' ? 1 : (num >= -128 && num <= 127) ? 1 : (num >= -32768 && num <= 32767) ? 2
            : (num >= -2147483647LL - 1 && num <= 2147483647LL) ? 4 : 8;
        for (size_t i = 0; i < len; i++) {
            v[i] = (uint8_t)((unsigned long long)num >> (8 * (len - 1 - i)));
        }
    }
    if (r->multi) {   /* a multiple resource around its one instance, 0 */
        uint8_t inner[LW_MAX / 2 + 8];
        size_t k = 0;
        return putTlv(inner, &k, sizeof(inner), 0x40, 0, v, len) && putTlv(out, n, max, 0x80, r->res, inner, k);
    }
    return putTlv(out, n, max, 0xC0, r->res, v, len);
}

static bool knownObject(unsigned obj)
{
    for (size_t i = 0; i < sizeof(kObjects) / sizeof(kObjects[0]); i++) {
        if (kObjects[i] == obj) {
            return true;
        }
    }
    return false;
}

/* Read (GET without link-format) and Discover (GET, Accept: 40) of /obj, /obj/0 or /obj/0/res. */
static unsigned readOrDiscover(lw_t *lw, const coapMsg_t *m, uint8_t *body, size_t *blen, unsigned *cf)
{
    unsigned ids[3] = { 0, 0, 0 };
    int count = 0;
    char extra;
    const char *p = m->path;

    *blen = 0;
    if (p[0] != '\0') {
        count = sscanf(p, "/%u/%u/%u%c", &ids[0], &ids[1], &ids[2], &extra);
    }
    if (count < 1 || count > 3 || !knownObject(ids[0]) || (count >= 2 && ids[1] != 0)) {
        return count < 1 ? CO_NOT_ALLOWED : CO_NOT_FOUND;
    }
    const lwRes_t *one = NULL;
    if (count == 3) {
        for (size_t i = 0; i < sizeof(kRes) / sizeof(kRes[0]); i++) {
            if (kRes[i].obj == ids[0] && kRes[i].res == ids[2]) {
                one = &kRes[i];
            }
        }
        if (one == NULL) {
            return CO_NOT_FOUND;
        }
    }
    if (m->accept == LW_CF_LINK) {
        int w = count < 3 ? snprintf((char *)body, LW_MAX, "</%u/0>", ids[0]) : 0;
        for (size_t i = 0; i < sizeof(kRes) / sizeof(kRes[0]) && w >= 0 && w < LW_MAX; i++) {
            if (kRes[i].obj == ids[0] && (one == NULL || one == &kRes[i])) {
                w += snprintf((char *)body + w, LW_MAX - (size_t)w, "%s</%u/0/%u>", w > 0 ? "," : "", ids[0],
                              kRes[i].res);
            }
        }
        *blen = (size_t)w;
        *cf = LW_CF_LINK;
        return CO_CONTENT;
    }
    if (one != NULL && one->kind == 'x') {
        return CO_NOT_ALLOWED;
    }
    if (ids[0] == 4 || ids[0] == 6) {
        refreshLive(lw);
        if (ids[0] == 6 && !lw->haveLoc) {
            return CO_NOT_FOUND;   /* no GNSS fix and no wifi.locate yet */
        }
    }
    if (one != NULL && (m->accept == -1 || m->accept == LW_CF_TEXT)) {
        long long num;
        double f;
        const char *s = resValue(lw, one, &num, &f);
        if (one->multi) {
            return CO_NOT_ACCEPT;   /* a multiple resource has no plain-text form */
        }
        *blen = (size_t)(s != NULL ? snprintf((char *)body, LW_MAX, "%s", s)
                         : one->kind == 'f' ? snprintf((char *)body, LW_MAX, "%.6f", f)
                         : snprintf((char *)body, LW_MAX, "%lld", num));
        *cf = LW_CF_TEXT;
        return CO_CONTENT;
    }
    if (m->accept != -1 && m->accept != LW_CF_TLV) {
        return CO_NOT_ACCEPT;
    }
    uint8_t inner[LW_MAX];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(kRes) / sizeof(kRes[0]); i++) {
        if (kRes[i].obj == ids[0] && kRes[i].kind != 'x' && (one == NULL || one == &kRes[i])) {
            (void)resTlv(lw, &kRes[i], inner, &n, sizeof(inner));
        }
    }
    if (count == 1) {
        (void)putTlv(body, blen, LW_MAX, 0x00, 0, inner, n);   /* the one object instance around its resources */
    } else {
        memcpy(body, inner, n);
        *blen = n;
    }
    *cf = LW_CF_TLV;
    return CO_CONTENT;
}

/* An integer written as plain text or as one TLV resource. */
static bool writtenInt(const coapMsg_t *m, long long *v)
{
    if (m->plen >= 3 && (m->payload[0] & 0xC0) == 0xC0 && (m->payload[0] & 0x18) == 0) {
        size_t len = m->payload[0] & 0x07;
        size_t at = (m->payload[0] & 0x20) ? 3 : 2;
        if (len == 0 || len > 8 || at + len != m->plen) {
            return false;
        }
        *v = (m->payload[at] & 0x80) ? -1 : 0;
        for (size_t i = 0; i < len; i++) {
            *v = (long long)(((unsigned long long)*v << 8) | m->payload[at + i]);
        }
        return true;
    }
    char text[24];
    char *end;
    if (m->plen == 0 || m->plen >= sizeof(text)) {
        return false;
    }
    memcpy(text, m->payload, m->plen);
    text[m->plen] = '\0';
    *v = strtoll(text, &end, 10);
    return *end == '\0';
}

static unsigned writeRes(lw_t *lw, const coapMsg_t *m)
{
    long long v;

    if (m->query && m->plen == 0) {
        return CO_CHANGED;   /* Write-Attributes (pmin, pmax): accepted, the client keeps its own */
    }
    if (strcmp(m->path, "/1/0/1") != 0) {
        return CO_NOT_ALLOWED;
    }
    if (!writtenInt(m, &v) || v < 10 || v > 86400) {
        return CO_BAD_REQUEST;
    }
    lw->lifetime = (int32_t)v;
    lw->sendLifetime = true;
    lw->updateNow = true;
    return CO_CHANGED;
}

static lwObs_t *findObs(lw_t *lw, const char *path)
{
    for (int i = 0; i < LW_OBS_MAX; i++) {
        if (lw->obs[i].used && strcmp(lw->obs[i].path, path) == 0) {
            return &lw->obs[i];
        }
    }
    return NULL;
}

/* Observe 0 on a Read starts an observation (one per path, the newest token wins), 1 ends it. */
static bool observe(lw_t *lw, const coapMsg_t *m, const uint8_t *body, size_t blen)
{
    lwObs_t *o = findObs(lw, m->path);

    if (m->observe != 0) {
        if (o != NULL) {
            o->used = false;
        }
        return false;
    }
    for (int i = 0; o == NULL && i < LW_OBS_MAX; i++) {
        if (!lw->obs[i].used) {
            o = &lw->obs[i];
        }
    }
    if (o == NULL || strlen(m->path) >= sizeof(o->path)) {
        return false;
    }
    memset(o, 0, sizeof(*o));
    o->used = true;
    (void)snprintf(o->path, sizeof(o->path), "%s", m->path);
    memcpy(o->token, m->token, m->tkl);
    o->tkl = m->tkl;
    o->accept = m->accept;
    o->sentAt = ucxapp_platform_tick_ms();
    memcpy(o->last, body, blen);
    o->lastLen = blen;
    return true;
}

/* Answers a request from the server: piggybacked on the ACK of a confirmable one. */
static void serve(lw_t *lw, const coapMsg_t *m)
{
    uint8_t out[LW_MAX];
    uint8_t body[LW_MAX];
    size_t blen = 0;
    unsigned cf = LW_CF_TEXT;
    unsigned code;
    unsigned prev = 0;
    bool observing = false;

    if (m->code == 0 || m->code >= 0x20) {
        if (m->type == CO_RST) {   /* the server forgot an observation */
            for (int i = 0; i < LW_OBS_MAX; i++) {
                if (lw->obs[i].used && lw->obs[i].seq > 0 && lw->obs[i].mid == m->mid) {
                    lw->obs[i].used = false;
                }
            }
        } else if (m->type == CO_CON) {   /* a ping, or a response nobody waits for */
            size_t n = putHeader(out, CO_RST, 0, m->mid, NULL, 0);
            (void)sendRaw(lw, out, n);
        }
        return;
    }
    if (m->code == CO_GET) {
        code = readOrDiscover(lw, m, body, &blen, &cf);
        if (code == CO_CONTENT && m->accept != LW_CF_LINK && m->observe >= 0) {
            observing = observe(lw, m, body, blen);
        }
    } else if (m->code == CO_POST && strcmp(m->path, "/1/0/8") == 0) {
        lw->updateNow = true;   /* Registration Update Trigger */
        code = CO_CHANGED;
    } else if (m->code == CO_POST && strcmp(m->path, "/3/0/4") == 0) {
        lw->rebootNow = true;   /* Reboot: deregister, then restart the module */
        code = CO_CHANGED;
    } else if (m->code == CO_PUT) {
        code = writeRes(lw, m);
    } else {
        code = CO_NOT_ALLOWED;
    }
    size_t n = putHeader(out, m->type == CO_CON ? CO_ACK : CO_NON, code, m->type == CO_CON ? m->mid : lw->mid++,
                         m->token, m->tkl);
    if (observing) {
        (void)putUint(out, &n, sizeof(out), &prev, OPT_OBSERVE, 0);
    }
    if (blen > 0) {
        (void)putUint(out, &n, sizeof(out), &prev, OPT_FORMAT, cf);
        (void)putPayload(out, &n, sizeof(out), body, blen);
    }
    (void)sendRaw(lw, out, n);
    lw->served++;
    static const char *const kMethod[] = { "?", "GET", "POST", "PUT", "DELETE" };
    ucxapp_print("lwm2m: %s %s%s -> %u.%02u\n", kMethod[m->code <= 4 ? m->code : 0], m->path[0] ? m->path : "/",
                 m->accept == LW_CF_LINK ? " (discover)" : observing ? " (observe)" : "", code >> 5, code & 0x1F);
}

/* A notification (non-confirmable) for each observation whose value changed, at most every LW_PMIN_MS, and at
 * least every LW_PMAX_MS. */
static void notifyChanged(lw_t *lw)
{
    uint32_t now = ucxapp_platform_tick_ms();

    for (int i = 0; i < LW_OBS_MAX; i++) {
        lwObs_t *o = &lw->obs[i];
        if (!o->used || now - o->sentAt < LW_PMIN_MS) {
            continue;
        }
        coapMsg_t q;
        uint8_t body[LW_MAX];
        size_t blen = 0;
        unsigned cf = LW_CF_TEXT;
        memset(&q, 0, sizeof(q));
        q.code = CO_GET;
        q.accept = o->accept;
        q.observe = -1;
        (void)snprintf(q.path, sizeof(q.path), "%s", o->path);
        unsigned code = readOrDiscover(lw, &q, body, &blen, &cf);
        bool changed = code != CO_CONTENT || blen != o->lastLen || memcmp(body, o->last, blen) != 0;
        if (!changed && now - o->sentAt < LW_PMAX_MS) {
            continue;
        }
        uint8_t out[LW_MAX];
        unsigned prev = 0;
        o->mid = lw->mid++;
        size_t n = putHeader(out, CO_NON, code, o->mid, o->token, o->tkl);
        (void)putUint(out, &n, sizeof(out), &prev, OPT_OBSERVE, ++o->seq & 0xFFFFFFu);
        if (code == CO_CONTENT) {
            (void)putUint(out, &n, sizeof(out), &prev, OPT_FORMAT, cf);
            (void)putPayload(out, &n, sizeof(out), body, blen);
            memcpy(o->last, body, blen);
            o->lastLen = blen;
        } else {
            o->used = false;   /* the value went away (no position any more): the observation ends */
        }
        o->sentAt = now;
        (void)sendRaw(lw, out, n);
        lw->notifies++;
    }
}

/* One request to the server; serves requests that arrive meanwhile. Returns the response code, 0 if none came. */
static unsigned request(lw_t *lw, unsigned code, const char *path, const char *const *query, size_t nq,
                        const char *payload, coapMsg_t *rsp)
{
    uint8_t out[LW_MAX];
    uint8_t buf[LW_MAX];
    uint8_t token[2] = { 0x4C, lw->tok++ };
    uint16_t mid = lw->mid++;
    unsigned prev = 0;
    size_t n = putHeader(out, CO_CON, code, mid, token, sizeof(token));

    if (!putPath(out, &n, sizeof(out), &prev, path)) {
        return 0;
    }
    if (payload != NULL && !putUint(out, &n, sizeof(out), &prev, OPT_FORMAT, LW_CF_LINK)) {
        return 0;
    }
    for (size_t i = 0; i < nq; i++) {
        if (!putOpt(out, &n, sizeof(out), &prev, OPT_URI_QUERY, query[i], strlen(query[i]))) {
            return 0;
        }
    }
    if (payload != NULL && !putPayload(out, &n, sizeof(out), (const uint8_t *)payload, strlen(payload))) {
        return 0;
    }
    for (int t = 0; t < LW_TRIES; t++) {
        if (!sendRaw(lw, out, n)) {
            return 0;
        }
        bool acked = false;
        uint32_t start = ucxapp_platform_tick_ms();
        while (ucxapp_platform_tick_ms() - start < (acked ? 3u * LW_WAIT_MS : (uint32_t)LW_WAIT_MS)) {
            int32_t rc = recvRaw(lw, buf, sizeof(buf));
            if (rc <= 0 || !parse(buf, (size_t)rc, rsp)) {
                ucxapp_platform_sleep_ms(LW_POLL_MS);
                continue;
            }
            bool ours = rsp->tkl == sizeof(token) && memcmp(rsp->token, token, sizeof(token)) == 0;
            if (rsp->type == CO_ACK && rsp->mid == mid && rsp->code == 0) {
                acked = true;   /* empty ACK: the response follows on its own */
            } else if (rsp->type == CO_ACK && rsp->mid == mid && ours) {
                return rsp->code;
            } else if (rsp->code >= 0x20 && ours) {
                if (rsp->type == CO_CON) {
                    size_t a = putHeader(out, CO_ACK, 0, rsp->mid, NULL, 0);
                    (void)sendRaw(lw, out, a);
                }
                return rsp->code;
            } else if (rsp->type == CO_RST && rsp->mid == mid) {
                return 0;
            } else {
                serve(lw, rsp);
            }
        }
        if (acked) {
            break;
        }
    }
    return 0;
}

static bool registerNow(lw_t *lw)
{
    char epq[LW_EP_MAX + 4];
    char lt[16];
    coapMsg_t rsp;
    (void)snprintf(epq, sizeof(epq), "ep=%s", lw->ep);
    (void)snprintf(lt, sizeof(lt), "lt=%d", (int)lw->lifetime);
    const char *const query[] = { epq, lt, "lwm2m=1.1", "b=U" };
    memset(lw->obs, 0, sizeof(lw->obs));   /* a new registration starts without observations */
    unsigned code = request(lw, CO_POST, "/rd", query, 4, "</1/0>,</3/0>,</4/0>,</6/0>", &rsp);
    if (code != CO_CREATED || rsp.location[0] == '\0') {
        if (code == 0) {
            ucxapp_print("ERROR: no LwM2M reply to the registration\n");
        } else {
            ucxapp_print("ERROR: registration refused: %u.%02u\n", code >> 5, code & 0x1F);
        }
        return false;
    }
    (void)snprintf(lw->location, sizeof(lw->location), "%s", rsp.location);
    return true;
}

static unsigned update(lw_t *lw)
{
    char lt[16];
    coapMsg_t rsp;
    const char *const query[] = { lt };
    (void)snprintf(lt, sizeof(lt), "lt=%d", (int)lw->lifetime);
    unsigned code = request(lw, CO_POST, lw->location, query, lw->sendLifetime ? 1 : 0, NULL, &rsp);
    if (code == CO_CHANGED) {
        lw->sendLifetime = false;
    }
    lw->updates++;
    return code;
}

static void deviceText(lw_t *lw, bool (*begin)(uCxHandle_t *, const char **), char *out, size_t size)
{
    const char *text = NULL;
    (void)snprintf(out, size, "%s", begin(lw->ucx, &text) && text != NULL ? text : "");
    uCxEnd(lw->ucx);
    for (char *q = out; *q != '\0'; q++) {
        if (*q == '"') {
            memmove(q, q + 1, strlen(q));   /* firmware answers may be quoted */
            q--;
        }
    }
}

/* ---- security: lwm2m-psk.key holds "<identity>\n<hex key>" ---- */

static int hexDigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : -1;
}

static size_t fromHex(const char *hex, uint8_t *out, size_t max)
{
    size_t len = strlen(hex);
    if (len == 0 || len % 2 != 0 || len / 2 > max) {
        return 0;
    }
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hexDigit(hex[2 * i]);
        int lo = hexDigit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return len / 2;
}

/* Reads the stored identity (and key when key != NULL); false when there is none or it is damaged. */
static bool loadPsk(char *id, size_t idSize, uint8_t *key, size_t *keyLen)
{
    size_t len = 0;
    uint8_t *data = ucxapp_store_load(LW_PSK_FILE, &len);
    bool ok = false;

    if (data == NULL) {
        return false;
    }
    char *nl = strchr((char *)data, '\n');
    if (nl != NULL && nl != (char *)data && (size_t)(nl - (char *)data) < idSize) {
        *nl = '\0';
        (void)snprintf(id, idSize, "%s", (char *)data);
        ok = key == NULL || (*keyLen = fromHex(nl + 1, key, LW_PSK_MAX)) > 0;
    }
    memset(data, 0, len);
    free(data);
    return ok;
}

/* DTLS on the connected socket: a pre-shared key, or the server certificate checked against the local CA. */
static bool secure(lw_t *lw, bool psk)
{
    char err[128] = "";
    char id[LW_PSK_MAX + 1];
    uint8_t key[LW_PSK_MAX];
    size_t keyLen = 0;

    lw->xport.send = xSend;
    lw->xport.recv = xRecv;
    lw->xport.ctx = lw;
    if (psk) {
        if (!loadPsk(id, sizeof(id), key, &keyLen)) {
            ucxapp_print("ERROR: no LwM2M pre-shared key; lwm2m.psk <identity> <hexkey> first\n");
            return false;
        }
        lw->dtls = ucxapp_dtls_client_psk_new(&lw->xport, (const unsigned char *)id, strlen(id), key, keyLen, err,
                                              sizeof(err));
        memset(key, 0, sizeof(key));
    } else {
        lw->dtls = ucxapp_dtls_client_new(&lw->xport, UCXAPP_TLS_CA_NAME, lw->host, err, sizeof(err));
    }
    if (lw->dtls == NULL) {
        ucxapp_print("ERROR: DTLS: %s\n", err[0] ? err : "setup failed");
        return false;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    int hs = 0;
    while (hs == 0 && ucxapp_platform_tick_ms() - start < LW_HS_MS) {
        hs = ucxapp_dtls_handshake(lw->dtls);
        if (hs == 0) {
            ucxapp_platform_sleep_ms(LW_POLL_MS);
        }
    }
    if (hs != 1) {
        ucxapp_print("ERROR: DTLS handshake with %s:%d %s\n", lw->host, (int)lw->port, hs == 0 ? "timed out" : "failed");
        ucxapp_dtls_free(lw->dtls);
        lw->dtls = NULL;
        return false;
    }
    (void)snprintf(lw->security, sizeof(lw->security), "%s %s", psk ? "PSK" : "certificate",
                   ucxapp_dtls_cipher(lw->dtls));
    return true;
}

/* ---- the client: set up and register, run, finish ---- */

static lw_t gLw;
static ucxapp_thread_t *gThread;
static volatile bool gStop;
static volatile bool gRunning;
static uint32_t gStartedAt;
static int32_t gSeconds;

/* <host> [port] [endpoint] [seconds] [dtls|psk|cert]: the keyword may stand anywhere after the host. */
static int setup(ucxapp_t *app, lw_t *lw, int argc, char *argv[], int32_t *seconds)
{
    int mode = 0;   /* 0 plain, 1 dtls (PSK when a key is stored), 2 psk, 3 cert */
    int32_t port = 0;
    const char *ep = NULL;
    int positional = 0;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        int k = strcmp(argv[i], "dtls") == 0 ? 1 : strcmp(argv[i], "psk") == 0 ? 2 : strcmp(argv[i], "cert") == 0 ? 3
              : 0;
        if (k != 0 && mode == 0) {
            mode = k;
        } else if (k != 0) {
            return UCXAPP_ERR_USAGE;
        } else if (positional == 0) {
            if (!ucxapp_parse_int(argv[i], &port) || port < 1 || port > 65535) {
                return UCXAPP_ERR_USAGE;
            }
            positional++;
        } else if (positional == 1) {
            ep = argv[i];
            positional++;
        } else if (positional == 2) {
            if (!ucxapp_parse_int(argv[i], seconds) || *seconds < 0 || *seconds > 86400) {
                return UCXAPP_ERR_USAGE;
            }
            positional++;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (ep != NULL && (strlen(ep) == 0 || strlen(ep) > LW_EP_MAX)) {
        ucxapp_print("ERROR: the endpoint name takes 1 to %d characters\n", LW_EP_MAX);
        return UCXAPP_ERR_USAGE;
    }
    if (strlen(argv[1]) >= sizeof(lw->host)) {
        return UCXAPP_ERR_USAGE;
    }
    if (mode == 1) {
        char id[LW_PSK_MAX + 1];
        mode = loadPsk(id, sizeof(id), NULL, NULL) ? 2 : 3;
    }
    memset(lw, 0, sizeof(*lw));
    lw->ucx = app->ucx;
    lw->mid = LW_FIRST_MID;
    lw->lifetime = LW_LIFETIME;
    lw->port = port > 0 ? port : mode != 0 ? 5684 : 5683;
    (void)snprintf(lw->host, sizeof(lw->host), "%s", argv[1]);
    (void)snprintf(lw->security, sizeof(lw->security), "none");
    deviceText(lw, uCxGeneralGetManufacturerIdentificationBegin, lw->maker, sizeof(lw->maker));
    deviceText(lw, uCxGeneralGetDeviceModelIdentificationBegin, lw->model, sizeof(lw->model));
    deviceText(lw, uCxGeneralGetSerialNumberBegin, lw->serial, sizeof(lw->serial));
    deviceText(lw, uCxGeneralGetSoftwareVersionBegin, lw->fw, sizeof(lw->fw));
    if (ep != NULL) {
        (void)snprintf(lw->ep, sizeof(lw->ep), "%s", ep);
    } else {
        (void)snprintf(lw->ep, sizeof(lw->ep), "ucxapp-%s", lw->serial[0] ? lw->serial : "module");
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &lw->sock) != 0) {
        ucxapp_print("ERROR: failed to create a UDP socket\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxSocketConnect(app->ucx, lw->sock, lw->host, lw->port) != 0) {
        ucxapp_print("ERROR: connect to %s:%d failed\n", lw->host, (int)lw->port);
        uCxSocketClose(app->ucx, lw->sock);
        return UCXAPP_ERR_FAILED;
    }
    if ((mode != 0 && !secure(lw, mode == 2)) || !registerNow(lw)) {
        ucxapp_dtls_free(lw->dtls);
        lw->dtls = NULL;
        uCxSocketClose(app->ucx, lw->sock);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("lwm2m: registered at %s:%d as %s (%s), lifetime %d s%s%s\n", lw->host, (int)lw->port, lw->ep,
                 lw->location, (int)lw->lifetime, mode != 0 ? ", DTLS " : "", mode != 0 ? lw->security : "");
    return UCXAPP_OK;
}

/* Serves, notifies and updates until seconds pass (0: no limit), stop() says so, or the server asks for a reboot. */
static int loop(lw_t *lw, int32_t seconds, bool (*stop)(void))
{
    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t lastUpdate = start;
    uint8_t buf[LW_MAX];

    while ((seconds == 0 || ucxapp_platform_tick_ms() - start < (uint32_t)seconds * 1000u) && !lw->rebootNow) {
        if (stop()) {
            break;
        }
        if (lw->updateNow || ucxapp_platform_tick_ms() - lastUpdate >= (uint32_t)lw->lifetime * 500u) {
            lw->updateNow = false;
            lastUpdate = ucxapp_platform_tick_ms();
            unsigned code = update(lw);
            ucxapp_print("lwm2m: update -> %u.%02u\n", code >> 5, code & 0x1F);
            if (code == CO_NOT_FOUND && !registerNow(lw)) {
                return UCXAPP_ERR_FAILED;
            }
            continue;
        }
        coapMsg_t m;
        int32_t n = recvRaw(lw, buf, sizeof(buf));
        if (n > 0 && parse(buf, (size_t)n, &m)) {
            serve(lw, &m);
        } else {
            notifyChanged(lw);
            ucxapp_platform_sleep_ms(LW_POLL_MS);
        }
    }
    return UCXAPP_OK;
}

/* Deregisters (when the run went well), ends DTLS, closes the socket and restarts the module when asked to. */
static void finish(lw_t *lw, int rc)
{
    if (rc == UCXAPP_OK) {
        coapMsg_t rsp;
        unsigned code = request(lw, CO_DELETE, lw->location, NULL, 0, NULL, &rsp);
        ucxapp_print("lwm2m: deregistered (%u.%02u), %u request(s) served\n", code >> 5, code & 0x1F, lw->served);
    }
    ucxapp_dtls_free(lw->dtls);
    lw->dtls = NULL;
    uCxSocketClose(lw->ucx, lw->sock);
    if (lw->rebootNow) {
        ucxapp_print("lwm2m: the server asked for a reboot: restarting the module\n");
        (void)uCxSystemReboot(lw->ucx);
    }
}

static bool keyPressed(void)
{
    if (ucxapp_platform_key_pressed()) {
        (void)ucxapp_platform_get_key();
        return true;
    }
    return false;
}

static bool stopAsked(void)
{
    return gStop;
}

static void backgroundLoop(void *arg)
{
    lw_t *lw = (lw_t *)arg;
    int rc = loop(lw, gSeconds, stopAsked);
    if (!gStop) {   /* ended by itself (time, reboot, failure): nobody calls lwm2m.stop to finish it */
        finish(lw, rc);
        gRunning = false;
    }
}

/* lwm2m.run <host> [port] [endpoint] [seconds] [dtls|psk|cert]: register, serve, update, deregister. */
static int cmdRun(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 60;

    if (gRunning) {
        ucxapp_print("ERROR: the LwM2M client runs in the background; lwm2m.stop first\n");
        return UCXAPP_ERR_FAILED;
    }
    int rc = setup(app, &gLw, argc, argv, &seconds);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    ucxapp_print("lwm2m: %s\n", seconds > 0 ? "any key stops" : "runs until a key is pressed");
    rc = loop(&gLw, seconds, keyPressed);
    finish(&gLw, rc);
    return rc;
}

/* lwm2m.start <host> [port] [endpoint] [seconds] [dtls|psk|cert]: the same client on its own thread. */
static int cmdStart(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 0;

    if (gRunning) {
        ucxapp_print("ERROR: already running; lwm2m.stop first\n");
        return UCXAPP_ERR_FAILED;
    }
    if (gThread != NULL) {   /* a client that ended by itself */
        ucxapp_thread_join(gThread);
        gThread = NULL;
    }
    int rc = setup(app, &gLw, argc, argv, &seconds);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    gStop = false;
    gSeconds = seconds;
    gStartedAt = ucxapp_platform_tick_ms();
    gRunning = true;
    gThread = ucxapp_thread_start(backgroundLoop, &gLw);
    if (gThread == NULL) {
        gRunning = false;
        finish(&gLw, UCXAPP_OK);
        ucxapp_print("ERROR: cannot start the LwM2M thread\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("lwm2m: runs in the background; lwm2m.status shows it, lwm2m.stop ends it\n");
    return UCXAPP_OK;
}

static int cmdStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        if (gThread != NULL) {
            ucxapp_thread_join(gThread);
            gThread = NULL;
        }
        ucxapp_print("lwm2m: not running\n");
        return UCXAPP_OK;
    }
    gStop = true;
    ucxapp_thread_join(gThread);
    gThread = NULL;
    finish(&gLw, UCXAPP_OK);
    gRunning = false;
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gRunning) {
        ucxapp_print("lwm2m: not running\n");
        return UCXAPP_OK;
    }
    ucxapp_print("lwm2m: registered at %s:%d as %s (%s), up %u s\n", gLw.host, (int)gLw.port, gLw.ep, gLw.location,
                 (unsigned)((ucxapp_platform_tick_ms() - gStartedAt) / 1000u));
    ucxapp_print("security: %s, lifetime %d s\n", gLw.security, (int)gLw.lifetime);
    ucxapp_print("served %u request(s), %u update(s), %u notification(s)\n", gLw.served, gLw.updates, gLw.notifies);
    for (int i = 0; i < LW_OBS_MAX; i++) {
        if (gLw.obs[i].used) {
            ucxapp_print("observed: %s (%u notification(s))\n", gLw.obs[i].path, (unsigned)gLw.obs[i].seq);
        }
    }
    return UCXAPP_OK;
}

/* lwm2m.psk [<identity> <hexkey>|clear] */
static int cmdPsk(ucxapp_t *app, int argc, char *argv[])
{
    char id[LW_PSK_MAX + 1];
    uint8_t key[LW_PSK_MAX];
    (void)app;

    if (argc == 1) {
        if (loadPsk(id, sizeof(id), NULL, NULL)) {
            ucxapp_print("lwm2m: pre-shared key stored for identity %s\n", id);
        } else {
            ucxapp_print("lwm2m: no pre-shared key\n");
        }
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        (void)ucxapp_store_remove(LW_PSK_FILE);
        ucxapp_print("lwm2m: pre-shared key removed\n");
        return UCXAPP_OK;
    }
    if (argc != 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (strlen(argv[1]) == 0 || strlen(argv[1]) > LW_PSK_MAX) {
        ucxapp_print("ERROR: the identity takes 1 to %d characters\n", LW_PSK_MAX);
        return UCXAPP_ERR_USAGE;
    }
    if (fromHex(argv[2], key, sizeof(key)) == 0) {
        ucxapp_print("ERROR: the key is 1 to %d bytes in hex, e.g. 000102030405060708090a0b0c0d0e0f\n", LW_PSK_MAX);
        return UCXAPP_ERR_USAGE;
    }
    memset(key, 0, sizeof(key));
    char data[LW_PSK_MAX * 3 + 4];
    int n = snprintf(data, sizeof(data), "%s\n%s", argv[1], argv[2]);
    bool ok = n > 0 && ucxapp_store_save(LW_PSK_FILE, data, (size_t)n);
    memset(data, 0, sizeof(data));
    if (!ok) {
        ucxapp_print("ERROR: cannot store the key\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("lwm2m: pre-shared key stored (sealed) for identity %s; lwm2m.run <host> psk uses it\n", argv[1]);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gLwm2mCmds[] = {
    { "lwm2m.run", "<host> [port] [endpoint] [seconds] [dtls|psk|cert]",
      "LwM2M 1.1 client over a module UDP socket: registers, serves objects 1, 3, 4 and 6 (Read, Discover, Observe, "
      "Write lifetime, Execute update/reboot), deregisters after seconds (default 60, 0 until a key); dtls, psk or "
      "cert secure it with DTLS (port 5684)", UCXAPP_CMD_NEEDS_DEVICE, cmdRun },
    { "lwm2m.start", "<host> [port] [endpoint] [seconds] [dtls|psk|cert]",
      "the same LwM2M client in the background (seconds 0, the default: until lwm2m.stop)", UCXAPP_CMD_NEEDS_DEVICE,
      cmdStart },
    { "lwm2m.stop", "", "deregisters and stops the background LwM2M client", UCXAPP_CMD_NEEDS_DEVICE, cmdStop },
    { "lwm2m.status", "", "the background LwM2M client: server, security, counters, observations", 0, cmdStatus },
    { "lwm2m.psk", "[<identity> <hexkey>|clear]",
      "stores the DTLS pre-shared key for lwm2m.run ... psk (sealed in the secure store), shows or removes it", 0,
      cmdPsk },
};

void ucxapp_register_lwm2m_cmds(void)
{
    ucxapp_cmd_register(gLwm2mCmds, sizeof(gLwm2mCmds) / sizeof(gLwm2mCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_lwm2m_cmds(void)
{
}

#endif
