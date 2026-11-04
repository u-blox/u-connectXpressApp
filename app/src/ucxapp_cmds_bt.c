#include "ucxapp_cmd.h"

#include "ucxapp_adv.h"
#include "ucxapp_store.h"
#include "ucxapp_ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_channel_sounding.h"
#include "u_cx_channel_sounding_initiator.h"
#include "u_cx_error_codes.h"
#include "u_cx_system.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef BT_SCAN_MAX_DEVICES
#define BT_SCAN_MAX_DEVICES 32
#endif
#define BT_SCAN_TIMEOUT_MS  30000

#define BT_ADV_MAX          64
#define BT_FILTER_SLOTS     10   /* discovery data filter index 0..9 */
#define BT_FILTER_MAX       10   /* bytes per filter pattern */
#define BT_NAME_FILTER_SLOT 9    /* bt.connect <name> borrows this slot and restores it */
#define BT_AD_COMPLETE_NAME 9

typedef struct {
    uBtLeAddress_t addr;
    int32_t rssi;
    char name[32];
    uint8_t adv[BT_ADV_MAX];   /* advertising data plus scan response, as far as it fits */
    uint8_t advLen;
} scanEntry_t;

/* Results of the last scan, so bt.connect can take a number or a name. */
static scanEntry_t gLastScan[BT_SCAN_MAX_DEVICES];
static int gLastScanCount;

static void fmtAddr(const uBtLeAddress_t *a, char *out, size_t size)
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X%s", a->address[0], a->address[1], a->address[2],
             a->address[3], a->address[4], a->address[5], a->type == U_BD_ADDRESS_TYPE_RANDOM ? ",random" : "");
}

static void printAddr(const uBtLeAddress_t *a)
{
    char text[32];

    fmtAddr(a, text, sizeof(text));
    ucxapp_print("%s", text);
}

static int hexVal(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "XX:XX:XX:XX:XX:XX" or "XX:XX:XX:XX:XX:XX,random"; public if no type is given. Parsed by hand: sscanf links
 * the floating-point scanf on picolibc, too big for the 128 KB lite builds. */
static bool parseAddr(const char *s, uBtLeAddress_t *addr)
{
    for (int i = 0; i < 6; i++) {
        int hi = hexVal(*s);
        int lo;

        if (hi < 0) {
            return false;
        }
        lo = hexVal(*++s);
        if (lo >= 0) {
            hi = hi * 16 + lo;
            s++;
        }
        addr->address[i] = (uint8_t)hi;
        if (i < 5 && *s++ != ':') {
            return false;
        }
    }
    addr->type = U_BD_ADDRESS_TYPE_PUBLIC;
    if (*s == ',') {
        if (strcmp(s + 1, "random") == 0) {
            addr->type = U_BD_ADDRESS_TYPE_RANDOM;
        } else if (strcmp(s + 1, "public") != 0) {
            return false;
        }
    } else if (*s != '\0') {
        return false;
    }
    return true;
}

static int hexDigit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "4E494E41", "4E:49:4E:41" or "4E-49" into bytes; -1 on an odd digit count, a bad character or more than max. */
static int parseHexBytes(const char *s, uint8_t *out, int max)
{
    int n = 0;

    while (*s != '\0') {
        if (*s == ':' || *s == '-') {
            s++;
            continue;
        }
        int hi = hexDigit(s[0]);
        int lo = hi < 0 ? -1 : hexDigit(s[1]);
        if (lo < 0 || n == max) {
            return -1;
        }
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return n;
}

#ifndef UCXAPP_LITE   /* bt.filter: not in the 128 KB lite builds */
/* A filter pattern: hex bytes ("4E494E41", "0x4E494E41", "4E:49") or else the text itself ("NORA"). */
static int parsePattern(const char *s, uint8_t *out, int max)
{
    const char *hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? s + 2 : s;
    int n = parseHexBytes(hex, out, max);
    size_t len = strlen(s);

    if (n > 0 || hex != s) {
        return n;
    }
    if (len == 0 || len > (size_t)max) {
        return -1;
    }
    memcpy(out, s, len);
    return (int)len;
}
#endif

static bool containsChunk(const uint8_t *buf, size_t len, const uint8_t *chunk, size_t clen)
{
    for (size_t i = 0; i + clen <= len; i++) {
        if (memcmp(buf + i, chunk, clen) == 0) {
            return true;
        }
    }
    return false;
}

/* Named devices first, then the strongest signal. */
static int compareScan(const void *pa, const void *pb)
{
    const scanEntry_t *a = (const scanEntry_t *)pa;
    const scanEntry_t *b = (const scanEntry_t *)pb;
    int aNamed = a->name[0] != '\0';
    int bNamed = b->name[0] != '\0';

    if (aNamed != bNamed) {
        return bNamed - aNamed;
    }
    return (int)(b->rssi - a->rssi);
}

/* One discovery report; filtered reports only devices matching a data filter (bt.filter). */
static bool nextDevice(ucxapp_t *app, bool filtered, uCxBtDiscovery_t *dev)
{
    uCxBtDiscoveryDefault_t d;

    if (filtered) {
        return uCxBluetoothDiscovery2GetNext(app->ucx, dev);
    }
    if (!uCxBluetoothDiscoveryDefaultGetNext(app->ucx, &d)) {
        return false;
    }
    dev->bd_addr = d.bd_addr;
    dev->rssi = d.rssi;
    dev->device_name = d.device_name;
    dev->data_type = d.data_type;
    dev->data = d.data;
    return true;
}

/* Fills gLastScan: one entry per address with the best RSSI, the longest name and the merged AD data. */
static int scanDevices(ucxapp_t *app, bool filtered)
{
    int count = 0;
    uCxBtDiscovery_t dev;

    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, BT_SCAN_TIMEOUT_MS, false);
    if (filtered) {
        uCxBluetoothDiscovery2Begin(app->ucx, U_BT_DISCOVERY_TYPE_DATA_FILTER, U_BT_DISCOVERY_MODE_ACTIVE);
    } else {
        uCxBluetoothDiscoveryDefaultBegin(app->ucx);
    }
    while (nextDevice(app, filtered, &dev)) {
        int i = 0;
        scanEntry_t *e;

        while (i < count && !(memcmp(gLastScan[i].addr.address, dev.bd_addr.address, 6) == 0 &&
                              gLastScan[i].addr.type == dev.bd_addr.type)) {
            i++;
        }
        if (i == count) {
            if (count == BT_SCAN_MAX_DEVICES) {
                /* List full: a nearby device replaces the weakest one, so it is never lost in a busy room. */
                int weakest = 0;
                for (int j = 1; j < count; j++) {
                    if (gLastScan[j].rssi < gLastScan[weakest].rssi) {
                        weakest = j;
                    }
                }
                if (dev.rssi <= gLastScan[weakest].rssi) {
                    continue;
                }
                i = weakest;
            } else {
                count++;
            }
            memset(&gLastScan[i], 0, sizeof(gLastScan[i]));
            gLastScan[i].addr = dev.bd_addr;
            gLastScan[i].rssi = dev.rssi;
        }
        e = &gLastScan[i];
        if (dev.rssi > e->rssi) {
            e->rssi = dev.rssi;
        }
        if (dev.device_name != NULL && strlen(dev.device_name) > strlen(e->name)) {
            snprintf(e->name, sizeof(e->name), "%.*s", (int)(sizeof(e->name) - 1), dev.device_name);
        }
        if (dev.data.pData != NULL && dev.data.length > 0 &&
            !containsChunk(e->adv, e->advLen, dev.data.pData, dev.data.length < BT_ADV_MAX ? dev.data.length : BT_ADV_MAX)) {
            size_t room = BT_ADV_MAX - e->advLen;
            size_t take = dev.data.length < room ? dev.data.length : room;

            memcpy(e->adv + e->advLen, dev.data.pData, take);
            e->advLen = (uint8_t)(e->advLen + take);
        }
    }
    uCxEnd(app->ucx);
    for (int i = 0; i < count; i++) {
        if (gLastScan[i].name[0] == '\0') {
            ucxapp_adv_local_name(gLastScan[i].adv, gLastScan[i].advLen, gLastScan[i].name, sizeof(gLastScan[i].name));
        }
    }
    qsort(gLastScan, (size_t)count, sizeof(gLastScan[0]), compareScan);
    gLastScanCount = count;
    return count;
}

/* Filtered discovery for a device name: the first BT_FILTER_MAX bytes on the Complete Local Name, in
 * BT_NAME_FILTER_SLOT, which is restored afterwards. The module compares bytes, so the case must match.
 * Returns the device count, -1 when the filter could not be set. */
static int scanByName(ucxapp_t *app, const char *name)
{
    uCxBtGetDiscoveryDataFilter_t old;
    uint8_t saved[BT_FILTER_MAX];
    int32_t savedType = 0;
    int32_t savedLen = 0;
    size_t len = strlen(name);

    if (uCxBluetoothGetDiscoveryDataFilterBegin(app->ucx, BT_NAME_FILTER_SLOT, &old) && old.data_filter.pData != NULL) {
        savedType = old.ad_type;
        savedLen = old.data_filter.length < BT_FILTER_MAX ? (int32_t)old.data_filter.length : BT_FILTER_MAX;
        memcpy(saved, old.data_filter.pData, (size_t)savedLen);
    }
    uCxEnd(app->ucx);
    if (len > BT_FILTER_MAX) {
        len = BT_FILTER_MAX;
    }
    int32_t r = uCxBluetoothSetDiscoveryDataFilter(app->ucx, BT_NAME_FILTER_SLOT, BT_AD_COMPLETE_NAME,
                                                   (const uint8_t *)name, (int32_t)len);
    if (r != 0) {
        ucxapp_print("ERROR: cannot set the name filter (%d%s)\n", (int)r, ucxapp_errname(r));
        return -1;
    }
    ucxapp_print("scanning for \"%.*s\"...\n", (int)len, name);
    int count = scanDevices(app, true);
    (void)uCxBluetoothSetDiscoveryDataFilter(app->ucx, BT_NAME_FILTER_SLOT, savedType, saved, savedLen);
    return count;
}

static int cmdBtScan(ucxapp_t *app, int argc, char *argv[])
{
    bool full = false;
    bool filtered = false;
    bool seenNamed = false;
    bool separated = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "full") == 0 && !full) {
            full = true;
        } else if (strcmp(argv[i], "filter") == 0 && !filtered) {
            filtered = true;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    ucxapp_print(filtered ? "scanning (only devices matching bt.filter)...\n" : "scanning...\n");
    int count = scanDevices(app, filtered);
    for (int i = 0; i < count; i++) {
        const scanEntry_t *e = &gLastScan[i];
        char summary[96], addr[32];

        seenNamed = seenNamed || e->name[0] != '\0';
        if (seenNamed && !separated && e->name[0] == '\0') {
            ucxapp_print("-- devices without a name --\n");
            separated = true;
        }
        ucxapp_adv_summary(e->adv, e->advLen, summary, sizeof(summary));
        fmtAddr(&e->addr, addr, sizeof(addr));
        ucxapp_print("%2d  %-24s %4d dBm  %-20s %s\n", i + 1, addr, (int)e->rssi, e->name, summary);
        if (full) {
            ucxapp_adv_print(e->adv, e->advLen, "        ");
        }
    }
    ucxapp_print("%d device(s)%s\n", count, count > 0 ? "; connect with bt.connect <number|name|address>" : "");
    return UCXAPP_OK;
}

