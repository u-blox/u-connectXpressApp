/* wifi.locate - position from a Wi-Fi scan, like ubxlib's uWifiLoc. NORA modules have no location command
 * (AT+ULOCWIFIPOS is NINA-W13/W15 only), so the host scans with the module, builds the request for the location
 * service (Google, Skyhook, HERE or Combain) and posts it over the module's own HTTPS client. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_store.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define LOC_STORE    "ucxapp.wifiloc"   /* "<service> <api-key>" per line, the default service first */
#define LOC_MAX_APS  20                 /* strongest access points sent; more rarely improves the fix */
#define LOC_MIN_APS  2                  /* the services need at least two */
#define LOC_KEY_LEN  200
#define LOC_BODY_LEN 2048

typedef struct {
    const char *name;
    const char *url;      /* the API key follows */
    const char *suffix;   /* after the key */
    bool here;            /* HERE has its own request format; the others take Google's */
} locService_t;

static const locService_t kServices[] = {
    { "google",  "https://www.googleapis.com/geolocation/v1/geolocate?key=", "",             false },
    { "skyhook", "https://global.skyhook.com/wps2/json/location?key=",       "&user=ucxapp", false },
    { "here",    "https://positioning.hereapi.com/v2/locate?apiKey=",        "",             true  },
    { "combain", "https://apiv2.combain.com/?key=",                          "",             false },
};
#define SERVICE_COUNT ((int)(sizeof(kServices) / sizeof(kServices[0])))

static const locService_t *findService(const char *name)
{
    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (strcasecmp(kServices[i].name, name) == 0) {
            return &kServices[i];
        }
    }
    return NULL;
}

static bool keySafe(const char *s)
{
    if (s[0] == '\0' || strlen(s) > LOC_KEY_LEN) {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              strchr("-_.~+/=", *s) != NULL)) {
            return false;
        }
    }
    return true;
}

/* ---- saved keys ---- */

/* True when the first len characters of line are the service name (any case). */
static bool nameIs(const char *line, size_t len, const char *service)
{
    if (strlen(service) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)service[i])) {
            return false;
        }
    }
    return true;
}

/* The saved key of a service (NULL: the default, the first line); false when there is none. */
static bool loadKey(const char *service, char *name, size_t nameSize, char *key, size_t keySize)
{
    size_t len = 0;
    char *d = (char *)ucxapp_store_load(LOC_STORE, &len);
    bool found = false;

    if (d == NULL) {
        return false;
    }
    for (char *line = d; *line != '\0' && !found; line += strcspn(line, "\r\n"), line += strspn(line, "\r\n")) {
        size_t end = strcspn(line, "\r\n");
        size_t sp = strcspn(line, " ");

        if (sp >= end || (service != NULL && !nameIs(line, sp, service))) {
            continue;
        }
        snprintf(name, nameSize, "%.*s", (int)sp, line);
        snprintf(key, keySize, "%.*s", (int)(end - sp - 1), line + sp + 1);
        found = findService(name) != NULL && keySafe(key);
    }
    free(d);
    return found;
}

/* Saves the key and makes its service the default (first line); keeps the other services' keys. */
static bool saveKey(const char *service, const char *key)
{
    size_t len = 0;
    char *d = (char *)ucxapp_store_load(LOC_STORE, &len);
    size_t size = len + strlen(service) + strlen(key) + 4;
    char *out = (char *)malloc(size);
    size_t n;
    bool ok;

    if (out == NULL) {
        free(d);
        return false;
    }
    n = (size_t)snprintf(out, size, "%s %s\n", service, key);
    for (char *line = d; line != NULL && *line != '\0'; line += strcspn(line, "\r\n"), line += strspn(line, "\r\n")) {
        size_t end = strcspn(line, "\r\n");
        size_t sp = strcspn(line, " ");

        if (sp < end && !nameIs(line, sp, service)) {
            n += (size_t)snprintf(out + n, size - n, "%.*s\n", (int)end, line);
        }
    }
    ok = ucxapp_store_save(LOC_STORE, out, n);
    if (ok) {
        ucxapp_store_make_private(LOC_STORE);
    }
    free(out);
    free(d);
    return ok;
}

