#include "ucxapp_cmd.h"
#include "ucxapp_platform.h"
#include "ucxapp_store.h"
#include "ucx_obfuscate.h"
#include "u_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_wifi.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define WIFI_STATE_CONNECTED 2   /* Wi-Fi connection status id: 1 = not connected, 2 = connected */

static bool isConnected(uCxHandle_t *ucx)
{
    uCxWifiStationStatus_t status;
    bool connected = false;

    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_CONNECTION, &status)) {
        connected = status.rsp.StatusIdInt.int_val == WIFI_STATE_CONNECTED;
    }
    uCxEnd(ucx);
    return connected;
}

static void printIpAddress(uCxHandle_t *ucx)
{
    uSockIpAddress_t ip;
    char str[40];

    if (uCxWifiStationGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_IPV4, &ip) == 0 &&
        uCxIpAddressToString(&ip, str, sizeof(str)) > 0) {
        ucxapp_print("ip=%s\n", str);
    }
}

bool ucxapp_wifi_get_status(uCxHandle_t *ucx, char *ssid, size_t ssidSize, char *ip, size_t ipSize)
{
    uCxWifiStationStatus_t status;
    uSockIpAddress_t addr;

    ssid[0] = '\0';
    ip[0] = '\0';
    if (!isConnected(ucx)) {
        return false;
    }
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_SSID, &status) && status.rsp.StatusIdStr.ssid != NULL) {
        snprintf(ssid, ssidSize, "%s", status.rsp.StatusIdStr.ssid);
    }
    uCxEnd(ucx);
    if (uCxWifiStationGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_IPV4, &addr) != 0 ||
        uCxIpAddressToString(&addr, ip, ipSize) <= 0) {
        ip[0] = '\0';
    }
    return true;
}

static void getNetStatus(uCxHandle_t *ucx, uWifiNetStatusId_t id, char *out, size_t size)
{
    uSockIpAddress_t ip;

    out[0] = '\0';
    if (uCxWifiStationGetNetworkStatus(ucx, id, &ip) != 0 || uCxIpAddressToString(&ip, out, size) <= 0) {
        out[0] = '\0';
    }
}

static int readStatusInt(uCxHandle_t *ucx, uWifiStatusId_t id, int fallback)
{
    uCxWifiStationStatus_t status;
    int value = fallback;

    if (uCxWifiStationStatusBegin(ucx, id, &status)) {
        value = (int)status.rsp.StatusIdInt.int_val;
    }
    uCxEnd(ucx);
    return value;
}

