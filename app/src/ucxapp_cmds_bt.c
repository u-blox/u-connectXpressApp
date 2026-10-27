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
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifndef BT_SCAN_MAX_DEVICES
#define BT_SCAN_MAX_DEVICES 32
#endif
#define BT_SCAN_TIMEOUT_MS  30000

#define BT_ADV_MAX          64

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

/* "XX:XX:XX:XX:XX:XX" or "XX:XX:XX:XX:XX:XX,random"; public if no type is given. */
static bool parseAddr(const char *s, uBtLeAddress_t *addr)
{
    unsigned int v[6];
    int used = 0;

    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &used) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        addr->address[i] = (uint8_t)v[i];
    }
    addr->type = U_BD_ADDRESS_TYPE_PUBLIC;
    if (s[used] == ',') {
        if (strcmp(&s[used + 1], "random") == 0) {
            addr->type = U_BD_ADDRESS_TYPE_RANDOM;
        } else if (strcmp(&s[used + 1], "public") != 0) {
            return false;
        }
    } else if (s[used] != '\0') {
        return false;
    }
    return true;
}

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

/* Fills gLastScan: one entry per address with the best RSSI, the longest name and the merged AD data. */
static int scanDevices(ucxapp_t *app)
{
    int count = 0;
    uCxBtDiscoveryDefault_t dev;

    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, BT_SCAN_TIMEOUT_MS, false);
    uCxBluetoothDiscoveryDefaultBegin(app->ucx);
    while (uCxBluetoothDiscoveryDefaultGetNext(app->ucx, &dev)) {
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

static int cmdBtScan(ucxapp_t *app, int argc, char *argv[])
{
    bool full = argc == 2 && strcmp(argv[1], "full") == 0;
    bool seenNamed = false;
    bool separated = false;

    if (argc > 2 || (argc == 2 && !full)) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("scanning...\n");
    int count = scanDevices(app);
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

#define BT_LAST_FILE  "ucxapp.blelast"

/* Starts a connect and waits until the link is listed; *up tells whether it came up. */
static void centralSecurity(ucxapp_t *app);

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
    int32_t handle = uCxBluetoothConnect(app->ucx, addr);

    *up = false;
    for (int tries = 0; handle >= 0 && !*up && tries < waitTenthsS; tries++) {
        uCxBtListConnections_t conn;

        uCxBluetoothListConnectionsBegin(app->ucx);
        while (uCxBluetoothListConnectionsGetNext(app->ucx, &conn)) {
            *up = *up || conn.conn_handle == handle;
        }
        uCxEnd(app->ucx);
        if (!*up) {
            U_CX_PORT_SLEEP_MS(100);
        }
    }
    if (*up) {
        ucxapp_handle_add(UCXAPP_HANDLE_CONN, handle);
    }
    return handle;
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

/* Address, scan number, saved profile name, or a scanned device name (whole or the start of it). */
static bool resolveTarget(const char *s, uBtLeAddress_t *addr, char *label, size_t labelSize)
{
    btProfile_t profiles[BT_MAX_PROFILES];
    int match = -1;
    int matches = 0;

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
    for (int i = 0; i < gLastScanCount; i++) {
        if (ciEqual(gLastScan[i].name, s)) {
            match = i;
            matches = 1;
            break;
        }
        if (s[0] != '\0' && ciPrefix(gLastScan[i].name, s)) {
            match = i;
            matches++;
        }
    }
    if (matches == 1) {
        *addr = gLastScan[match].addr;
        snprintf(label, labelSize, "%s", gLastScan[match].name);
        return true;
    }
    if (matches > 1) {
        ucxapp_print("ERROR: '%s' matches %d scanned devices; use the number from bt.scan\n", s, matches);
    } else {
        ucxapp_print("ERROR: '%s' is not an address, a scan number, a profile or a scanned device name\n", s);
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
    if (!resolveTarget(argv[1], &addr, label, sizeof(label))) {
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
    int n = scanDevices(app);
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

void ucxapp_bt_bond_status(const uBtLeAddress_t *addr, int32_t status)
{
    if (addr == NULL) {
        return;
    }
    ucxapp_print("[BT] bonding %s: ", status == 0 ? "succeeded" : "FAILED");
    printAddr(addr);
    if (status != 0) {
        ucxapp_print(" (status %d)", (int)status);
    }
    ucxapp_print("\n");
    if (status == U_BT_BOND_STATUS_BONDING_FAILED_AUTH) {
        ucxapp_print("[BT] the peer rejected pairing; if it shows a code, enter it with bt.passkey; to retry run bt.unbond <addr> first\n");
    } else if (status == U_BT_BOND_STATUS_BONDING_FAILED_PEER_LOST_BOND) {
        ucxapp_print("[BT] the peer lost its bond: run bt.unbond <addr> and connect again\n");
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

static const ucxapp_cmd_t gBtCmds[] = {
    { "bt.scan",       "[full]",              "Scan for BLE devices (up to 30 s): named first, strongest first, with what they advertise; full decodes the data", UCXAPP_CMD_NEEDS_DEVICE, cmdBtScan },
    { "bt.connect",    "[addr[,random]|n|name]", "Connect by address, by number from bt.scan, or by profile or device name; with no argument: the last used profile (same as bt.profile.auto)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtConnect },
    { "bt.disconnect", "[conn]",            "Close a BLE connection",                         UCXAPP_CMD_NEEDS_DEVICE, cmdBtDisconnect },
    { "bt.list",       "",                    "List BLE connections",                           UCXAPP_CMD_NEEDS_DEVICE, cmdBtList },
    { "bt.pairing",    "<on|off>",            "Enable or disable pairing mode",                 UCXAPP_CMD_NEEDS_DEVICE, cmdBtPairing },
    { "bt.iocap",      "<none|display|yesno|keyboard|keydisplay>", "Set the I/O capabilities (none = Just Works)", UCXAPP_CMD_NEEDS_DEVICE, cmdBtIoCap },
    { "bt.security",   "<none|unauth|auth|sc|sconly>", "Set the required security level",    UCXAPP_CMD_NEEDS_DEVICE, cmdBtSecurity },
    { "bt.defaults",   "[secure]",            "Just Works (default), or secure: LE Secure Connections with legacy fallback + numeric comparison", UCXAPP_CMD_NEEDS_DEVICE, cmdBtDefaults },
    { "bt.autoconfirm", "<on|off>",           "Accept numeric-comparison pairing automatically (default on)", 0, cmdBtAutoConfirm },
    { "bt.confirm",    "<yes|no>",            "Answer a waiting pairing request",               UCXAPP_CMD_NEEDS_DEVICE, cmdBtConfirm },
    { "bt.passkey",    "<digits|deny>",       "Answer a waiting passkey request",               UCXAPP_CMD_NEEDS_DEVICE, cmdBtPasskey },
    { "bt.bonds",      "",                    "List bonded devices",                            UCXAPP_CMD_NEEDS_DEVICE, cmdBtBonds },
    { "bt.unbond",     "<addr|all>",          "Remove one or all bonds",                        UCXAPP_CMD_NEEDS_DEVICE, cmdBtUnbond },
    { "bt.profile.add", "<name> <addr[,random]>", "Save a BLE device as a named profile",          0, cmdBtProfileAdd },
    { "bt.profile.list", "",                  "List saved BLE profiles",                        0, cmdBtProfileList },
    { "bt.profile.remove", "<name>",          "Delete a BLE profile",                           UCXAPP_CMD_DANGER, cmdBtProfileRemove },
    { "bt.profile.connect", "<name>",         "Connect to a saved BLE profile",                 UCXAPP_CMD_NEEDS_DEVICE, cmdBtProfileConnect },
    { "bt.profile.auto", "",                  "Connect to the last used profile, else scan and take the strongest known device", UCXAPP_CMD_NEEDS_DEVICE, cmdBtProfileAuto },
};

void ucxapp_register_bt_cmds(void)
{
    ucxapp_cmd_register(gBtCmds, sizeof(gBtCmds) / sizeof(gBtCmds[0]));
}