static int listKeys(void)
{
    int count = 0;

    for (int i = 0; i < SERVICE_COUNT; i++) {
        char name[16];
        char key[LOC_KEY_LEN + 1];
        char def[16];
        char defKey[LOC_KEY_LEN + 1];

        if (loadKey(kServices[i].name, name, sizeof(name), key, sizeof(key))) {
            bool isDefault = loadKey(NULL, def, sizeof(def), defKey, sizeof(defKey)) && strcasecmp(def, name) == 0;
            ucxapp_print("%-8s %.4s...%s\n", kServices[i].name, key, isDefault ? "  (default)" : "");
            count++;
        }
    }
    if (count == 0) {
        ucxapp_print("no API keys saved (wifi.locate key <google|skyhook|here|combain> <api-key>)\n");
    }
    return UCXAPP_OK;
}

/* ---- request and answer ---- */

static bool noMap(const char *ssid)
{
    size_t len = strlen(ssid);

    return len >= 6 && strcmp(ssid + len - 6, "_nomap") == 0;   /* the owner opted out of location services */
}

/* The request body for the service from the access points; returns how many went in, 0 when it did not fit. */
static int buildBody(const locService_t *s, const ucxapp_wifi_ap_t *aps, int count, char *out, size_t size)
{
    size_t n = (size_t)snprintf(out, size, s->here ? "{\"wlan\":[" : "{\"considerIp\":false,\"wifiAccessPoints\":[");
    int used = 0;

    for (int i = 0; i < count && used < LOC_MAX_APS && n < size; i++) {
        const ucxapp_wifi_ap_t *a = &aps[i];
        char mac[18];

        if (noMap(a->ssid)) {
            continue;
        }
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", a->bssid[0], a->bssid[1], a->bssid[2],
                 a->bssid[3], a->bssid[4], a->bssid[5]);
        n += (size_t)(s->here ? snprintf(out + n, size - n, "%s{\"mac\":\"%s\",\"rss\":%d}", used ? "," : "", mac,
                                         a->rssi)
                              : snprintf(out + n, size - n, "%s{\"macAddress\":\"%s\",\"signalStrength\":%d,\"channel\":%d}",
                                         used ? "," : "", mac, a->rssi, a->channel));
        used++;
    }
    if (n < size) {
        n += (size_t)snprintf(out + n, size - n, "]}");
    }
    return n < size ? used : 0;
}

/* The number after "key": in json (from the first match). */
static bool jsonNumber(const char *json, const char *key, double *out)
{
    char pat[24];
    const char *p;
    char *end;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (json == NULL || (p = strstr(json, pat)) == NULL) {
        return false;
    }
    p += strlen(pat);
    p += strspn(p, " \t\r\n");
    if (*p != ':') {
        return false;
    }
    *out = strtod(p + 1, &end);
    return end != p + 1;
}

/* The string after "key": in json. */
static bool jsonString(const char *json, const char *key, char *out, size_t size)
{
    char pat[24];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    if ((p = strstr(json, pat)) == NULL) {
        return false;
    }
    p += strlen(pat);
    p += strspn(p, " \t\r\n");
    if (*p != ':') {
        return false;
    }
    p += 1 + strspn(p + 1, " \t\r\n");
    if (*p != '"') {
        return false;
    }
    snprintf(out, size, "%.*s", (int)strcspn(p + 1, "\""), p + 1);
    return out[0] != '\0';
}

/* Google, Skyhook and Combain: {"location":{"lat":..,"lng":..},"accuracy":..};
   HERE: {"location":{"lat":..,"lng":..,"accuracy":..}}. */
static bool parseAnswer(const char *json, double *lat, double *lng, double *acc)
{
    const char *loc = strstr(json, "\"location\"");

    if (loc == NULL || !jsonNumber(loc, "lat", lat) || !jsonNumber(loc, "lng", lng)) {
        return false;
    }
    if (!jsonNumber(loc, "accuracy", acc) && !jsonNumber(json, "accuracy", acc)) {
        *acc = -1;
    }
    return *lat >= -90 && *lat <= 90 && *lng >= -180 && *lng <= 180;
}

