/* u-connectXplorer remote control client: a WebSocket (RFC 6455) client on the platform TCP socket, JSON-RPC 2.0 on
 * top, and the small JSON readers the xplorer and perf commands need. */

#include "ucxapp_xplorer.h"

#if UCXAPP_XPLORER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp.h"
#include "ucxapp_crypt.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"

#define XPLORER_FILE        "xplorer.cfg"
#define XPLORER_TOKEN_MAX   64
#define XPLORER_MSG_MAX     (4u * 1024u * 1024u)   /* a whole BLE scan list fits */
#define XPLORER_EVENTS_MAX  32

static intptr_t gSock = -1;
static char gHost[64];
static uint8_t *gIn;          /* received bytes not yet parsed as frames */
static size_t gInLen;
static size_t gInCap;
static char *gMsg;            /* text message being reassembled from fragments */
static size_t gMsgLen;
static int32_t gNextId = 1;
static char *gEvents[XPLORER_EVENTS_MAX];
static int gEventCount;

/* ---- JSON readers ----------------------------------------------------------------------------------------------- */

static const char *skipWs(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/* End of the string starting at the opening quote p (just past the closing quote). */
static const char *stringEnd(const char *p)
{
    for (p++; *p != '\0' && *p != '"'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
    }
    return *p == '"' ? p + 1 : p;
}

/* End of the value starting at p. */
static const char *valueEnd(const char *p)
{
    int depth = 0;

    p = skipWs(p);
    if (*p == '"') {
        return stringEnd(p);
    }
    if (*p != '{' && *p != '[') {
        while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n') {
            p++;
        }
        return p;
    }
    while (*p != '\0') {
        if (*p == '"') {
            p = stringEnd(p);
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) {
                return p + 1;
            }
        }
        p++;
    }
    return p;
}

/* Value of key (len bytes) in the object at p, NULL if missing; *end is set to the end of the value. */
static const char *findKey(const char *p, const char *key, size_t len, const char **end)
{
    p = skipWs(p);
    if (*p != '{') {
        return NULL;
    }
    p++;
    for (;;) {
        p = skipWs(p);
        if (*p != '"') {
            return NULL;
        }
        const char *k = p + 1;
        const char *ke = stringEnd(p);
        bool match = ke > k && (size_t)(ke - 1 - k) == len && memcmp(k, key, len) == 0;
        p = skipWs(ke);
        if (*p != ':') {
            return NULL;
        }
        p = skipWs(p + 1);
        const char *ve = valueEnd(p);
        if (match) {
            *end = ve;
            return p;
        }
        p = skipWs(ve);
        if (*p != ',') {
            return NULL;
        }
        p++;
    }
}

static const char *findPath(const char *json, const char *path, const char **end)
{
    const char *v = json;

    while (v != NULL) {
        const char *dot = strchr(path, '.');
        size_t len = dot != NULL ? (size_t)(dot - path) : strlen(path);
        v = findKey(v, path, len, end);
        if (dot == NULL) {
            return v;
        }
        path = dot + 1;
    }
    return NULL;
}

static char *dupSpan(const char *s, const char *e)
{
    char *out = (char *)malloc((size_t)(e - s) + 1);

    if (out != NULL) {
        memcpy(out, s, (size_t)(e - s));
        out[e - s] = '\0';
    }
    return out;
}

char *ucxapp_json_get(const char *json, const char *path)
{
    const char *end = NULL;
    const char *v = json != NULL ? findPath(json, path, &end) : NULL;

    return v != NULL ? dupSpan(v, end) : NULL;
}