bool ucxapp_wifi_dash(uCxHandle_t *ucx, char *out, size_t size)
{
    char ssid[40];
    char ip[40];

    if (!ucxapp_wifi_get_status(ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        snprintf(out, size, "not connected");
        return false;
    }
    int rssi = readStatusInt(ucx, U_WIFI_STATUS_ID_RSSI, 0);
    int ch = readStatusInt(ucx, U_WIFI_STATUS_ID_CHANNEL, 0);

    snprintf(out, size, "%s  %d dBm  ch %d\n%s", ssid, rssi, ch, ip[0] ? ip : "no IP");
    return true;
}

static const char *bandOf(int channel)
{
    return channel >= 1 && channel <= 14 ? "2.4" : (channel >= 36 ? "5" : "?");
}

/* What the user wants to know right after joining: address, mask, gateway, channel and signal. */
static void printLinkDetails(uCxHandle_t *ucx)
{
    char mask[40], gateway[40];
    int channel = readStatusInt(ucx, U_WIFI_STATUS_ID_CHANNEL, 0);
    int rssi = readStatusInt(ucx, U_WIFI_STATUS_ID_RSSI, -32768);

    printIpAddress(ucx);
    getNetStatus(ucx, U_WIFI_NET_STATUS_ID_SUBNET, mask, sizeof(mask));
    getNetStatus(ucx, U_WIFI_NET_STATUS_ID_GATE_WAY, gateway, sizeof(gateway));
    if (mask[0] != '\0' || gateway[0] != '\0') {
        ucxapp_print("  mask %s  gateway %s\n", mask, gateway);
    }
    if (channel > 0 && rssi != -32768) {
        ucxapp_print("  channel %d (%s GHz)  signal %d dBm\n", channel, bandOf(channel), rssi);
    }
}

/* Disconnect if needed and set the SSID; the caller then sets the security and calls startAndWait(). */
static bool prepareConnect(uCxHandle_t *ucx, const char *ssid)
{
    if (isConnected(ucx)) {
        ucxapp_print("Already connected, disconnecting first...\n");
        uCxWifiStationDisconnect(ucx);
        U_CX_PORT_SLEEP_MS(1000);
    }
    if (uCxWifiStationSetConnectionParams(ucx, 0, ssid) != 0) {
        ucxapp_print("ERROR: failed to set connection parameters\n");
        return false;
    }
    return true;
}

#define WIFI_RETRY_AFTER_REBOOT 1000   /* startAndWait: module was stuck and has been rebooted, set up again */
#define WIFI_ERR_ALREADY_CONNECTED (-65)

static int startAndWait(ucxapp_t *app, const char *ssid, uint32_t timeoutS)
{
    uCxHandle_t *ucx = app->ucx;

    ucxapp_events_clear(URC_FLAG_NETWORK_UP | URC_FLAG_NETWORK_DOWN);
    int32_t r = uCxWifiStationConnect(ucx, 0);
    if (r != 0) {
        /* A previous attempt can still be pending in the module; stop it and try once more. */
        uCxWifiStationDisconnect(ucx);
        U_CX_PORT_SLEEP_MS(2000);
        ucxapp_events_clear(URC_FLAG_NETWORK_UP | URC_FLAG_NETWORK_DOWN);
        r = uCxWifiStationConnect(ucx, 0);
    }
    if (r == WIFI_ERR_ALREADY_CONNECTED && !isConnected(ucx)) {
        /* Seen on FW 3.2: the station reports "not connected" but refuses to connect; only a reboot clears it. */
        ucxapp_print("Wi-Fi state is stuck (%d%s), rebooting the module...\n", (int)r, ucxapp_errname(r));
        if (ucxapp_module_reboot(app)) {
            return WIFI_RETRY_AFTER_REBOOT;
        }
    }
    if (r != 0) {
        ucxapp_print("ERROR: failed to start connection (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_events_wait(URC_FLAG_NETWORK_UP, timeoutS)) {
        ucxapp_print("ERROR: timeout waiting for network up\n");
        uCxWifiStationDisconnect(ucx);   /* abort the pending attempt, else the next connect gets ERROR:15 */
        U_CX_PORT_SLEEP_MS(1000);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Connected to '%s'\n", ssid);
    printLinkDetails(ucx);
    return UCXAPP_OK;
}

static int connectTo(ucxapp_t *app, const char *ssid, const char *password, uint32_t timeoutS)
{
    uCxHandle_t *ucx = app->ucx;
    int rc = WIFI_RETRY_AFTER_REBOOT;

    for (int attempt = 0; attempt < 2 && rc == WIFI_RETRY_AFTER_REBOOT; attempt++) {
        if (!prepareConnect(ucx, ssid)) {
            return UCXAPP_ERR_FAILED;
        }
        if (password[0] != '\0') {
            if (uCxWifiStationSetSecurityWpa(ucx, 0, password, U_WIFI_WPA_THRESHOLD_WPA2) != 0) {
                ucxapp_print("ERROR: failed to set WPA security\n");
                return UCXAPP_ERR_FAILED;
            }
        } else if (uCxWifiStationSetSecurityOpen(ucx, 0) != 0) {
            ucxapp_print("ERROR: failed to set open security\n");
            return UCXAPP_ERR_FAILED;
        }
        rc = startAndWait(app, ssid, timeoutS);
    }
    return rc == WIFI_RETRY_AFTER_REBOOT ? UCXAPP_ERR_FAILED : rc;
}

#ifndef UCXAPP_LITE
/* The Enterprise settings are lost by the reboot, so the user re-runs the command. */
static int finishEnterprise(int rc)
{
    if (rc == WIFI_RETRY_AFTER_REBOOT) {
        ucxapp_print("ERROR: module rebooted, run the command again\n");
        return UCXAPP_ERR_FAILED;
    }
    return rc;
}

/* "1.2" | "1.3" | "any" -> TLS version for EAP; false on anything else. */
static bool parseTlsVersion(const char *s, uWifiTlsVersion_t *v)
{
    if (strcmp(s, "1.2") == 0) {
        *v = U_WIFI_TLS_VERSION_TLS1_2;
    } else if (strcmp(s, "1.3") == 0) {
        *v = U_WIFI_TLS_VERSION_TLS1_3;
    } else if (strcmp(s, "any") == 0) {
        *v = U_WIFI_TLS_VERSION_TLS1_2_OR_TLS1_3;
    } else {
        return false;
    }
    return true;
}

/* The module rejects AT+UWSSE without an identity and AT+UWSSP without a CA (FW 3.2-3.5), so both are required here. */

/* wifi.eap <ssid> <ca> <client-cert> <client-key> <identity> [1.2|1.3|any]: names are certificates on the module. */
static int cmdWifiEap(ucxapp_t *app, int argc, char *argv[])
{
    uWifiTlsVersion_t tls = U_WIFI_TLS_VERSION_TLS1_2_OR_TLS1_3;
    int32_t r;

    if (argc < 6 || argc > 7 || (argc == 7 && !parseTlsVersion(argv[6], &tls))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!prepareConnect(app->ucx, argv[1])) {
        return UCXAPP_ERR_FAILED;
    }
    r = uCxWifiStationSetSecurityEnterprise6(app->ucx, 0, tls, argv[2], argv[3], argv[4], argv[5]);
    if (r != 0) {
        ucxapp_print("ERROR: failed to set EAP-TLS security (%d%s); check the names with cert.list\n", (int)r,
                     ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    return finishEnterprise(startAndWait(app, argv[1], 30));
}

/* wifi.peap <ssid> <user> <password|-> <ca> [1.2|1.3|any] */
static int cmdWifiPeap(ucxapp_t *app, int argc, char *argv[])
{
    uWifiTlsVersion_t tls = U_WIFI_TLS_VERSION_TLS1_2_OR_TLS1_3;
    char password[128];
    int32_t r;

    if (argc < 5 || argc > 6 || (argc == 6 && !parseTlsVersion(argv[5], &tls))) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[3], "-") == 0) {
        if (ucxapp_readline("password: ", password, sizeof(password)) < 0) {
            return UCXAPP_ERR_FAILED;
        }
    } else {
        snprintf(password, sizeof(password), "%s", argv[3]);
    }
    if (!prepareConnect(app->ucx, argv[1])) {
        memset(password, 0, sizeof(password));
        return UCXAPP_ERR_FAILED;
    }
    r = uCxWifiStationSetSecurityPeap5(app->ucx, 0, tls, argv[2], password, argv[4]);
    memset(password, 0, sizeof(password));
    if (r != 0) {
        ucxapp_print("ERROR: failed to set PEAP security (%d%s); check the CA name with cert.list\n", (int)r,
                     ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    return finishEnterprise(startAndWait(app, argv[1], 30));
}
#endif

/* ---- saved profiles: one "name<TAB>ssid<TAB>obfuscated password" line each (older files add an ignored field) ---- */

#define PROFILES_FILE  "ucxapp.wifi"
#define MAX_PROFILES   16
#define NAME_LEN       32
#define SSID_LEN       33
#define PASS_LEN       64
#define OBF_LEN        (PASS_LEN * 2 + 1)
#define LINE_MAX_LEN   (NAME_LEN + SSID_LEN + OBF_LEN + 4)

typedef struct {
    char name[NAME_LEN];
    char ssid[SSID_LEN];
    char password[PASS_LEN];
} profile_t;

static bool fieldOk(const char *s, size_t maxLen)
{
    return s[0] != '\0' && strlen(s) < maxLen && strpbrk(s, "\t\r\n") == NULL;
}

static int loadProfiles(profile_t *list)
{
    size_t len = 0;
    uint8_t *data = ucxapp_store_load(PROFILES_FILE, &len);
    int count = 0;
    char *line = (char *)data;

    if (data == NULL) {
        return 0;
    }
    while (*line != '\0' && count < MAX_PROFILES) {
        char *end = line + strcspn(line, "\r\n");
        char *next = end;
        char *tab1;
        char *tab2;
        profile_t *p = &list[count];

        while (*next == '\r' || *next == '\n') {
            *next++ = '\0';
        }
        tab1 = strchr(line, '\t');
        tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
        if (tab2 != NULL) {
            *tab1 = '\0';
            *tab2 = '\0';
            tab2[1 + strcspn(tab2 + 1, "\t")] = '\0';
            if (strlen(line) < NAME_LEN && strlen(tab1 + 1) < SSID_LEN && strlen(tab2 + 1) < OBF_LEN) {
                snprintf(p->name, sizeof(p->name), "%s", line);
                snprintf(p->ssid, sizeof(p->ssid), "%s", tab1 + 1);
                deobfuscatePassword(tab2 + 1, p->password, sizeof(p->password));
                count++;
            }
        }
        line = next;
    }
    free(data);
    return count;
}

static bool saveProfiles(const profile_t *list, int count)
{
    char *text = (char *)calloc((size_t)count + 1, LINE_MAX_LEN);
    bool ok;

    if (text == NULL) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        char obf[OBF_LEN];
        size_t used = strlen(text);

        obfuscatePassword(list[i].password, obf, sizeof(obf));
        snprintf(text + used, LINE_MAX_LEN, "%s\t%s\t%s\n", list[i].name, list[i].ssid, obf);
    }
    ok = ucxapp_store_save(PROFILES_FILE, text, strlen(text));
    free(text);
    return ok;
}

static int findProfile(const profile_t *list, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

#define LAST_FILE       "ucxapp.wifilast"
#define REGDOMAIN_FILE  "ucxapp.regdomain"

static const struct { const char *name; uWifiRegDomain_t value; } gRegDomains[] = {
    { "world", U_WIFI_REG_DOMAIN_WORLD }, { "etsi", U_WIFI_REG_DOMAIN_ETSI }, { "fcc", U_WIFI_REG_DOMAIN_FCC },
    { "ic", U_WIFI_REG_DOMAIN_IC }, { "nz", U_WIFI_REG_DOMAIN_NZ }, { "mkk", U_WIFI_REG_DOMAIN_MKK },
    { "ncc", U_WIFI_REG_DOMAIN_NCC }, { "acma", U_WIFI_REG_DOMAIN_ACMA }, { "kcc", U_WIFI_REG_DOMAIN_KCC },
    { "sa", U_WIFI_REG_DOMAIN_SA }, { "br", U_WIFI_REG_DOMAIN_BR },
};

static int findRegDomain(const char *name)
{
    for (size_t i = 0; i < sizeof(gRegDomains) / sizeof(gRegDomains[0]); i++) {
        if (strcmp(gRegDomains[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Applies the saved regulatory domain; defaults to ETSI so channels 1-13 are scanned. */
static void applyRegDomain(uCxHandle_t *ucx)
{
    size_t len = 0;
    char *saved = (char *)ucxapp_store_load(REGDOMAIN_FILE, &len);
    int dom = saved != NULL ? findRegDomain(saved) : -1;

    free(saved);
    if (dom < 0) {
        dom = findRegDomain("etsi");
    }
    if (uCxWifiSetRegulatoryDomain(ucx, gRegDomains[dom].value) != 0) {
        ucxapp_print("WARNING: could not set regulatory domain '%s'\n", gRegDomains[dom].name);
    }
}

static bool loadLastUsed(char *name, size_t size)
{
    size_t len = 0;
    char *data = (char *)ucxapp_store_load(LAST_FILE, &len);

    if (data == NULL) {
        return false;
    }
    data[strcspn(data, "\r\n")] = '\0';
    snprintf(name, size, "%s", data);
    free(data);
    return name[0] != '\0';
}

/* Connects and remembers the profile as last used.
 * reuse: keep an existing link to the same SSID instead of reconnecting. */
static int connectProfile(ucxapp_t *app, const profile_t *list, int idx, uint32_t timeoutS, bool reuse)
{
    const profile_t *p = &list[idx];
    char ip[40], current[SSID_LEN];
    int rc;

    if (reuse && ucxapp_wifi_get_status(app->ucx, current, sizeof(current), ip, sizeof(ip)) &&
        strcmp(current, p->ssid) == 0) {
        ucxapp_print("Already connected to '%s'\n", current);
        printLinkDetails(app->ucx);
        rc = UCXAPP_OK;
    } else {
        rc = connectTo(app, p->ssid, p->password, timeoutS);
    }
    if (rc == UCXAPP_OK) {
        ucxapp_store_save(LAST_FILE, p->name, strlen(p->name));
    }
    return rc;
}

/* ---- scan results, kept so wifi.connect can take a number ---- */

#ifndef SCAN_MAX
#define SCAN_MAX 64
#endif

typedef struct {
    char ssid[SSID_LEN];
    uint8_t bssid[6];
    int channel;
    int rssi;
    uint32_t auth;      /* bit 3 WPA, bit 4 WPA2, bit 5 WPA3; 0 = open */
} scanNet_t;

static scanNet_t gScan[SCAN_MAX];
static int gScanCount;

static void appendWord(char *out, size_t size, const char *word)
{
    size_t used = strlen(out);

    snprintf(out + used, size - used, "%s%s", used > 0 ? "/" : "", word);
}

static void securityText(uint32_t auth, char *out, size_t size)
{
    out[0] = '\0';
    if (auth == 0) {
        snprintf(out, size, "Open");
        return;
    }
    if (auth & (1u << 3)) {
        appendWord(out, size, "WPA");
    }
    if (auth & (1u << 4)) {
        appendWord(out, size, "WPA2");
    }
    if (auth & (1u << 5)) {
        appendWord(out, size, "WPA3");
    }
    if (out[0] == '\0') {
        snprintf(out, size, "Secured");
    }
}

static const char *signalBars(int rssi)
{
    return rssi >= -50 ? "#####" : rssi >= -60 ? "####." : rssi >= -70 ? "###.." : rssi >= -80 ? "##..." : "#....";
}

static int compareNet(const void *pa, const void *pb)
{
    return ((const scanNet_t *)pb)->rssi - ((const scanNet_t *)pa)->rssi;
}

/* Quietest channel among the candidates; 2.4 GHz only has three that do not overlap. */
static int quietest(const int *counts, const int *candidates, int n, int *found)
{
    int best = candidates[0];

    for (int i = 1; i < n; i++) {
        if (counts[candidates[i]] < counts[best]) {
            best = candidates[i];
        }
    }
    *found = counts[best];
    return best;
}

static void printChannelSummary(void)
{
    static const int best24[] = { 1, 6, 11 };
    static const int best5[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136,
                                 140, 144, 149, 153, 157, 161, 165 };
    int count24[15] = { 0 };
    int count5[166] = { 0 };
    int n24, n5;
    int ch24, ch5;

    for (int i = 0; i < gScanCount; i++) {
        int ch = gScan[i].channel;

        if (ch >= 1 && ch <= 14) {
            count24[ch]++;
        } else if (ch >= 36 && ch <= 165) {
            count5[ch]++;
        }
    }
    ch24 = quietest(count24, best24, 3, &n24);
    ch5 = quietest(count5, best5, (int)(sizeof(best5) / sizeof(best5[0])), &n5);
    ucxapp_print("Quietest for wifi.ap.enable: 2.4 GHz ch%d (%d network(s)), 5 GHz ch%d (%d)\n", ch24, n24, ch5, n5);
}

static int cmdWifiScan(ucxapp_t *app, int argc, char *argv[])
{
    uCxWifiStationScanDefault_t net;
    profile_t profiles[MAX_PROFILES];
    int profileCount = loadProfiles(profiles);
    int count = 0;
    (void)argc;
    (void)argv;

    applyRegDomain(app->ucx);
    ucxapp_print("Scanning (can take up to 40 s)...\n");
    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, 60000, false);
    uCxWifiStationScanDefaultBegin(app->ucx);
    while (uCxWifiStationScanDefaultGetNext(app->ucx, &net)) {
        scanNet_t *n;

        if (count == SCAN_MAX) {
            continue;
        }
        n = &gScan[count++];
        snprintf(n->ssid, sizeof(n->ssid), "%s", net.ssid != NULL ? net.ssid : "");
        memcpy(n->bssid, net.bssid.address, sizeof(n->bssid));
        n->channel = (int)net.channel;
        n->rssi = (int)net.rssi;
        n->auth = (uint32_t)net.authentication_suites;
    }
    uCxEnd(app->ucx);
    qsort(gScan, (size_t)count, sizeof(gScan[0]), compareNet);
    gScanCount = count;

    for (int i = 0; i < count; i++) {
        const scanNet_t *n = &gScan[i];
        char sec[24];
        int known = -1;

        securityText(n->auth, sec, sizeof(sec));
        for (int p = 0; p < profileCount; p++) {
            if (n->ssid[0] != '\0' && strcmp(profiles[p].ssid, n->ssid) == 0) {
                known = p;
            }
        }
        ucxapp_print("%2d  %-32s %02X:%02X:%02X:%02X:%02X:%02X ch%-3d %3s GHz %4d dBm [%s] %s%s%s%s\n", i + 1,
                     n->ssid[0] != '\0' ? n->ssid : "<hidden>", n->bssid[0], n->bssid[1], n->bssid[2], n->bssid[3],
                     n->bssid[4], n->bssid[5], n->channel, bandOf(n->channel), n->rssi, signalBars(n->rssi), sec,
                     known >= 0 ? "  <- profile '" : "", known >= 0 ? profiles[known].name : "", known >= 0 ? "'" : "");
    }
    ucxapp_print("%d network(s)%s\n", count, count > 0 ? "; connect with wifi.connect <number|ssid> [password]" : "");
    if (count > 0) {
        printChannelSummary();
    }
    return UCXAPP_OK;
}

static int findProfileBySsid(const profile_t *list, int count, const char *ssid)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static int cmdProfileAuto(ucxapp_t *app, int argc, char *argv[]);

/* A number picks a network from the last wifi.scan; a saved profile supplies the password if none is given. */
static int cmdWifiConnect(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    char ssid[SSID_LEN];
    int count, idx;

    if (argc == 1) {
        return cmdProfileAuto(app, argc, argv);
    }
    if (argc < 2 || argc > 3 || strlen(argv[1]) >= sizeof(ssid)) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(ssid, sizeof(ssid), "%s", argv[1]);
    if (strspn(ssid, "0123456789") == strlen(ssid) && gScanCount > 0 && atoi(ssid) >= 1 && atoi(ssid) <= gScanCount) {
        const scanNet_t *n = &gScan[atoi(ssid) - 1];

        if (n->ssid[0] == '\0') {
            ucxapp_print("ERROR: network %d is hidden; give its SSID\n", atoi(ssid));
            return UCXAPP_ERR_FAILED;
        }
        snprintf(ssid, sizeof(ssid), "%s", n->ssid);
        ucxapp_print("network %s is '%s'\n", argv[1], ssid);
    }
    count = loadProfiles(list);
    idx = findProfileBySsid(list, count, ssid);
    if (argc == 3 && strcmp(argv[2], "-") == 0) {
        /* Password from a prompt (or piped stdin), so it is not on the command line. */
        static char typed[PASS_LEN];
        if (ucxapp_readline("password: ", typed, sizeof(typed)) < 0) {
            return UCXAPP_ERR_FAILED;
        }
        argv[2] = typed;
    }
    if (argc == 3) {
        int rc = connectTo(app, ssid, argv[2], 20);

        /* Remember the network so wifi.connect / wifi.profile.auto (and the hardware tests) can reuse it. */
        if (rc == UCXAPP_OK && fieldOk(ssid, SSID_LEN) && fieldOk(argv[2], PASS_LEN) &&
            (idx >= 0 || count < MAX_PROFILES)) {
            if (idx < 0) {
                idx = count++;
                memset(&list[idx], 0, sizeof(list[idx]));
                snprintf(list[idx].name, sizeof(list[idx].name), "%.*s", NAME_LEN - 1, ssid);
                snprintf(list[idx].ssid, sizeof(list[idx].ssid), "%s", ssid);
            }
            snprintf(list[idx].password, sizeof(list[idx].password), "%s", argv[2]);
            if (saveProfiles(list, count)) {
                ucxapp_store_save(LAST_FILE, list[idx].name, strlen(list[idx].name));
                ucxapp_print("saved as profile '%s'\n", list[idx].name);
            }
        }
        return rc;
    }
    if (idx >= 0) {
        ucxapp_print("using saved profile '%s'\n", list[idx].name);
        return connectProfile(app, list, idx, 20, false);
    }
    for (int i = 0; i < gScanCount; i++) {
        if (strcmp(gScan[i].ssid, ssid) == 0 && gScan[i].auth != 0) {
            ucxapp_print("ERROR: '%s' is secured; give the password: wifi.connect \"%s\" <password>\n", ssid, ssid);
            return UCXAPP_ERR_FAILED;
        }
    }
    return connectTo(app, ssid, "", 20);
}

static int cmdProfileAdd(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    const char *password = argc > 3 ? argv[3] : "";
    int count;
    int idx;

    (void)app;
    if (argc < 3 || argc > 4 || !fieldOk(argv[1], NAME_LEN) || !fieldOk(argv[2], SSID_LEN) ||
        strlen(password) >= PASS_LEN || strpbrk(password, "\t\r\n") != NULL) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadProfiles(list);
    idx = findProfile(list, count, argv[1]);
    if (idx < 0) {
        if (count >= MAX_PROFILES) {
            ucxapp_print("ERROR: at most %d profiles\n", MAX_PROFILES);
            return UCXAPP_ERR_FAILED;
        }
        idx = count++;
    }
    snprintf(list[idx].name, sizeof(list[idx].name), "%s", argv[1]);
    snprintf(list[idx].ssid, sizeof(list[idx].ssid), "%s", argv[2]);
    snprintf(list[idx].password, sizeof(list[idx].password), "%s", password);
    if (!saveProfiles(list, count)) {
        ucxapp_print("ERROR: cannot write %s\n", PROFILES_FILE);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("profile '%s' saved (ssid '%s')\n", argv[1], argv[2]);
    return UCXAPP_OK;
}

static int cmdProfileList(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    int count = loadProfiles(list);

    (void)app;
    (void)argc;
    (void)argv;
    for (int i = 0; i < count; i++) {
        ucxapp_print("%s: ssid=%s %s\n", list[i].name, list[i].ssid, list[i].password[0] != '\0' ? "WPA" : "open");
    }
    ucxapp_print("%d profile(s)\n", count);
    return UCXAPP_OK;
}

static int cmdProfileRemove(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    int count;
    int idx;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadProfiles(list);
    idx = findProfile(list, count, argv[1]);
    if (idx < 0) {
        ucxapp_print("ERROR: no profile '%s'\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    list[idx] = list[--count];
    if (!saveProfiles(list, count)) {
        ucxapp_print("ERROR: cannot write %s\n", PROFILES_FILE);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("profile '%s' removed\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdProfileConnect(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    int count;
    int idx;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadProfiles(list);
    idx = findProfile(list, count, argv[1]);
    if (idx < 0) {
        ucxapp_print("ERROR: no profile '%s' (see wifi.profile.list)\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    return connectProfile(app, list, idx, 20, false);
}

#define AUTO_FIRST_TRY_S  10

static int cmdProfileAuto(ucxapp_t *app, int argc, char *argv[])
{
    profile_t list[MAX_PROFILES];
    char last[NAME_LEN];
    int count = loadProfiles(list);
    int idx;
    int best = -1;
    int bestRssi = -1000;
    uCxWifiStationScanDefault_t net;

    (void)argc;
    (void)argv;
    if (count == 0) {
        ucxapp_print("ERROR: no profiles (see wifi.profile.add)\n");
        return UCXAPP_ERR_FAILED;
    }

    if (loadLastUsed(last, sizeof(last)) && (idx = findProfile(list, count, last)) >= 0) {
        ucxapp_print("Trying last used profile '%s'...\n", last);
        if (connectProfile(app, list, idx, AUTO_FIRST_TRY_S, true) == UCXAPP_OK) {
            return UCXAPP_OK;
        }
    }

    applyRegDomain(app->ucx);
    ucxapp_print("Scanning for known networks (can take up to 40 s)...\n");
    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, 60000, false);
    uCxWifiStationScanDefaultBegin(app->ucx);
    while (uCxWifiStationScanDefaultGetNext(app->ucx, &net)) {
        for (int i = 0; i < count; i++) {
            if (net.ssid != NULL && strcmp(net.ssid, list[i].ssid) == 0 && net.rssi > bestRssi) {
                best = i;
                bestRssi = (int)net.rssi;
            }
        }
    }
    uCxEnd(app->ucx);
    if (best < 0) {
        ucxapp_print("ERROR: no known network found\n");
        return UCXAPP_ERR_FAILED;
    }
    if (strcmp(list[best].name, list[best].ssid) != 0) {
        ucxapp_print("Strongest known network: '%s (%s)' (%d dBm)\n", list[best].name, list[best].ssid, bestRssi);
    } else {
        ucxapp_print("Strongest known network: '%s' (%d dBm)\n", list[best].name, bestRssi);
    }
    return connectProfile(app, list, best, 20, true);
}

static int cmdWifiRegDomain(ucxapp_t *app, int argc, char *argv[])
{
    uWifiRegDomain_t current;
    int dom;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        dom = findRegDomain(argv[1]);
        if (dom < 0) {
            return UCXAPP_ERR_USAGE;
        }
        if (!ucxapp_store_save(REGDOMAIN_FILE, gRegDomains[dom].name, strlen(gRegDomains[dom].name))) {
            ucxapp_print("ERROR: cannot write %s\n", REGDOMAIN_FILE);
            return UCXAPP_ERR_FAILED;
        }
        if (uCxWifiSetRegulatoryDomain(app->ucx, gRegDomains[dom].value) != 0) {
            ucxapp_print("ERROR: module rejected the regulatory domain\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    if (uCxWifiGetRegulatoryDomain(app->ucx, &current) != 0) {
        ucxapp_print("ERROR: cannot read the regulatory domain\n");
        return UCXAPP_ERR_FAILED;
    }
    for (size_t i = 0; i < sizeof(gRegDomains) / sizeof(gRegDomains[0]); i++) {
        if (gRegDomains[i].value == current) {
            ucxapp_print("regdomain=%s\n", gRegDomains[i].name);
            return UCXAPP_OK;
        }
    }
    ucxapp_print("regdomain=%d\n", (int)current);
    return UCXAPP_OK;
}

static int cmdWifiStatus(ucxapp_t *app, int argc, char *argv[])
{
    uCxHandle_t *ucx = app->ucx;
    uCxWifiStationStatus_t status;
    (void)argc;
    (void)argv;

    if (!isConnected(ucx)) {
        ucxapp_print("wifi: disconnected\n");
        return UCXAPP_OK;
    }
    ucxapp_print("wifi: connected\n");
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_SSID, &status)) {
        ucxapp_print("ssid=%s\n", status.rsp.StatusIdStr.ssid != NULL ? status.rsp.StatusIdStr.ssid : "");
    }
    uCxEnd(ucx);
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_RSSI, &status)) {
        ucxapp_print("rssi=%d\n", (int)status.rsp.StatusIdInt.int_val);
    }
    uCxEnd(ucx);
    printIpAddress(ucx);
    return UCXAPP_OK;
}

static int cmdWifiDisconnect(ucxapp_t *app, int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    ucxapp_events_clear(URC_FLAG_NETWORK_DOWN | URC_FLAG_WIFI_LINK_DOWN);
    if (uCxWifiStationDisconnect(app->ucx) != 0) {
        ucxapp_print("ERROR: failed to disconnect\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_events_wait(URC_FLAG_WIFI_LINK_DOWN, 3);
    ucxapp_print("Disconnected\n");
    return UCXAPP_OK;
}

static int cmdWifiApEnable(ucxapp_t *app, int argc, char *argv[])
{
    uCxHandle_t *ucx = app->ucx;
    const char *ssid = argc > 1 ? argv[1] : "u-connectXpressApp AP";
    const char *password = argc > 2 ? argv[2] : "12345678";
    int32_t channel = 6;

    if (argc > 4 || (argc > 3 && (!ucxapp_parse_int(argv[3], &channel) || channel < 1 || channel > 13))) {
        return UCXAPP_ERR_USAGE;
    }
    if (strlen(password) < 8) {
        ucxapp_print("ERROR: password must be at least 8 characters\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxWifiApSetConnectionParams2(ucx, ssid, channel) != 0 ||
        uCxWifiApSetSecurityWpa2(ucx, password, U_WIFI_WPA_VERSION_WPA2) != 0) {
        ucxapp_print("ERROR: failed to configure the access point\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxWifiApActivate(ucx) != 0) {
        ucxapp_print("ERROR: failed to activate AP (disconnect the Wi-Fi station first)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Access point enabled: ssid='%s' channel=%d WPA2\n", ssid, (int)channel);
    ucxapp_qr_print_wifi(ssid, password, "WPA", false);
    return UCXAPP_OK;
}

static int cmdWifiApDisable(ucxapp_t *app, int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (uCxWifiApDeactivate(app->ucx) != 0) {
        ucxapp_print("ERROR: failed to deactivate AP\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Access point disabled\n");
    return UCXAPP_OK;
}

static int cmdWifiApStatus(ucxapp_t *app, int argc, char *argv[])
{
    uCxHandle_t *ucx = app->ucx;
    uCxWifiApGetConnectionParams_t params;
    uCxWifiApGetSecurity_t sec;
    uCxWifiApListNetworkStatus_t net;
    uMacAddress_t mac;
    int count = 0;
    (void)argc;
    (void)argv;

    if (uCxWifiApGetConnectionParamsBegin(ucx, &params)) {
        ucxapp_print("ssid=%s channel=%d\n", params.ssid, (int)params.channel);
    }
    uCxEnd(ucx);
    if (uCxWifiApGetSecurity(ucx, &sec) == 0) {
        if (sec.type == U_CX_WIFI_AP_GET_SECURITY_RSP_TYPE_SECURITY_MODE_WPA_VERSION) {
            ucxapp_print("security: wpa_version=%d mode=%d\n", (int)sec.rsp.SecurityModeWpaVersion.wpa_version,
                         (int)sec.rsp.SecurityModeWpaVersion.security_mode);
        } else if (sec.type == U_CX_WIFI_AP_GET_SECURITY_RSP_TYPE_SECURITY_MODE) {
            ucxapp_print("security: mode=%d\n", (int)sec.rsp.SecurityMode.security_mode);
        }
    }
    uCxWifiApListNetworkStatusBegin(ucx);
    while (uCxWifiApListNetworkStatusGetNext(ucx, &net)) {
        static const char *const names[] = { "ip", "subnet", "gateway", "dns" };
        char str[40];
        if (net.net_status_id >= 0 && net.net_status_id <= 3 &&
            uCxIpAddressToString(&net.net_status_val, str, sizeof(str)) > 0) {
            ucxapp_print("%s=%s\n", names[net.net_status_id], str);
        }
    }
    uCxEnd(ucx);
    uCxWifiApListStationsBegin(ucx);
    while (uCxWifiApListStationsGetNext(ucx, &mac)) {
        ucxapp_print("station %02X:%02X:%02X:%02X:%02X:%02X\n", mac.address[0], mac.address[1], mac.address[2],
                     mac.address[3], mac.address[4], mac.address[5]);
        count++;
    }
    uCxEnd(ucx);
    ucxapp_print("%d station(s)\n", count);
    return UCXAPP_OK;
}

static bool parseIp(const char *s, uSockIpAddress_t *ip)
{
    if (uCxStringToIpAddress(s, ip) != 0) {
        ucxapp_print("ERROR: invalid IP address '%s'\n", s);
        return false;
    }
    return true;
}

static void printIpField(const char *label, uSockIpAddress_t *ip)
{
    char str[40];

    if ((ip->type == U_SOCK_ADDRESS_TYPE_V4 || ip->type == U_SOCK_ADDRESS_TYPE_V6) &&
        uCxIpAddressToString(ip, str, sizeof(str)) > 0) {
        ucxapp_print("%s=%s\n", label, str);
    }
}

static int cmdWifiIp(ucxapp_t *app, int argc, char *argv[])
{
    uCxHandle_t *ucx = app->ucx;
    uSockIpAddress_t ip, mask, gw, dns1, dns2;
    int32_t result;

    if (argc == 1) {
        uCxWifiStationGetIpConfig_t cfg;
        memset(&cfg, 0xFF, sizeof(cfg));   /* a reply without +UWSIP leaves cfg untouched */
        if (uCxWifiStationGetIpConfig(ucx, 0, &cfg) != 0) {
            return UCXAPP_ERR_FAILED;
        }
        if (cfg.type == U_CX_WIFI_STATION_GET_IP_CONFIG_RSP_TYPE_IP_MODE) {
            ucxapp_print("mode=dhcp\n");
        } else {
            ucxapp_print("mode=%s\n", cfg.rsp.IpModeIpIpIpIpIp.ip_mode == U_WIFI_IP_MODE_STATIC ? "static" : "dhcp");
            printIpField("ip", &cfg.rsp.IpModeIpIpIpIpIp.ip_addr);
            printIpField("mask", &cfg.rsp.IpModeIpIpIpIpIp.subnet_mask);
            printIpField("gateway", &cfg.rsp.IpModeIpIpIpIpIp.gateway);
            printIpField("dns1", &cfg.rsp.IpModeIpIpIpIpIp.prim_dns);
            printIpField("dns2", &cfg.rsp.IpModeIpIpIpIpIp.sec_dns);
        }
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "dhcp") == 0) {
        result = uCxWifiStationSetIpConfigDhcp(ucx, 0);
    } else if (argc >= 4 && argc <= 6) {
        if (!parseIp(argv[1], &ip) || !parseIp(argv[2], &mask) || !parseIp(argv[3], &gw) ||
            (argc > 4 && !parseIp(argv[4], &dns1)) || (argc > 5 && !parseIp(argv[5], &dns2))) {
            return UCXAPP_ERR_FAILED;
        }
        if (argc == 4) {
            result = uCxWifiStationSetIpConfigStatic4(ucx, 0, &ip, &mask, &gw);
        } else if (argc == 5) {
            result = uCxWifiStationSetIpConfigStatic5(ucx, 0, &ip, &mask, &gw, &dns1);
        } else {
            result = uCxWifiStationSetIpConfigStatic6(ucx, 0, &ip, &mask, &gw, &dns1, &dns2);
        }
    } else {
        return UCXAPP_ERR_USAGE;
    }
    if (result != 0) {
        ucxapp_print("ERROR: could not set IP configuration (disconnect first?)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("IP configuration set; it applies on the next wifi.connect\n");
    return UCXAPP_OK;
}

static int cmdWifiRssi(ucxapp_t *app, int argc, char *argv[])
{
    int32_t count = 1;
    int32_t intervalS = 1;
    uCxWifiStationStatus_t status;

    if (argc > 3 || (argc > 1 && (!ucxapp_parse_int(argv[1], &count) || count < 1)) ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &intervalS) || intervalS < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    for (int32_t i = 0; i < count; i++) {
        if (i > 0) {
            U_CX_PORT_SLEEP_MS(intervalS * 1000);
        }
        if (uCxWifiStationStatusBegin(app->ucx, U_WIFI_STATUS_ID_RSSI, &status)) {
            ucxapp_print("rssi=%d dBm\n", (int)status.rsp.StatusIdInt.int_val);
            uCxEnd(app->ucx);
        } else {
            uCxEnd(app->ucx);
            ucxapp_print("ERROR: not connected\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    return UCXAPP_OK;
}

static int cmdWifiRoaming(ucxapp_t *app, int argc, char *argv[])
{
    uWifiRoaming_t mode;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2 && uCxWifiSetWifiRoaming(app->ucx, strcmp(argv[1], "on") == 0 ? U_WIFI_ROAMING_ENABLE : U_WIFI_ROAMING_DISABLE) != 0) {
        ucxapp_print("ERROR: roaming not accepted\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxWifiGetWifiRoaming(app->ucx, &mode) != 0) {
        ucxapp_print("ERROR: cannot read the roaming setting\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("roaming=%s\n", mode == U_WIFI_ROAMING_ENABLE ? "on" : "off");
    return UCXAPP_OK;
}

#ifndef UCXAPP_LITE
/* Signal rating for a device that sends telemetry and takes firmware updates over the air. */
static int surveyRating(int avg, int spread, int lost)
{
    int rating = avg >= -55 ? 3 : avg >= -67 ? 2 : avg >= -75 ? 1 : 0;

    if ((spread > 15 || lost > 0) && rating > 0) {
        rating--;
    }
    return rating;
}

/* "Is the Wi-Fi here good enough for my device?": signal over time, stability and the way to the internet. */
static int cmdWifiSurvey(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kRating[] = { "POOR", "FAIR", "GOOD", "EXCELLENT" };
    static const char *const kAdvice[] = {
        "the link will drop: move the device or the access point closer, or check the antenna",
        "fine for small telemetry messages; firmware updates and streaming will be slow",
        "fine for MQTT telemetry, the web UI and firmware updates",
        "strong and steady: fine for streaming and firmware updates",
    };
    int32_t seconds = 10;
    const char *host = "u-blox.com";
    char ssid[64], ip[48], line[128];
    int min = 0, max = -128, sum = 0, got = 0, lost = 0;
    int rating, channel;
    bool internet = true;

    if (argc > 3 || (argc > 1 && (!ucxapp_parse_int(argv[1], &seconds) || seconds < 1 || seconds > 3600))) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc > 2) {
        host = argv[2];
    }
    if (!ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("ERROR: Wi-Fi not connected; join the network first: wifi.connect <ssid> <password>\n");
        return UCXAPP_ERR_FAILED;
    }
    channel = readStatusInt(app->ucx, U_WIFI_STATUS_ID_CHANNEL, 0);
    ucxapp_print("Surveying %s, channel %d (%s GHz), for %d s\n", ssid, channel, bandOf(channel), (int)seconds);
    for (int32_t i = 0; i < seconds; i++) {
        int rssi;

        if (i > 0) {
            U_CX_PORT_SLEEP_MS(1000);
        }
        rssi = readStatusInt(app->ucx, U_WIFI_STATUS_ID_RSSI, 0);
        if (rssi >= 0 || !isConnected(app->ucx)) {
            lost++;
            continue;
        }
        min = got == 0 || rssi < min ? rssi : min;
        max = rssi > max ? rssi : max;
        sum += rssi;
        got++;
    }
    if (got == 0) {
        ucxapp_print("ERROR: no signal readings; the connection dropped\n");
        return UCXAPP_ERR_FAILED;
    }
    rating = surveyRating(sum / got, max - min, lost);
    ucxapp_print("Signal:     %d dBm average (%d to %d)\n", sum / got, min, max);
    ucxapp_print("Stability:  %s (%d dB spread, %d lost reading%s)\n",
                 max - min > 15 || lost > 0 ? "UNSTABLE" : "STABLE", max - min, lost, lost == 1 ? "" : "s");
    if (ucxapp_cmd_find("net.ping") != NULL) {
        ucxapp_print("--- internet (net.ping %s 4)\n", host);
        snprintf(line, sizeof(line), "net.ping %s 4", host);
        internet = ucxapp_cmd_exec(app, line) == UCXAPP_OK;
        ucxapp_print("Internet:   %s\n", internet ? "REACHABLE" : "NOT REACHABLE (the access point has no internet, or blocks ping)");
    }
    ucxapp_print("Verdict:    %s - %s\n", kRating[rating], kAdvice[rating]);
    return internet ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}
#endif

static const ucxapp_cmd_t gWifiCmds[] = {
    { "wifi.scan",       "",                    "Scan for networks: strongest first with band, signal and security, channel usage and a quiet channel for an AP", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiScan },
    { "wifi.connect",    "[ssid|n] [password|-]", "Connect by SSID or number from wifi.scan; '-' prompts for the password; a saved profile supplies the password; with no argument: the best known network (same as wifi.profile.auto)", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiConnect },
#ifndef UCXAPP_LITE
    { "wifi.eap",        "<ssid> <ca> <cert> <key> <identity> [1.2|1.3|any]", "Connect with WPA2/WPA3-Enterprise EAP-TLS using certificates on the module (cert.list)", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiEap },
    { "wifi.peap",       "<ssid> <user> <password|-> <ca> [1.2|1.3|any]", "Connect with WPA2/WPA3-Enterprise PEAP (MSCHAPv2); the CA verifies the server", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiPeap },
#endif
    { "wifi.disconnect", "",                    "Disconnect from Wi-Fi",            UCXAPP_CMD_NEEDS_DEVICE, cmdWifiDisconnect },
    { "wifi.status",     "",                    "Show connection, SSID, RSSI, IP",  UCXAPP_CMD_NEEDS_DEVICE, cmdWifiStatus },
    { "wifi.rssi",       "[count] [interval_s]", "Read the signal strength (repeatedly)", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiRssi },
#ifndef UCXAPP_LITE
    { "wifi.survey",     "[seconds] [host]",    "Is the Wi-Fi here good enough? Signal and stability over time (default 10 s), internet reachability, and a verdict with advice", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiSurvey },
#endif
    { "wifi.ip",         "[dhcp | <ip> <mask> <gw> [dns1 [dns2]]]", "Show or set DHCP / static IP (applies on next connect)", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiIp },
    { "wifi.profile.add", "<name> <ssid> [password]", "Save or update a Wi-Fi profile", 0, cmdProfileAdd },
    { "wifi.profile.list", "",                   "List saved Wi-Fi profiles",        0, cmdProfileList },
    { "wifi.profile.remove", "<name>",           "Delete a saved Wi-Fi profile",     UCXAPP_CMD_DANGER, cmdProfileRemove },
    { "wifi.profile.connect", "<name>",          "Connect using a saved Wi-Fi profile", UCXAPP_CMD_NEEDS_DEVICE, cmdProfileConnect },
    { "wifi.profile.auto", "",                   "Connect: the last used profile, else scan and pick the strongest known", UCXAPP_CMD_NEEDS_DEVICE, cmdProfileAuto },
    { "wifi.regdomain",  "[world|etsi|fcc|ic|nz|mkk|ncc|acma|kcc|sa|br]", "Show or set (and save) the regulatory domain", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiRegDomain },
    { "wifi.roaming",    "[on|off]",            "Show or set Wi-Fi roaming between access points", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiRoaming },
    { "wifi.ap.enable",  "[ssid] [password] [channel]", "Start a WPA2 access point and show its join QR code", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiApEnable },
    { "wifi.ap.disable", "",                    "Stop the access point",            UCXAPP_CMD_NEEDS_DEVICE, cmdWifiApDisable },
    { "wifi.ap.status",  "",                    "Show AP parameters, addresses and stations", UCXAPP_CMD_NEEDS_DEVICE, cmdWifiApStatus },
};

void ucxapp_register_wifi_cmds(void)
{
    ucxapp_cmd_register(gWifiCmds, sizeof(gWifiCmds) / sizeof(gWifiCmds[0]));
}