#define BT_LAST_FILE  UCXAPP_BLE_LAST_FILE

/* Starts a connect and waits until the link is listed; *up tells whether it came up. */
static void centralSecurity(ucxapp_t *app);

static bool linkListed(ucxapp_t *app, int32_t handle)
{
    uCxBtListConnections_t conn;
    bool found = false;

    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &conn)) {
        found = found || conn.conn_handle == handle;
    }
    uCxEnd(app->ucx);
    return found;
}

/* A connect request the peer never received still reports a link, which the controller drops after six
 * connection intervals (about 250 ms); wait past that and connect again, so no command starts on a dead link. */
#define CONNECT_SETTLE_MS 500
#define CONNECT_ATTEMPTS  3

static int32_t connectAddr(ucxapp_t *app, uBtLeAddress_t *addr, int waitTenthsS, bool *up)
{
    uCxBtListConnections_t existing;
    char want[32], have[32];
    int32_t known = -1;

    fmtAddr(addr, want, sizeof(want));
    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &existing)) {
        fmtAddr(&existing.bd_addr, have, sizeof(have));
        if (strcmp(want, have) == 0) {
            known = existing.conn_handle;
        }
    }
    uCxEnd(app->ucx);
    if (known >= 0) {
        ucxapp_print("already connected to %s\n", want);
        ucxapp_handle_add(UCXAPP_HANDLE_CONN, known);
        *up = true;
        return known;
    }
    centralSecurity(app);
    for (int attempt = 1;; attempt++) {
        int32_t handle = uCxBluetoothConnect(app->ucx, addr);

        *up = false;
        for (int tries = 0; handle >= 0 && !*up && tries < waitTenthsS; tries++) {
            *up = linkListed(app, handle);
            if (!*up) {
                U_CX_PORT_SLEEP_MS(100);
            }
        }
        if (!*up) {
            return handle;
        }
        U_CX_PORT_SLEEP_MS(CONNECT_SETTLE_MS);
        if (linkListed(app, handle)) {
            ucxapp_handle_add(UCXAPP_HANDLE_CONN, handle);
            return handle;
        }
        ucxapp_handle_remove(UCXAPP_HANDLE_CONN, handle);
        *up = false;
        if (attempt == CONNECT_ATTEMPTS) {
            ucxapp_print("link to %s dropped right after connecting %d times (weak signal or the peer stopped "
                         "advertising)\n", want, CONNECT_ATTEMPTS);
            return U_ERROR_COMMON_NOT_CONNECTED;
        }
        ucxapp_print("link dropped right after connecting (peer did not answer); retrying (%d/%d)\n", attempt + 1,
                     CONNECT_ATTEMPTS);
    }
}

static void rememberLast(const char *addrText)
{
    ucxapp_store_save(BT_LAST_FILE, addrText, strlen(addrText));
}

/* ---- saved profiles: one "name<TAB>address" line each ---- */

#define BT_PROFILES_FILE  "ucxapp.ble"
#define BT_MAX_PROFILES   16
#define BT_NAME_LEN       32
#define BT_ADDR_LEN       24
#define BT_FIRST_TRY_TENTHS 100

typedef struct {
    char name[BT_NAME_LEN];
    char addr[BT_ADDR_LEN];
} btProfile_t;

static int loadBtProfiles(btProfile_t *list)
{
    size_t len = 0;
    uint8_t *data = ucxapp_store_load(BT_PROFILES_FILE, &len);
    char *line = (char *)data;
    int count = 0;

    if (data == NULL) {
        return 0;
    }
    while (*line != '\0' && count < BT_MAX_PROFILES) {
        char *next = line + strcspn(line, "\r\n");
        char *tab;

        while (*next == '\r' || *next == '\n') {
            *next++ = '\0';
        }
        tab = strchr(line, '\t');
        if (tab != NULL) {
            *tab = '\0';
            if (strlen(line) < BT_NAME_LEN && strlen(tab + 1) < BT_ADDR_LEN) {
                snprintf(list[count].name, sizeof(list[count].name), "%s", line);
                snprintf(list[count].addr, sizeof(list[count].addr), "%s", tab + 1);
                count++;
            }
        }
        line = next;
    }
    free(data);
    return count;
}

static bool saveBtProfiles(const btProfile_t *list, int count)
{
    char *text = (char *)calloc((size_t)count + 1, BT_NAME_LEN + BT_ADDR_LEN + 3);
    bool ok;

    if (text == NULL) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        size_t used = strlen(text);

        snprintf(text + used, BT_NAME_LEN + BT_ADDR_LEN + 3, "%s\t%s\n", list[i].name, list[i].addr);
    }
    ok = ucxapp_store_save(BT_PROFILES_FILE, text, strlen(text));
    free(text);
    return ok;
}

