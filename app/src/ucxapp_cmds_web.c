/* web.start / web.stop / web.status: HTTP + SSE server hosted on the module (NORA-W36 TCP listener).
 * The HTTP engine is u_webserver.c from u-connectMatter; this file adds routes, the output stream and the
 * threads that move socket work out of the URC context. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ucxapp_cmd.h"
#include "ucxapp_store.h"
#include "ucxapp_thread.h"
#include "ucxapp_tls.h"
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
#define TOKEN_LEN      12
#define MAX_EVENTS     64
#define OUT_CHUNK_ESC  380      /* escaped bytes per SSE event; the engine caps an event at 512 */
#define KEEPALIVE_MS   15000
#define MAX_JOB        256
#define DL_CHUNK       16384    /* download slice; the page asks again until a shorter slice arrives */
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

static uint32_t gLogCur, gEvCur, gDoneSeen;   /* this front end's positions in the shared streams */

static ucxapp_store_writer_t *gUpload;      /* one upload at a time, in slices */
static char gUploadName[FILE_NAME_MAX];
static uint32_t gUploadOff;

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

/* Sends as much pending output as the engine takes; returns true if output is still waiting. */
static bool flushOutput(void)
{
    char raw[256], json[600];

    if (uWebServerGetSSEClientCount(gServer) <= 0) {
        return false;
    }
    for (int round = 0; round < 32; round++) {
        int avail = (int)ucxapp_ui_log_peek(&gLogCur, raw, sizeof(raw));
        if (avail == 0) {
            return false;
        }
        int used = escapeOutput(raw, avail, json, sizeof(json));
        if (uWebServerBroadcastSSEEvent(gServer, "out", json) <= 0) {
            return true;   /* previous event still draining; retry next pass */
        }
        gLogCur += (uint32_t)used;
    }
    return true;
}

static void flushEvents(void)
{
    char line[96], json[2 * sizeof(line) + 4];

    if (uWebServerGetSSEClientCount(gServer) <= 0) {
        return;
    }
    for (int round = 0; round < 4 && ucxapp_ui_event_peek(&gEvCur, line, sizeof(line)); round++) {
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
        if (uWebServerBroadcastSSEEvent(gServer, "evt", json) <= 0) {
            return;   /* previous event still draining; retry next pass */
        }
        gEvCur++;
    }
}

/* Sent on change, when a page connects and every 5 s. */
static void flushState(void)
{
    static uint32_t seen = UINT32_MAX;
    static int32_t lastMs, lastClients;
    char text[200], json[210];
    int32_t now = U_CX_PORT_GET_TIME_MS();
    uint32_t version = ucxapp_ui_state(text, sizeof(text));
    int32_t clients = uWebServerGetSSEClientCount(gServer);
    bool joined = clients > lastClients;

    lastClients = clients;
    if ((version == seen && now - lastMs < 5000 && !joined) || clients <= 0) {
        return;
    }
    snprintf(json, sizeof(json), "\"%s\"", text);
    if (uWebServerBroadcastSSEEvent(gServer, "state", json) > 0) {
        seen = version;
        lastMs = now;
    } else if (joined) {
        lastClients--;   /* try again next pass */
    }
}

static void flushDone(void)
{
    int rc;
    uint32_t seq = ucxapp_ui_done_seq(&rc);
    uint32_t cur = gLogCur;
    char probe[8];

    /* send "done" only after the command's output has been delivered */
    if (seq == gDoneSeen || ucxapp_ui_log_peek(&cur, probe, sizeof(probe)) > 0 ||
        uWebServerGetSSEClientCount(gServer) <= 0) {
        return;
    }
    char text[16];
    snprintf(text, sizeof(text), "%d", rc);
    if (uWebServerBroadcastSSEEvent(gServer, "done", text) > 0) {
        gDoneSeen = seq;
    }
}