static void printError(const char *service, int status, const char *resp)
{
    char msg[160];

    if (status < 0) {
        ucxapp_print("ERROR: no answer from %s (is the module on Wi-Fi? wifi.connect)\n", service);
        return;
    }
    if (!jsonString(resp, "message", msg, sizeof(msg)) && !jsonString(resp, "error_description", msg, sizeof(msg)) &&
        !jsonString(resp, "title", msg, sizeof(msg)) && !jsonString(resp, "error", msg, sizeof(msg))) {
        snprintf(msg, sizeof(msg), "%.150s", resp);
    }
    ucxapp_print("ERROR: %s answered HTTP %d%s%s\n", service, status, msg[0] != '\0' ? ": " : "", msg);
    if (status == 404) {
        ucxapp_print("(the service knows none of the access points here)\n");
    } else if (status == 400 || status == 401 || status == 403) {
        ucxapp_print("(check the API key: wifi.locate key %s <api-key>)\n", service);
    }
}

/* Appends the key to the URL, percent-encoding the base64 characters. */
static void buildUrl(const locService_t *s, const char *key, char *url, size_t size)
{
    size_t n = (size_t)snprintf(url, size, "%s", s->url);

    for (; *key != '\0' && n + 4 < size; key++) {
        n += (size_t)(strchr("+/=", *key) != NULL ? snprintf(url + n, size - n, "%%%02X", (unsigned char)*key)
                                                   : snprintf(url + n, size - n, "%c", *key));
    }
    snprintf(url + n, size - n, "%s", s->suffix);
}

/* Scans and builds the request; returns the number of access points in it, 0 on failure (printed). */
static int scanAndBuild(ucxapp_t *app, const locService_t *s, char *body, size_t size)
{
    ucxapp_wifi_ap_t *aps = (ucxapp_wifi_ap_t *)malloc(sizeof(ucxapp_wifi_ap_t) * LOC_MAX_APS * 2);
    int count;
    int used;

    if (aps == NULL) {
        ucxapp_print("ERROR: out of memory\n");
        return 0;
    }
    count = ucxapp_wifi_scan(app, aps, LOC_MAX_APS * 2);
    if (count < 0) {
        free(aps);
        return 0;
    }
    used = buildBody(s, aps, count, body, size);
    free(aps);
    if (used < LOC_MIN_APS) {
        ucxapp_print("ERROR: %d usable access point(s) found, %s needs at least %d\n", used, s->name, LOC_MIN_APS);
        return 0;
    }
    return used;
}

static int locate(ucxapp_t *app, const locService_t *s, const char *key)
{
    char *body = (char *)malloc(LOC_BODY_LEN);
    char url[96 + 3 * LOC_KEY_LEN];
    const char *resp = "";
    double lat, lng, acc;
    int used;
    int status;

    if (body == NULL) {
        ucxapp_print("ERROR: out of memory\n");
        return UCXAPP_ERR_FAILED;
    }
    used = scanAndBuild(app, s, body, LOC_BODY_LEN);
    if (used == 0) {
        free(body);
        return UCXAPP_ERR_FAILED;
    }
    buildUrl(s, key, url, sizeof(url));
    ucxapp_print("Asking %s with %d access points...\n", s->name, used);
    status = ucxapp_http_post(app, url, body, "application/json", &resp);
    free(body);
    if (status < 200 || status >= 300) {
        printError(s->name, status, resp);
        return UCXAPP_ERR_FAILED;
    }
    if (!parseAnswer(resp, &lat, &lng, &acc)) {
        ucxapp_print("ERROR: no position in the answer from %s: %.200s\n", s->name, resp);
        return UCXAPP_ERR_FAILED;
    }
    if (acc >= 0) {
        ucxapp_print("Position: %.6f, %.6f +/- %.0f m (%s, %d access points)\n", lat, lng, acc, s->name, used);
    } else {
        ucxapp_print("Position: %.6f, %.6f (%s, %d access points)\n", lat, lng, s->name, used);
    }
    ucxapp_print("Map: https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f#map=17/%.6f/%.6f\n", lat, lng, lat, lng);
    ucxapp_print("As a GNSS receiver for bridges and tests: gnss.virtual %.6f %.6f\n", lat, lng);
    return UCXAPP_OK;
}

