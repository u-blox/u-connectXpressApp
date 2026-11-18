/* lwm2m.* - a small OMA LwM2M 1.1 client (CoAP over UDP, no security) on a module UDP socket. It registers with a
 * server such as Eclipse Leshan, answers Read and Discover of the Server (1) and Device (3) objects with the
 * module's identity, executes Registration Update Trigger, sends updates and deregisters at the end.
 * Host builds only. */

#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
#include "u_cx_socket.h"
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

#define CO_CON           0
#define CO_NON           1
#define CO_ACK           2
#define CO_RST           3
#define CO_GET           0x01
#define CO_POST          0x02
#define CO_DELETE        0x04
#define CO_CREATED       0x41
#define CO_DELETED       0x42
#define CO_CHANGED       0x44
#define CO_CONTENT       0x45
#define CO_NOT_FOUND     0x84
#define CO_NOT_ALLOWED   0x85
#define CO_NOT_ACCEPT    0x86
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
} coapMsg_t;

typedef struct {
    uCxHandle_t *ucx;
    int32_t sock;
    uint16_t mid;
    uint8_t tok;
    char location[64];
    char maker[48];
    char model[48];
    char serial[48];
    char fw[48];
    bool updateNow;
    unsigned served;
} lw_t;

typedef struct {
    uint16_t obj;
    uint16_t res;
    char kind;            /* 's' string, 'i' integer, 'b' boolean, 't' time */
} lwRes_t;

static const lwRes_t kRes[] = {
    { 1, 0, 'i' }, { 1, 1, 'i' }, { 1, 6, 'b' }, { 1, 7, 's' },
    { 3, 0, 's' }, { 3, 1, 's' }, { 3, 2, 's' }, { 3, 3, 's' }, { 3, 13, 't' }, { 3, 16, 's' },
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

static bool putUint(uint8_t *out, size_t *n, size_t max, unsigned *prev, unsigned num, unsigned v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    size_t len = v > 0xFF ? 2 : v > 0 ? 1 : 0;
    return putOpt(out, n, max, prev, num, b + 2 - len, len);
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
        if (num == OPT_ACCEPT) {
            m->accept = 0;
            for (unsigned i = 0; i < l && i < 4; i++) {
                m->accept = (m->accept << 8) | v[i];
            }
        }
    }
    return true;
}

/* ---- LwM2M objects ---- */

static bool sendRaw(lw_t *lw, const uint8_t *buf, size_t len)
{
    return uCxSocketWrite(lw->ucx, lw->sock, buf, (int32_t)len) >= 0;
}