static void pumpLoop(void *arg)
{
    int32_t lastKeepAlive = U_CX_PORT_GET_TIME_MS();

    (void)arg;
    while (!gStop) {
        webEvent_t e;
        while (!gStop && popEvent(&e)) {
            switch (e.type) {
            case EVT_INCOMING: {
                /* Accept either handle order in +UESOIC (see sockIncomingUrc in ucxapp_urc.c). */
                int32_t listen = uWebServerGetListenSocket(gServer);
                int32_t client = (e.sock == listen) ? e.n : e.sock;
                uWebServerHandleIncomingConnection(gServer, gApp->ucx, client, NULL, listen);
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
        bool pending = flushOutput();
        flushEvents();
        flushState();
        flushDone();
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

static void routePage(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    /* The page holds no data; the token travels in the URL fragment (never sent to the server)
     * and every API call checks it. */
    (void)req;
    (void)user;
    uWebServerSendHTML(resp, kPage);
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

#define API_BUF   16384
#define MAX_HITS  60

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

static void appendCmd(size_t *pn, bool *first, const ucxapp_cmd_t *c)
{
    ucxapp_ui_field_t fields[UCXAPP_UI_MAX_FIELDS];
    int nf = ucxapp_ui_parse_usage(c->usage, fields, UCXAPP_UI_MAX_FIELDS);
    size_t n = *pn;

    if (strncmp(c->name, "web.", 4) == 0 || n + 700 >= API_BUF) {
        return;
    }
    gBuf[n++] = *first ? ' ' : ',';
    *first = false;
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "{\"n\":\"");
    jsonStr(gBuf, &n, c->name);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"h\":\"");
    jsonStr(gBuf, &n, c->help);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",\"u\":\"");
    jsonStr(gBuf, &n, c->usage);
    n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",%s\"f\":", (c->flags & UCXAPP_CMD_DANGER) ? "\"d\":1," : "");
    if (nf < 0) {
        n += (size_t)snprintf(gBuf + n, API_BUF - n, "null");
    } else {
        gBuf[n++] = '[';
        for (int k = 0; k < nf; k++) {
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s[\"", k > 0 ? "," : "");
            jsonStr(gBuf, &n, fields[k].name);
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "\",%d,\"", fields[k].optional ? 1 : 0);
            jsonStr(gBuf, &n, fields[k].choices);
            n += (size_t)snprintf(gBuf + n, API_BUF - n, "\"]");
        }
        gBuf[n++] = ']';
    }
    gBuf[n++] = '}';
    *pn = n;
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

static void routeList(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    static char groups[UCXAPP_UI_MAX_GROUPS][UCXAPP_UI_GROUP_LEN];
    static const ucxapp_cmd_t *cmds[UCXAPP_UI_MAX_GROUP_CMDS];
    char group[16];
    size_t n = 0;
    bool first = true;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    gBuf[n++] = '[';
    if (strcmp(req->path, "/api/groups") == 0) {
        int count = ucxapp_ui_groups(groups, UCXAPP_UI_MAX_GROUPS);

        for (int pass = 0; pass < 2; pass++) {
            const char *where = pass == 0 ? "module" : "host";

            for (int i = 0; i < count; i++) {
                if (strcmp(groups[i], "web") != 0 && strcmp(ucxapp_ui_runs_on(groups[i]), where) == 0) {
                    n += (size_t)snprintf(gBuf + n, API_BUF - n, "%s\"%s:%s%s\"", first ? "" : ",", groups[i], where,
                                          hasExamples(groups[i]) ? ":x" : "");
                    first = false;
                }
            }
        }
        sendList(resp, n);
        /* the event stream cannot send X-Token, so it authenticates with this cookie */
        snprintf(resp->custom_headers, sizeof(resp->custom_headers),
                 "Set-Cookie: " COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict\r\n", gToken);
        return;
    }
    if (uWebServerGetQueryParam(req, "g", group, sizeof(group)) != 0) {
        uWebServerSendError(resp, 400, "missing g");
        return;
    }
    int count = ucxapp_ui_group_cmds(group, cmds, UCXAPP_UI_MAX_GROUP_CMDS);

    for (int i = 0; i < count; i++) {
        appendCmd(&n, &first, cmds[i]);
    }
    sendList(resp, n);
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

/* /api/examples?g=group: [{"t":title,"l":lines}] */
static void routeExamples(const uHttpRequest_t *req, uHttpResponse_t *resp, void *user)
{
    char group[16];
    const char *g, *t, *l;
    size_t n = 0;

    (void)user;
    if (!tokenOk(req)) {
        uWebServerSendError(resp, 403, "token required");
        return;
    }
    if (uWebServerGetQueryParam(req, "g", group, sizeof(group)) != 0) {
        uWebServerSendError(resp, 400, "missing g");
        return;
    }
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
    sendList(resp, n);
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
    if (!gHttps && (isKey(name) || strncmp(name, "tls-", 4) == 0)) {
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

static int cmdWebStart(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port = DEFAULT_PORT;
    char ssid[64], ip[48], err[160] = "";
    const char *cert = NULL;
    ucxapp_tls_server_t *tls = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "https") == 0) {
            if (argc - i > 2) {
                return UCXAPP_ERR_USAGE;
            }
            cert = i + 1 < argc ? argv[i + 1] : "server";
            port = 443;
            argc = i;
            break;
        }
    }
    if (argc > 3 || (argc > 1 && (!ucxapp_parse_int(argv[1], &port) || port < 1 || port > 65535))) {
        return UCXAPP_ERR_USAGE;
    }
    if (gServer != NULL) {
        ucxapp_print("already running; web.stop first\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip)) || ip[0] == '\0') {
        ucxapp_print("ERROR: connect Wi-Fi first (wifi.connect)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (cert != NULL) {
        tls = ucxapp_tls_server_new(cert, false, err, sizeof(err));
        if (tls == NULL) {
            ucxapp_print("ERROR: TLS certificate '%s': %s (try utils.tls.ca.create, then utils.tls.server.create %s %s)\n",
                         cert, err, cert, ip);
            return UCXAPP_ERR_FAILED;
        }
    }
    if (argc == 3) {
        snprintf(gToken, sizeof(gToken), "%s", argv[2]);
    } else if (!makeToken(gToken)) {
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
    gLogCur = ucxapp_ui_log_start();
    gEvCur = ucxapp_ui_event_start();
    gDoneSeen = ucxapp_ui_done_seq(&(int){ 0 });

    (void)uCxSocketSetReadMode(app->ucx, U_READ_MODE_BUFFERED);
    gServer = uWebServerInit(app->ucx, gPort);
    if (gServer == NULL) {
        ucxapp_tls_server_free(tls);
    }
    uWebServerSetTls(gServer, tls);
    gHttps = tls != NULL;
    if (gServer == NULL ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/", routePage, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/events", routeEvents, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_POST, "/api/exec", routeExec, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/groups", routeList, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/commands", routeList, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/find", routeFind, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/examples", routeExamples, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/files", routeFiles, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_GET, "/api/file", routeFile, NULL) != 0 ||
        uWebServerAddRoute(gServer, U_HTTP_METHOD_POST, "/api/upload", routeUpload, NULL) != 0) {
        ucxapp_print("ERROR: cannot create the web server\n");
        uWebServerFree(gServer);
        gServer = NULL;
        ucxapp_ui_release();
        U_CX_MUTEX_DELETE(gLock);
        return UCXAPP_ERR_FAILED;
    }
    int32_t result = uWebServerStart(gServer);
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
    ucxapp_print("web server on %s: %s://%s:%d/#t=%s\n", ssid, gHttps ? "https" : "http", ip, (int)port, gToken);
    if (gHttps) {
        ucxapp_print("trust tls-ca.crt in the browser (utils.tls.pem ca) to avoid the certificate warning\n");
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
    webStop();
    ucxapp_print("web server stopped\n");
    return UCXAPP_OK;
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

static const ucxapp_cmd_t gWebCmds[] = {
    { "web.start",  "[port] [token] [https [cert]]", "Serve a web UI from the module (needs Wi-Fi); port 80, or 443 with https", UCXAPP_CMD_NEEDS_DEVICE, cmdWebStart },
    { "web.stop",   "",               "Stop the web server",                                          0, cmdWebStop },
    { "web.status", "",               "Show web server state",                                        0, cmdWebStatus },
};

void ucxapp_register_web_cmds(void)
{
    ucxapp_cmd_register(gWebCmds, sizeof(gWebCmds) / sizeof(gWebCmds[0]));
}
