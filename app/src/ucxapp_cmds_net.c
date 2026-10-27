#include "ucxapp_cmd.h"
#include "ucxapp_stats.h"
#include "ucxapp_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_diagnostics.h"
#include "u_cx_socket.h"
#include "u_cx_http.h"
#include "u_cx_wifi.h"
#include "u_cx_network_time.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define HTTP_SESSION      0
#define HTTP_CHUNK        2048          /* AT+UHTCGBB, binary body read (3.5.0) */
#define HTTP_HDR_CHUNK    1000          /* AT+UHTCGH is a string read, max 1000 */
#define HTTP_MAX_BODY     (64 * 1024)   /* bytes read from the module before giving up */
#define HTTP_DEFAULT_SHOW 512
#define HTTP_MAX_URL      2048          /* u-connectXpress 3.5.0 and later */

static int cmdNetPing(ucxapp_t *app, int argc, char *argv[])
{
    int32_t count = 4;

    if (argc < 2 || argc > 3 ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &count) || count < 1 || count > 10))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_clear(URC_FLAG_PING_COMPLETE);
    int32_t result = uCxDiagnosticsPing2(app->ucx, argv[1], count);
    if (result != 0) {
        ucxapp_print("ERROR: failed to start ping (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_events_wait(URC_FLAG_PING_COMPLETE, (uint32_t)count * 2 + 5)) {
        ucxapp_print("ERROR: ping timeout\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s: sent %d, received %d, average %d ms\n", argv[1],
                 (int)gUcxappPingSent, (int)gUcxappPingReceived, (int)gUcxappPingAvgMs);
    return gUcxappPingReceived > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdNetDns(ucxapp_t *app, int argc, char *argv[])
{
    uSockIpAddress_t addr;
    char text[64];

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t result = uCxSocketGetHostByName(app->ucx, argv[1], &addr);
    if (result != 0) {
        ucxapp_print("ERROR: cannot resolve %s (%d%s)\n", argv[1], (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    uCxIpAddressToString(&addr, text, sizeof(text));
    ucxapp_print("%s = %s\n", argv[1], text);
    return UCXAPP_OK;
}

/* Splits "[http[s]://]host[/path]" in place. */
static bool splitUrl(char *url, bool *https, const char **host, const char **path)
{
    *https = false;
    if (strncmp(url, "https://", 8) == 0) {
        *https = true;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }
    char *slash = strchr(url, '/');
    if (slash != NULL) {
        memmove(slash + 1, slash, strlen(slash) + 1);   /* room for the host terminator */
        *slash = '\0';
        *path = slash + 1;
    } else {
        *path = "/";
    }
    *host = url;
    return url[0] != '\0';
}

static void printPrintable(const uint8_t *data, int32_t len)
{
    for (int32_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if ((data[i] < 0x20 && c != '\n' && c != '\r') || data[i] > 0x7e) {
            c = '.';
        }
        ucxapp_print("%c", c);
    }
}

static int32_t configureSession(ucxapp_t *app, bool https, const char *host, const char *path)
{
    static char hostUrl[HTTP_MAX_URL + 16];   /* static: keeps the STM32 stack small */
    int32_t err;
    const char *colon = strrchr(host, ':');

    if (colon != NULL && strchr(host, ']') == NULL && atoi(colon + 1) > 0) {
        snprintf(hostUrl, sizeof(hostUrl), "%s://%.*s", https ? "https" : "http", (int)(colon - host), host);
        err = https ? uCxHttpSetTLS2(app->ucx, HTTP_SESSION, U_WIFI_TLS_VERSION_TLS1_2) : 0;
        if (err >= 0) {
            err = uCxHttpSetConnectionParams3(app->ucx, HTTP_SESSION, hostUrl, atoi(colon + 1));
        }
    } else if (https) {
        snprintf(hostUrl, sizeof(hostUrl), "https://%s", host);
        err = uCxHttpSetTLS2(app->ucx, HTTP_SESSION, U_WIFI_TLS_VERSION_TLS1_2);
        if (err >= 0) {
            err = uCxHttpSetConnectionParams3(app->ucx, HTTP_SESSION, hostUrl, 443);
        }
    } else {
        snprintf(hostUrl, sizeof(hostUrl), "http://%s", host);
        err = uCxHttpSetConnectionParams2(app->ucx, HTTP_SESSION, hostUrl);
    }
    if (err >= 0) {
        err = uCxHttpSetRequestPath(app->ucx, HTTP_SESSION, path);
    }
    return err;
}

static int cmdHttpGet(ucxapp_t *app, int argc, char *argv[])
{
    bool https;
    const char *host;
    const char *path;
    static char url[HTTP_MAX_URL + 2];
    int32_t show = HTTP_DEFAULT_SHOW;
    int32_t err;
    int rc = UCXAPP_ERR_FAILED;

    if (argc < 2 || argc > 3 || strlen(argv[1]) > HTTP_MAX_URL ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &show) || show < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(url, sizeof(url), "%s", argv[1]);
    if (!splitUrl(url, &https, &host, &path)) {
        return UCXAPP_ERR_USAGE;
    }

    err = configureSession(app, https, host, path);
    if (err < 0) {
        ucxapp_print("ERROR: cannot configure HTTP session (%d%s)\n", (int)err, ucxapp_errname(err));
        return UCXAPP_ERR_FAILED;
    }

    ucxapp_events_clear(URC_FLAG_HTTP_RESPONSE_READY);
    err = uCxHttpGetRequest(app->ucx, HTTP_SESSION);
    if (err < 0) {
        ucxapp_print("ERROR: GET request failed (%d%s)\n", (int)err, ucxapp_errname(err));
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_events_wait(URC_FLAG_HTTP_RESPONSE_READY, 15)) {
        ucxapp_print("ERROR: timeout waiting for the HTTP response\n");
        goto done;
    }

    /* The first header chunk starts with the status line. */
    uCxHttpGetHeader_t header;
    bool first = true;
    bool more = true;
    while (more) {
        memset(&header, 0, sizeof(header));
        if (!uCxHttpGetHeader2Begin(app->ucx, HTTP_SESSION, HTTP_HDR_CHUNK, &header)) {
            ucxapp_print("ERROR: cannot read HTTP headers\n");
            uCxEnd(app->ucx);
            goto done;
        }
        more = header.more_to_read != 0;
        if (first && header.byte_array_data.pData != NULL) {
            const uint8_t *d = header.byte_array_data.pData;
            size_t n = 0;
            while (n < header.byte_array_data.length && d[n] != '\r' && d[n] != '\n') {
                n++;
            }
            ucxapp_print("%.*s\n", (int)n, (const char *)d);
        }
        first = false;
        uCxEnd(app->ucx);
    }

    uint8_t buf[HTTP_CHUNK];
    int32_t total = 0;
    int32_t shown = 0;
    int32_t moreToRead = 1;
    while (moreToRead && total < HTTP_MAX_BODY) {
        int32_t n = uCxHttpGetBody(app->ucx, HTTP_SESSION, HTTP_CHUNK, buf, &moreToRead);
        if (n < 0) {
            ucxapp_print("\nERROR: reading body failed (%d%s)\n", (int)n, ucxapp_errname(n));
            goto done;
        }
        total += n;
        ucxapp_stats_add(UCXAPP_STATS_HTTP, 0, (size_t)n);
        if (shown < show) {
            int32_t part = (show - shown < n) ? show - shown : n;
            printPrintable(buf, part);
            shown += part;
        }
    }
    ucxapp_print("\n[%d bytes%s]\n", (int)total, total > shown ? ", output truncated" : "");
    rc = UCXAPP_OK;

done:
    uCxHttpDisconnect(app->ucx, HTTP_SESSION);   /* may fail if the server already closed */
    return rc;
}

/* Copies the value of a header line (name includes the colon) into out. */
static bool headerValue(const char *headers, const char *name, char *out, size_t outSize)
{
    size_t nameLen = strlen(name);
    for (const char *p = headers; *p;) {
        size_t i = 0;
        while (i < nameLen && p[i] != '\0' && tolower((unsigned char)p[i]) == tolower((unsigned char)name[i])) {
            i++;
        }
        if (i == nameLen) {
            p += nameLen;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            size_t n = 0;
            while (p[n] != '\0' && p[n] != '\r' && p[n] != '\n') {
                n++;
            }
            if (n == 0 || n >= outSize) {
                return false;
            }
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p != '\0') {
            p++;
        }
    }
    return false;
}

/* One GET hop. Returns 0 on a 2xx response with the body appended to w, 1 for a redirect
   (location filled in), -1 on failure. */
static int downloadHop(ucxapp_t *app, char *url, ucxapp_store_writer_t *w, char *location, size_t locationSize,
                       char *scheme, char *hostOut, size_t hostOutSize)
{
    static char headers[4096];
    bool https;
    const char *host;
    const char *path;
    int rc = -1;

    if (!splitUrl(url, &https, &host, &path)) {
        ucxapp_print("ERROR: invalid URL\n");
        return -1;
    }
    snprintf(scheme, 8, "%s", https ? "https" : "http");
    snprintf(hostOut, hostOutSize, "%s", host);

    int32_t err = configureSession(app, https, host, path);
    if (err >= 0) {
        err = uCxHttpAddHeaderField(app->ucx, HTTP_SESSION, "User-Agent", "u-connectXpressApp/1.0");
    }
    if (err < 0) {
        ucxapp_print("ERROR: cannot configure HTTP session (%d%s)\n", (int)err, ucxapp_errname(err));
        return -1;
    }

    ucxapp_events_clear(URC_FLAG_HTTP_RESPONSE_READY);
    err = uCxHttpGetRequest(app->ucx, HTTP_SESSION);
    if (err < 0) {
        ucxapp_print("ERROR: GET request failed (%d%s)\n", (int)err, ucxapp_errname(err));
        return -1;
    }
    if (!ucxapp_events_wait(URC_FLAG_HTTP_RESPONSE_READY, 20)) {
        ucxapp_print("ERROR: timeout waiting for the HTTP response\n");
        goto done;
    }

    size_t hlen = 0;
    bool more = true;
    while (more) {
        uCxHttpGetHeader_t header;
        memset(&header, 0, sizeof(header));
        if (!uCxHttpGetHeader2Begin(app->ucx, HTTP_SESSION, HTTP_HDR_CHUNK, &header)) {
            ucxapp_print("ERROR: cannot read HTTP headers\n");
            uCxEnd(app->ucx);
            goto done;
        }
        more = header.more_to_read != 0;
        if (header.byte_array_data.pData != NULL &&
            hlen + header.byte_array_data.length < sizeof(headers)) {
            memcpy(headers + hlen, header.byte_array_data.pData, header.byte_array_data.length);
            hlen += header.byte_array_data.length;
        }
        uCxEnd(app->ucx);
    }
    headers[hlen] = '\0';

    int status = 0;
    if (strncmp(headers, "HTTP/", 5) == 0) {
        const char *sp = strchr(headers, ' ');
        status = sp != NULL ? atoi(sp + 1) : 0;
    }

    if (headerValue(headers, "Location:", location, locationSize) &&
        (status == 0 || (status >= 300 && status < 400))) {
        ucxapp_print("redirect (HTTP %d)\n", status);
        rc = 1;
        goto done;
    }
    if (status != 0 && (status < 200 || status >= 300)) {
        ucxapp_print("ERROR: HTTP status %d\n", status);
        goto done;
    }

    char lenText[24];
    int32_t expected = -1;
    if (headerValue(headers, "Content-Length:", lenText, sizeof(lenText))) {
        expected = atoi(lenText);
    }

    uint8_t buf[HTTP_CHUNK];
    int32_t total = 0;
    int32_t moreToRead = 1;
    while (moreToRead) {
        int32_t n = uCxHttpGetBody(app->ucx, HTTP_SESSION, HTTP_CHUNK, buf, &moreToRead);
        if (n < 0) {
            ucxapp_print("\nERROR: reading body failed (%d%s)\n", (int)n, ucxapp_errname(n));
            goto done;
        }
        if (!ucxapp_store_append(w, buf, (size_t)n)) {
            ucxapp_print("\nERROR: cannot write file\n");
            goto done;
        }
        total += n;
        ucxapp_stats_add(UCXAPP_STATS_HTTP, 0, (size_t)n);
        if (expected > 0) {
            ucxapp_print("\r%d/%d bytes (%d%%)", (int)total, (int)expected, (int)((int64_t)total * 100 / expected));
        }
    }
    ucxapp_print("\n");
    if (expected >= 0 && total != expected) {
        ucxapp_print("ERROR: incomplete download (%d of %d bytes)\n", (int)total, (int)expected);
        goto done;
    }
    ucxapp_print("%d bytes saved\n", (int)total);
    rc = 0;

done:
    uCxHttpDisconnect(app->ucx, HTTP_SESSION);
    return rc;
}

static int cmdHttpDownload(ucxapp_t *app, int argc, char *argv[])
{
    enum { URL_SIZE = 2048, MAX_REDIRECTS = 5 };
    static char url[URL_SIZE];
    static char location[URL_SIZE];
    char scheme[8];
    char host[256];

    if (argc != 3 || strlen(argv[1]) >= URL_SIZE / 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_store_writer_t *w = ucxapp_store_begin(argv[2]);
    if (w == NULL) {
        ucxapp_print("ERROR: cannot create %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    snprintf(url, sizeof(url), "%s", argv[1]);

    int rc = UCXAPP_ERR_FAILED;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int r = downloadHop(app, url, w, location, sizeof(location), scheme, host, sizeof(host));
        if (r == 0) {
            rc = UCXAPP_OK;
            break;
        }
        if (r < 0) {
            break;
        }
        int n = location[0] == '/' ? snprintf(url, URL_SIZE, "%s://%s%s", scheme, host, location)
                                   : snprintf(url, URL_SIZE, "%s", location);
        if (n < 0 || n >= URL_SIZE) {
            ucxapp_print("ERROR: redirect URL too long\n");
            break;
        }
        if (hop == MAX_REDIRECTS) {
            ucxapp_print("ERROR: too many redirects\n");
        }
    }
    if (!ucxapp_store_end(w, rc == UCXAPP_OK)) {
        if (rc == UCXAPP_OK) {
            ucxapp_print("ERROR: cannot save %s\n", argv[2]);
        }
        rc = UCXAPP_ERR_FAILED;
    }
    return rc;
}

/* ---- generic request for http.post and wifi.portal ---- */

#define PORTAL_PROBE "http://connectivitycheck.gstatic.com/generate_204"
#define PORTAL_MAX_FIELDS 16
#if defined(_WIN32) || defined(__linux__)
#define REQ_BODY_SIZE (32 * 1024)
#else
#define REQ_BODY_SIZE (8 * 1024)
#endif

static char gReqHdr[2048];
static char gReqBody[REQ_BODY_SIZE];
static char gPortalUrl[HTTP_MAX_URL + 2];
static char gCookie[256];

/* GET when body is NULL, else POST. Fills gReqHdr and gReqBody (truncated, NUL-terminated).
   Returns the HTTP status, 0 if the module gave no status line, -1 on failure. */
static int httpRequest(ucxapp_t *app, const char *urlIn, const char *body, const char *ctype, const char *cookie)
{
    static char url[HTTP_MAX_URL + 2];
    bool https;
    const char *host;
    const char *path;
    int status = -1;
    int32_t err;

    gReqHdr[0] = '\0';
    gReqBody[0] = '\0';
    if (strlen(urlIn) > HTTP_MAX_URL) {
        return -1;
    }
    snprintf(url, sizeof(url), "%s", urlIn);
    if (!splitUrl(url, &https, &host, &path)) {
        return -1;
    }
    err = configureSession(app, https, host, path);
    (void)uCxHttpClearHeaderCustom(app->ucx, HTTP_SESSION);
    if (err >= 0) {
        err = uCxHttpAddHeaderField(app->ucx, HTTP_SESSION, "User-Agent", "u-connectXpressApp/1.0");
    }
    if (err >= 0 && ctype != NULL) {
        err = uCxHttpAddHeaderField(app->ucx, HTTP_SESSION, "Content-Type", ctype);
    }
    if (err >= 0 && cookie != NULL && cookie[0] != '\0') {
        err = uCxHttpAddHeaderField(app->ucx, HTTP_SESSION, "Cookie", cookie);
    }
    if (err < 0) {
        ucxapp_print("ERROR: cannot configure HTTP session (%d%s)\n", (int)err, ucxapp_errname(err));
        return -1;
    }

    ucxapp_events_clear(URC_FLAG_HTTP_RESPONSE_READY);
    err = body != NULL ? uCxHttpPostRequest(app->ucx, HTTP_SESSION, (const uint8_t *)body, (int32_t)strlen(body))
                       : uCxHttpGetRequest(app->ucx, HTTP_SESSION);
    if (err < 0) {
        ucxapp_print("ERROR: %s request failed (%d%s)\n", body != NULL ? "POST" : "GET", (int)err, ucxapp_errname(err));
        goto done;
    }
    if (!ucxapp_events_wait(URC_FLAG_HTTP_RESPONSE_READY, 15)) {
        ucxapp_print("ERROR: timeout waiting for the HTTP response\n");
        goto done;
    }

    size_t hlen = 0;
    bool more = true;
    while (more) {
        uCxHttpGetHeader_t header;
        memset(&header, 0, sizeof(header));
        if (!uCxHttpGetHeader2Begin(app->ucx, HTTP_SESSION, HTTP_HDR_CHUNK, &header)) {
            ucxapp_print("ERROR: cannot read HTTP headers\n");
            uCxEnd(app->ucx);
            goto done;
        }
        more = header.more_to_read != 0;
        if (header.byte_array_data.pData != NULL && hlen + header.byte_array_data.length < sizeof(gReqHdr)) {
            memcpy(gReqHdr + hlen, header.byte_array_data.pData, header.byte_array_data.length);
            hlen += header.byte_array_data.length;
        }
        uCxEnd(app->ucx);
    }
    gReqHdr[hlen] = '\0';

    uint8_t buf[HTTP_CHUNK];
    size_t blen = 0;
    int32_t moreToRead = 1;
    while (moreToRead) {
        int32_t n = uCxHttpGetBody(app->ucx, HTTP_SESSION, HTTP_CHUNK, buf, &moreToRead);
        if (n < 0) {
            break;   /* no body (204, 302) ends here as well */
        }
        ucxapp_stats_add(UCXAPP_STATS_HTTP, 0, (size_t)n);
        size_t take = blen + (size_t)n < sizeof(gReqBody) - 1 ? (size_t)n : sizeof(gReqBody) - 1 - blen;
        memcpy(gReqBody + blen, buf, take);
        blen += take;
    }
    gReqBody[blen] = '\0';
    status = 0;
    if (strncmp(gReqHdr, "HTTP/", 5) == 0 && strchr(gReqHdr, ' ') != NULL) {
        status = atoi(strchr(gReqHdr, ' ') + 1);
    }

done:
    uCxHttpDisconnect(app->ucx, HTTP_SESSION);
    return status;
}

static int cmdHttpPost(ucxapp_t *app, int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    int status = httpRequest(app, argv[1], argv[2], argc > 3 ? argv[3] : "application/x-www-form-urlencoded", NULL);
    if (status < 0) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%.*s\n", (int)strcspn(gReqHdr, "\r\n"), gReqHdr);
    size_t len = strlen(gReqBody);
    printPrintable((const uint8_t *)gReqBody, (int32_t)(len < HTTP_DEFAULT_SHOW ? len : HTTP_DEFAULT_SHOW));
    ucxapp_print("\n[%d bytes%s]\n", (int)len, len > HTTP_DEFAULT_SHOW ? ", output truncated" : "");
    return status >= 200 && status < 400 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* ---- wifi.portal: hotel / guest Wi-Fi login pages (captive portals) ---- */

static const char *findCi(const char *h, const char *lowerNeedle)
{
    for (; *h != '\0'; h++) {
        size_t i = 0;
        while (lowerNeedle[i] != '\0' && tolower((unsigned char)h[i]) == lowerNeedle[i]) {
            i++;
        }
        if (lowerNeedle[i] == '\0') {
            return h;
        }
    }
    return NULL;
}

/* Reads attribute name (lower case) from the tag in [tag, end); decodes &amp;. */
static bool tagAttr(const char *tag, const char *end, const char *name, char *out, size_t size)
{
    size_t nl = strlen(name);

    for (const char *p = tag + 1; p + nl < end; p++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)p[i]) == name[i]) {
            i++;
        }
        if (i < nl || p[nl] != '=' || !isspace((unsigned char)p[-1])) {
            continue;
        }
        const char *v = p + nl + 1;
        char q = (*v == '"' || *v == '\'') ? *v++ : '\0';
        size_t n = 0;
        while (v < end && n + 1 < size && (q != '\0' ? *v != q : (!isspace((unsigned char)*v) && *v != '>'))) {
            if (strncmp(v, "&amp;", 5) == 0) {
                out[n++] = '&';
                v += 5;
                continue;
            }
            out[n++] = *v++;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

static void resolveUrl(const char *base, const char *ref, char *out, size_t size)
{
    const char *s = strstr(base, "://");
    const char *hostEnd = s != NULL ? strchr(s + 3, '/') : NULL;
    size_t hostLen = hostEnd != NULL ? (size_t)(hostEnd - base) : strlen(base);
    size_t baseLen = strcspn(base, "?#");

    if (ref[0] == '\0') {
        snprintf(out, size, "%.*s", (int)baseLen, base);
    } else if (strstr(ref, "://") != NULL) {
        snprintf(out, size, "%s", ref);
    } else if (ref[0] == '/') {
        snprintf(out, size, "%.*s%s", (int)hostLen, base, ref);
    } else {
        size_t dir = hostLen;
        for (size_t i = hostLen; i < baseLen; i++) {
            if (base[i] == '/') {
                dir = i + 1;
            }
        }
        snprintf(out, size, dir > hostLen ? "%.*s%s" : "%.*s/%s", (int)dir, base, ref);
    }
}

static void urlEncodeAppend(char *out, size_t size, const char *name, const char *value)
{
    size_t n = strlen(out);
    const char *parts[2] = { name, value };

    for (int k = 0; k < 2; k++) {
        if (n + 2 < size && (k == 1 || n > 0)) {
            out[n++] = k == 1 ? '=' : '&';
        }
        for (const unsigned char *c = (const unsigned char *)parts[k]; *c != '\0' && n + 4 < size; c++) {
            if (isalnum(*c) || strchr("-_.~", *c) != NULL) {
                out[n++] = (char)*c;
            } else if (*c == ' ') {
                out[n++] = '+';
            } else {
                n += (size_t)snprintf(out + n, size - n, "%%%02X", *c);
            }
        }
    }
    out[n] = '\0';
}

static void grabCookie(void)
{
    char v[sizeof(gCookie)];

    if (headerValue(gReqHdr, "Set-Cookie:", v, sizeof(v))) {
        v[strcspn(v, ";")] = '\0';
        snprintf(gCookie, sizeof(gCookie), "%s", v);
    }
}

/* 0 online, 1 login page in the way (gPortalUrl set), -1 no internet. */
static int portalCheck(ucxapp_t *app, const char *probe)
{
    static char loc[HTTP_MAX_URL + 2];
    int s = httpRequest(app, probe, NULL, NULL, NULL);

    if (s == 204) {
        ucxapp_print("online: %s answered 204, no login page\n", probe);
        return 0;
    }
    if (s >= 300 && s < 400 && headerValue(gReqHdr, "Location:", loc, sizeof(loc))) {
        resolveUrl(probe, loc, gPortalUrl, sizeof(gPortalUrl));
        ucxapp_print("login page: %s (redirected, HTTP %d)\n", gPortalUrl, s);
        return 1;
    }
    if (s == 200) {
        snprintf(gPortalUrl, sizeof(gPortalUrl), "%s", probe);
        ucxapp_print("login page served in place of %s\n", probe);
        return 1;
    }
    if (s < 0) {
        ucxapp_print("no answer from %s: no internet, or DNS is blocked\n", probe);
    } else {
        ucxapp_print("unexpected HTTP %d from %s\n", s, probe);
    }
    return -1;
}

/* Fills in the first <form> of html (page is its URL): hidden fields kept, checkboxes ticked,
   name=value overrides applied or appended. Returns false when there is no form. */
static bool buildForm(const char *html, const char *page, int nOv, char *ov[], bool *post,
                      char *action, size_t actionSize, char *form, size_t formSize, char *names, size_t namesSize)
{
    char method[8] = "get", act[512] = "", name[64], type[16], value[128];
    bool used[PORTAL_MAX_FIELDS] = { false };
    const char *f = findCi(html, "<form");
    const char *fe = f != NULL ? strchr(f, '>') : NULL;

    if (fe == NULL) {
        return false;
    }
    (void)tagAttr(f, fe, "method", method, sizeof(method));
    (void)tagAttr(f, fe, "action", act, sizeof(act));
    resolveUrl(page, act, action, actionSize);
    *post = tolower((unsigned char)method[0]) == 'p';
    const char *formEnd = findCi(fe, "</form");
    if (formEnd == NULL) {
        formEnd = fe + strlen(fe);
    }

    form[0] = names[0] = '\0';
    for (const char *p = fe; (p = findCi(p, "<input")) != NULL && p < formEnd; p++) {
        const char *te = strchr(p, '>');
        if (te == NULL) {
            break;
        }
        if (!tagAttr(p, te, "name", name, sizeof(name)) || name[0] == '\0') {
            continue;
        }
        if (!tagAttr(p, te, "type", type, sizeof(type))) {
            snprintf(type, sizeof(type), "text");
        }
        for (char *t = type; *t != '\0'; t++) {
            *t = (char)tolower((unsigned char)*t);
        }
        if (!tagAttr(p, te, "value", value, sizeof(value))) {
            snprintf(value, sizeof(value), "%s", strcmp(type, "checkbox") == 0 ? "on" : "");
        }
        const char *checked = findCi(p, " checked");
        if (strcmp(type, "reset") == 0 || strcmp(type, "button") == 0 ||
            (strcmp(type, "radio") == 0 && (checked == NULL || checked > te))) {
            continue;
        }
        for (int k = 0; k < nOv; k++) {
            size_t kl = strcspn(ov[k], "=");
            if (strlen(name) == kl && strncmp(ov[k], name, kl) == 0) {
                snprintf(value, sizeof(value), "%s", ov[k] + kl + 1);
                used[k] = true;
            }
        }
        urlEncodeAppend(form, formSize, name, value);
        snprintf(names + strlen(names), namesSize - strlen(names), " %s", name);
    }
    for (int k = 0; k < nOv; k++) {
        if (!used[k]) {
            size_t kl = strcspn(ov[k], "=");
            snprintf(name, sizeof(name), "%.*s", (int)kl, ov[k]);
            urlEncodeAppend(form, formSize, name, ov[k] + kl + 1);
            snprintf(names + strlen(names), namesSize - strlen(names), " %s", name);
        }
    }
    return true;
}

static int portalLogin(ucxapp_t *app, const char *probe, int nOv, char *ov[])
{
    static char page[HTTP_MAX_URL + 2], next[HTTP_MAX_URL + 2], loc[HTTP_MAX_URL + 2];
    static char form[1024], names[256];
    bool post;
    int s = -1;

    int r = portalCheck(app, probe);
    if (r <= 0) {
        return r == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    gCookie[0] = '\0';
    snprintf(page, sizeof(page), "%s", gPortalUrl);
    for (int hop = 0; hop < 5; hop++) {
        s = httpRequest(app, page, NULL, NULL, gCookie);
        grabCookie();
        if (s < 300 || s >= 400 || !headerValue(gReqHdr, "Location:", loc, sizeof(loc))) {
            break;
        }
        resolveUrl(page, loc, next, sizeof(next));
        snprintf(page, sizeof(page), "%s", next);
    }
    if (s != 200) {
        ucxapp_print("ERROR: cannot load the login page %s (HTTP %d)\n", page, s);
        return UCXAPP_ERR_FAILED;
    }
    if (!buildForm(gReqBody, page, nOv, ov, &post, next, sizeof(next), form, sizeof(form), names, sizeof(names))) {
        ucxapp_print("ERROR: no <form> on the login page; it probably needs a browser (JavaScript)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("submitting the form (%s %s), fields:%s\n", post ? "POST" : "GET", next, names);
    if (post) {
        s = httpRequest(app, next, form, "application/x-www-form-urlencoded", gCookie);
    } else {
        size_t urlLen = strlen(next) + 1 + strlen(form);
        if (urlLen >= sizeof(page)) {
            ucxapp_print("ERROR: the login form URL is too long (%u bytes)\n", (unsigned)urlLen);
            return UCXAPP_ERR_FAILED;
        }
        memcpy(page, next, strlen(next));
        page[strlen(next)] = strchr(next, '?') != NULL ? '&' : '?';
        memcpy(&page[strlen(next) + 1], form, strlen(form) + 1);
        s = httpRequest(app, page, NULL, NULL, gCookie);
    }
    if (s < 0) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("login page answered HTTP %d\n", s);
    if (portalCheck(app, probe) != 0) {
        ucxapp_print("still not online: add the missing fields as name=value (see the field list above)\n");
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static int cmdWifiPortal(ucxapp_t *app, int argc, char *argv[])
{
    const char *probe = PORTAL_PROBE;
    int i = 2;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 2 && strncmp(argv[2], "http", 4) == 0) {
        probe = argv[i++];
    }
    if (strcmp(argv[1], "check") == 0 && i == argc) {
        return portalCheck(app, probe) == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (strcmp(argv[1], "login") != 0 || argc - i > PORTAL_MAX_FIELDS) {
        return UCXAPP_ERR_USAGE;
    }
    for (int k = i; k < argc; k++) {
        if (strchr(argv[k], '=') == NULL) {
            return UCXAPP_ERR_USAGE;
        }
    }
    return portalLogin(app, probe, argc - i, argv + i);
}

/* Offline: shows what wifi.portal login would submit for a saved login page. */
static int cmdWifiPortalForm(ucxapp_t *app, int argc, char *argv[])
{
    static char action[HTTP_MAX_URL + 2], form[1024], names[256];
    size_t len;
    bool post;

    (void)app;
    if (argc < 3 || argc - 3 > PORTAL_MAX_FIELDS) {
        return UCXAPP_ERR_USAGE;
    }
    for (int k = 3; k < argc; k++) {
        if (strchr(argv[k], '=') == NULL) {
            return UCXAPP_ERR_USAGE;
        }
    }
    uint8_t *html = ucxapp_store_load(argv[1], &len);
    if (html == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    bool ok = buildForm((const char *)html, argv[2], argc - 3, argv + 3, &post, action, sizeof(action),
                        form, sizeof(form), names, sizeof(names));
    free(html);
    if (!ok) {
        ucxapp_print("no <form> in %s; the page probably needs a browser (JavaScript)\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s %s\nfields:%s\nbody: %s\n", post ? "POST" : "GET", action, names, form);
    return UCXAPP_OK;
}

static void iperfOutput(struct uCxHandle *h, const char *text)
{
    (void)h;
    ucxapp_print("iperf: %s\n", text != NULL ? text : "");
}

static int cmdNetIperf(ucxapp_t *app, int argc, char *argv[])
{
    uDiagProtocolType_t proto = U_DIAG_PROTOCOL_TYPE_TCP;
    uSockIpAddress_t ip;
    int32_t seconds = 10, port = 5001, r;
    bool client = argc > 1 && strcmp(argv[1], "client") == 0;
    int next = client ? 3 : 2;

    if (argc >= 2 && strcmp(argv[1], "stop") == 0 && argc == 2) {
        r = uCxDiagnosticsIperf2(app->ucx, U_DIAG_IPERF_ACTION_STOP, proto);
        ucxapp_print(r == 0 ? "iperf stopped\n" : "ERROR: nothing to stop\n");
        return r == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (argc < 2 || (!client && strcmp(argv[1], "server") != 0) || (client && argc < 3)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > next && (strcmp(argv[next], "tcp") == 0 || strcmp(argv[next], "udp") == 0)) {
        proto = argv[next][0] == 't' ? U_DIAG_PROTOCOL_TYPE_TCP : U_DIAG_PROTOCOL_TYPE_UDP;
        next++;
    }
    if (client && argc > next) {
        if (!ucxapp_parse_int(argv[next++], &seconds) || seconds < 1 || seconds > 300) {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (argc > next && (!ucxapp_parse_int(argv[next], &port) || port < 1 || port > 65535)) {
        return UCXAPP_ERR_USAGE;
    }
    if (client && uCxStringToIpAddress(argv[2], &ip) != 0) {
        ucxapp_print("ERROR: give the server as an IP address (net.dns resolves a name)\n");
        return UCXAPP_ERR_FAILED;
    }
    uCxDiagnosticsRegisterIperfOutput(app->ucx, iperfOutput);
    if (client) {
        r = uCxDiagnosticsIperf7(app->ucx, U_DIAG_IPERF_ACTION_START, proto, U_DIAG_ROLE_CLIENT, port, 1, seconds, &ip);
    } else {
        memset(&ip, 0, sizeof(ip));
        r = uCxDiagnosticsIperf5(app->ucx, U_DIAG_IPERF_ACTION_START, proto, U_DIAG_ROLE_SERVER, port, 1);
    }
    if (r != 0) {
        ucxapp_print("ERROR: iperf failed to start (%d%s); the Wi-Fi station must be connected\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    if (!client) {
        ucxapp_print("iperf server on port %d; results appear here, stop with net.iperf stop\n", (int)port);
        return UCXAPP_OK;
    }
    U_CX_PORT_SLEEP_MS((seconds + 3) * 1000);
    uCxDiagnosticsIperf2(app->ucx, U_DIAG_IPERF_ACTION_STOP, proto);
    return UCXAPP_OK;
}

static int cmdNetNtp(ucxapp_t *app, int argc, char *argv[])
{
    const char *sub = argc >= 2 ? argv[1] : "status";

    if (strcmp(sub, "status") == 0 && argc <= 2) {
        uNtpClientStatus_t status = U_NTP_CLIENT_STATUS_DISABLE;
        int32_t err = uCxNetworkTimeGetClientEnabled(app->ucx, &status);
        if (err < 0) {
            ucxapp_print("ERROR: cannot read NTP client status (%d%s)\n", (int)err, ucxapp_errname(err));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("NTP client: %s\n", status == U_NTP_CLIENT_STATUS_ENABLE_AUTO ? "enabled, DHCP servers" :
                     status == U_NTP_CLIENT_STATUS_ENABLE_MANUAL ? "enabled, configured servers" : "disabled");
        uCxNtpGetNtpServer_t srv;
        if (uCxNetworkTimeGetNtpServerBegin(app->ucx, &srv)) {
            char ip[64] = "-";
            uCxIpAddressToString(&srv.ntp_server_ip, ip, sizeof(ip));
            ucxapp_print("server %d: %s  ip %s  reachable %d\n", (int)srv.ntp_server_id,
                         srv.ntp_server_address != NULL ? srv.ntp_server_address : "-", ip, (int)srv.reachable);
        }
        uCxEnd(app->ucx);
        return UCXAPP_OK;
    }
    if (strcmp(sub, "on") == 0 && argc <= 3) {
        uNtpClientStatus_t mode = U_NTP_CLIENT_STATUS_ENABLE_MANUAL;
        if (argc == 3) {
            if (strcmp(argv[2], "manual") == 0) {
                mode = U_NTP_CLIENT_STATUS_ENABLE_MANUAL;
            } else if (strcmp(argv[2], "auto") == 0) {
                mode = U_NTP_CLIENT_STATUS_ENABLE_AUTO;
            } else {
                return UCXAPP_ERR_USAGE;
            }
        }
        int32_t err = uCxNetworkTimeSetClientEnabled(app->ucx, mode);
        if (err < 0) {
            ucxapp_print("ERROR: cannot enable NTP (%d%s)\n", (int)err, ucxapp_errname(err));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("NTP client enabled (%s)\n", mode == U_NTP_CLIENT_STATUS_ENABLE_AUTO ? "auto" : "manual");
        return UCXAPP_OK;
    }
    if (strcmp(sub, "off") == 0 && argc == 2) {
        int32_t err = uCxNetworkTimeSetClientEnabled(app->ucx, U_NTP_CLIENT_STATUS_DISABLE);
        if (err < 0) {
            ucxapp_print("ERROR: cannot disable NTP (%d%s)\n", (int)err, ucxapp_errname(err));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("NTP client disabled\n");
        return UCXAPP_OK;
    }
    if (strcmp(sub, "server") == 0 && argc == 4) {
        int32_t id;
        if (!ucxapp_parse_int(argv[2], &id) || id < 0 || id > 1) {
            return UCXAPP_ERR_USAGE;
        }
        const char *host = strcmp(argv[3], "-") == 0 ? "" : argv[3];
        int32_t err = uCxNetworkTimeSetNtpServer(app->ucx, id, host);
        if (err < 0) {
            ucxapp_print("ERROR: cannot set NTP server %d (%d%s)\n", (int)id, (int)err, ucxapp_errname(err));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("NTP server %d: %s\n", (int)id, *host != '\0' ? host : "removed");
        return UCXAPP_OK;
    }
    return UCXAPP_ERR_USAGE;
}

static const ucxapp_cmd_t gNetCmds[] = {
    { "net.ping", "<host> [count]",    "ICMP ping (count 1-10, default 4)",     UCXAPP_CMD_NEEDS_DEVICE, cmdNetPing },
    { "net.ntp", "<status|on|off|server> [args...]", "NTP client: 'status', 'on [manual|auto]', 'off', 'server <0|1> <host|->' ('-' removes)", UCXAPP_CMD_NEEDS_DEVICE, cmdNetNtp },
    { "net.iperf", "<server|client|stop> [args...]", "iperf throughput: 'server [tcp|udp] [port]', 'client <ip> [tcp|udp] [seconds] [port]', 'stop'", UCXAPP_CMD_NEEDS_DEVICE, cmdNetIperf },
    { "net.dns",  "<host>",            "Resolve a host name to an IP address",  UCXAPP_CMD_NEEDS_DEVICE, cmdNetDns },
    { "http.get", "<url> [show_bytes]", "HTTP/HTTPS GET, prints status and body", UCXAPP_CMD_NEEDS_DEVICE, cmdHttpGet },
    { "http.download", "<url> <file>", "Download to a file, following redirects", UCXAPP_CMD_NEEDS_DEVICE, cmdHttpDownload },
    { "http.post", "<url> <body> [content_type]", "HTTP/HTTPS POST (default form-urlencoded), prints status and body", UCXAPP_CMD_NEEDS_DEVICE, cmdHttpPost },
    { "wifi.portal", "<check|login> [probe_url] [field=value ...]", "Hotel/guest Wi-Fi login page: 'check' detects it, 'login' fills in and submits its form", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiPortal },
    { "wifi.portal.form", "<html_file> <page_url> [field=value ...]", "Show what wifi.portal login would submit for a saved login page (no module needed)", 0, cmdWifiPortalForm },
};

void ucxapp_register_net_cmds(void)
{
    ucxapp_cmd_register(gNetCmds, sizeof(gNetCmds) / sizeof(gNetCmds[0]));
}
