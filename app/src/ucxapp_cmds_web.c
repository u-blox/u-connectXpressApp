/* web.start / web.stop / web.status: HTTP + SSE server hosted on the module (NORA-W36 TCP listener).
 * The HTTP engine is u_webserver.c from u-connectMatter; this file adds routes, the output stream and the
 * threads that move socket work out of the URC context. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ucxapp_cmd.h"
#include "ucxapp_logcfg.h"
#include "ucxapp_store.h"
#include "ucxapp_thread.h"
#include "ucxapp_tls.h"
#include "ucxapp_tui.h"
#include "ucxapp_ui.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#include "u_port.h"
#include "u_webserver.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define DEFAULT_PORT   80
#define TOKEN_LEN      32   /* hex: 128 random bits */
#define MAX_EVENTS     64
#define OUT_CHUNK_ESC  380      /* escaped bytes per SSE event; the engine caps an event at 512 */
#define KEEPALIVE_MS   15000
#define MAX_JOB        256
#ifndef DL_CHUNK
#define DL_CHUNK       16384    /* download slice; the page asks again until a shorter slice arrives */
#endif
#define FILE_NAME_MAX  64
#define COOKIE         "ucxt"

enum { EVT_INCOMING, EVT_DATA, EVT_CLOSED };

typedef struct {
    int type;
    int32_t sock;
    int32_t n;
} webEvent_t;

/* kPage: app/web/index.html, turned into a C array by CMake at configure time */
#include "ucxapp_webpage.h"
#ifdef UCXAPP_PAGE_GZ
#include "ucxapp_webpage_gz.h"   /* kPageGz: the same page gzip-compressed, for browsers that accept it */
#endif
#include "ucxapp_favicon.h"

static ucxapp_t *gApp;
static uWebServer_t *gServer;
static uint16_t gPort;
static bool gHttps;
static char gToken[TOKEN_LEN + 1];
static volatile bool gRunning;
static volatile bool gStop;
static U_CX_MUTEX_HANDLE gLock;
static ucxapp_thread_t *gPumpThread;

static webEvent_t gEvents[MAX_EVENTS];
static int gEvHead, gEvCount;

static ucxapp_store_writer_t *gUpload;      /* one upload at a time, in slices */
static char gUploadName[FILE_NAME_MAX];
static uint32_t gUploadOff;

/* WebAssembly build: the page calls the routes directly and polls the events, no socket server. */
static bool gLocal;
static uHttpRequest_t gLocalReq;            /* the page's requests; only these skip the token */
static char gLocalEvents[4096];
static size_t gLocalLen;

/* ---- queues shared with the URC and print contexts ------------------------ */

static void pushEvent(int type, int32_t sock, int32_t n)
{
    if (!gRunning) {
        return;
    }
    U_CX_MUTEX_LOCK(gLock);
    if (gEvCount < MAX_EVENTS) {
        webEvent_t *e = &gEvents[(gEvHead + gEvCount) % MAX_EVENTS];
        e->type = type;
        e->sock = sock;
        e->n = n;
        gEvCount++;
    }
    U_CX_MUTEX_UNLOCK(gLock);
}

void ucxapp_web_on_incoming(int32_t sock, int32_t listenSock)
{
    pushEvent(EVT_INCOMING, sock, listenSock);
}

void ucxapp_web_on_data(int32_t sock, int32_t bytes)
{
    pushEvent(EVT_DATA, sock, bytes);
}

void ucxapp_web_on_closed(int32_t sock)
{
    pushEvent(EVT_CLOSED, sock, 0);
}