static int findBtProfile(const btProfile_t *list, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool sameAddr(const uBtLeAddress_t *a, const uBtLeAddress_t *b)
{
    return memcmp(a->address, b->address, 6) == 0 && a->type == b->type;
}

static bool ciEqual(const char *a, const char *b)
{
    for (; *a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {
    }
    return *a == *b;
}

static bool ciPrefix(const char *text, const char *prefix)
{
    for (; *prefix != '\0' && tolower((unsigned char)*text) == tolower((unsigned char)*prefix); text++, prefix++) {
    }
    return *prefix == '\0';
}

/* Scanned devices whose name is s (case-insensitive) or starts with it; *match is the last hit, or the exact one. */
static int matchScanName(const char *s, int *match)
{
    int matches = 0;

    for (int i = 0; i < gLastScanCount; i++) {
        if (ciEqual(gLastScan[i].name, s)) {
            *match = i;
            return 1;
        }
        if (s[0] != '\0' && ciPrefix(gLastScan[i].name, s)) {
            *match = i;
            matches++;
        }
    }
    return matches;
}

/* Address, scan number, saved profile name, or a device name (whole or the start of it): from the last scan,
 * else from a filtered scan for that name. */
static bool resolveTarget(ucxapp_t *app, const char *s, uBtLeAddress_t *addr, char *label, size_t labelSize)
{
    btProfile_t profiles[BT_MAX_PROFILES];
    int match = -1;
    int matches;

    label[0] = '\0';
    if (parseAddr(s, addr)) {
        return true;
    }
    if (s[0] != '\0' && strspn(s, "0123456789") == strlen(s)) {
        int n = atoi(s);

        if (gLastScanCount == 0) {
            ucxapp_print("ERROR: no scan results yet (run bt.scan first)\n");
        } else if (n < 1 || n > gLastScanCount) {
            ucxapp_print("ERROR: no device %d in the last scan (1-%d)\n", n, gLastScanCount);
        } else {
            *addr = gLastScan[n - 1].addr;
            snprintf(label, labelSize, "%s", gLastScan[n - 1].name);
            return true;
        }
        return false;
    }
    int count = loadBtProfiles(profiles);
    int idx = findBtProfile(profiles, count, s);

    if (idx >= 0 && parseAddr(profiles[idx].addr, addr)) {
        snprintf(label, labelSize, "%s", profiles[idx].name);
        return true;
    }
    matches = matchScanName(s, &match);
    if (matches == 0 && s[0] != '\0' && scanByName(app, s) > 0) {
        matches = matchScanName(s, &match);
    }
    if (matches == 1) {
        *addr = gLastScan[match].addr;
        snprintf(label, labelSize, "%s", gLastScan[match].name);
        return true;
    }
    if (matches > 1) {
        ucxapp_print("ERROR: '%s' matches %d scanned devices; use the number from bt.scan\n", s, matches);
    } else {
        ucxapp_print("ERROR: '%s' is not an address, a scan number, a profile or the name of a device in range\n", s);
    }
    return false;
}

static bool addrIsSaved(const uBtLeAddress_t *addr)
{
    btProfile_t profiles[BT_MAX_PROFILES];
    int count = loadBtProfiles(profiles);

    for (int i = 0; i < count; i++) {
        uBtLeAddress_t saved;

        if (parseAddr(profiles[i].addr, &saved) && sameAddr(&saved, addr)) {
            return true;
        }
    }
    return false;
}

static int cmdBtProfileAuto(ucxapp_t *app, int argc, char *argv[]);

static int cmdBtConnect(ucxapp_t *app, int argc, char *argv[])
{
    uBtLeAddress_t addr;
    char label[32], text[32];
    bool up;

    if (argc == 1) {
        return cmdBtProfileAuto(app, argc, argv);
    }
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!resolveTarget(app, argv[1], &addr, label, sizeof(label))) {
        return UCXAPP_ERR_FAILED;
    }
    fmtAddr(&addr, text, sizeof(text));
    if (label[0] != '\0') {
        ucxapp_print("connecting to %s (%s)\n", label, text);
    } else {
        ucxapp_print("connecting to %s\n", text);
    }
    int32_t handle = connectAddr(app, &addr, 50, &up);
    if (handle < 0) {
        ucxapp_print("ERROR: connect failed (%d%s)\n", (int)handle, ucxapp_errname(handle));
        return UCXAPP_ERR_FAILED;
    }
    if (up) {
        rememberLast(text);
    }
    ucxapp_print("connection %d (current)\n", (int)handle);
    if (up && label[0] != '\0' && strchr(label, '"') == NULL && !addrIsSaved(&addr)) {
        ucxapp_print("tip: save it with bt.profile.add \"%s\" %s\n", label, text);
    }
    return UCXAPP_OK;
}

static int connectBtProfile(ucxapp_t *app, const btProfile_t *p, int waitTenthsS)
{
    uBtLeAddress_t addr;
    bool up;

    if (!parseAddr(p->addr, &addr)) {
        ucxapp_print("ERROR: bad address in profile '%s'\n", p->name);
        return UCXAPP_ERR_FAILED;
    }
    int32_t handle = connectAddr(app, &addr, waitTenthsS, &up);
    if (!up) {
        if (handle >= 0) {
            uCxBluetoothDisconnect(app->ucx, handle);
        }
        ucxapp_print("ERROR: could not connect to '%s'\n", p->name);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_store_save(BT_LAST_FILE, p->name, strlen(p->name));
    ucxapp_print("connection %d (profile '%s')\n", (int)handle, p->name);
    return UCXAPP_OK;
}

static int cmdBtProfileAdd(ucxapp_t *app, int argc, char *argv[])
{
    btProfile_t list[BT_MAX_PROFILES];
    uBtLeAddress_t addr;
    int count;
    int idx;

    (void)app;
    if (argc != 3 || argv[1][0] == '\0' || strlen(argv[1]) >= BT_NAME_LEN || strpbrk(argv[1], "\t\r\n") != NULL ||
        strlen(argv[2]) >= BT_ADDR_LEN || !parseAddr(argv[2], &addr)) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadBtProfiles(list);
    idx = findBtProfile(list, count, argv[1]);
    if (idx < 0) {
        if (count >= BT_MAX_PROFILES) {
            ucxapp_print("ERROR: at most %d profiles\n", BT_MAX_PROFILES);
            return UCXAPP_ERR_FAILED;
        }
        idx = count++;
    }
    snprintf(list[idx].name, sizeof(list[idx].name), "%s", argv[1]);
    snprintf(list[idx].addr, sizeof(list[idx].addr), "%s", argv[2]);
    if (!saveBtProfiles(list, count)) {
        ucxapp_print("ERROR: cannot write %s\n", BT_PROFILES_FILE);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("profile '%s' saved (%s)\n", argv[1], argv[2]);
    return UCXAPP_OK;
}

static int cmdBtProfileList(ucxapp_t *app, int argc, char *argv[])
{
    btProfile_t list[BT_MAX_PROFILES];
    int count = loadBtProfiles(list);

    (void)app;
    (void)argc;
    (void)argv;
    for (int i = 0; i < count; i++) {
        ucxapp_print("%s: %s\n", list[i].name, list[i].addr);
    }
    ucxapp_print("%d profile(s)\n", count);
    return UCXAPP_OK;
}

static int cmdBtProfileRemove(ucxapp_t *app, int argc, char *argv[])
{
    btProfile_t list[BT_MAX_PROFILES];
    int count;
    int idx;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadBtProfiles(list);
    idx = findBtProfile(list, count, argv[1]);
    if (idx < 0) {
        ucxapp_print("ERROR: no profile '%s'\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    list[idx] = list[--count];
    if (!saveBtProfiles(list, count)) {
        ucxapp_print("ERROR: cannot write %s\n", BT_PROFILES_FILE);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("profile '%s' removed\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdBtProfileConnect(ucxapp_t *app, int argc, char *argv[])
{
    btProfile_t list[BT_MAX_PROFILES];
    int count;
    int idx;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    count = loadBtProfiles(list);
    idx = findBtProfile(list, count, argv[1]);
    if (idx < 0) {
        ucxapp_print("ERROR: no profile '%s' (see bt.profile.list)\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    return connectBtProfile(app, &list[idx], 100);
}

/* Last used profile first; if that fails, scan and take the strongest known device. */
static int cmdBtProfileAuto(ucxapp_t *app, int argc, char *argv[])
{
    btProfile_t list[BT_MAX_PROFILES];
    int count = loadBtProfiles(list);
    int best = -1;
    int32_t bestRssi = -1000;
    size_t len = 0;
    uint8_t *last;

    (void)argc;
    (void)argv;
    if (count == 0) {
        ucxapp_print("ERROR: no profiles (see bt.profile.add)\n");
        return UCXAPP_ERR_FAILED;
    }
    last = ucxapp_store_load(BT_LAST_FILE, &len);
    if (last != NULL) {
        int idx;

        last[strcspn((char *)last, "\r\n")] = '\0';
        idx = findBtProfile(list, count, (char *)last);
        free(last);
        if (idx >= 0) {
            ucxapp_print("Trying last used profile '%s'...\n", list[idx].name);
            if (connectBtProfile(app, &list[idx], BT_FIRST_TRY_TENTHS) == UCXAPP_OK) {
                return UCXAPP_OK;
            }
        }
    }

    ucxapp_print("Scanning for known devices...\n");
    int n = scanDevices(app, false);
    for (int i = 0; i < n; i++) {
        for (int p = 0; p < count; p++) {
            uBtLeAddress_t addr;

            if (parseAddr(list[p].addr, &addr) && sameAddr(&addr, &gLastScan[i].addr) && gLastScan[i].rssi > bestRssi) {
                best = p;
                bestRssi = gLastScan[i].rssi;
            }
        }
    }
    if (best < 0) {
        ucxapp_print("ERROR: no known device found\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Strongest known device: '%s' (%d dBm)\n", list[best].name, (int)bestRssi);
    return connectBtProfile(app, &list[best], BT_FIRST_TRY_TENTHS);
}

static int cmdBtDisconnect(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;

    if (argc != 2 || !ucxapp_parse_int(argv[1], &handle) || handle < 0) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t result = uCxBluetoothDisconnect(app->ucx, handle);
    if (result != 0) {
        ucxapp_print("ERROR: disconnect failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_handle_remove(UCXAPP_HANDLE_CONN, handle);
    ucxapp_print("connection %d closed\n", (int)handle);
    return UCXAPP_OK;
}

static int cmdBtList(ucxapp_t *app, int argc, char *argv[])
{
    uCxBtListConnections_t conn;
    int count = 0;
    int32_t cur = ucxapp_handle_current(UCXAPP_HANDLE_CONN);
    (void)argc;
    (void)argv;

    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &conn)) {
        count++;
        ucxapp_print("%cconnection %d: ", conn.conn_handle == cur ? '*' : ' ', (int)conn.conn_handle);
        printAddr(&conn.bd_addr);
        ucxapp_print("\n");
    }
    uCxEnd(app->ucx);
    ucxapp_print("%d connection(s)\n", count);
    return UCXAPP_OK;
}

/* ---- security: pairing events and replies ------------------------------- */

static bool gAutoConfirm = true;
static volatile bool gConfirmPending;
static volatile bool gPasskeyPending;
static uBtLeAddress_t gPendingAddr;
static bool gDefaultsApplied;
static volatile int32_t gBondResult = -1;   /* last bond status from the module, -1 while waiting */

void ucxapp_bt_bond_status(const uBtLeAddress_t *addr, int32_t status)
{
    if (addr == NULL) {
        return;
    }
    gBondResult = status;
    ucxapp_print("[BT] bonding %s: ", status == 0 ? "succeeded" : "FAILED");
    printAddr(addr);
    if (status != 0) {
        ucxapp_print(" (status %d)", (int)status);
    }
    ucxapp_print("\n");
    if (status == U_BT_BOND_STATUS_BONDING_FAILED_AUTH) {
        ucxapp_print("[BT] the peer rejected pairing; if it shows a code, enter it with bt.passkey; to retry run bt.unbond <addr> first\n");
    } else if (status == U_BT_BOND_STATUS_BONDING_FAILED_PEER_LOST_BOND) {
        ucxapp_print("[BT] stale bond: one side still has keys the other forgot; run bt.unbond <addr> here and/or "
                     "remove this module from the peer's Bluetooth settings (e.g. Windows: Bluetooth & devices), "
                     "then connect again\n");
    } else if (status == U_BT_BOND_STATUS_BONDING_FAILED_MITM) {
        ucxapp_print("[BT] MITM protection not possible with these I/O capabilities: try bt.iocap keydisplay\n");
    }
}

/* Keyboard+display lets the module answer any pairing method (passkey either way, numeric comparison, Just Works). */
static void centralSecurity(ucxapp_t *app)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    if (uCxBluetoothSetIoCapabilities(app->ucx, U_BT_IO_CAP_KEYBOARD_DISPLAY) != 0 ||
        uCxBluetoothSetPairingMode(app->ucx, U_BT_PAIRING_MODE_ENABLE) != 0) {
        ucxapp_print("WARNING: could not set up pairing for outgoing connections\n");
    }
    gDefaultsApplied = true;
}

void ucxapp_bt_user_confirmation(uCxHandle_t *ucx, const uBtLeAddress_t *addr, int32_t value)
{
    if (addr == NULL) {
        return;
    }
    gPendingAddr = *addr;
    ucxapp_print("[BT] pairing request from ");
    printAddr(addr);
    ucxapp_print(", code %06d\n", (int)value);
    if (gAutoConfirm) {
        ucxapp_print("[BT] accepting (bt.autoconfirm off to decide with bt.confirm)\n");
        if (uCxBluetoothUserConfirmation(ucx, &gPendingAddr, U_BT_CONFIRM_YES) != 0) {
            ucxapp_print("[BT] ERROR: could not confirm pairing\n");
        }
    } else {
        gConfirmPending = true;
        ucxapp_print("[BT] answer with bt.confirm yes|no\n");
        {
            char text[96];

            snprintf(text, sizeof(text), "PAIRING REQUEST, code %06d: Enter accepts, Esc rejects", (int)value);
            ucxapp_ui_prompt(text, "bt.confirm yes", "bt.confirm no");
        }
    }
}

void ucxapp_bt_passkey_display(const uBtLeAddress_t *addr, int32_t passkey)
{
    if (addr == NULL) {
        return;
    }
    ucxapp_print("[BT] enter passkey %06d on the remote device ", (int)passkey);
    printAddr(addr);
    ucxapp_print("\n");
}

void ucxapp_bt_passkey_request(const uBtLeAddress_t *addr)
{
    if (addr == NULL) {
        return;
    }
    gPendingAddr = *addr;
    gPasskeyPending = true;
    ucxapp_print("[BT] passkey requested for ");
    printAddr(addr);
    ucxapp_print("; answer with bt.passkey <digits> or bt.passkey deny\n");
    ucxapp_ui_prompt("PASSKEY REQUEST: type the 6 digits shown on the remote, Enter sends, Esc denies",
                     "bt.passkey ", "bt.passkey deny");
}

/* Just Works: no I/O, unauthenticated bonding, pairing on. Applied once per run before the first server use. */
void ucxapp_bt_security_defaults(ucxapp_t *app)
{
    if (gDefaultsApplied) {
        return;
    }
    gDefaultsApplied = true;
    if (uCxBluetoothSetIoCapabilities(app->ucx, U_BT_IO_CAP_NO_INPUT_NO_OUTPUT) != 0 ||
        uCxBluetoothSetSecurityMode(app->ucx, U_BT_SECURITY_MODE_NONE) != 0 ||
        uCxBluetoothSetPairingMode(app->ucx, U_BT_PAIRING_MODE_ENABLE) != 0) {
        ucxapp_print("WARNING: could not apply the Just Works pairing defaults\n");
    }
}

static bool parseOnOff(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "off") == 0) {
        *out = s[1] == 'n';
        return true;
    }
    return false;
}

static int cmdBtPairing(ucxapp_t *app, int argc, char *argv[])
{
    bool on;

    if (argc != 2 || !parseOnOff(argv[1], &on)) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = uCxBluetoothSetPairingMode(app->ucx, on ? U_BT_PAIRING_MODE_ENABLE : U_BT_PAIRING_MODE_DISABLE);
    if (r != 0) {
        ucxapp_print("ERROR: set pairing mode failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("pairing %s\n", on ? "enabled" : "disabled");
    return UCXAPP_OK;
}

static int cmdBtIoCap(ucxapp_t *app, int argc, char *argv[])
{
    static const struct { const char *name; uBtIoCap_t cap; } kCaps[] = {
        { "none", U_BT_IO_CAP_NO_INPUT_NO_OUTPUT }, { "display", U_BT_IO_CAP_DISPLAY_ONLY },
        { "yesno", U_BT_IO_CAP_DISPLAY_YES_NO }, { "keyboard", U_BT_IO_CAP_KEYBOARD_ONLY },
        { "keydisplay", U_BT_IO_CAP_KEYBOARD_DISPLAY },
    };

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < sizeof(kCaps) / sizeof(kCaps[0]); i++) {
        if (strcmp(argv[1], kCaps[i].name) == 0) {
            int32_t r = uCxBluetoothSetIoCapabilities(app->ucx, kCaps[i].cap);
            if (r != 0) {
                ucxapp_print("ERROR: set I/O capabilities failed (%d%s)\n", (int)r, ucxapp_errname(r));
                return UCXAPP_ERR_FAILED;
            }
            ucxapp_print("I/O capabilities: %s\n", kCaps[i].name);
            return UCXAPP_OK;
        }
    }
    return UCXAPP_ERR_USAGE;
}

static int cmdBtSecurity(ucxapp_t *app, int argc, char *argv[])
{
    static const struct { const char *name; uBtSecurityMode_t mode; } kModes[] = {
        { "none", U_BT_SECURITY_MODE_NONE }, { "unauth", U_BT_SECURITY_MODE_UNAUTHENTICATED },
        { "auth", U_BT_SECURITY_MODE_AUTHENTICATED }, { "sc", U_BT_SECURITY_MODE_AUTHENTICATED_SECURE_CONNECTION },
        { "sconly", U_BT_SECURITY_MODE_AUTHENTICATED_SECURE_CONNECTION_ONLY },
    };

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++) {
        if (strcmp(argv[1], kModes[i].name) == 0) {
            int32_t r = uCxBluetoothSetSecurityMode(app->ucx, kModes[i].mode);
            if (r != 0) {
                ucxapp_print("ERROR: set security mode failed (%d%s)\n", (int)r, ucxapp_errname(r));
                return UCXAPP_ERR_FAILED;
            }
            ucxapp_print("security mode: %s\n", kModes[i].name);
            return UCXAPP_OK;
        }
    }
    return UCXAPP_ERR_USAGE;
}

static int cmdBtDefaults(ucxapp_t *app, int argc, char *argv[])
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "secure") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        /* Mode 3 needs authenticated bonding, so Just Works is impossible: use numeric comparison. */
        if (uCxBluetoothSetIoCapabilities(app->ucx, U_BT_IO_CAP_DISPLAY_YES_NO) != 0 ||
            uCxBluetoothSetSecurityMode(app->ucx, U_BT_SECURITY_MODE_AUTHENTICATED_SECURE_CONNECTION) != 0 ||
            uCxBluetoothSetPairingMode(app->ucx, U_BT_PAIRING_MODE_ENABLE) != 0) {
            ucxapp_print("ERROR: could not apply the secure profile\n");
            return UCXAPP_ERR_FAILED;
        }
        gDefaultsApplied = true;
        ucxapp_print("secure connections with legacy fallback: numeric comparison, authenticated bonding, pairing enabled\n");
        return UCXAPP_OK;
    }
    gDefaultsApplied = false;
    ucxapp_bt_security_defaults(app);
    ucxapp_print("Just Works: no I/O, unauthenticated bonding, pairing enabled\n");
    return UCXAPP_OK;
}

static int cmdBtAutoConfirm(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 2 || !parseOnOff(argv[1], &gAutoConfirm)) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("auto-confirm %s\n", gAutoConfirm ? "on" : "off");
    return UCXAPP_OK;
}

static int cmdBtConfirm(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2 || (strcmp(argv[1], "yes") != 0 && strcmp(argv[1], "no") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gConfirmPending) {
        ucxapp_print("no pairing request is waiting\n");
        return UCXAPP_ERR_FAILED;
    }
    gConfirmPending = false;
    int32_t r = uCxBluetoothUserConfirmation(app->ucx, &gPendingAddr,
                                             argv[1][0] == 'y' ? U_BT_CONFIRM_YES : U_BT_CONFIRM_NO);
    if (r != 0) {
        ucxapp_print("ERROR: confirmation failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static int cmdBtPasskey(ucxapp_t *app, int argc, char *argv[])
{
    int32_t key = 0;
    bool deny = argc == 2 && strcmp(argv[1], "deny") == 0;

    if (argc != 2 || (!deny && (!ucxapp_parse_int(argv[1], &key) || key < 0 || key > 999999))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gPasskeyPending) {
        ucxapp_print("no passkey request is waiting\n");
        return UCXAPP_ERR_FAILED;
    }
    gPasskeyPending = false;
    int32_t r = deny ? uCxBluetoothUserPasskeyEntry2(app->ucx, &gPendingAddr, U_BT_CONFIRM_NO)
                     : uCxBluetoothUserPasskeyEntry3(app->ucx, &gPendingAddr, U_BT_CONFIRM_YES, key);
    if (r != 0) {
        ucxapp_print("ERROR: passkey reply failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static int cmdBtBonds(ucxapp_t *app, int argc, char *argv[])
{
    uBtLeAddress_t a;
    int count = 0;
    (void)argc;
    (void)argv;

    uCxBluetoothListBondedDevicesBegin(app->ucx);
    while (uCxBluetoothListBondedDevicesGetNext(app->ucx, &a)) {
        count++;
        printAddr(&a);
        ucxapp_print("\n");
    }
    uCxEnd(app->ucx);
    ucxapp_print("%d bonded device(s)\n", count);
    return UCXAPP_OK;
}

static int cmdBtUnbond(ucxapp_t *app, int argc, char *argv[])
{
    uBtLeAddress_t a;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    bool all = strcmp(argv[1], "all") == 0;
    if (!all && !parseAddr(argv[1], &a)) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = all ? uCxBluetoothUnbondAll(app->ucx) : uCxBluetoothUnbond(app->ucx, &a);
    if (r != 0) {
        ucxapp_print("ERROR: unbond failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("bond(s) removed\n");
    return UCXAPP_OK;
}

static bool connAddr(ucxapp_t *app, int32_t conn, uBtLeAddress_t *addr)
{
    uCxBtListConnections_t c;
    bool found = false;

    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &c)) {
        if (c.conn_handle == conn) {
            *addr = c.bd_addr;
            found = true;
        }
    }
    uCxEnd(app->ucx);
    return found;
}

/* Starts bonding and waits up to seconds for the module's bond status; true when bonded. */
static bool btBond(ucxapp_t *app, const uBtLeAddress_t *addr, int32_t seconds)
{
    ucxapp_bt_security_defaults(app);
    gBondResult = -1;
    int32_t r = uCxBluetoothBond(app->ucx, (uBtLeAddress_t *)addr);
    if (r != 0) {
        ucxapp_print("ERROR: bond request failed (%d%s); is the device connected (bt.list)?\n", (int)r, ucxapp_errname(r));
        return false;
    }
    uint32_t start = ucxapp_platform_tick_ms();
    while (gBondResult < 0 && ucxapp_platform_tick_ms() - start < (uint32_t)seconds * 1000u) {
        ucxapp_platform_sleep_ms(20);
    }
    if (gBondResult < 0) {
        ucxapp_print("no bond result in %d s; if the peer shows a code, enter it with bt.passkey\n", (int)seconds);
    }
    return gBondResult == 0;
}

static int cmdBtBond(ucxapp_t *app, int argc, char *argv[])
{
    uBtLeAddress_t a;
    int32_t conn = 0;
    int32_t seconds = 15;

    if (argc < 2 || argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1 || seconds > 120))) {
        return UCXAPP_ERR_USAGE;
    }
    if (strlen(argv[1]) <= 3 && ucxapp_parse_int(argv[1], &conn)) {
        if (!connAddr(app, conn, &a)) {
            ucxapp_print("ERROR: connection %d not found (bt.list)\n", (int)conn);
            return UCXAPP_ERR_FAILED;
        }
    } else if (!parseAddr(argv[1], &a)) {
        return UCXAPP_ERR_USAGE;
    }
    return btBond(app, &a, seconds) ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdBtAddr(ucxapp_t *app, int argc, char *argv[])
{
    uMacAddress_t mac;
    uBtLeAddress_t addr;
    int32_t r;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        if (!parseAddr(argv[1], &addr) || addr.type != U_BD_ADDRESS_TYPE_PUBLIC) {
            return UCXAPP_ERR_USAGE;
        }
        memcpy(mac.address, addr.address, sizeof(mac.address));
        r = uCxSystemSetLocalAddress(app->ucx, U_INTERFACE_ID_BLUETOOTH, &mac);
        if (r == 0) {
            r = uCxSystemStoreConfiguration(app->ucx);
        }
        if (r != 0) {
            ucxapp_print("ERROR: set the Bluetooth address failed (%d%s)\n", (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("Bluetooth address stored; it takes effect after dev.reboot\n");
        return UCXAPP_OK;
    }
    r = uCxSystemGetLocalAddress(app->ucx, U_INTERFACE_ID_BLUETOOTH, &mac);
    if (r != 0) {
        ucxapp_print("ERROR: read the Bluetooth address failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    memcpy(addr.address, mac.address, sizeof(addr.address));
    addr.type = U_BD_ADDRESS_TYPE_PUBLIC;
    printAddr(&addr);
    ucxapp_print("\n");
    return UCXAPP_OK;
}

static int cmdBtName(ucxapp_t *app, int argc, char *argv[])
{
    const char *name = NULL;
    int32_t r;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        r = uCxBluetoothSetLocalName(app->ucx, argv[1]);
        if (r == 0) {
            r = uCxSystemStoreConfiguration(app->ucx);
        }
        if (r != 0) {
            ucxapp_print("ERROR: set the Bluetooth name failed (%d%s)\n", (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("Bluetooth name \"%s\" set and stored\n", argv[1]);
        return UCXAPP_OK;
    }
    bool ok = uCxBluetoothGetLocalNameBegin(app->ucx, &name) && name != NULL;
    if (ok) {
        ucxapp_print("%s\n", name);
    }
    r = uCxEnd(app->ucx);
    if (!ok || r != 0) {
        ucxapp_print("ERROR: read the Bluetooth name failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

#define ADV_DATA_MAX 31

static const char *advTypeName(int32_t type)
{
    return type == U_BT_ADV_TYPE_NON_CONNECTABLE_SCANNABLE ? "scannable" :
           type == U_BT_ADV_TYPE_NON_CONNECTABLE_NON_SCANNABLE ? "broadcast" : "connectable";
}

static int advFailed(const char *what, int32_t r)
{
    ucxapp_print("ERROR: %s failed (%d%s)\n", what, (int)r, ucxapp_errname(r));
    return UCXAPP_ERR_FAILED;
}

/* bt.adv: legacy advertising state, interval and the custom AD payload (CHIPoBLE, beacons, service UUIDs). */
static int cmdBtAdv(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t data[ADV_DATA_MAX];
    int32_t r;

    if (argc == 1) {
        uCxBtGetAdvertiseInformation_t info;
        uCxBtGetLegacyAdvertisementConfig_t cfg;
        uCxBtGetLegacyAdvertiseDataAndType_t adv;

        r = uCxBluetoothGetAdvertiseInformation(app->ucx, &info);
        if (r != 0) {
            return advFailed("reading the advertising state", r);
        }
        ucxapp_print("Advertising:   %s%s\n", info.legacy_adv ? "on" : "off", info.directed_adv ? " (directed)" : "");
        if (uCxBluetoothGetLegacyAdvertisementConfig(app->ucx, &cfg) == 0) {
            ucxapp_print("Interval:      %d-%d ms\n", (int)(cfg.advertisement_interval_minimum * 5 / 8),
                         (int)(cfg.advertisement_interval_maximum * 5 / 8));
        }
        if (uCxBluetoothGetLegacyAdvertiseDataAndTypeBegin(app->ucx, &adv)) {
            ucxapp_print("Type:          %s\n", advTypeName(adv.adv_type));
            if (adv.adv_data.length > 0) {
                ucxapp_print("Data:          ");
                for (size_t i = 0; i < adv.adv_data.length; i++) {
                    ucxapp_print("%02X", adv.adv_data.pData[i]);
                }
                ucxapp_print(" (%u bytes)\n", (unsigned)adv.adv_data.length);
                ucxapp_adv_print(adv.adv_data.pData, adv.adv_data.length, "               ");
            } else {
                ucxapp_print("Data:          default (flags, name, SPS UUID)\n");
            }
        }
        uCxEnd(app->ucx);
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        r = uCxBluetoothLegacyAdvertisementStart(app->ucx);
        return r != 0 ? advFailed("starting advertising", r) : (ucxapp_print("advertising on\n"), UCXAPP_OK);
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        r = uCxBluetoothLegacyAdvertisementStop(app->ucx);
        return r != 0 ? advFailed("stopping advertising", r) : (ucxapp_print("advertising off\n"), UCXAPP_OK);
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "interval") == 0) {
        int32_t minMs, maxMs = 0;

        if (!ucxapp_parse_int(argv[2], &minMs) || minMs < 20 || minMs > 10240 ||
            (argc == 4 && (!ucxapp_parse_int(argv[3], &maxMs) || maxMs < minMs || maxMs > 10240))) {
            return UCXAPP_ERR_USAGE;
        }
        if (argc == 3) {
            maxMs = minMs;
        }
        r = uCxBluetoothSetLegacyAdvertisementConfig(app->ucx, minMs * 8 / 5, maxMs * 8 / 5);
        if (r != 0) {
            return advFailed("setting the interval", r);
        }
        ucxapp_print("advertising interval %d-%d ms (applies to the next bt.adv on)\n", (int)minMs, (int)maxMs);
        return UCXAPP_OK;
    }
    if (argc == 2 || argc == 3) {
        const char *hex = (argv[1][0] == '0' && (argv[1][1] == 'x' || argv[1][1] == 'X')) ? argv[1] + 2 : argv[1];
        int n = parseHexBytes(hex, data, ADV_DATA_MAX);
        uBtAdvType_t type = U_BT_ADV_TYPE_CONNECTABLE_SCANNABLE;

        if (n < 3) {
            if (n < 0) {
                ucxapp_print("ERROR: advertising data is 3-31 bytes of hex (AD structures: length, type, value)\n");
            }
            return UCXAPP_ERR_USAGE;
        }
        if (argc == 3) {
            if (strcmp(argv[2], "scannable") == 0) {
                type = U_BT_ADV_TYPE_NON_CONNECTABLE_SCANNABLE;
            } else if (strcmp(argv[2], "broadcast") == 0) {
                type = U_BT_ADV_TYPE_NON_CONNECTABLE_NON_SCANNABLE;
            } else if (strcmp(argv[2], "connectable") != 0) {
                return UCXAPP_ERR_USAGE;
            }
        }
        for (int i = 0; i < n; i += data[i] + 1) {
            if (data[i] == 0 || i + data[i] >= n) {
                ucxapp_print("ERROR: AD structure at byte %d runs past the data; each one is <len> <type> <value...>\n", i);
                return UCXAPP_ERR_USAGE;
            }
        }
        r = argc == 3 ? uCxBluetoothSetLegacyAdvertiseDataAndType2(app->ucx, data, n, type)
                      : uCxBluetoothSetLegacyAdvertiseDataAndType1(app->ucx, data, n);
        if (r != 0) {
            advFailed("setting the advertising data", r);
            ucxapp_print("connectable advertising takes 28 bytes (the module adds the flags), others 31\n");
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("advertising data set (%d bytes, %s)\n", n, advTypeName((int32_t)type));
        ucxapp_adv_print(data, (size_t)n, "  ");
        return UCXAPP_OK;
    }
    return UCXAPP_ERR_USAGE;
}

#ifndef UCXAPP_LITE
static void printFilter(int32_t index, int32_t adType, const uint8_t *data, size_t len)
{
    char text[BT_FILTER_MAX + 1];
    bool printable = len > 0;

    ucxapp_print("%d  AD type 0x%02X  ", (int)index, (unsigned)adType);
    for (size_t i = 0; i < len; i++) {
        ucxapp_print("%02X", data[i]);
        printable = printable && data[i] >= 0x20 && data[i] < 0x7F;
        if (i < BT_FILTER_MAX) {
            text[i] = (char)data[i];
        }
    }
    text[len < BT_FILTER_MAX ? len : BT_FILTER_MAX] = '\0';
    ucxapp_print(printable ? "  \"%s\"\n" : "\n", text);
}

static int cmdBtFilter(ucxapp_t *app, int argc, char *argv[])
{
    int32_t index, adType = 0;
    uint8_t data[BT_FILTER_MAX];
    int len = 0;
    int32_t r;

    if (argc == 1) {
        uCxBtListDiscoveryDataFilters_t f;
        int n = 0;

        uCxBluetoothListDiscoveryDataFiltersBegin(app->ucx);
        while (uCxBluetoothListDiscoveryDataFiltersGetNext(app->ucx, &f)) {
            if (f.data_filter.length > 0) {
                printFilter(f.data_filter_index, f.ad_type, f.data_filter.pData, f.data_filter.length);
                n++;
            }
        }
        r = uCxEnd(app->ucx);
        if (r != 0) {
            ucxapp_print("ERROR: read the data filters failed (%d%s)\n", (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        ucxapp_print("%d data filter(s)%s\n", n, n > 0 ? "; bt.scan filter shows only matching devices"
                                                     : "; set one with bt.filter <0-9> <ad_type> <text|hex>");
        return UCXAPP_OK;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        for (index = 0; index < BT_FILTER_SLOTS; index++) {
            r = uCxBluetoothSetDiscoveryDataFilter(app->ucx, index, 0, data, 0);
            if (r != 0) {
                ucxapp_print("ERROR: clear data filter %d failed (%d%s)\n", (int)index, (int)r, ucxapp_errname(r));
                return UCXAPP_ERR_FAILED;
            }
        }
        ucxapp_print("all data filters cleared\n");
        return UCXAPP_OK;
    }
    if (!ucxapp_parse_int(argv[1], &index) || index < 0 || index >= BT_FILTER_SLOTS) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        len = 0;
    } else if (argc != 4 || !ucxapp_parse_int(argv[2], &adType) || adType < 0 || adType > 255 ||
               (len = parsePattern(argv[3], data, BT_FILTER_MAX)) <= 0) {
        return UCXAPP_ERR_USAGE;
    }
    r = uCxBluetoothSetDiscoveryDataFilter(app->ucx, index, adType, data, len);
    if (r != 0) {
        ucxapp_print("ERROR: set data filter %d failed (%d%s)\n", (int)index, (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    if (len == 0) {
        ucxapp_print("data filter %d cleared\n", (int)index);
    } else {
        printFilter(index, adType, data, (size_t)len);
    }
    return UCXAPP_OK;
}

static int cmdBtCs(ucxapp_t *app, int argc, char *argv[])
{
    uBtChannelSoundingMode_t mode;
    int32_t r;

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0 &&
                     strcmp(argv[1], "initiator") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        mode = strcmp(argv[1], "on") == 0          ? U_BT_CHANNEL_SOUNDING_MODE_REFLECTOR :
               strcmp(argv[1], "initiator") == 0   ? U_CX_CS_MODE_INITIATOR :
                                                     U_BT_CHANNEL_SOUNDING_MODE_DISABLE;
        r = uCxChannelSoundingSetChannelSoundingMode(app->ucx, mode);
        if (r == 0) {
            r = uCxSystemStoreConfiguration(app->ucx);
        }
    } else {
        r = uCxChannelSoundingGetChannelSoundingMode(app->ucx, &mode);
    }
    if (r != 0) {
        ucxapp_print("ERROR: Channel Sounding failed (%d%s); it needs a NORA-B26 or NORA-B27 with 3.4 or later%s\n",
                     (int)r, ucxapp_errname(r),
                     argc == 2 && strcmp(argv[1], "initiator") == 0 ? ", the initiator the cs_initiator evaluation firmware" : "");
        return UCXAPP_ERR_FAILED;
    }
    if (mode == U_BT_CHANNEL_SOUNDING_MODE_REFLECTOR) {
        ucxapp_print("Channel Sounding on: reflector, a connected initiator (phone or tag reader) measures the distance\n");
    } else if (mode == U_CX_CS_MODE_INITIATOR) {
        ucxapp_print("Channel Sounding initiator: bond with a reflector, then bt.cs.range <conn> shows the module's distance\n");
    } else {
        ucxapp_print("Channel Sounding off\n");
    }
    return UCXAPP_OK;
}

/* Distances come from the module (+UEBTCSM, median filtered, Kalman filter per bt.cs.filter); nothing is
 * computed here. The URC handler queues them so a burst is not lost between two polls. */
#define CS_RING 16
static struct {
    int32_t conn[CS_RING];
    int32_t mm[CS_RING];
    uint32_t head;
    uint32_t pctCount;
} gCs;

void ucxapp_bt_cs_distance(int32_t conn, int32_t mm)
{
    ucxapp_events_lock();
    gCs.conn[gCs.head % CS_RING] = conn;
    gCs.mm[gCs.head % CS_RING] = mm;
    gCs.head++;
    ucxapp_events_unlock();
}

void ucxapp_bt_cs_pct(int32_t conn, int32_t antenna, int32_t channels)
{
    (void)conn;
    (void)antenna;
    (void)channels;
    ucxapp_events_lock();
    gCs.pctCount++;
    ucxapp_events_unlock();
}

static int csFailed(const char *what, int32_t r)
{
    ucxapp_print("ERROR: %s failed (%d%s); it needs the NORA-B26 cs_initiator evaluation firmware\n", what, (int)r,
                 ucxapp_errname(r));
    return UCXAPP_ERR_FAILED;
}

/* Ranging needs an encrypted, bonded link: bond with the peer of conn, then start again. */
static int32_t csBondAndStart(ucxapp_t *app, int32_t conn)
{
    uBtLeAddress_t addr;

    if (!connAddr(app, conn, &addr)) {
        ucxapp_print("ERROR: connection %d not found (bt.list)\n", (int)conn);
        return -1;
    }
    ucxapp_print("ranging refused, bonding with the reflector...\n");
    if (!btBond(app, &addr, 15)) {
        return -1;
    }
    return uCxChannelSoundingSetProcedure(app->ucx, conn, true);
}

static int cmdBtCsRange(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn = 0;
    int32_t seconds = 10;
    bool toggle = argc == 3 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0);

    if (argc < 2 || argc > 3 || !ucxapp_parse_int(argv[1], &conn) || conn < 0 ||
        (argc == 3 && !toggle && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1 || seconds > 3600))) {
        return UCXAPP_ERR_USAGE;
    }
    if (toggle) {
        int32_t r = uCxChannelSoundingSetProcedure(app->ucx, conn, strcmp(argv[2], "on") == 0);
        if (r != 0) {
            return csFailed("ranging", r);
        }
        ucxapp_print("ranging %s on conn %d%s\n", argv[2], (int)conn,
                     strcmp(argv[2], "on") == 0 ? "; distances appear in the events (app.log.events.on)" : "");
        return UCXAPP_OK;
    }
    ucxapp_events_lock();
    uint32_t seen = gCs.head;
    ucxapp_events_unlock();
    int32_t r = uCxChannelSoundingSetProcedure(app->ucx, conn, true);
    if (r != 0) {
        r = csBondAndStart(app, conn);
    }
    if (r != 0) {
        ucxapp_print("hint: this module must be in initiator mode (bt.cs initiator) and the reflector accept bonding\n");
        return csFailed("ranging", r);
    }
    ucxapp_print("ranging on conn %d for %d s, distance from the module (any key stops)\n", (int)conn, (int)seconds);
    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t got = 0;
    int32_t lo = INT32_MAX;
    int32_t hi = INT32_MIN;
    while (ucxapp_platform_tick_ms() - start < (uint32_t)seconds * 1000u) {
        if (ucxapp_platform_key_pressed()) {
            (void)ucxapp_platform_get_key();
            break;
        }
        int32_t batch[CS_RING];
        int n = 0;
        ucxapp_events_lock();
        if (gCs.head - seen > CS_RING) {
            seen = gCs.head - CS_RING;
        }
        for (; seen != gCs.head; seen++) {
            if (gCs.conn[seen % CS_RING] == conn) {
                batch[n++] = gCs.mm[seen % CS_RING];
            }
        }
        ucxapp_events_unlock();
        for (int i = 0; i < n; i++) {
            got++;
            lo = batch[i] < lo ? batch[i] : lo;
            hi = batch[i] > hi ? batch[i] : hi;
            ucxapp_print("%6.2f m\n", batch[i] / 1000.0);
        }
        ucxapp_platform_sleep_ms(20);
    }
    (void)uCxChannelSoundingSetProcedure(app->ucx, conn, false);
    if (got == 0) {
        ucxapp_print("no distance in %d s; check bt.cs on the reflector and bt.cs.rate\n", (int)seconds);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%u readings, %.2f..%.2f m\n", (unsigned)got, lo / 1000.0, hi / 1000.0);
    return UCXAPP_OK;
}

static int cmdBtCsRate(ucxapp_t *app, int argc, char *argv[])
{
    int32_t rate = 0;
    int32_t r;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &rate) || rate < 1 || rate > 65535))) {
        return UCXAPP_ERR_USAGE;
    }
    r = argc == 2 ? uCxChannelSoundingSetRate(app->ucx, rate) : uCxChannelSoundingGetRate(app->ucx, &rate);
    if (r != 0) {
        return csFailed("Channel Sounding rate", r);
    }
    ucxapp_print("one measurement every %d connection intervals\n", (int)rate);
    return UCXAPP_OK;
}

static int cmdBtCsFilter(ucxapp_t *app, int argc, char *argv[])
{
    uCxCsKalman_t k = { 1, 120, true };
    int32_t r;

    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0 || strcmp(argv[1], "default") == 0)) {
        if (strcmp(argv[1], "default") != 0 && (r = uCxChannelSoundingGetKalman(app->ucx, &k)) != 0) {
            return csFailed("Kalman filter", r);
        }
        if (strcmp(argv[1], "default") != 0) {
            k.enabled = strcmp(argv[1], "on") == 0;
        }
    } else if (argc == 3) {
        if (!ucxapp_parse_int(argv[1], &k.processNoise) || k.processNoise < 0 || k.processNoise > 1000 ||
            !ucxapp_parse_int(argv[2], &k.measurementNoise) || k.measurementNoise < 10 || k.measurementNoise > 10000) {
            return UCXAPP_ERR_USAGE;
        }
    } else if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    r = argc == 1 ? uCxChannelSoundingGetKalman(app->ucx, &k) : uCxChannelSoundingSetKalman(app->ucx, &k);
    if (r != 0) {
        return csFailed("Kalman filter", r);
    }
    ucxapp_print("Kalman filter %s: Q %.3f, R %.3f\n", k.enabled ? "on" : "off (raw distances)",
                 k.processNoise / 1000.0, k.measurementNoise / 1000.0);
    return UCXAPP_OK;
}

static int cmdBtCsPct(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kRefs[] = { "off", "500", "1000", "10000" };
    int ref = -1;
    bool on = false;
    int32_t r;

    if (argc == 3 && strcmp(argv[1], "ref") == 0) {
        for (int i = 0; i < 4; i++) {
            ref = strcmp(argv[2], kRefs[i]) == 0 ? i : ref;
        }
        if (ref < 0) {
            return UCXAPP_ERR_USAGE;
        }
        r = uCxChannelSoundingSetReference(app->ucx, (uCxCsReference_t)ref);
        if (r != 0) {
            return csFailed("PCT reference", r);
        }
        ucxapp_print(ref == 0 ? "PCT from real measurements\n" : "PCT replaced by the stored %s mm reference\n", kRefs[ref]);
        return UCXAPP_OK;
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    r = argc == 2 ? uCxChannelSoundingSetPctReporting(app->ucx, strcmp(argv[1], "on") == 0)
                  : uCxChannelSoundingGetPctReporting(app->ucx, &on);
    if (r != 0) {
        return csFailed("PCT reporting", r);
    }
    on = argc == 2 ? strcmp(argv[1], "on") == 0 : on;
    ucxapp_events_lock();
    uint32_t n = gCs.pctCount;
    ucxapp_events_unlock();
    ucxapp_print("PCT reporting %s, %u reports so far\n", on ? "on" : "off", (unsigned)n);
    return UCXAPP_OK;
}
#endif

static const ucxapp_cmd_t gBtCmds[] = {
    { "bt.scan",       "[full] [filter]",     "Scan for BLE devices (up to 30 s), named and strongest first; full decodes the advertising data, filter keeps only bt.filter matches", UCXAPP_CMD_NEEDS_DEVICE, cmdBtScan },
    { "bt.connect",    "[addr[,random]|n|name]", "Connect by address, scan number, profile or device name (case-sensitive; an unscanned name is looked for first); no argument: the last used profile", UCXAPP_CMD_NEEDS_DEVICE, cmdBtConnect },
    { "bt.name",       "[name]",              "Show or set the module's Bluetooth name (stored)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtName },
    { "bt.addr",       "[addr]",              "Show or set the module's Bluetooth address (XX:XX:XX:XX:XX:XX, applies after dev.reboot)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtAddr },
    { "bt.adv",        "[on|off|<hex> [connectable|scannable|broadcast]|interval <min_ms> [max_ms]]", "Legacy advertising: show it, start or stop it, set the custom AD payload (3-31 bytes, e.g. Matter service data) and its type, or the interval", UCXAPP_CMD_NEEDS_DEVICE, cmdBtAdv },
#ifndef UCXAPP_LITE
    { "bt.filter",     "[<0-9> <ad_type> <text|hex>|<0-9> clear|clear]", "Scan data filters for bt.scan filter: AD type (9 name, 0xFF manufacturer data) and up to 10 bytes as text (NORA) or hex (4E4F5241)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtFilter },
    { "bt.cs",         "[on|off|initiator]",  "Channel Sounding (NORA-B26/B27, stored): on makes the module a reflector an initiator can range; initiator (cs_initiator evaluation firmware) measures", UCXAPP_CMD_NEEDS_DEVICE, cmdBtCs },
    { "bt.cs.range",   "<conn> [seconds=10|on|off]", "Initiator: range a reflector (bonds first if needed) and print the filtered distance for some seconds, or start/stop ranging in the background (events)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtCsRange },
    { "bt.cs.rate",    "[intervals=20]",      "Initiator: connection intervals between measurements (1-65535, stored)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtCsRate },
    { "bt.cs.filter",  "[on|off|default|<q> <r>]", "Initiator: the module's Kalman filter; off gives raw distances, Q 0-1000 and R 10-10000 in 0.001 (default 1 and 120)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtCsFilter },
    { "bt.cs.phase",     "[on|off|ref off|500|1000|10000]", "Initiator: phase correction term reports for your own algorithms, or a stored reference instead of real data (debug)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtCsPct },
#endif
    { "bt.disconnect", "[conn]",            "Close a BLE connection",                         UCXAPP_CMD_NEEDS_DEVICE, cmdBtDisconnect },
    { "bt.list",       "",                    "List BLE connections",                           UCXAPP_CMD_NEEDS_DEVICE, cmdBtList },
    { "bt.pairing",    "<on|off>",            "Enable or disable pairing mode",                 UCXAPP_CMD_NEEDS_DEVICE, cmdBtPairing },
    { "bt.iocap",      "<none|display|yesno|keyboard|keydisplay>", "Set the I/O capabilities (none = Just Works)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtIoCap },
    { "bt.security",   "<none|unauth|auth|sc|sconly>", "Set the required security level",    UCXAPP_CMD_NEEDS_DEVICE, cmdBtSecurity },
    { "bt.defaults",   "[secure]",            "Just Works (default), or secure: LE Secure Connections with legacy fallback and numeric comparison", UCXAPP_CMD_NEEDS_DEVICE, cmdBtDefaults },
    { "bt.autoconfirm", "<on|off>",           "Accept numeric-comparison pairing automatically (default on)", 0, cmdBtAutoConfirm },
    { "bt.confirm",    "<yes|no>",            "Answer a waiting pairing request",               UCXAPP_CMD_NEEDS_DEVICE, cmdBtConfirm },
    { "bt.passkey",    "<digits|deny>",       "Answer a waiting passkey request",               UCXAPP_CMD_NEEDS_DEVICE, cmdBtPasskey },
    { "bt.bonds",      "",                    "List bonded devices",                            UCXAPP_CMD_NEEDS_DEVICE, cmdBtBonds },
    { "bt.bond",       "<conn|addr> [seconds=15]", "Bond (pair and store keys) with a connected device and wait for the result; answer a code with bt.passkey", UCXAPP_CMD_NEEDS_DEVICE, cmdBtBond },
    { "bt.unbond",     "<addr|all>",          "Remove one or all bonds",                        UCXAPP_CMD_NEEDS_DEVICE, cmdBtUnbond },
    { "bt.profile.add", "<name> <addr[,random]>", "Save a BLE device as a named profile",          0, cmdBtProfileAdd },
    { "bt.profile.list", "",                  "List saved BLE profiles",                        0, cmdBtProfileList },
    { "bt.profile.remove", "<name>",          "Delete a BLE profile",                           UCXAPP_CMD_DANGER, cmdBtProfileRemove },
    { "bt.profile.connect", "<name>",         "Connect to a saved BLE profile",                 UCXAPP_CMD_NEEDS_DEVICE, cmdBtProfileConnect },
    { "bt.profile.auto", "",                  "Connect to the last used profile, else to the strongest known device in a scan", UCXAPP_CMD_NEEDS_DEVICE, cmdBtProfileAuto },
};

void ucxapp_register_bt_cmds(void)
{
    ucxapp_cmd_register(gBtCmds, sizeof(gBtCmds) / sizeof(gBtCmds[0]));
}