/* A module with Wi-Fi is connected; prints why not. */
static bool haveWifiModule(ucxapp_t *app)
{
    if (app->ucx == NULL) {
        ucxapp_print("ERROR: not connected to a module\n");
        return false;
    }
    if (!ucxapp_module_has_wifi(app)) {
        ucxapp_print("ERROR: wifi.locate needs Wi-Fi; this module has Bluetooth LE only (see dev.caps)\n");
        return false;
    }
    return true;
}

static int cmdLocate(ucxapp_t *app, int argc, char *argv[])
{
    const char *sub = argc > 1 ? argv[1] : "";
    const locService_t *s;
    char name[16];
    char key[LOC_KEY_LEN + 1];

    if (strcasecmp(sub, "key") == 0) {
        if (argc == 2) {
            return listKeys();
        }
        if (argc != 4 || (s = findService(argv[2])) == NULL || !keySafe(argv[3])) {
            return UCXAPP_ERR_USAGE;
        }
        if (!saveKey(s->name, argv[3])) {
            ucxapp_print("ERROR: cannot save %s\n", LOC_STORE);
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("%s API key saved to %s; %s is the default service\n", s->name, LOC_STORE, s->name);
        return UCXAPP_OK;
    }
    if (strcasecmp(sub, "body") == 0) {
        char *body;
        int used;

        if (argc > 3) {
            return UCXAPP_ERR_USAGE;
        }
        s = argc == 3 ? findService(argv[2])
            : loadKey(NULL, name, sizeof(name), key, sizeof(key)) ? findService(name) : &kServices[0];
        if (s == NULL) {
            return UCXAPP_ERR_USAGE;
        }
        if (!haveWifiModule(app)) {
            return UCXAPP_ERR_NOT_CONNECTED;
        }
        if ((body = (char *)malloc(LOC_BODY_LEN)) == NULL) {
            return UCXAPP_ERR_FAILED;
        }
        used = scanAndBuild(app, s, body, LOC_BODY_LEN);
        if (used > 0) {
            ucxapp_print("POST %s<api-key>%s (Content-Type: application/json)\n%s\n", s->url, s->suffix, body);
        }
        free(body);
        return used > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
    }
    if (argc > 3 || (argc == 3 && !keySafe(argv[2]))) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 1) {
        if ((s = findService(argv[1])) == NULL) {
            return UCXAPP_ERR_USAGE;
        }
        if (argc == 3) {
            snprintf(key, sizeof(key), "%s", argv[2]);
        } else if (!loadKey(s->name, name, sizeof(name), key, sizeof(key))) {
            ucxapp_print("ERROR: no %s API key (wifi.locate key %s <api-key>, or wifi.locate %s <api-key>)\n", s->name,
                         s->name, s->name);
            return UCXAPP_ERR_FAILED;
        }
    } else if (loadKey(NULL, name, sizeof(name), key, sizeof(key))) {
        s = findService(name);
    } else {
        ucxapp_print("ERROR: no API key saved (wifi.locate key <google|skyhook|here|combain> <api-key>)\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!haveWifiModule(app)) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    return locate(app, s, key);
}

static const ucxapp_cmd_t gWifiLocCmds[] = {
    { "wifi.locate", "[google|skyhook|here|combain] [api-key] | key [<service> <api-key>] | body [service]",
      "Position from a Wi-Fi scan: the access points go to Google, Skyhook, HERE or Combain over the module's HTTPS "
      "(needs Wi-Fi and the service's API key); key saves a key and makes its service the default, body prints the "
      "request without sending it", 0, cmdLocate },
};

void ucxapp_register_wifiloc_cmds(void)
{
    ucxapp_cmd_register(gWifiLocCmds, sizeof(gWifiLocCmds) / sizeof(gWifiLocCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_wifiloc_cmds(void)
{
}

#endif