static bool popEvent(webEvent_t *e)
{
    bool have = false;

    U_CX_MUTEX_LOCK(gLock);
    if (gEvCount > 0) {
        *e = gEvents[gEvHead];
        gEvHead = (gEvHead + 1) % MAX_EVENTS;
        gEvCount--;
        have = true;
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return have;
}

/* ---- pump thread: the only thread that touches the HTTP engine ------------ */

/* Builds a JSON string from the start of raw; returns the raw bytes used. */
static int escapeOutput(const char *raw, int rawLen, char *out, size_t outSize)
{
    int used = 0;
    size_t n = 0;

    out[n++] = '"';
    while (used < rawLen && n + 8 < outSize && n < OUT_CHUNK_ESC) {
        unsigned char c = (unsigned char)raw[used];
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (c == '\t') {
            out[n++] = '\\';
            out[n++] = 't';
        } else if (c == '\r') {
            /* dropped */
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, outSize - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
        used++;
    }
    /* do not split a UTF-8 sequence */
    while (used < rawLen && ((unsigned char)raw[used] & 0xC0) == 0x80 && n + 8 < outSize) {
        out[n++] = raw[used];
        used++;
    }
    out[n++] = '"';
    out[n] = '\0';
    return used;
}

/* Every page has its own place in the shared log and event streams: a chunk one tab cannot take yet
   (its last event still draining) is not lost for it when another tab takes it. Slot i is SSE client i of the
   server, telling a new connection in a reused slot by its generation; the browser build has one viewer. */
typedef struct {
    uint32_t gen;        /* 0: no page here */
    uint32_t logCur, evCur, doneSeen;
    uint32_t stateSeen;
    int32_t stateMs;
    uint32_t cardSeen[4];   /* hash of the GNSS, cellular, connections and Matter cards the page has */
} viewer_t;

/* The server's event clients, then the browser build's own page: the pump thread serves the first ones,
   ucxapp_web_local_poll the last, so a module web server started from the page does not share cursors. */
#define LOCAL_SLOT U_WEBSERVER_MAX_CLIENTS
static viewer_t gViewers[U_WEBSERVER_MAX_CLIENTS + 1];

/* > 0 when the event was taken by viewer i. */
static int32_t sendTo(int i, const char *name, const char *data)
{
    if (i != LOCAL_SLOT) {
        return uWebServerSendSSEEvent(gServer, i, name, data) == 0 ? 1 : 0;
    }
    size_t need = strlen(name) + strlen(data) + 2;
    if (gLocalLen + need >= sizeof(gLocalEvents)) {
        return 0;
    }
    gLocalLen += (size_t)snprintf(gLocalEvents + gLocalLen, sizeof(gLocalEvents) - gLocalLen, "%s\t%s\n", name, data);
    return 1;
}

/* The viewer of slot i, started at the current end of the streams when its page is new; NULL when the slot is empty. */
static viewer_t *viewer(int i)
{
    viewer_t *v = &gViewers[i];
    uint32_t gen = 1;

    if (i == LOCAL_SLOT ? !gLocal : gServer == NULL || !uWebServerGetSSEClient(gServer, i, &gen)) {
        v->gen = 0;
        return NULL;
    }
    if (v->gen != gen) {
        v->gen = gen;
        v->logCur = ucxapp_ui_log_start();
        v->evCur = ucxapp_ui_event_start();
        v->doneSeen = ucxapp_ui_done_seq(&(int){ 0 });
        v->stateSeen = UINT32_MAX;   /* a new page gets the state at once */
        v->cardSeen[0] = UINT32_MAX;
        v->cardSeen[1] = UINT32_MAX;
        v->cardSeen[2] = UINT32_MAX;
        v->cardSeen[3] = UINT32_MAX;
        v->stateMs = U_CX_PORT_GET_TIME_MS();
    }
    return v;
}

/* Sends as much pending output as the engine takes to viewers first..last; returns true if output is still waiting for someone. */
static bool flushOutput(int first, int last)
{
    char raw[256], json[600];
    bool pending = false;

    for (int i = first; i <= last; i++) {
        viewer_t *v = viewer(i);

        for (int round = 0; v != NULL && round < 32; round++) {
            int avail = (int)ucxapp_ui_log_peek(&v->logCur, raw, sizeof(raw));
            if (avail == 0) {
                break;
            }
            int used = escapeOutput(raw, avail, json, sizeof(json));
            if (sendTo(i, "out", json) <= 0) {
                pending = true;   /* its previous event still draining; retry next pass */
                break;
            }
            v->logCur += (uint32_t)used;
            pending = pending || round == 31;
        }
    }
    return pending;
}

static void flushEvents(int first, int last)
{
    char line[96], json[2 * sizeof(line) + 4];

    for (int i = first; i <= last; i++) {
        viewer_t *v = viewer(i);

        for (int round = 0; v != NULL && round < 4 && ucxapp_ui_event_peek(&v->evCur, line, sizeof(line)); round++) {
            size_t n = 0;

            json[n++] = '"';
            for (const char *p = line; *p != '\0'; p++) {
                if (*p == '"' || *p == '\\') {
                    json[n++] = '\\';
                    json[n++] = *p;
                } else if ((unsigned char)*p >= 0x20) {
                    json[n++] = *p;
                }
            }
            json[n++] = '"';
            json[n] = '\0';
            if (sendTo(i, "evt", json) <= 0) {
                break;   /* retry next pass */
            }
            v->evCur++;
        }
    }
}

/* Sent to a page on change, when it connects and every 5 s. */
static void flushState(int first, int last)
{
    char text[360], json[370];
    int32_t now = U_CX_PORT_GET_TIME_MS();
    uint32_t version = ucxapp_ui_state(text, sizeof(text));

    snprintf(json, sizeof(json), "\"%s\"", text);
    for (int i = first; i <= last; i++) {
        viewer_t *v = viewer(i);

        if (v != NULL && (version != v->stateSeen || now - v->stateMs >= 5000) && sendTo(i, "state", json) > 0) {
            v->stateSeen = version;
            v->stateMs = now;
        }
    }
}

/* A live card (empty: hide it), rebuilt every 250 ms and sent to a page when it changed. */
typedef struct {
    const char *event;
    bool (*get)(char *out, size_t size);
    int slot;   /* in viewer_t.cardSeen */
    char text[UCXAPP_CONN_SNAPSHOT];
    char json[UCXAPP_CONN_SNAPSHOT + 512];
    uint32_t hash;
    int32_t builtMs;
    bool built;
} card_t;

static void flushCard(card_t *c, int first, int last)
{
    int32_t now = U_CX_PORT_GET_TIME_MS();

    if (!c->built || now - c->builtMs >= 250) {
        size_t n = 0;

        c->built = true;
        c->builtMs = now;
        if (!c->get(c->text, sizeof(c->text))) {
            c->text[0] = '\0';
        }
        c->hash = 2166136261u;
        c->json[n++] = '"';
        for (const char *p = c->text; *p != '\0' && n + 4 < sizeof(c->json); p++) {
            c->hash = (c->hash ^ (uint8_t)*p) * 16777619u;
            if (*p == '\n') {
                c->json[n++] = '\\';
                c->json[n++] = 'n';
            } else if (*p == '"' || *p == '\\') {
                c->json[n++] = '\\';
                c->json[n++] = *p;
            } else if ((unsigned char)*p >= 0x20) {
                c->json[n++] = *p;
            }
        }
        c->json[n++] = '"';
        c->json[n] = '\0';
    }
    for (int i = first; i <= last; i++) {
        viewer_t *v = viewer(i);

        if (v != NULL && v->cardSeen[c->slot] != c->hash && sendTo(i, c->event, c->json) > 0) {
            v->cardSeen[c->slot] = c->hash;
        }
    }
}

/* The GNSS and cellular cards (the cellular one from the worker's cache: the modem is never asked from here). */
static void flushCards(int first, int last)
{
    static card_t gnss = { .event = "gnss", .get = ucxapp_gnss_card, .slot = 0 };
    static card_t cell = { .event = "cell", .get = ucxapp_ui_cell_card, .slot = 1 };
    static card_t conn = { .event = "conn", .get = ucxapp_ui_conn_card, .slot = 2 };

    flushCard(&gnss, first, last);
    flushCard(&cell, first, last);
    flushCard(&conn, first, last);
#if UCXAPP_HOST   /* the boards run no Matter plug-in */
    static card_t matter = { .event = "matter", .get = ucxapp_matter_card, .slot = 3 };

    flushCard(&matter, first, last);
#endif
}

static void flushDone(int first, int last)
{
    int rc;
    uint32_t seq = ucxapp_ui_done_seq(&rc);
    char text[16], probe[8];

    snprintf(text, sizeof(text), "%d", rc);
    for (int i = first; i <= last; i++) {
        viewer_t *v = viewer(i);
        uint32_t cur = v != NULL ? v->logCur : 0;

        /* "done" only after the command's output has reached this page */
        if (v != NULL && seq != v->doneSeen && ucxapp_ui_log_peek(&cur, probe, sizeof(probe)) == 0 &&
            sendTo(i, "done", text) > 0) {
            v->doneSeen = seq;
        }
    }
}

static void pumpLoop(void *arg)
{
    int32_t lastKeepAlive = U_CX_PORT_GET_TIME_MS();

    (void)arg;
    ucxappLogAtQuiet(true);
    while (!gStop) {
        webEvent_t e;
        while (!gStop && popEvent(&e)) {
            switch (e.type) {
            case EVT_INCOMING: {
                /* Accept either handle order in +UESOIC (see sockIncomingUrc in ucxapp_urc.c). A connection to
                 * another listener (a bridge, sock.listen) is not the page's: taking it would read its data. */
                int32_t listen = uWebServerGetListenSocket(gServer);
                if (e.sock == listen || e.n == listen) {
                    uWebServerHandleIncomingConnection(gServer, gApp->ucx, e.sock == listen ? e.n : e.sock, NULL,
                                                       listen);
                }
                break;
            }
            case EVT_DATA:
                uWebServerHandleDataAvailable(gServer, gApp->ucx, e.sock, e.n);
                break;
            default:
                uWebServerHandleSocketClosed(gServer, gApp->ucx, e.sock);
                break;
            }
        }
        bool pending = flushOutput(0, LOCAL_SLOT - 1);
        flushEvents(0, LOCAL_SLOT - 1);
        flushState(0, LOCAL_SLOT - 1);
        flushCards(0, LOCAL_SLOT - 1);
        flushDone(0, LOCAL_SLOT - 1);
        if (U_CX_PORT_GET_TIME_MS() - lastKeepAlive > KEEPALIVE_MS) {
            uWebServerBroadcastSSEEvent(gServer, "ka", "");
            lastKeepAlive = U_CX_PORT_GET_TIME_MS();
        }
        uWebServerProcess(gServer);
        U_CX_PORT_SLEEP_MS(pending ? 1 : 20);
    }
}

/* ---- routes ---------------------------------------------------------------- */

/* The token from the cookie set by /api/groups (EventSource cannot send headers). */
static bool cookieToken(const uHttpRequest_t *req, char *t, size_t size)
{
    char cookies[256];
    const char *p = cookies;

    if (uWebServerGetHeader(req, "Cookie", cookies, sizeof(cookies)) != 0) {
        return false;
    }
    while (*p != '\0') {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        if (strncmp(p, COOKIE "=", sizeof(COOKIE)) == 0) {
            size_t n = strcspn(p + sizeof(COOKIE), ";");
            snprintf(t, size, "%.*s", (int)n, p + sizeof(COOKIE));
            return true;
        }
        p += strcspn(p, ";");
    }
    return false;
}

static bool tokenOk(const uHttpRequest_t *req)
{
    char t[TOKEN_LEN + 8];
    unsigned diff = 0;

    if (req == &gLocalReq || (gHttps && gToken[0] == '\0')) {
        return true;   /* the same browser tab, or HTTPS without a token (the default) */
    }
    if (uWebServerGetHeader(req, "X-Token", t, sizeof(t)) != 0 && !cookieToken(req, t, sizeof(t)) &&
        uWebServerGetQueryParam(req, "t", t, sizeof(t)) != 0) {
        return false;
    }
    size_t len = strlen(gToken);
    if (len == 0 || strlen(t) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        diff |= (unsigned char)t[i] ^ (unsigned char)gToken[i];
    }
    return diff == 0;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void urlDecode(char *s)
{
    char *w = s;

    for (const char *r = s; *r != '\0'; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && hexVal(r[1]) >= 0 && hexVal(r[2]) >= 0) {
            *w++ = (char)(hexVal(r[1]) * 16 + hexVal(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}


#ifdef UCXAPP_PAGE_GZ
/* True when an Accept-Encoding header lists gzip (header names are case-insensitive). */
static bool acceptsGzip(const char *headers)
{
    static const char kName[] = "accept-encoding:";
    for (const char *line = headers; line != NULL && *line != '\0';) {
        size_t i = 0;
        while (kName[i] != '\0' && tolower((unsigned char)line[i]) == kName[i]) {
            i++;
        }
        const char *end = strchr(line, '\n');
        if (kName[i] == '\0') {
            size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
            for (size_t j = i; j + 4 <= len; j++) {
                size_t k = 0;
                while (k < 4 && tolower((unsigned char)line[j + k]) == "gzip"[k]) {
                    k++;
                }
                if (k == 4) {
                    return true;
                }
            }
            return false;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}
#endif

static void routePage(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    /* The page holds no data; the token travels in the URL fragment (never sent to the server)
     * and every API call checks it. */
    (void)req;
    (void)user;
#ifdef kPageBytes_GZIP   /* boards keep the page gzip-compressed in flash */
    resp->status_code = 200;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/html");
    snprintf(resp->custom_headers, sizeof(resp->custom_headers), "Content-Encoding: gzip\r\n");
    resp->body = (const char *)kPageBytes;
    resp->body_length = (int32_t)sizeof(kPageBytes);
#else
#ifdef UCXAPP_PAGE_GZ
    if (acceptsGzip(req->headers)) {
        resp->status_code = 200;
        snprintf(resp->content_type, sizeof(resp->content_type), "text/html");
        snprintf(resp->custom_headers, sizeof(resp->custom_headers), "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
        resp->body = (const char *)kPageGz;
        resp->body_length = (int32_t)sizeof(kPageGz);
        resp->body_static = true;
        return;
    }
#endif
    uWebServerSendHTML(resp, kPage);
#endif
    resp->body_static = true;
}

static void routeFavicon(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    (void)req;
    (void)user;
    resp->status_code = 200;
    snprintf(resp->content_type, sizeof(resp->content_type), "image/svg+xml");
    resp->body = (const char *)kFavicon;
    resp->body_length = (int32_t)sizeof(kFavicon);
    resp->body_static = true;
}

static void routeManual(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    const char *text = ucxapp_manual_text();
    (void)req;
    (void)user;
    resp->status_code = 200;
    snprintf(resp->content_type, sizeof(resp->content_type), "text/markdown; charset=utf-8");
    resp->body = text;
    resp->body_length = (int32_t)strlen(text);
    resp->body_static = true;
}

static void routeEvents(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerRegisterSSE(gServer, req, resp) < 0) {
        uWebServerSendError(resp, 500, "no event slot");
    }
}

static void routeExec(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char cmd[MAX_JOB], line[MAX_JOB], vals[UCXAPP_UI_MAX_FIELDS][96], tmp[8];
    const char *ptrs[UCXAPP_UI_MAX_FIELDS];
    int count = 0;
    const char *name = cmd;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerGetQueryParam(req, "cmd", cmd, sizeof(cmd)) != 0) {
        uWebServerSendError(resp, 400, "missing cmd");
        return;
    }
    urlDecode(cmd);
    while (*name == ' ' || *name == '\t') {
        name++;
    }
    if (*name == '\0' || strncmp(name, "web.", 4) == 0) {
        uWebServerSendError(resp, 400, "command not allowed");
        return;
    }
    {
        char word[48];
        const ucxapp_cmd_t *c;

        snprintf(word, sizeof(word), "%.*s", (int)strcspn(name, " \t"), name);
        c = ucxapp_cmd_find(word);
        if (c != NULL && (c->flags & UCXAPP_CMD_DANGER) && uWebServerGetQueryParam(req, "y", tmp, sizeof(tmp)) != 0) {
            uWebServerSendError(resp, 428, "confirm");
            return;
        }
    }
    for (int i = 0; i < UCXAPP_UI_MAX_FIELDS; i++) {
        char key[8];

        snprintf(key, sizeof(key), "a%d", i);
        ptrs[i] = "";
        if (uWebServerGetQueryParam(req, key, vals[i], sizeof(vals[i])) == 0) {
            urlDecode(vals[i]);
            ptrs[i] = vals[i];
            count = i + 1;
        }
    }
    if (count > 0) {
        bool raw = uWebServerGetQueryParam(req, "r", tmp, sizeof(tmp)) == 0;
        ucxapp_ui_compose(name, ptrs, count, raw, line, sizeof(line));
    } else {
        snprintf(line, sizeof(line), "%s", name);
    }
    if (ucxapp_ui_submit(line)) {
        uWebServerSendText(resp, "accepted");
    } else {
        uWebServerSendError(resp, 409, "busy");
    }
}

/* ---- generated command list for the page ------------------------------------- */

#ifndef API_BUF
#define API_BUF    65536   /* the boards set less: their command list is shorter */
#endif
#define API_MARGIN 8192   /* the largest command entry (gnss.bridge with all its forms) */
#define MAX_HITS   60

/* Response bodies must outlive the handler, hence static storage; only the pump thread runs handlers. */
static char gBuf[API_BUF];
static uint8_t gFileBuf[DL_CHUNK];

static void jsonStr(char *buf, size_t *n, const char *s)
{
    for (; *s != '\0' && *n + 8 < API_BUF; s++) {
        if (*s == '"' || *s == '\\') {
            buf[(*n)++] = '\\';
            buf[(*n)++] = *s;
        } else if (*s == '\n') {
            buf[(*n)++] = '\\';
            buf[(*n)++] = 'n';
        } else if ((unsigned char)*s >= 0x20) {
            buf[(*n)++] = *s;
        }
    }
}

static void appendFields(size_t *pn, const ucxapp_ui_field_t *fields, int nf)
{
    size_t n = *pn;

    gBuf[n++] = '[';
    for (int k = 0; k < nf; k++) {
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s[\"", k > 0 ? "," : "");
        jsonStr(gBuf, &n, fields[k].name);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",%d,\"", fields[k].optional ? 1 : 0);
        jsonStr(gBuf, &n, fields[k].choices);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"");
        jsonStr(gBuf, &n, fields[k].def);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"%c\",\"", fields[k].kind);
        jsonStr(gBuf, &n, fields[k].key);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\"]");
    }
    gBuf[n++] = ']';
    *pn = n;
}

static void appendCmd(size_t *pn, bool *first, const ucxapp_cmd_t *c)
{
    /* 48 KB of parse space: on the heap only while one entry is written, so the boards keep it out of static RAM. */
    ucxapp_ui_variant_t *forms = malloc(sizeof(*forms) * UCXAPP_UI_MAX_VARIANTS);
    size_t n = *pn;

    if (forms == NULL || strncmp(c->name, "web.", 4) == 0 || n + API_MARGIN >= API_BUF) {
        free(forms);
        return;
    }
    int nv = ucxapp_ui_parse_variants(c->usage, forms, UCXAPP_UI_MAX_VARIANTS);
    gBuf[n++] = *first ? ' ' : ',';
    *first = false;
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "{\"n\":\"");
    jsonStr(gBuf, &n, c->name);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"h\":\"");
    jsonStr(gBuf, &n, c->help);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"u\":\"");
    jsonStr(gBuf, &n, c->usage);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",%s\"f\":", (c->flags & UCXAPP_CMD_DANGER) ? "\"d\":1," : "");
    if (nv == 1 && forms[0].literal[0] == '\0') {
        appendFields(&n, forms[0].fields, forms[0].count);
    } else if (nv < 1) {
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "null");
    } else {
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "null,\"v\":[");
        for (int k = 0; k < nv; k++) {
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s{\"l\":\"", k > 0 ? "," : "");
            jsonStr(gBuf, &n, forms[k].label);
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"p\":\"");
            jsonStr(gBuf, &n, forms[k].literal);
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"f\":");
            appendFields(&n, forms[k].fields, forms[k].count);
            gBuf[n++] = '}';
        }
        gBuf[n++] = ']';
    }
    gBuf[n++] = '}';
    *pn = n;
    free(forms);
}