/* The resource's value as text (strings) or a number (the other kinds). */
static const char *resValue(const lw_t *lw, const lwRes_t *r, long long *num)
{
    *num = 0;
    switch (r->obj * 100 + r->res) {
    case 100: *num = LW_SSID; return NULL;
    case 101: *num = LW_LIFETIME; return NULL;
    case 106: return NULL;
    case 107: return "U";
    case 300: return lw->maker;
    case 301: return lw->model;
    case 302: return lw->serial;
    case 303: return lw->fw;
    case 313: *num = (long long)time(NULL); return NULL;
    default:  return "U";   /* 3/16 supported binding */
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
    const char *s = resValue(lw, r, &num);
    if (s != NULL) {
        return putTlv(out, n, max, 0xC0, r->res, (const uint8_t *)s, strlen(s));
    }
    uint8_t v[8];
    size_t len = r->kind == 'b' ? 1 : (num >= -128 && num <= 127) ? 1 : (num >= -32768 && num <= 32767) ? 2
               : (num >= -2147483647LL - 1 && num <= 2147483647LL) ? 4 : 8;
    for (size_t i = 0; i < len; i++) {
        v[i] = (uint8_t)((unsigned long long)num >> (8 * (len - 1 - i)));
    }
    return putTlv(out, n, max, 0xC0, r->res, v, len);
}

/* Read (GET without link-format) and Discover (GET, Accept: 40) of /obj, /obj/0 or /obj/0/res. */
static unsigned readOrDiscover(const lw_t *lw, const coapMsg_t *m, uint8_t *body, size_t *blen, unsigned *cf)
{
    unsigned ids[3] = { 0, 0, 0 };
    int count = 0;
    char extra;
    const char *p = m->path;

    *blen = 0;
    if (p[0] != '\0') {
        count = sscanf(p, "/%u/%u/%u%c", &ids[0], &ids[1], &ids[2], &extra);
    }
    if (count < 1 || count > 3 || (ids[0] != 1 && ids[0] != 3) || (count >= 2 && ids[1] != 0)) {
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
    if (one != NULL && (m->accept == -1 || m->accept == LW_CF_TEXT)) {
        long long num;
        const char *s = resValue(lw, one, &num);
        *blen = (size_t)(s != NULL ? snprintf((char *)body, LW_MAX, "%s", s) : snprintf((char *)body, LW_MAX, "%lld", num));
        *cf = LW_CF_TEXT;
        return CO_CONTENT;
    }
    if (m->accept != -1 && m->accept != LW_CF_TLV) {
        return CO_NOT_ACCEPT;
    }
    uint8_t inner[LW_MAX];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(kRes) / sizeof(kRes[0]); i++) {
        if (kRes[i].obj == ids[0] && (one == NULL || one == &kRes[i])) {
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

/* Answers a request from the server: piggybacked on the ACK of a confirmable one. */
static void serve(lw_t *lw, const coapMsg_t *m)
{
    uint8_t out[LW_MAX];
    uint8_t body[LW_MAX];
    size_t blen = 0;
    unsigned cf = LW_CF_TEXT;
    unsigned code;
    unsigned prev = 0;

    if (m->code == 0 || m->code >= 0x20) {
        if (m->type == CO_CON) {   /* a ping, or a response nobody waits for */
            size_t n = putHeader(out, CO_RST, 0, m->mid, NULL, 0);
            (void)sendRaw(lw, out, n);
        }
        return;
    }
    if (m->code == CO_GET) {
        code = readOrDiscover(lw, m, body, &blen, &cf);
    } else if (m->code == CO_POST && strcmp(m->path, "/1/0/8") == 0) {
        lw->updateNow = true;   /* Registration Update Trigger */
        code = CO_CHANGED;
    } else {
        code = CO_NOT_ALLOWED;
    }
    size_t n = putHeader(out, m->type == CO_CON ? CO_ACK : CO_NON, code, m->type == CO_CON ? m->mid : lw->mid++,
                         m->token, m->tkl);
    if (blen > 0) {
        (void)putUint(out, &n, sizeof(out), &prev, OPT_FORMAT, cf);
        (void)putPayload(out, &n, sizeof(out), body, blen);
    }
    (void)sendRaw(lw, out, n);
    lw->served++;
    static const char *const kMethod[] = { "?", "GET", "POST", "PUT", "DELETE" };
    ucxapp_print("lwm2m: %s %s%s -> %u.%02u\n", kMethod[m->code <= 4 ? m->code : 0], m->path[0] ? m->path : "/",
                 m->accept == LW_CF_LINK ? " (discover)" : "", code >> 5, code & 0x1F);
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
            uCxSocketReceiveFrom_t from;
            memset(&from, 0, sizeof(from));
            int32_t rc = ucxapp_udp_recvfrom(lw->ucx, lw->sock, (int32_t)sizeof(buf), buf, &from);
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

static bool registerNow(lw_t *lw, const char *ep)
{
    char epq[LW_EP_MAX + 4];
    char lt[16];
    coapMsg_t rsp;
    (void)snprintf(epq, sizeof(epq), "ep=%s", ep);
    (void)snprintf(lt, sizeof(lt), "lt=%d", LW_LIFETIME);
    const char *const query[] = { epq, lt, "lwm2m=1.1", "b=U" };
    unsigned code = request(lw, CO_POST, "/rd", query, 4, "</1/0>,</3/0>", &rsp);
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

/* lwm2m.run <host> [port] [endpoint] [seconds]: register, serve, update, deregister. */
static int cmdRun(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = 5683;
    int32_t seconds = 60;
    char ep[LW_EP_MAX + 1];
    lw_t lw;

    if (argc < 2 || argc > 5 || (argc >= 3 && (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535)) ||
        (argc == 5 && (!ucxapp_parse_int(argv[4], &seconds) || seconds < 0 || seconds > 86400))) {
        return UCXAPP_ERR_USAGE;
    }
    memset(&lw, 0, sizeof(lw));
    lw.ucx = app->ucx;
    lw.mid = LW_FIRST_MID;
    deviceText(&lw, uCxGeneralGetManufacturerIdentificationBegin, lw.maker, sizeof(lw.maker));
    deviceText(&lw, uCxGeneralGetDeviceModelIdentificationBegin, lw.model, sizeof(lw.model));
    deviceText(&lw, uCxGeneralGetSerialNumberBegin, lw.serial, sizeof(lw.serial));
    deviceText(&lw, uCxGeneralGetSoftwareVersionBegin, lw.fw, sizeof(lw.fw));
    if (argc >= 4) {
        if (strlen(argv[3]) == 0 || strlen(argv[3]) > LW_EP_MAX) {
            ucxapp_print("ERROR: the endpoint name takes 1 to %d characters\n", LW_EP_MAX);
            return UCXAPP_ERR_USAGE;
        }
        (void)snprintf(ep, sizeof(ep), "%s", argv[3]);
    } else {
        (void)snprintf(ep, sizeof(ep), "ucxapp-%s", lw.serial[0] ? lw.serial : "module");
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &lw.sock) != 0) {
        ucxapp_print("ERROR: failed to create a UDP socket\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxSocketConnect(app->ucx, lw.sock, argv[1], port) != 0) {
        ucxapp_print("ERROR: connect to %s:%d failed\n", argv[1], (int)port);
        uCxSocketClose(app->ucx, lw.sock);
        return UCXAPP_ERR_FAILED;
    }
    if (!registerNow(&lw, ep)) {
        uCxSocketClose(app->ucx, lw.sock);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("lwm2m: registered at %s:%d as %s (%s), lifetime %d s; %s\n", argv[1], (int)port, ep, lw.location,
                 LW_LIFETIME, seconds > 0 ? "any key stops" : "runs until a key is pressed");

    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t lastUpdate = start;
    uint8_t buf[LW_MAX];
    int rc = UCXAPP_OK;
    while (seconds == 0 || ucxapp_platform_tick_ms() - start < (uint32_t)seconds * 1000u) {
        if (ucxapp_platform_key_pressed()) {
            (void)ucxapp_platform_get_key();
            break;
        }
        if (lw.updateNow || ucxapp_platform_tick_ms() - lastUpdate >= LW_LIFETIME * 500u) {
            coapMsg_t rsp;
            lw.updateNow = false;
            lastUpdate = ucxapp_platform_tick_ms();
            unsigned code = request(&lw, CO_POST, lw.location, NULL, 0, NULL, &rsp);
            ucxapp_print("lwm2m: update -> %u.%02u\n", code >> 5, code & 0x1F);
            if (code == CO_NOT_FOUND && !registerNow(&lw, ep)) {
                rc = UCXAPP_ERR_FAILED;
                break;
            }
            continue;
        }
        uCxSocketReceiveFrom_t from;
        coapMsg_t m;
        memset(&from, 0, sizeof(from));
        int32_t n = ucxapp_udp_recvfrom(app->ucx, lw.sock, (int32_t)sizeof(buf), buf, &from);
        if (n > 0 && parse(buf, (size_t)n, &m)) {
            serve(&lw, &m);
        } else {
            ucxapp_platform_sleep_ms(LW_POLL_MS);
        }
    }
    if (rc == UCXAPP_OK) {
        coapMsg_t rsp;
        unsigned code = request(&lw, CO_DELETE, lw.location, NULL, 0, NULL, &rsp);
        ucxapp_print("lwm2m: deregistered (%u.%02u), %u request(s) served\n", code >> 5, code & 0x1F, lw.served);
    }
    uCxSocketClose(app->ucx, lw.sock);
    return rc;
}

static const ucxapp_cmd_t gLwm2mCmds[] = {
    { "lwm2m.run", "<host> [port] [endpoint] [seconds]",
      "LwM2M 1.1 client over a module UDP socket (no DTLS): registers, answers Read/Discover of objects 1 and 3, "
      "deregisters after seconds (default 60, 0 until a key)", UCXAPP_CMD_NEEDS_DEVICE, cmdRun },
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