bool ucxapp_json_str(const char *json, const char *path, char *out, size_t size)
{
    const char *end = NULL;
    const char *v = json != NULL ? findPath(json, path, &end) : NULL;
    size_t n = 0;

    if (v == NULL || *v != '"' || size == 0) {
        return false;
    }
    for (const char *p = v + 1; p < end - 1 && n + 1 < size; p++) {
        char c = *p;
        if (c == '\\' && p + 1 < end - 1) {
            c = *++p;
            if (c == 'n') {
                c = '\n';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'r') {
                c = '\r';
            } else if (c == 'u' && p + 4 < end - 1) {
                unsigned u = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = p[i];
                    u = u * 16u + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                }
                p += 4;
                c = u < 0x80u ? (char)u : '?';
            }
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

bool ucxapp_json_int(const char *json, const char *path, int64_t *out)
{
    const char *end = NULL;
    const char *v = json != NULL ? findPath(json, path, &end) : NULL;
    char *stop = NULL;

    if (v == NULL || (*v != '-' && (*v < '0' || *v > '9'))) {
        return false;
    }
    *out = (int64_t)strtoll(v, &stop, 10);
    return stop != v;
}

bool ucxapp_json_bool(const char *json, const char *path, bool *out)
{
    const char *end = NULL;
    const char *v = json != NULL ? findPath(json, path, &end) : NULL;

    if (v != NULL && strncmp(v, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (v != NULL && strncmp(v, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

char *ucxapp_json_item(const char *array, int index)
{
    const char *p = array != NULL ? skipWs(array) : "";

    if (*p != '[') {
        return NULL;
    }
    p = skipWs(p + 1);
    for (int i = 0; *p != '\0' && *p != ']'; i++) {
        const char *e = valueEnd(p);
        if (i == index) {
            return dupSpan(p, e);
        }
        p = skipWs(e);
        if (*p != ',') {
            return NULL;
        }
        p = skipWs(p + 1);
    }
    return NULL;
}

bool ucxapp_json_quote(const char *s, char *out, size_t size)
{
    size_t n = 0;

    if (size < 3) {
        return false;
    }
    out[n++] = '"';
    for (; *s != '\0'; s++) {
        char esc[8];
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            snprintf(esc, sizeof(esc), "\\%c", (char)c);
        } else if (c < 0x20u) {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
        } else {
            esc[0] = (char)c;
            esc[1] = '\0';
        }
        size_t len = strlen(esc);
        if (n + len + 2 > size) {
            return false;
        }
        memcpy(out + n, esc, len);
        n += len;
    }
    out[n++] = '"';
    out[n] = '\0';
    return true;
}

/* ---- WebSocket ------------------------------------------------------------------------------------------------- */

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;

    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0u) |
                     (i + 2 < len ? (uint32_t)in[i + 2] : 0u);
        out[o++] = kAlpha[(v >> 18) & 63u];
        out[o++] = kAlpha[(v >> 12) & 63u];
        out[o++] = i + 1 < len ? kAlpha[(v >> 6) & 63u] : '=';
        out[o++] = i + 2 < len ? kAlpha[v & 63u] : '=';
    }
    out[o] = '\0';
}

static void dropConnection(void)
{
    ucxapp_platform_tcp_close(gSock);
    gSock = -1;
    gHost[0] = '\0';
    gInLen = 0;
    free(gMsg);
    gMsg = NULL;
    gMsgLen = 0;
    for (int i = 0; i < gEventCount; i++) {
        free(gEvents[i]);
    }
    gEventCount = 0;
}

/* Reads more bytes into gIn; false when the connection is gone. *timedOut tells a timeout from a close. */
static bool fill(uint32_t timeoutMs, bool *timedOut)
{
    *timedOut = false;
    if (gInCap - gInLen < 4096u) {
        size_t cap = gInCap == 0 ? 16384u : gInCap * 2u;
        uint8_t *p = cap <= XPLORER_MSG_MAX * 2u ? (uint8_t *)realloc(gIn, cap) : NULL;
        if (p == NULL) {
            return false;
        }
        gIn = p;
        gInCap = cap;
    }
    int32_t n = ucxapp_platform_tcp_recv(gSock, gIn + gInLen, gInCap - gInLen, timeoutMs);
    if (n == 0) {
        *timedOut = true;
        return false;
    }
    if (n < 0) {
        return false;
    }
    gInLen += (size_t)n;
    return true;
}

static bool sendFrame(uint8_t opcode, const void *data, size_t len)
{
    uint8_t head[14];
    size_t h = 0;
    uint8_t mask[4];
    uint8_t *buf;
    bool ok;

    head[h++] = (uint8_t)(0x80u | opcode);
    if (len < 126u) {
        head[h++] = (uint8_t)(0x80u | len);
    } else if (len < 65536u) {
        head[h++] = 0x80u | 126u;
        head[h++] = (uint8_t)(len >> 8);
        head[h++] = (uint8_t)len;
    } else {
        head[h++] = 0x80u | 127u;
        for (int i = 7; i >= 0; i--) {
            head[h++] = (uint8_t)((uint64_t)len >> (8 * i));
        }
    }
    for (int i = 0; i < 4; i++) {
        mask[i] = (uint8_t)rand();
        head[h++] = mask[i];
    }
    buf = (uint8_t *)malloc(h + len);
    if (buf == NULL) {
        return false;
    }
    memcpy(buf, head, h);
    for (size_t i = 0; i < len; i++) {
        buf[h + i] = ((const uint8_t *)data)[i] ^ mask[i & 3u];
    }
    ok = ucxapp_platform_tcp_send(gSock, buf, h + len);
    free(buf);
    return ok;
}

/* One whole text message from gIn if it is there: malloc'ed, NUL terminated. Answers pings; *closed on a close
 * frame or a protocol error. */
static char *parseMessage(bool *closed)
{
    for (;;) {
        if (gInLen < 2) {
            return NULL;
        }
        uint8_t op = gIn[0] & 0x0Fu;
        bool fin = (gIn[0] & 0x80u) != 0;
        bool masked = (gIn[1] & 0x80u) != 0;
        uint64_t len = gIn[1] & 0x7Fu;
        size_t h = 2;
        if (len == 126u) {
            if (gInLen < 4) {
                return NULL;
            }
            len = (uint64_t)gIn[2] << 8 | gIn[3];
            h = 4;
        } else if (len == 127u) {
            if (gInLen < 10) {
                return NULL;
            }
            len = 0;
            for (int i = 0; i < 8; i++) {
                len = len << 8 | gIn[2 + i];
            }
            h = 10;
        }
        if (len > XPLORER_MSG_MAX) {
            *closed = true;
            return NULL;
        }
        size_t maskAt = h;
        h += masked ? 4u : 0u;
        if (gInLen < h + (size_t)len) {
            return NULL;
        }
        uint8_t *payload = gIn + h;
        if (masked) {
            for (size_t i = 0; i < (size_t)len; i++) {
                payload[i] ^= gIn[maskAt + (i & 3u)];
            }
        }
        char *done = NULL;
        if (op == 0x8u) {
            *closed = true;
        } else if (op == 0x9u) {
            (void)sendFrame(0xAu, payload, (size_t)len);
        } else if (op == 0x0u || op == 0x1u || op == 0x2u) {
            char *m = (char *)realloc(gMsg, gMsgLen + (size_t)len + 1);
            if (m == NULL || gMsgLen + (size_t)len > XPLORER_MSG_MAX) {
                free(m != NULL ? m : gMsg);   /* a failed realloc leaves the old block, still ours */
                gMsg = NULL;
                gMsgLen = 0;
                *closed = true;
            } else {
                memcpy(m + gMsgLen, payload, (size_t)len);
                gMsgLen += (size_t)len;
                m[gMsgLen] = '\0';
                gMsg = m;
                if (fin) {
                    done = gMsg;
                    gMsg = NULL;
                    gMsgLen = 0;
                }
            }
        }
        size_t used = h + (size_t)len;
        memmove(gIn, gIn + used, gInLen - used);
        gInLen -= used;
        if (done != NULL || *closed) {
            return done;
        }
    }
}

/* Next text message within timeoutMs (0 polls once); NULL on timeout or when the connection dropped (then closed). */
static char *readMessage(uint32_t timeoutMs)
{
    uint32_t t0 = ucxapp_platform_tick_ms();
    bool polled = false;

    while (gSock >= 0) {
        bool closed = false;
        char *m = parseMessage(&closed);
        if (m != NULL) {
            return m;
        }
        if (closed) {
            ucxapp_print("u-connectXplorer closed the remote-control connection\n");
            dropConnection();
            return NULL;
        }
        uint32_t spent = ucxapp_platform_tick_ms() - t0;
        if (spent >= timeoutMs && polled) {
            return NULL;
        }
        polled = true;
        bool timedOut;
        if (!fill(spent < timeoutMs ? timeoutMs - spent : 0, &timedOut)) {
            if (!timedOut) {
                ucxapp_print("lost the connection to u-connectXplorer\n");
                dropConnection();
            }
            return NULL;
        }
    }
    return NULL;
}

static bool handshake(const char *host, uint16_t port)
{
    uint8_t nonce[16];
    char key[32];
    char req[256];

    for (size_t i = 0; i < sizeof(nonce); i++) {
        nonce[i] = (uint8_t)rand();
    }
    base64(nonce, sizeof(nonce), key);
    int n = snprintf(req, sizeof(req),
                     "GET / HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", host, (unsigned)port, key);
    if (n <= 0 || (size_t)n >= sizeof(req) || !ucxapp_platform_tcp_send(gSock, req, (size_t)n)) {
        return false;
    }
    uint32_t t0 = ucxapp_platform_tick_ms();
    for (;;) {
        for (size_t i = 0; i + 3 < gInLen; i++) {
            if (memcmp(gIn + i, "\r\n\r\n", 4) == 0) {
                bool ok = gInLen >= 12 && memcmp(gIn, "HTTP/1.1 101", 12) == 0;
                memmove(gIn, gIn + i + 4, gInLen - i - 4);
                gInLen -= i + 4;
                return ok;
            }
        }
        bool timedOut;
        uint32_t spent = ucxapp_platform_tick_ms() - t0;
        if (spent >= 3000u || !fill(3000u - spent, &timedOut)) {
            return false;
        }
    }
}

/* ---- JSON-RPC --------------------------------------------------------------------------------------------------- */

static void queueEvent(char *msg)
{
    if (gEventCount == XPLORER_EVENTS_MAX) {
        free(gEvents[0]);
        memmove(gEvents, gEvents + 1, sizeof(gEvents[0]) * (XPLORER_EVENTS_MAX - 1));
        gEventCount--;
    }
    gEvents[gEventCount++] = msg;
}

char *ucxapp_xplorer_call(const char *method, const char *params, uint32_t timeoutMs)
{
    int32_t id = gNextId++;
    size_t size = strlen(method) + (params != NULL ? strlen(params) : 2u) + 80u;
    char *req = (char *)malloc(size);
    uint32_t t0 = ucxapp_platform_tick_ms();

    if (gSock < 0) {
        ucxapp_print("ERROR: not connected to u-connectXplorer\n");
        free(req);
        return NULL;
    }
    if (req == NULL) {
        return NULL;
    }
    int n = snprintf(req, size, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}", method,
                     params != NULL ? params : "{}", (int)id);
    bool sent = sendFrame(0x1u, req, (size_t)n);
    free(req);
    if (!sent) {
        ucxapp_print("ERROR: %s: lost the connection to u-connectXplorer\n", method);
        dropConnection();
        return NULL;
    }
    for (;;) {
        uint32_t spent = ucxapp_platform_tick_ms() - t0;
        char *msg = spent < timeoutMs ? readMessage(timeoutMs - spent) : NULL;
        if (msg == NULL) {
            if (gSock >= 0) {
                ucxapp_print("ERROR: %s: no answer from the phone within %u s\n", method,
                             (unsigned)(timeoutMs / 1000u));
            }
            return NULL;
        }
        int64_t got;
        if (!ucxapp_json_int(msg, "id", &got)) {
            queueEvent(msg);
            continue;
        }
        if (got != id) {
            free(msg);
            continue;
        }
        char *result = ucxapp_json_get(msg, "result");
        if (result == NULL) {
            char text[200] = "no result";
            int64_t code = 0;
            (void)ucxapp_json_str(msg, "error.message", text, sizeof(text));
            (void)ucxapp_json_int(msg, "error.code", &code);
            ucxapp_print("ERROR: %s: %s (%d)\n", method, text, (int)code);
        }
        free(msg);
        return result;
    }
}

char *ucxapp_xplorer_event(const char *method, uint32_t timeoutMs)
{
    uint32_t t0 = ucxapp_platform_tick_ms();
    char name[64];

    for (;;) {
        char *msg = NULL;
        if (gEventCount > 0) {
            msg = gEvents[0];
            memmove(gEvents, gEvents + 1, sizeof(gEvents[0]) * (size_t)(gEventCount - 1));
            gEventCount--;
        } else {
            uint32_t spent = ucxapp_platform_tick_ms() - t0;
            msg = readMessage(spent < timeoutMs ? timeoutMs - spent : 0);
            if (msg == NULL) {
                return NULL;
            }
        }
        if (ucxapp_json_str(msg, "method", name, sizeof(name)) && strcmp(name, method) == 0) {
            char *params = ucxapp_json_get(msg, "params");
            free(msg);
            if (params != NULL) {
                return params;
            }
            continue;
        }
        free(msg);
    }
}

/* ---- connection ------------------------------------------------------------------------------------------------ */

bool ucxapp_xplorer_open_to(const char *host, uint16_t port, const char *token)
{
    char params[XPLORER_TOKEN_MAX * 2 + 16];
    char quoted[XPLORER_TOKEN_MAX * 2];

    ucxapp_xplorer_close();
    srand(ucxapp_platform_tick_ms());
    /* iOS stops the server while the app is in the background, so the first attempts can be refused. */
    for (int attempt = 0; attempt < 3 && gSock < 0; attempt++) {
        if (attempt > 0) {
            ucxapp_platform_sleep_ms(500);
        }
        gSock = ucxapp_platform_tcp_connect(host, port, 3000);
    }
    if (gSock < 0) {
        ucxapp_print("ERROR: cannot reach u-connectXplorer at %s:%u; open the app on the phone (Settings > Remote "
                     "control on, app in the foreground, same Wi-Fi)\n", host, (unsigned)port);
        return false;
    }
    gInLen = 0;
    if (!handshake(host, port)) {
        ucxapp_print("ERROR: %s:%u is not the u-connectXplorer remote control (WebSocket upgrade failed)\n", host,
                     (unsigned)port);
        dropConnection();
        return false;
    }
    snprintf(gHost, sizeof(gHost), "%s", host);
    if (!ucxapp_json_quote(token, quoted, sizeof(quoted))) {
        ucxapp_print("ERROR: token too long\n");
        dropConnection();
        return false;
    }
    snprintf(params, sizeof(params), "{\"token\":%s}", quoted);
    char *r = ucxapp_xplorer_call("app.authenticate", params, 5000);
    if (r == NULL) {
        ucxapp_print("check the token in u-connectXplorer Settings > Remote control\n");
        ucxapp_xplorer_close();
        return false;
    }
    free(r);
    return true;
}

bool ucxapp_xplorer_saved(char *host, size_t hostSize, uint16_t *port, char *token, size_t tokenSize)
{
    size_t len = 0;
    char *text = (char *)ucxapp_store_load(XPLORER_FILE, &len);
    char *tab1 = text != NULL ? strchr(text, '\t') : NULL;
    char *tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
    int32_t p = 0;

    if (tab2 == NULL) {
        free(text);
        ucxapp_print("ERROR: no phone saved; run xplorer.connect <phone-ip> <token> first\n");
        return false;
    }
    *tab1 = '\0';
    *tab2 = '\0';
    tab2[1 + strcspn(tab2 + 1, "\r\n")] = '\0';
    p = atoi(tab1 + 1);
    bool ok = p > 0 && p < 65536 && ucxapp_secret_open(tab2 + 1, token, tokenSize);
    if (ok) {
        snprintf(host, hostSize, "%s", text);
        *port = (uint16_t)p;
    } else {
        ucxapp_print("ERROR: cannot read the saved phone (storage/ucxapp.key changed?); run xplorer.connect again\n");
    }
    free(text);
    return ok;
}

bool ucxapp_xplorer_save(const char *host, uint16_t port, const char *token)
{
    char sealed[UCXAPP_SECRET_LEN(XPLORER_TOKEN_MAX)];
    char line[sizeof(sealed) + 96];

    if (strlen(token) > XPLORER_TOKEN_MAX || !ucxapp_secret_seal(token, sealed, sizeof(sealed))) {
        ucxapp_print("ERROR: cannot save the token\n");
        return false;
    }
    int n = snprintf(line, sizeof(line), "%s\t%u\t%s\n", host, (unsigned)port, sealed);
    if (n <= 0 || (size_t)n >= sizeof(line) || !ucxapp_store_save(XPLORER_FILE, line, (size_t)n)) {
        ucxapp_print("ERROR: cannot save %s\n", XPLORER_FILE);
        return false;
    }
    ucxapp_store_make_private(XPLORER_FILE);
    return true;
}

bool ucxapp_xplorer_open(void)
{
    char host[64];
    char token[XPLORER_TOKEN_MAX + 1];
    uint16_t port = 0;

    if (gSock >= 0) {
        return true;
    }
    if (!ucxapp_xplorer_saved(host, sizeof(host), &port, token, sizeof(token))) {
        return false;
    }
    return ucxapp_xplorer_open_to(host, port, token);
}

void ucxapp_xplorer_close(void)
{
    if (gSock >= 0) {
        (void)sendFrame(0x8u, "\x03\xe8", 2);
    }
    dropConnection();
}

const char *ucxapp_xplorer_host(void)
{
    return gHost;
}

#endif /* UCXAPP_XPLORER */