static bool hasExamples(const char *group)
{
    const char *g, *t, *l;

    for (size_t i = 0; ucxapp_example_get(i, &g, &t, &l); i++) {
        if (strcmp(g, group) == 0) {
            return true;
        }
    }
    return false;
}

static void sendList(uHttpResponse_t *resp, size_t n)
{
    gBuf[n++] = ']';
    gBuf[n] = '\0';
    uWebServerSendJSON(resp, gBuf);
}

static char gGroups[UCXAPP_UI_MAX_GROUPS][UCXAPP_UI_GROUP_LEN];

/* The JSON builders fill gBuf (terminated) and return its length. */
static size_t buildGroups(void)
{
    int count = ucxapp_ui_groups(gGroups, UCXAPP_UI_MAX_GROUPS);
    size_t n = 0;
    bool first = true;

    gBuf[n++] = '[';
    for (int pass = 0; pass < 2; pass++) {
        const char *where = pass == 0 ? "module" : "host";

        for (int i = 0; i < count; i++) {
            if (strcmp(gGroups[i], "web") != 0 && strcmp(ucxapp_ui_runs_on(gGroups[i]), where) == 0) {
                n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s\"%s:%s:%s:%s:", first ? "" : ",", gGroups[i], where,
                                      hasExamples(gGroups[i]) ? "x" : "", ucxapp_ui_group_section(gGroups[i]));
                jsonStr(gBuf, &n, ucxapp_ui_group_help(gGroups[i]));
                gBuf[n++] = '"';
                first = false;
            }
        }
    }
    gBuf[n++] = ']';
    gBuf[n] = '\0';
    return n;
}

static size_t buildCommands(const char *group)
{
    static const ucxapp_cmd_t *cmds[UCXAPP_UI_MAX_GROUP_CMDS];
    int count = ucxapp_ui_group_cmds(group, cmds, UCXAPP_UI_MAX_GROUP_CMDS);
    size_t n = 0;
    bool first = true;

    gBuf[n++] = '[';
    for (int i = 0; i < count; i++) {
        appendCmd(&n, &first, cmds[i]);
    }
    gBuf[n++] = ']';
    gBuf[n] = '\0';
    return n;
}

/* [{"t":title,"l":lines}] */
static size_t buildExamples(const char *group)
{
    const char *g, *t, *l;
    size_t n = 0;

    gBuf[n++] = '[';
    for (size_t i = 0; ucxapp_example_get(i, &g, &t, &l) && n + 1024 < API_BUF; i++) {
        if (strcmp(g, group) != 0) {
            continue;
        }
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s{\"t\":\"", n > 1 ? "," : "");
        jsonStr(gBuf, &n, t);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"l\":\"");
        jsonStr(gBuf, &n, l);
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "\"}");
    }
    gBuf[n++] = ']';
    gBuf[n] = '\0';
    return n;
}

static void routeList(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char group[16];

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (strcmp(req->path, "/api/groups") == 0) {
        buildGroups();
        uWebServerSendJSON(resp, gBuf);
        /* the event stream cannot send X-Token, so it authenticates with this cookie */
        if (gToken[0] != '\0') {
            snprintf(resp->custom_headers, sizeof(resp->custom_headers),
                     "Set-Cookie: " COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict\r\n", gToken);
        }
        return;
    }
    if (uWebServerGetQueryParam(req, "g", group, sizeof(group)) != 0) {
        uWebServerSendError(resp, 400, "missing g");
        return;
    }
    buildCommands(group);
    uWebServerSendJSON(resp, gBuf);
}

static void routeVersion(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    size_t n = 0;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    n += (size_t)snprintf(gBuf, API_BUF, "{\"v\":\"");
    jsonStr(gBuf, &n, ucxapp_version_text());
    /* real: a hardware module is open, so the page hides Demo (it only runs on a virtual module) */
    snprintf(gBuf + n, API_BUF - n, "\",\"at\":%d,\"real\":%d}", ucxappLogAtShown() ? 1 : 0,
             gApp != NULL && gApp->ucx != NULL && !ucxapp_module_is_virtual(gApp) ? 1 : 0);
    uWebServerSendJSON(resp, gBuf);
}

typedef struct {
    char q[48];
    bool prefix;
    const ucxapp_cmd_t *hits[MAX_HITS];
    int count;
} findCtx_t;

static bool containsNoCase(const char *hay, const char *needle)
{
    size_t len = strlen(needle);

    for (; *hay != '\0'; hay++) {
        size_t i = 0;
        while (i < len && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == len) {
            return true;
        }
    }
    return len == 0;
}

static void findVisit(const ucxapp_cmd_t *c, void *user)
{
    findCtx_t *ctx = (findCtx_t *)user;
    bool hit = ctx->prefix ? strncmp(c->name, ctx->q, strlen(ctx->q)) == 0
                           : containsNoCase(c->name, ctx->q) || containsNoCase(c->help, ctx->q);

    if (hit && ctx->count < MAX_HITS) {
        ctx->hits[ctx->count++] = c;
    }
}

/* /api/find?q=text[&p=1]: commands whose name or help holds q, or (p=1) whose name starts with q. */
static void routeFind(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    static findCtx_t ctx;
    char tmp[4];
    size_t n = 0;
    bool first = true;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerGetQueryParam(req, "q", ctx.q, sizeof(ctx.q)) != 0) {
        uWebServerSendError(resp, 400, "missing q");
        return;
    }
    urlDecode(ctx.q);
    ctx.prefix = uWebServerGetQueryParam(req, "p", tmp, sizeof(tmp)) == 0;
    ctx.count = 0;
    ucxapp_cmd_foreach(findVisit, &ctx);
    gBuf[n++] = '[';
    for (int i = 0; i < ctx.count; i++) {
        appendCmd(&n, &first, ctx.hits[i]);
    }
    sendList(resp, n);
}

/* /api/complete?q=line: {"l":completed line,"n":count,"c":"candidates"}, the same as Tab in the TUI. */
static void routeComplete(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    static char q[200], line[200], list[1024];
    size_t n = 0;
    int count;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerGetQueryParam(req, "q", q, sizeof(q)) != 0) {
        uWebServerSendError(resp, 400, "missing q");
        return;
    }
    urlDecode(q);
    count = ucxapp_cmd_complete_line(q, line, sizeof(line), list, sizeof(list));
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "{\"l\":\"");
    jsonStr(gBuf, &n, line);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"n\":%d,\"c\":\"", count);
    jsonStr(gBuf, &n, count > 1 ? list : "");
    snprintf(gBuf + n, API_BUF - n, "\"}");
    uWebServerSendJSON(resp, gBuf);
}

/* /api/conn: {"t":"snapshot lines"} for the Connections view; asking keeps the worker refreshing it. */
static void routeConn(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    static char text[UCXAPP_CONN_SNAPSHOT];
    size_t n = 0;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    (void)ucxapp_ui_conn_text(text, sizeof(text));
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "{\"t\":\"");
    jsonStr(gBuf, &n, text);
    snprintf(gBuf + n, API_BUF - n, "\"}");
    uWebServerSendJSON(resp, gBuf);
}

/* /api/examples?g=group */
static void routeExamples(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char group[16];

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerGetQueryParam(req, "g", group, sizeof(group)) != 0) {
        uWebServerSendError(resp, 400, "missing g");
        return;
    }
    buildExamples(group);
    uWebServerSendJSON(resp, gBuf);
}

/* ---- files ------------------------------------------------------------------ */

/* Plain names only: no folders, no parent references, no hidden files. */
static bool fileName(const uHttpRequest_t *req, char *name)
{
    if (uWebServerGetQueryParam(req, "n", name, FILE_NAME_MAX) != 0) {
        return false;
    }
    urlDecode(name);
    if (name[0] == '\0' || name[0] == '.') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if ((unsigned char)*p < 0x20 || strchr("/\\:*?\"<>|", *p) != NULL) {
            return false;
        }
    }
    return true;
}

static bool isKey(const char *name)
{
    size_t n = strlen(name);

    return n >= 4 && (strcmp(name + n - 4, ".key") == 0 || strcmp(name + n - 4, ".KEY") == 0);
}

static uint32_t queryOffset(const uHttpRequest_t *req)
{
    char v[16];

    return uWebServerGetQueryParam(req, "o", v, sizeof(v)) == 0 ? (uint32_t)strtoul(v, NULL, 10) : 0;
}

typedef struct {
    size_t n;
    bool first;
} listCtx_t;

static void listVisit(const char *name, uint64_t size, void *user)
{
    listCtx_t *ctx = (listCtx_t *)user;

    if (ctx->n + 2 * strlen(name) + 48 >= API_BUF) {
        return;
    }
    ctx->n += (size_t)snprintf(gBuf + ctx->n, API_BUF - ctx->n, "%s{\"n\":\"", ctx->first ? "" : ",");
    jsonStr(gBuf, &ctx->n, name);
    ctx->n += (size_t)snprintf(gBuf + ctx->n, API_BUF - ctx->n, "\",\"s\":%llu}", (unsigned long long)size);
    ctx->first = false;
}

static void routeFiles(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    listCtx_t ctx = { 1, true };

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    gBuf[0] = '[';
    if (ucxapp_store_list(listVisit, &ctx) < 0) {
        uWebServerSendError(resp, 500, "cannot list files");
        return;
    }
    sendList(resp, ctx.n);
}

/* /api/file?n=name&o=offset: up to DL_CHUNK bytes; a shorter slice is the last one. */
static void routeFile(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char name[FILE_NAME_MAX];
    uint32_t skip;
    size_t got = 0;
    void *h;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (!fileName(req, name) || (h = ucxapp_store_open_read(name)) == NULL) {
        uWebServerSendError(resp, 404, "no such file");
        return;
    }
    if (isKey(name)) {
        ucxapp_store_close(h);
        uWebServerSendError(resp, 403, "private keys are not downloadable");
        return;
    }
    for (skip = queryOffset(req); skip > 0;) {
        int32_t r = ucxapp_store_read(h, gFileBuf, skip < DL_CHUNK ? skip : DL_CHUNK);
        if (r <= 0) {
            break;
        }
        skip -= (uint32_t)r;
    }
    while (got < DL_CHUNK) {
        int32_t r = ucxapp_store_read(h, gFileBuf + got, DL_CHUNK - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    ucxapp_store_close(h);
    resp->status_code = 200;
    snprintf(resp->content_type, sizeof(resp->content_type), "application/octet-stream");
    resp->body = (const char *)gFileBuf;
    resp->body_length = (int32_t)got;
}

static void uploadAbort(void)
{
    if (gUpload != NULL) {
        ucxapp_store_end(gUpload, false);
        gUpload = NULL;
    }
}

/* POST /api/upload?n=name&o=offset[&e=1]: slices in order; the file appears when the e=1 slice arrives. */
static void routeUpload(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char name[FILE_NAME_MAX], tmp[4];
    uint32_t off = queryOffset(req);

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (!fileName(req, name)) {
        uWebServerSendError(resp, 400, "bad file name");
        return;
    }
    if (!gHttps && req != &gLocalReq && (isKey(name) || strncmp(name, "tls-", 4) == 0)) {
        uWebServerSendError(resp, 403, "keys and certificates need https (web.start https)");
        return;
    }
    if (off == 0) {
        uploadAbort();
        gUpload = ucxapp_store_begin(name);
        snprintf(gUploadName, sizeof(gUploadName), "%s", name);
        gUploadOff = 0;
    }
    if (gUpload == NULL || off != gUploadOff || strcmp(name, gUploadName) != 0) {
        uWebServerSendError(resp, 409, "upload out of order");
        return;
    }
    if (req->body_length > 0 && !ucxapp_store_append(gUpload, req->body, (size_t)req->body_length)) {
        uploadAbort();
        uWebServerSendError(resp, 500, "write failed");
        return;
    }
    gUploadOff += req->body_length > 0 ? (uint32_t)req->body_length : 0;
    if (uWebServerGetQueryParam(req, "e", tmp, sizeof(tmp)) == 0) {
        bool ok = ucxapp_store_end(gUpload, true);
        gUpload = NULL;
        if (!ok) {
            uWebServerSendError(resp, 500, "save failed");
            return;
        }
        ucxapp_print("web: received %s (%lu bytes)\n", name, (unsigned long)gUploadOff);
    }
    uWebServerSendText(resp, "ok");
}

/* ---- commands --------------------------------------------------------------- */

static const struct {
    uHttpMethod_t method;
    const char *path;
    uRouteHandler_t fn;
} kRoutes[] = {
    { U_HTTP_METHOD_GET,  "/",             routePage },
    { U_HTTP_METHOD_GET,  "/icon.svg",     routeFavicon },
    { U_HTTP_METHOD_GET,  "/events",       routeEvents },
    { U_HTTP_METHOD_POST, "/api/exec",     routeExec },
    { U_HTTP_METHOD_GET,  "/api/groups",   routeList },
    { U_HTTP_METHOD_GET,  "/api/commands", routeList },
    { U_HTTP_METHOD_GET,  "/api/version",  routeVersion },
    { U_HTTP_METHOD_GET,  "/api/find",     routeFind },
    { U_HTTP_METHOD_GET,  "/api/complete", routeComplete },
    { U_HTTP_METHOD_GET,  "/api/examples", routeExamples },
    { U_HTTP_METHOD_GET,  "/api/conn",     routeConn },
    { U_HTTP_METHOD_GET,  "/api/manual",   routeManual },
    { U_HTTP_METHOD_GET,  "/api/files",    routeFiles },
    { U_HTTP_METHOD_GET,  "/api/file",     routeFile },
    { U_HTTP_METHOD_POST, "/api/upload",   routeUpload },
};

static bool makeToken(char *out)
{
    uint8_t rnd[TOKEN_LEN / 2];

    if (!ucxapp_tls_random(rnd, sizeof(rnd))) {
        return false;
    }
    for (size_t i = 0; i < sizeof(rnd); i++) {
        snprintf(out + i * 2, 3, "%02x", rnd[i]);
    }
    out[TOKEN_LEN] = '\0';
    return true;
}

static void webStop(void)
{
    gRunning = false;
    gStop = true;
    ucxapp_thread_join(gPumpThread);
    gPumpThread = NULL;
    uploadAbort();
    ucxapp_ui_release();
    uWebServerFree(gServer);
    gServer = NULL;
    U_CX_MUTEX_DELETE(gLock);
}

/* A killed earlier run leaves its listener and page connections on the module; close TCP sockets this run does not own. */
static int closeStaleSockets(ucxapp_t *app)
{
    int32_t mine[8], stale[8];
    int mineCount = ucxapp_handle_list(UCXAPP_HANDLE_SOCK, mine, 8);
    int count = 0;
    uCxSocketListStatus_t sock;

    uCxSocketListStatusBegin(app->ucx);
    while (count < 8 && uCxSocketListStatusGetNext(app->ucx, &sock)) {
        bool own = false;
        for (int i = 0; i < mineCount; i++) {
            own = own || mine[i] == sock.socket_handle;
        }
        if (!own && sock.protocol == U_SOCKET_PROTOCOL_TCP) {
            stale[count++] = sock.socket_handle;
        }
    }
    if (uCxEnd(app->ucx) != 0) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        ucxapp_print("closing socket %d left by an earlier run\n", (int)stale[i]);
        (void)uCxSocketClose(app->ucx, stale[i]);
    }
    return count;
}

static int cmdWebStart(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = -1;
    char ssid[64], ip[48], err[160] = "";
    const char *cert = NULL;
    bool plain = false, autoCert = true;
    ucxapp_tls_server_t *tls = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "http") == 0 || strcmp(argv[i], "https") == 0) {
            plain = argv[i][4] == '\0';
            if (argc - i > (plain ? 1 : 2)) {
                return UCXAPP_ERR_USAGE;
            }
            if (!plain && i + 1 < argc) {
                cert = argv[i + 1];
                autoCert = false;
            }
            argc = i;
            break;
        }
    }
    if (argc > 3 || (argc > 1 && (!ucxapp_parse_int(argv[1], &port) || port < 1 || port > 65535))) {
        return UCXAPP_ERR_USAGE;
    }
    if (port < 0) {
        port = plain ? DEFAULT_PORT : 443;
    }
#ifdef UCXAPP_WEB_HTTPS_ONLY
    if (plain) {   /* the boards serve the page, tokens and keys over TLS only */
        ucxapp_print("ERROR: this board serves the web UI over https only\n");
        return UCXAPP_ERR_FAILED;
    }
#define WEB_HTTP_HINT ""
#else
#define WEB_HTTP_HINT " (or web.start http)"
#endif
    if (gServer != NULL) {
        ucxapp_print("already running; web.stop first\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip)) || ip[0] == '\0') {
        ucxapp_print("ERROR: connect Wi-Fi first (wifi.connect)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!plain && autoCert) {
        bool created = false;
        if (!ucxapp_tls_web_cert(ip, &created, err, sizeof(err))) {
            ucxapp_print("ERROR: TLS certificate for %s: %s" WEB_HTTP_HINT "\n", ip, err);
            return UCXAPP_ERR_FAILED;
        }
        if (created) {
            ucxapp_print("created certificate tls-web for %s, signed by the local CA (tls-ca)\n", ip);
        }
        cert = UCXAPP_TLS_WEB_NAME;
        if (ucxapp_tls_ca_trust(false, NULL, 0) == UCXAPP_TRUST_NO) {
            if (!ucxapp_shell_interactive()) {
                ucxapp_print("note: the browser will warn until you run utils.tls.ca.trust\n");
            } else {
                ucxapp_print("trusting the local CA so the browser does not warn, confirm the Windows dialog...\n");
                if (ucxapp_tls_ca_trust(true, NULL, 0) != UCXAPP_TRUST_YES) {
                    ucxapp_print("note: not trusted, the browser will warn (utils.tls.ca.trust)\n");
                }
            }
        }
    }
    if (cert != NULL) {
        tls = ucxapp_tls_server_new(cert, false, err, sizeof(err));
        if (tls == NULL) {
            ucxapp_print("ERROR: TLS certificate '%s': %s (try utils.tls.ca.create, then utils.tls.server.create %s %s)\n",
                         cert, err, cert, ip);
            return UCXAPP_ERR_FAILED;
        }
    }
    gToken[0] = '\0';
    if (argc == 3 && strcmp(argv[2], "token") != 0) {
        snprintf(gToken, sizeof(gToken), "%s", argv[2]);
    } else if ((plain || argc == 3) && !makeToken(gToken)) {
        ucxapp_print("ERROR: no random source for the access token\n");
        ucxapp_tls_server_free(tls);
        return UCXAPP_ERR_FAILED;
    }

    gApp = app;
    gPort = (uint16_t)port;
    gStop = false;
    gEvHead = gEvCount = 0;
    U_CX_MUTEX_CREATE(gLock);

    if (!ucxapp_ui_acquire(app)) {
        ucxapp_print("ERROR: cannot start the command runner\n");
        ucxapp_tls_server_free(tls);
        U_CX_MUTEX_DELETE(gLock);
        return UCXAPP_ERR_FAILED;
    }
    memset(gViewers, 0, sizeof(gViewers[0]) * LOCAL_SLOT);   /* each page starts at the streams' end when it connects */

    (void)uCxSocketSetReadMode(app->ucx, U_READ_MODE_BUFFERED);
    gServer = uWebServerInit(app->ucx, gPort);
    if (gServer == NULL) {
        ucxapp_tls_server_free(tls);
    }
    uWebServerSetTls(gServer, tls);
    gHttps = tls != NULL;
    bool routed = gServer != NULL;
    for (size_t i = 0; routed && i < sizeof(kRoutes) / sizeof(kRoutes[0]); i++) {
        routed = uWebServerAddRoute(gServer, kRoutes[i].method, kRoutes[i].path, kRoutes[i].fn, NULL) == 0;
    }
    if (!routed) {
        ucxapp_print("ERROR: cannot create the web server\n");
        uWebServerFree(gServer);
        gServer = NULL;
        ucxapp_ui_release();
        U_CX_MUTEX_DELETE(gLock);
        return UCXAPP_ERR_FAILED;
    }
    int32_t result = uWebServerStart(gServer);
    if (result < 0 && closeStaleSockets(app) > 0) {
        result = uWebServerStart(gServer);
    }
    if (result < 0) {
        ucxapp_print("ERROR: listen on port %d failed (%d%s)\n", (int)port, (int)result, ucxapp_errname(result));
        uWebServerFree(gServer);
        gServer = NULL;
        ucxapp_ui_release();
        U_CX_MUTEX_DELETE(gLock);
        return UCXAPP_ERR_FAILED;
    }

    gRunning = true;
    gPumpThread = ucxapp_thread_start(pumpLoop, NULL);
    if (gPumpThread == NULL) {
        ucxapp_print("ERROR: cannot start the web thread\n");
        webStop();
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("web server on %s: %s://%s:%d/%s%s\n", ssid, gHttps ? "https" : "http", ip, (int)port,
                 gToken[0] != '\0' ? "#t=" : "", gToken);
    if (gHttps) {
        if (gToken[0] == '\0') {
            ucxapp_print("no access token: anyone who can reach this address can use the page (web.start %d token adds one)\n",
                         (int)port);
        }
        ucxapp_print("trust tls-ca.crt in the browser (utils.tls.pem ca) to avoid the certificate warning\n");
    } else {
        ucxapp_print("note: plain HTTP: anyone on this network can read the page and the link token; leave out http to serve HTTPS\n");
    }
    return UCXAPP_OK;
}

static int cmdWebStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    if (gServer == NULL) {
        return UCXAPP_OK;
    }
    if (ucxapp_ui_sole_on_worker()) {
        /* reached through a command the page ran (an example, a script): stopping would end the worker this runs on */
        ucxapp_print("ERROR: web.stop cannot run from the page it serves; run it in the app\n");
        return UCXAPP_ERR_FAILED;
    }
    webStop();
    ucxapp_print("web server stopped\n");
    return UCXAPP_OK;
}

bool ucxapp_web_running(void)
{
    return gServer != NULL && gRunning;
}

bool ucxapp_web_client_socket(int32_t sock)
{
    return gServer != NULL && sock != uWebServerGetListenSocket(gServer) && uWebServerOwnsSocket(gServer, sock);
}

static int cmdWebStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    if (gServer == NULL) {
        ucxapp_print("web server: stopped\n");
    } else {
        ucxapp_print("web server: %s port %u, %d event client(s)\n", gHttps ? "https" : "http", (unsigned)gPort,
                     (int)uWebServerGetSSEClientCount(gServer));
    }
    return UCXAPP_OK;
}

static bool writeJson(const char *dir, const char *name, const char *group, size_t len)
{
    char path[256];
    FILE *f;
    bool ok;

    snprintf(path, sizeof(path), "%s/%s%s%s.json", dir, name, group[0] != '\0' ? "-" : "", group);
    f = fopen(path, "wb");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot write %s\n", path);
        return false;
    }
    ok = fwrite(gBuf, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

/* web.export <dir>: the /api JSON of this build as files, for a preview or a static demo of the page. */
static int cmdWebExport(ucxapp_t *app, int argc, char *argv[])
{
    int count;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gServer != NULL) {
        ucxapp_print("ERROR: stop the web server first (web.stop)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!writeJson(argv[1], "groups", "", buildGroups())) {
        return UCXAPP_ERR_FAILED;
    }
    count = ucxapp_ui_groups(gGroups, UCXAPP_UI_MAX_GROUPS);
    for (int i = 0; i < count; i++) {
        if (strcmp(gGroups[i], "web") != 0 &&
            (!writeJson(argv[1], "commands", gGroups[i], buildCommands(gGroups[i])) ||
             !writeJson(argv[1], "examples", gGroups[i], buildExamples(gGroups[i])))) {
            return UCXAPP_ERR_FAILED;
        }
    }
    /* Use case steps (uc-*) are example groups without a command group of their own; they are listed together. */
    {
        const char *g, *t, *l, *prev = "";

        for (size_t i = 0; ucxapp_example_get(i, &g, &t, &l); i++) {
            if (strncmp(g, "uc-", 3) == 0 && strcmp(g, prev) != 0 &&
                !writeJson(argv[1], "examples", g, buildExamples(g))) {
                return UCXAPP_ERR_FAILED;
            }
            prev = g;
        }
    }
    ucxapp_print("exported %d groups to %s\n", count, argv[1]);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gWebCmds[] = {
    { "web.export", "<dir>",          "Write the page's command lists and examples as JSON files (preview, static demo)", 0, cmdWebExport },
    { "web.start",  "[port] [token] [http|https [cert]]", "Serve the web UI from the module (needs Wi-Fi) over HTTPS on port 443, with a certificate for the module's address made by the local CA on first use (tls-web, tls-ca); https <cert> uses tls-<cert>; http serves plain HTTP on port 80. HTTPS needs no token unless one is given (token makes a random one); plain HTTP always uses one", UCXAPP_CMD_NEEDS_DEVICE, cmdWebStart },
    { "web.stop",   "",               "Stop the web server",                                          0, cmdWebStop },
    { "web.status", "",               "Web server state",                                             0, cmdWebStatus },
};

void ucxapp_register_web_cmds(void)
{
    ucxapp_cmd_register(gWebCmds, sizeof(gWebCmds) / sizeof(gWebCmds[0]));
}

/* ---- browser build: the page in the same tab calls the routes and polls the events ---- */

static uHttpResponse_t gLocalResp;

bool ucxapp_web_local_begin(ucxapp_t *app)
{
    if (gLocal || !ucxapp_ui_acquire(app)) {
        return gLocal;
    }
    gApp = app;
    gLocal = true;
    memset(&gViewers[LOCAL_SLOT], 0, sizeof(gViewers[0]));
    (void)viewer(LOCAL_SLOT);   /* the page's place in the streams starts here, not at its first poll */
    return true;
}

/* One request; the reply is read with ucxapp_web_local_body/_length/_type. Runs on the page's thread. */
int ucxapp_web_local_request(const char *method, const char *path, const char *query, const char *body, int len)
{
    uHttpRequest_t *req = &gLocalReq;

    memset(req, 0, sizeof(*req));
    memset(&gLocalResp, 0, sizeof(gLocalResp));
    req->method = strcmp(method, "POST") == 0 ? U_HTTP_METHOD_POST : U_HTTP_METHOD_GET;
    snprintf(req->path, sizeof(req->path), "%s", path);
    snprintf(req->query, sizeof(req->query), "%s", query);
    req->body = body;
    req->body_length = len;
    for (size_t i = 0; gLocal && i < sizeof(kRoutes) / sizeof(kRoutes[0]); i++) {
        if (kRoutes[i].method == req->method && strcmp(kRoutes[i].path, path) == 0) {
            kRoutes[i].fn(req, &gLocalResp, NULL);
            return gLocalResp.status_code;
        }
    }
    uWebServerSendError(&gLocalResp, 404, "not found");
    return 404;
}

const char *ucxapp_web_local_body(void)
{
    return gLocalResp.body != NULL ? gLocalResp.body : "";
}

int ucxapp_web_local_length(void)
{
    return gLocalResp.body_length >= 0 ? gLocalResp.body_length : (int)strlen(ucxapp_web_local_body());
}

const char *ucxapp_web_local_type(void)
{
    return gLocalResp.content_type;
}

/* 1 while a command from /api/exec is queued or running; the page waits on it before the next dev.connect. */
int ucxapp_web_local_busy(void)
{
    return ucxapp_ui_busy() ? 1 : 0;
}

/* The events since the last call as "name<TAB>data" lines, the data as the event stream would carry it. */
const char *ucxapp_web_local_poll(void)
{
    gLocalLen = 0;
    gLocalEvents[0] = '\0';
    if (gLocal) {
        flushOutput(LOCAL_SLOT, LOCAL_SLOT);
        flushEvents(LOCAL_SLOT, LOCAL_SLOT);
        flushState(LOCAL_SLOT, LOCAL_SLOT);
        flushCards(LOCAL_SLOT, LOCAL_SLOT);
        flushDone(LOCAL_SLOT, LOCAL_SLOT);
    }
    return gLocalEvents;
}
