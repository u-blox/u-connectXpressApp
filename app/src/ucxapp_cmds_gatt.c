#include "ucxapp_cmd.h"
#include "ucxapp_stats.h"
#include "ucxapp_stream.h"
#include "ucxapp_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_gatt_client.h"
#include "u_cx_gatt_server.h"
#include "u_cx_wifi.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "../third-party/bluetooth-sig/bt_service_uuids.h"
#ifdef UCXAPP_LITE   /* saves the characteristic name table */
static const char *btGetCharacteristicName(uint16_t uuid) { (void)uuid; return NULL; }
#else
#include "../third-party/bluetooth-sig/bt_characteristic_uuids.h"
#endif

#ifndef MAX_SVC
#define MAX_SVC          32
#endif
#ifndef MAX_CHR
#define MAX_CHR          96
#endif
#define MAX_VALUE        512
#ifndef MAX_SRV_CHARS
#define MAX_SRV_CHARS    24
#endif
#define MAX_INIT         32
#define LISTEN_DEFAULT_S 10

#define PROP_READ        0x02
#define PROP_WRITE_NR    0x04
#define PROP_WRITE       0x08
#define PROP_NOTIFY      0x10
#define PROP_INDICATE    0x20

/* 128-bit UUIDs in the byte order the module reports them. */
static const uint8_t kNusSvc[16]  = {0x6E,0x40,0x00,0x01,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t kNusRx[16]   = {0x6E,0x40,0x00,0x02,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t kNusTx[16]   = {0x6E,0x40,0x00,0x03,0xB5,0xA3,0xF3,0x93,0xE0,0xA9,0xE5,0x0E,0x24,0xDC,0xCA,0x9E};
static const uint8_t kSpsSvc[16]  = {0x24,0x56,0xE1,0xB9,0x26,0xE2,0x8F,0x83,0xE7,0x44,0xF3,0x4F,0x01,0xE9,0xD7,0x01};
static const uint8_t kSpsFifo[16] = {0x24,0x56,0xE1,0xB9,0x26,0xE2,0x8F,0x83,0xE7,0x44,0xF3,0x4F,0x03,0xE9,0xD7,0x01};
static const uint8_t kSpsCred[16] = {0x24,0x56,0xE1,0xB9,0x26,0xE2,0x8F,0x83,0xE7,0x44,0xF3,0x4F,0x04,0xE9,0xD7,0x01};

/* Wi-Fi provisioning: 14387800-130c-49e7-b877-2881c89cb258 (info 01, control point 02, data out 03). */
static const uint8_t kWpsSvc[16]  = {0x14,0x38,0x78,0x00,0x13,0x0C,0x49,0xE7,0xB8,0x77,0x28,0x81,0xC8,0x9C,0xB2,0x58};
static const uint8_t kWpsInfo[16] = {0x14,0x38,0x78,0x01,0x13,0x0C,0x49,0xE7,0xB8,0x77,0x28,0x81,0xC8,0x9C,0xB2,0x58};
static const uint8_t kWpsCtrl[16] = {0x14,0x38,0x78,0x02,0x13,0x0C,0x49,0xE7,0xB8,0x77,0x28,0x81,0xC8,0x9C,0xB2,0x58};
static const uint8_t kWpsData[16] = {0x14,0x38,0x78,0x03,0x13,0x0C,0x49,0xE7,0xB8,0x77,0x28,0x81,0xC8,0x9C,0xB2,0x58};

/* WitMotion BLE 5.0 IMU (WT901BLE and similar): service 0xFFE5, notify 0xFFE4, write 0xFFE9. */
static const uint8_t kWitSvc[16]    = {0x00,0x00,0xFF,0xE5,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB};
static const uint8_t kWitNotify[16] = {0x00,0x00,0xFF,0xE4,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB};

/* ---- helpers --------------------------------------------------------- */

static bool isUuid128(const uint8_t *u, size_t len, const uint8_t *ref)
{
    return len == 16 && memcmp(u, ref, 16) == 0;
}

static bool isUuid16(const uint8_t *u, size_t len, uint16_t v)
{
    return len == 2 && (((unsigned)u[0] << 8) | u[1]) == v;
}

static uint16_t uuid16Of(const uint8_t *u)
{
    return (uint16_t)(((unsigned)u[0] << 8) | u[1]);
}

static void uuidStr(const uint8_t *u, size_t len, char *out, size_t size)
{
    size_t pos = 0;

    if (len == 2) {
        snprintf(out, size, "0x%04X", (unsigned)uuid16Of(u));
        return;
    }
    out[0] = '\0';
    for (size_t i = 0; i < len && pos + 3 <= size; i++) {
        pos += (size_t)snprintf(out + pos, size - pos, "%02X", (unsigned)u[i]);
    }
}

static const char *serviceName(const uint8_t *u, size_t len)
{
    if (len == 2) {
        return btGetServiceName(uuid16Of(u));
    }
    if (isUuid128(u, len, kNusSvc)) {
        return "Nordic UART Service";
    }
    if (isUuid128(u, len, kSpsSvc)) {
        return "u-blox Serial Port Service";
    }
    if (isUuid128(u, len, kWpsSvc)) {
        return "Wi-Fi Provisioning Service";
    }
    return NULL;
}

static const char *charName(const uint8_t *u, size_t len)
{
    if (len == 2) {
        return btGetCharacteristicName(uuid16Of(u));
    }
    if (isUuid128(u, len, kNusTx)) {
        return "NUS TX";
    }
    if (isUuid128(u, len, kNusRx)) {
        return "NUS RX";
    }
    if (isUuid128(u, len, kSpsFifo)) {
        return "SPS FIFO";
    }
    if (isUuid128(u, len, kSpsCred)) {
        return "SPS Credits";
    }
    if (isUuid128(u, len, kWpsInfo)) {
        return "WPS Info";
    }
    if (isUuid128(u, len, kWpsCtrl)) {
        return "WPS Control Point";
    }
    if (isUuid128(u, len, kWpsData)) {
        return "WPS Data Out";
    }
    return NULL;
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

/* "text:abc" is sent as ASCII; anything else must be an even number of hex digits. */
static bool parseData(const char *s, uint8_t *out, size_t max, size_t *len)
{
    if (strncmp(s, "text:", 5) == 0) {
        size_t n = strlen(s + 5);
        if (n == 0 || n > max) {
            return false;
        }
        memcpy(out, s + 5, n);
        *len = n;
        return true;
    }
    size_t n = strlen(s);
    if (n == 0 || (n % 2) != 0 || n / 2 > max) {
        return false;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexVal(s[i * 2]);
        int lo = hexVal(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    *len = n / 2;
    return true;
}

static void printText(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (d[i] >= 32 && d[i] < 127) {
            ucxapp_print("%c", d[i]);
        } else {
            ucxapp_print("\\x%02X", (unsigned)d[i]);
        }
    }
}

static void printHex(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ucxapp_print("%02X", (unsigned)d[i]);
    }
}

static bool parseConn(const char *s, int32_t *conn)
{
    return ucxapp_parse_int(s, conn) && *conn >= 0;
}

/* ---- client: discovery cache ------------------------------------------ */

typedef struct {
    int32_t start;
    int32_t end;
    uint8_t uuid[16];
    uint8_t uuidLen;
} svc_t;

typedef struct {
    int32_t svc;
    int32_t handle;
    uint8_t props;
    uint8_t uuid[16];
    uint8_t uuidLen;
} chr_t;

/* Cache of the last discovery; only one connection is cached at a time. */
static svc_t gSvc[MAX_SVC];
static chr_t gChr[MAX_CHR];
static int gSvcCount;
static int gChrCount;
static int32_t gCacheConn = -1;

/* SPS flow control state shared between the credits parser and gatt.send. */
static bool gSpsFlow;

/* Subscriptions made by the client examples; notifications print until gatt.listen off. */
#define LISTEN_MAX 16
static bool gListen;
static int32_t gListenConn;
static int32_t gListenHandles[LISTEN_MAX];
static int gListenCount;
static int gSpsRemoteCredits;

static int discover(ucxapp_t *app, int32_t conn)
{
    uCxGattClientDiscoverPrimaryServices_t s;
    uCxGattClientDiscoverServiceChars_t c;
    int32_t r;

    gSvcCount = 0;
    gChrCount = 0;
    gCacheConn = -1;

    uCxGattClientDiscoverPrimaryServicesBegin(app->ucx, conn);
    while (uCxGattClientDiscoverPrimaryServicesGetNext(app->ucx, &s)) {
        if (gSvcCount < MAX_SVC && s.uuid.length <= 16) {
            svc_t *d = &gSvc[gSvcCount++];
            d->start = s.start_handle;
            d->end = s.end_handle;
            d->uuidLen = (uint8_t)s.uuid.length;
            memcpy(d->uuid, s.uuid.pData, s.uuid.length);
        }
    }
    r = uCxEnd(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: service discovery failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }

    for (int i = 0; i < gSvcCount; i++) {
        uCxGattClientDiscoverServiceCharsBegin(app->ucx, conn, gSvc[i].start, gSvc[i].end);
        while (uCxGattClientDiscoverServiceCharsGetNext(app->ucx, &c)) {
            if (gChrCount < MAX_CHR && c.uuid.length <= 16) {
                chr_t *d = &gChr[gChrCount++];
                d->svc = i;
                d->handle = c.value_handle;
                d->props = c.properties.length > 0 ? c.properties.pData[0] : 0;
                d->uuidLen = (uint8_t)c.uuid.length;
                memcpy(d->uuid, c.uuid.pData, c.uuid.length);
            }
        }
        r = uCxEnd(app->ucx);
        if (r != 0) {
            ucxapp_print("ERROR: characteristic discovery failed for service %d (%d%s)\n", i, (int)r, ucxapp_errname(r));
        }
    }
    gCacheConn = conn;
    return UCXAPP_OK;
}

static int ensureCache(ucxapp_t *app, int32_t conn)
{
    return gCacheConn == conn ? UCXAPP_OK : discover(app, conn);
}

static int findService16(uint16_t uuid)
{
    for (int i = 0; i < gSvcCount; i++) {
        if (isUuid16(gSvc[i].uuid, gSvc[i].uuidLen, uuid)) {
            return i;
        }
    }
    return -1;
}

static int findService128(const uint8_t *uuid)
{
    for (int i = 0; i < gSvcCount; i++) {
        if (isUuid128(gSvc[i].uuid, gSvc[i].uuidLen, uuid)) {
            return i;
        }
    }
    return -1;
}

static const chr_t *findChar128(const uint8_t *uuid)
{
    for (int i = 0; i < gChrCount; i++) {
        if (isUuid128(gChr[i].uuid, gChr[i].uuidLen, uuid)) {
            return &gChr[i];
        }
    }
    return NULL;
}

bool ucxapp_gatt_client_find128(ucxapp_t *app, int32_t conn, const uint8_t uuid[16], int32_t *handle)
{
    if (ensureCache(app, conn) != UCXAPP_OK) {
        return false;
    }
    const chr_t *c = findChar128(uuid);
    if (c == NULL) {
        return false;
    }
    *handle = c->handle;
    return true;
}

static const chr_t *findCharByHandle(int32_t handle)
{
    for (int i = 0; i < gChrCount; i++) {
        if (gChr[i].handle == handle) {
            return &gChr[i];
        }
    }
    return NULL;
}

static void propsStr(uint8_t p, char *out)
{
    size_t n = 0;
    if (p & PROP_READ) {
        out[n++] = 'R';
    }
    if (p & PROP_WRITE) {
        out[n++] = 'W';
    }
    if (p & PROP_WRITE_NR) {
        out[n++] = 'w';
    }
    if (p & PROP_NOTIFY) {
        out[n++] = 'N';
    }
    if (p & PROP_INDICATE) {
        out[n++] = 'I';
    }
    out[n] = '\0';
}

/* ---- client: value decoding ------------------------------------------ */

static void parseHr(const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[HRS] invalid length %u\n", (unsigned)n);
        return;
    }
    unsigned bpm = ((d[0] & 0x01) && n >= 3) ? (unsigned)(d[1] | (d[2] << 8)) : d[1];
    ucxapp_print("[HRS] BPM: %u\n", bpm);
}

static void parseCts(const uint8_t *d, size_t n)
{
    if (n < 10) {
        ucxapp_print("[CTS] invalid length %u\n", (unsigned)n);
        return;
    }
    ucxapp_print("[CTS] %04u-%02u-%02u %02u:%02u:%02u (day of week %u)\n",
                 (unsigned)(d[0] | (d[1] << 8)), (unsigned)d[2], (unsigned)d[3],
                 (unsigned)d[4], (unsigned)d[5], (unsigned)d[6], (unsigned)d[7]);
}

static void parseTemp(const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[ESS] invalid temperature length %u\n", (unsigned)n);
        return;
    }
    ucxapp_print("[ESS] Temperature: %.2f C\n", (int16_t)(d[0] | (d[1] << 8)) / 100.0);
}

static void parseHum(const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[ESS] invalid humidity length %u\n", (unsigned)n);
        return;
    }
    ucxapp_print("[ESS] Humidity: %.2f %%\n", (uint16_t)(d[0] | (d[1] << 8)) / 100.0);
}

/* Location and Speed (GSS): the flags say which optional fields precede Location. */
static void parseLoc(const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[LNS] invalid length %u\n", (unsigned)n);
        return;
    }
    unsigned flags = (unsigned)(d[0] | (d[1] << 8));
    size_t off = 2 + ((flags & 0x01) ? 2 : 0) + ((flags & 0x02) ? 3 : 0);
    if (!(flags & 0x04)) {
        ucxapp_print("[LNS] no location (flags 0x%04X)\n", flags);
        return;
    }
    if (n < off + 8) {
        ucxapp_print("[LNS] invalid length %u\n", (unsigned)n);
        return;
    }
    const uint8_t *p = d + off;
    int32_t lat = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    int32_t lon = (int32_t)((uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24));
    ucxapp_print("[LNS] lat=%.7f lon=%.7f (flags 0x%04X)\n", lat / 1e7, lon / 1e7, flags);
}

static void parseAioDigital(const uint8_t *d, size_t n)
{
    ucxapp_print("[AIO] Digital: 0x");
    printHex(d, n);
    ucxapp_print("  ");
    for (size_t bit = 0; bit < n * 8; bit++) {
        ucxapp_print("D%u=%d ", (unsigned)bit, (d[bit / 8] >> (bit % 8)) & 1);
    }
    ucxapp_print("\n");
}

static void parseAioAnalog(const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[AIO] invalid analog length %u\n", (unsigned)n);
        return;
    }
    ucxapp_print("[AIO] Analog: %d\n", (int)(int16_t)(d[0] | (d[1] << 8)));
}

static void parseBattery(const uint8_t *d, size_t n)
{
    if (n < 1) {
        ucxapp_print("[BAS] invalid length\n");
        return;
    }
    ucxapp_print("[BAS] Battery Level: %u %%\n", d[0] > 100 ? 100u : (unsigned)d[0]);
}

static void parseRasFeatures(const uint8_t *d, size_t n)
{
    if (n < 4) {
        ucxapp_print("[RAS] invalid features length %u\n", (unsigned)n);
        return;
    }
    uint32_t f = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    ucxapp_print("[RAS] Features 0x%08lX: real-time %s, retrieve lost segments %s, abort %s, filtering %s\n",
                 (unsigned long)f, (f & 1u) ? "yes" : "no", (f & 2u) ? "yes" : "no", (f & 4u) ? "yes" : "no",
                 (f & 8u) ? "yes" : "no");
}

static void parseRasCounter(const char *what, const uint8_t *d, size_t n)
{
    if (n < 2) {
        ucxapp_print("[RAS] invalid %s length %u\n", what, (unsigned)n);
        return;
    }
    ucxapp_print("[RAS] %s: ranging counter %u\n", what, (unsigned)(d[0] | (d[1] << 8)));
}

static void parseCredits(const uint8_t *d, size_t n)
{
    if (n < 1) {
        ucxapp_print("[SPS] invalid credits packet\n");
        return;
    }
    int8_t credits = (int8_t)d[0];
    if (credits == -1) {
        gSpsFlow = false;
        gSpsRemoteCredits = 0;
        ucxapp_print("[SPS] flow control disconnected (credits=-1)\n");
        return;
    }
    gSpsRemoteCredits += credits;
    ucxapp_print("[SPS] credits received: %d (remote credits: %d)\n", (int)credits, gSpsRemoteCredits);
}

/* Number of Wi-Fi provisioning responses received; gatt.wps waits on it. */
static volatile int gWpsResponses;

static void parseWps(const uint8_t *d, size_t n)
{
    char text[257];
    size_t len = n < sizeof(text) - 1 ? n : sizeof(text) - 1;

    memcpy(text, d, len);
    text[len] = '\0';
    ucxapp_print("[WPS] %s\n", text);
    gWpsResponses++;
}

/* Last parsed notification text, mirrored to the Events pane while listening. */
static char gParsed[200];

/* 20-byte packet: 0x55 0x61, then accel, gyro and angle as nine little-endian int16 (x, y, z each). */
static bool parseWit(const uint8_t *d, size_t n)
{
    int16_t w[9];

    if (n < 20 || d[0] != 0x55 || d[1] != 0x61) {
        return false;
    }
    for (int i = 0; i < 9; i++) {
        w[i] = (int16_t)(d[2 + 2 * i] | (d[3 + 2 * i] << 8));
    }
    snprintf(gParsed, sizeof(gParsed),
             "[WIT] acc %7.3f %7.3f %7.3f g  gyro %8.2f %8.2f %8.2f dps  roll %7.2f pitch %7.2f yaw %7.2f deg",
             w[0] * 16.0 / 32768.0, w[1] * 16.0 / 32768.0, w[2] * 16.0 / 32768.0,
             w[3] * 2000.0 / 32768.0, w[4] * 2000.0 / 32768.0, w[5] * 2000.0 / 32768.0,
             w[6] * 180.0 / 32768.0, w[7] * 180.0 / 32768.0, w[8] * 180.0 / 32768.0);
    ucxapp_print("%s\n", gParsed);
    return true;
}

static void printValue(const chr_t *c, const uint8_t *d, size_t n)
{
    if (c != NULL && (isUuid16(c->uuid, c->uuidLen, 0xFFE4) || isUuid128(c->uuid, c->uuidLen, kWitNotify)) &&
        parseWit(d, n)) {
        return;
    }
    if (c != NULL && isUuid128(c->uuid, c->uuidLen, kWpsData)) {
        parseWps(d, n);
        return;
    }
    if (c != NULL && isUuid128(c->uuid, c->uuidLen, kWpsInfo)) {
        ucxapp_print("[WPS] Info: ");
        printText(d, n);
        ucxapp_print("\n");
        return;
    }
    if (c != NULL && c->uuidLen == 2) {
        switch (uuid16Of(c->uuid)) {
        case 0x2A37: parseHr(d, n); return;
        case 0x2A2B: parseCts(d, n); return;
        case 0x2A6E: parseTemp(d, n); return;
        case 0x2A6F: parseHum(d, n); return;
        case 0x2A67: parseLoc(d, n); return;
        case 0x2A56: parseAioDigital(d, n); return;
        case 0x2A58: parseAioAnalog(d, n); return;
        case 0x2A19: parseBattery(d, n); return;
        case 0x2C14: parseRasFeatures(d, n); return;
        case 0x2C18: parseRasCounter("Data Ready", d, n); return;
        case 0x2C19: parseRasCounter("Data Overwritten", d, n); return;
        case 0x2A23:
            ucxapp_print("[DIS] System ID: ");
            for (size_t i = 0; i < n; i++) {
                ucxapp_print("%s%02X", i ? ":" : "", (unsigned)d[i]);
            }
            ucxapp_print("\n");
            return;
        case 0x2A24: case 0x2A25: case 0x2A26: case 0x2A27: case 0x2A28: case 0x2A29: {
            const char *dis = btGetCharacteristicName(uuid16Of(c->uuid));

            ucxapp_print("[DIS] %s: ", dis != NULL ? dis : "?");
            printText(d, n);
            ucxapp_print("\n");
            return;
        }
        default:
            break;
        }
    } else if (c != NULL && isUuid128(c->uuid, c->uuidLen, kSpsCred)) {
        parseCredits(d, n);
        return;
    } else if (c != NULL && (isUuid128(c->uuid, c->uuidLen, kNusTx) || isUuid128(c->uuid, c->uuidLen, kSpsFifo))) {
        ucxapp_print("[%s RX] ", isUuid128(c->uuid, c->uuidLen, kNusTx) ? "NUS" : "SPS");
        printText(d, n);
        ucxapp_print("\n");
        return;
    }
    ucxapp_print("[GATT] ");
    printHex(d, n);
    ucxapp_print("  \"");
    printText(d, n);
    ucxapp_print("\"\n");
}

/* Called from the URC task for GATT notifications and indications. */
void ucxapp_gatt_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    if (ucxapp_ft_client_notification(conn, handle, data, len) || ucxapp_ancs_notification(conn, handle, data, len) ||
        ucxapp_hid_client_notification(conn, handle, data, len)) {
        return;
    }
    const chr_t *c = gCacheConn == conn ? findCharByHandle(handle) : NULL;

    if (c == NULL) {
        ucxapp_print("[notify conn=%d handle=0x%04X] ", (int)conn, (unsigned)handle);
    }
    gParsed[0] = '\0';
    printValue(c, data, len);
    if (gListen) {
        char ev[220];
        if (gParsed[0] != '\0') {
            snprintf(ev, sizeof(ev), "%s", gParsed);
        } else {
            const char *name = c ? charName(c->uuid, c->uuidLen) : NULL;
            int n = snprintf(ev, sizeof(ev), "[GATT] %s 0x%04X:", name ? name : "handle", (unsigned)handle);
            for (size_t i = 0; i < len && i < 16 && n < (int)sizeof(ev) - 4; i++) {
                n += snprintf(ev + n, sizeof(ev) - (size_t)n, " %02X", (unsigned)data[i]);
            }
        }
        ucxapp_ui_event(ev);
    }
}

/* ---- client: raw operations ----------------------------------------- */

static void wpsPoll(ucxapp_t *app);

static bool readHandle(ucxapp_t *app, int32_t conn, int32_t handle, uint8_t *buf, size_t *len)
{
    uByteArray_t v;

    v.pData = buf;
    v.length = 0;
    bool ok = uCxGattClientReadBegin(app->ucx, conn, handle, &v);
    int32_t r = uCxEnd(app->ucx);
    if (!ok || r != 0) {
        return false;
    }
    /* The driver may point pData at its own buffer instead of ours. */
    if (v.pData != buf && v.length <= MAX_VALUE) {
        memcpy(buf, v.pData, v.length);
    }
    *len = v.length;
    return true;
}

/* mode: 0 = off, 1 = notifications, 2 = indications. The CCCD is assumed to follow the value handle. */
static bool setCccd(ucxapp_t *app, int32_t conn, int32_t valueHandle, uint8_t mode)
{
    uGattClientConfig_t cfg = mode == 0 ? U_GATT_CLIENT_CONFIG_NONE
                            : mode == 1 ? U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS
                                        : U_GATT_CLIENT_CONFIG_ENABLE_INDICATIONS;
    int32_t r = uCxGattClientConfigWrite(app->ucx, conn, valueHandle + 1, cfg);
    if (r != 0) {
        ucxapp_print("  CCCD write to handle 0x%04X failed (%d%s)\n", (unsigned)(valueHandle + 1), (int)r, ucxapp_errname(r));
    }
    return r == 0;
}

static int cmdGattServices(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;

    if (argc != 2 || !parseConn(argv[1], &conn)) {
        return UCXAPP_ERR_USAGE;
    }
    int rc = discover(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    for (int i = 0; i < gSvcCount; i++) {
        char u[40];
        const char *name = serviceName(gSvc[i].uuid, gSvc[i].uuidLen);
        uuidStr(gSvc[i].uuid, gSvc[i].uuidLen, u, sizeof(u));
        ucxapp_print("[%d] handles 0x%04X-0x%04X  %s%s%s%s\n", i, (unsigned)gSvc[i].start, (unsigned)gSvc[i].end,
                     u, name ? " (" : "", name ? name : "", name ? ")" : "");
    }
    ucxapp_print("%d service(s), %d characteristic(s)\n", gSvcCount, gChrCount);
    return UCXAPP_OK;
}

static int cmdGattChars(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t only = -1;

    if (argc < 2 || argc > 3 || !parseConn(argv[1], &conn) ||
        (argc == 3 && (!ucxapp_parse_int(argv[2], &only) || only < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    int rc = ensureCache(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    for (int i = 0; i < gChrCount; i++) {
        char u[40];
        char p[8];
        if (only >= 0 && gChr[i].svc != only) {
            continue;
        }
        const char *name = charName(gChr[i].uuid, gChr[i].uuidLen);
        uuidStr(gChr[i].uuid, gChr[i].uuidLen, u, sizeof(u));
        propsStr(gChr[i].props, p);
        ucxapp_print("service %d  handle 0x%04X  %-5s %s%s%s%s\n", (int)gChr[i].svc, (unsigned)gChr[i].handle, p,
                     u, name ? " (" : "", name ? name : "", name ? ")" : "");
    }
    return UCXAPP_OK;
}

static int cmdGattRead(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t handle;
    uint8_t buf[MAX_VALUE];
    size_t len = 0;

    if (argc != 3 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &handle) || handle < 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!readHandle(app, conn, handle, buf, &len)) {
        ucxapp_print("ERROR: read failed\n");
        return UCXAPP_ERR_FAILED;
    }
    printValue(gCacheConn == conn ? findCharByHandle(handle) : NULL, buf, len);
    return UCXAPP_OK;
}

static int cmdGattWrite(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t handle;
    uint8_t data[MAX_VALUE];
    size_t len = 0;

    if (argc < 4 || argc > 5 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &handle) ||
        handle < 0 || !parseData(argv[3], data, sizeof(data), &len) ||
        (argc == 5 && strcmp(argv[4], "norsp") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = argc == 5 ? uCxGattClientWriteNoRsp(app->ucx, conn, handle, data, (int32_t)len)
                          : uCxGattClientWrite(app->ucx, conn, handle, data, (int32_t)len);
    if (r != 0) {
        ucxapp_print("ERROR: write failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_GATT, len, 0);
    ucxapp_print("wrote %u byte(s)\n", (unsigned)len);
    return UCXAPP_OK;
}

static int cmdGattSubscribe(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t handle;
    uint8_t mode = 1;

    if (argc < 3 || argc > 4 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &handle) || handle < 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 4) {
        if (strcmp(argv[3], "notify") == 0) {
            mode = 1;
        } else if (strcmp(argv[3], "indicate") == 0) {
            mode = 2;
        } else if (strcmp(argv[3], "off") == 0) {
            mode = 0;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (!setCccd(app, conn, handle, mode)) {
        ucxapp_print("ERROR: CCCD write failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s on handle 0x%04X\n", mode == 0 ? "off" : mode == 1 ? "notifications enabled" : "indications enabled",
                 (unsigned)handle);
    return UCXAPP_OK;
}

/* Dashboard text; empty unless a client example is listening. */
size_t ucxapp_gatt_summary(char *out, size_t size)
{
    out[0] = '\0';
    if (!gListen) {
        return 0;
    }
    int n = snprintf(out, size, "GATT listening %d", gListenCount);
    return n > 0 ? (size_t)n : 0;
}

static int cmdGattListen(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 0;

    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        for (int i = 0; i < gListenCount; i++) {
            setCccd(app, gListenConn, gListenHandles[i], 0);
        }
        gListenCount = 0;
        gListen = false;
        ucxapp_print("stopped listening\n");
        return UCXAPP_OK;
    }
    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &seconds) || seconds < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 1) {
        wpsPoll(app);
        ucxapp_print(gListen ? "listening on %d subscription(s) (gatt.listen off to stop)\n" : "not listening\n",
                     gListenCount);
    }
    for (int32_t i = 0; i < seconds; i++) {
        U_CX_PORT_SLEEP_MS(1000);
        wpsPoll(app);
    }
    return UCXAPP_OK;
}

/* ---- client: predefined service examples ------------------------------ */

typedef struct {
    const char *key;
    const char *title;
    uint16_t u16;
    const uint8_t *u128;
} example_t;

/* Spec links are at the server tables below; DIS 1.2:
 * https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/DIS_v1.2/out/en/index-en.html */
static const example_t kExamples[] = {
    { "hrs", "Heart Rate",               0x180D, NULL },
    { "cts", "Current Time",             0x1805, NULL },
    { "ess", "Environmental Sensing",    0x181A, NULL },
    { "lns", "Location and Navigation",  0x1819, NULL },
    { "aio", "Automation IO",            0x1815, NULL },
    { "bas", "Battery",                  0x180F, NULL },
    { "dis", "Device Information",       0x180A, NULL },
    { "ras", "Ranging (Channel Sounding)", 0x185B, NULL },
    { "nus", "Nordic UART",              0,      kNusSvc },
    { "sps", "u-blox Serial Port",       0,      kSpsSvc },
    { "wps", "Wi-Fi Provisioning",        0,      kWpsSvc },
    { "wit", "WitMotion IMU",            0xFFE5, NULL },
};

#define EXAMPLE_COUNT (sizeof(kExamples) / sizeof(kExamples[0]))

static const example_t *findExample(const char *key)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; i++) {
        if (strcmp(kExamples[i].key, key) == 0) {
            return &kExamples[i];
        }
    }
    return NULL;
}

/* Discover, then read every readable characteristic of the service and subscribe to every notifiable one. */
static int cmdGattExample(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    bool flow = false;

    if (argc < 3 || argc > 4 || !parseConn(argv[1], &conn)) {
        return UCXAPP_ERR_USAGE;
    }
    const example_t *ex = findExample(argv[2]);
    if (ex == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 4) {
        if (strcmp(ex->key, "sps") != 0 || strcmp(argv[3], "flow") != 0) {
            return UCXAPP_ERR_USAGE;
        }
        flow = true;
    }

    ucxapp_print("--- %s service ---\n", ex->title);
    int rc = ensureCache(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    int svc = ex->u128 ? findService128(ex->u128) : findService16(ex->u16);
    if (svc < 0 && strcmp(ex->key, "wit") == 0) {
        svc = findService128(kWitSvc);
    }
    if (svc < 0) {
        ucxapp_print("ERROR: %s service not found on connection %d\n", ex->title, (int)conn);
        return UCXAPP_ERR_FAILED;
    }

    int subscribed = 0;
    gListenCount = 0;
    gListenConn = conn;
    for (int i = 0; i < gChrCount; i++) {
        const chr_t *c = &gChr[i];
        uint8_t buf[MAX_VALUE];
        size_t len = 0;
        char u[40];

        if (c->svc != svc) {
            continue;
        }
        const char *name = charName(c->uuid, c->uuidLen);
        uuidStr(c->uuid, c->uuidLen, u, sizeof(u));
        ucxapp_print("%s%s%s%s  handle 0x%04X\n", u, name ? " (" : "", name ? name : "", name ? ")" : "",
                     (unsigned)c->handle);

        if (c->props & PROP_READ) {
            if (readHandle(app, conn, c->handle, buf, &len)) {
                printValue(c, buf, len);
            } else {
                ucxapp_print("  read failed\n");
            }
        }
        if (c->props & (PROP_NOTIFY | PROP_INDICATE)) {
            if (setCccd(app, conn, c->handle, (c->props & PROP_NOTIFY) ? 1 : 2)) {
                if (gListenCount < LISTEN_MAX) {
                    gListenHandles[gListenCount++] = c->handle;
                }
                subscribed++;
            } else {
                ucxapp_print("  subscribe failed\n");
            }
        }
    }

    if (strcmp(ex->key, "sps") == 0) {
        const chr_t *cred = findChar128(kSpsCred);
        gSpsFlow = flow;
        gSpsRemoteCredits = 0;
        if (flow && cred != NULL) {
            uint8_t initial = 10;
            if (uCxGattClientWriteNoRsp(app->ucx, conn, cred->handle, &initial, 1) == 0) {
                ucxapp_print("sent %u initial credits\n", (unsigned)initial);
            }
        }
    }
    gListen = subscribed > 0;
    ucxapp_print("%d subscription(s); listening, updates also go to the Events pane (gatt.listen off to stop)\n",
                 subscribed);
    return UCXAPP_OK;
}

static int cmdGattSend(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    char text[256];
    size_t pos = 0;

    if (argc < 4 || !parseConn(argv[1], &conn) || (strcmp(argv[2], "nus") != 0 && strcmp(argv[2], "sps") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 3; i < argc; i++) {
        int n = snprintf(text + pos, sizeof(text) - pos, "%s%s", i > 3 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - pos) {
            ucxapp_print("ERROR: text too long\n");
            return UCXAPP_ERR_FAILED;
        }
        pos += (size_t)n;
    }

    int rc = ensureCache(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    bool sps = strcmp(argv[2], "sps") == 0;
    const chr_t *c = findChar128(sps ? kSpsFifo : kNusRx);
    if (c == NULL) {
        ucxapp_print("ERROR: %s characteristic not found (run gatt.client.%s first)\n", sps ? "SPS FIFO" : "NUS RX", sps ? "sps" : "nus");
        return UCXAPP_ERR_FAILED;
    }
    if (sps && gSpsFlow) {
        if (gSpsRemoteCredits <= 0) {
            ucxapp_print("ERROR: no SPS credits available\n");
            return UCXAPP_ERR_FAILED;
        }
        gSpsRemoteCredits--;
    }
    int32_t r = uCxGattClientWriteNoRsp(app->ucx, conn, c->handle, (const uint8_t *)text, (int32_t)pos);
    if (r != 0) {
        if (sps && gSpsFlow) {
            gSpsRemoteCredits++;
        }
        ucxapp_print("ERROR: send failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent %u byte(s)\n", (unsigned)pos);
    return UCXAPP_OK;
}

static int cmdGattCredits(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t credits;

    if (argc != 3 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &credits) ||
        credits < 1 || credits > 127) {
        return UCXAPP_ERR_USAGE;
    }
    int rc = ensureCache(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    const chr_t *c = findChar128(kSpsCred);
    if (c == NULL) {
        ucxapp_print("ERROR: SPS Credits characteristic not found (run gatt.client.sps first)\n");
        return UCXAPP_ERR_FAILED;
    }
    uint8_t v = (uint8_t)credits;
    if (uCxGattClientWriteNoRsp(app->ucx, conn, c->handle, &v, 1) != 0) {
        ucxapp_print("ERROR: sending credits failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent %d credit(s)\n", (int)credits);
    return UCXAPP_OK;
}

/* ---- server: predefined profiles --------------------------------------- */

typedef struct {
    const char *name;
    uint16_t u16;
    const uint8_t *u128;
    uint8_t props;
    uint8_t initLen;
    uint8_t init[10];
    uint8_t maxLen;
    uint8_t dyn;            /* 1 = current time, 2 = location, built when the profile is set up */
    bool writeUnauth;
    const char *text;       /* initial value as text; overrides init */
} srvChar_t;

typedef struct {
    const char *key;
    const char *title;
    uint16_t u16;
    const uint8_t *u128;
    const srvChar_t *chars;
    size_t nChars;
    bool encrypt;           /* every characteristic needs an encrypted link */
} srvProfile_t;

/* HRS 1.0: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/HRS_v1.0/out/en/index-en.html */
static const srvChar_t kHrsChars[] = {
    { "hrs.measurement", 0x2A37, NULL, PROP_NOTIFY, 2, { 0x00, 60 }, 20, 0, false, NULL },
};
/* BAS 1.1: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/BAS_v1.1/out/en/index-en.html */
static const srvChar_t kBasChars[] = {
    { "bas.level", 0x2A19, NULL, PROP_READ | PROP_NOTIFY, 1, { 100 }, 1, 0, false, NULL },
};
/* ESS 1.0.1: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/ESS_v1.0.1/out/en/index-en.html */
static const srvChar_t kEssChars[] = {
    { "ess.temperature", 0x2A6E, NULL, PROP_READ | PROP_NOTIFY, 2, { 0x29, 0x09 }, 2, 0, false, NULL },
    { "ess.humidity",    0x2A6F, NULL, PROP_READ | PROP_NOTIFY, 2, { 0x94, 0x11 }, 2, 0, false, NULL },
};
/* AIOS 1.0: https://www.bluetooth.com/specifications/specs/automation-io-service-1-0/
 * Digital carries the mandatory Number of Digitals descriptor, added in cmdServerSetup. */
static const srvChar_t kAioChars[] = {
    { "aio.digital", 0x2A56, NULL, PROP_READ | PROP_WRITE | PROP_NOTIFY, 1, { 0x00 }, 1, 0, false, NULL },
    { "aio.analog",  0x2A58, NULL, PROP_READ | PROP_NOTIFY, 2, { 0xF4, 0x01 }, 2, 0, false, NULL },
};
/* LNS 1.0 (PDF only): https://www.bluetooth.com/specifications/specs/location-and-navigation-service-1-0/
 * LN Feature is mandatory; Location and Speed is notify-only. */
static const srvChar_t kLnsChars[] = {
    { "lns.feature",  0x2A6A, NULL, PROP_READ, 4, { 0x04, 0x00, 0x00, 0x00 }, 4, 0, false, NULL },
    { "lns.location", 0x2A67, NULL, PROP_NOTIFY, 10, { 0 }, 10, 2, false, NULL },
};
/* CTS 1.1: https://www.bluetooth.com/specifications/specs/current-time-service-1-1/ */
static const srvChar_t kCtsChars[] = {
    { "cts.time", 0x2A2B, NULL, PROP_READ | PROP_NOTIFY, 10, { 0 }, 10, 1, false, NULL },
};
/* RAS 1.0: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/RAS_v1.0/out/en/index-en.html
 * Mandatory characteristics only (On-demand mode, no optional features); ranging data needs a CS radio. */
static const srvChar_t kRasChars[] = {
    { "ras.features",    0x2C14, NULL, PROP_READ, 4, { 0x00, 0x00, 0x00, 0x00 }, 4, 0, false, NULL },
    { "ras.ondemand",    0x2C16, NULL, PROP_NOTIFY | PROP_INDICATE, 1, { 0x00 }, 244, 0, false, NULL },
    { "ras.control",     0x2C17, NULL, PROP_WRITE_NR | PROP_INDICATE, 1, { 0x00 }, 20, 0, false, NULL },
    { "ras.ready",       0x2C18, NULL, PROP_READ | PROP_NOTIFY | PROP_INDICATE, 2, { 0x00, 0x00 }, 2, 0, false, NULL },
    { "ras.overwritten", 0x2C19, NULL, PROP_READ | PROP_NOTIFY | PROP_INDICATE, 2, { 0x00, 0x00 }, 2, 0, false, NULL },
};
static const srvChar_t kNusChars[] = {
    { "nus.tx", 0, kNusTx, PROP_NOTIFY,   1, { 0x00 }, 244, 0, false, NULL },
    { "nus.rx", 0, kNusRx, PROP_WRITE_NR, 1, { 0x00 }, 244, 0, false, NULL },
};
static const srvChar_t kSpsChars[] = {
    { "sps.fifo",    0, kSpsFifo, PROP_INDICATE | PROP_NOTIFY | PROP_WRITE | PROP_WRITE_NR, 1, { 0x00 }, 244, 0, false, NULL },
    { "sps.credits", 0, kSpsCred, PROP_INDICATE | PROP_NOTIFY | PROP_WRITE | PROP_WRITE_NR, 1, { 0x00 }, 4,   0, false, NULL },
};

static const srvChar_t kWpsChars[] = {
    { "wps.info", 0, kWpsInfo, PROP_READ,  0, { 0 }, 64,  0, false, "NORA-W36 WiFi-Prov 1.0" },
    { "wps.ctrl", 0, kWpsCtrl, PROP_WRITE, 1, { 0 }, 244, 0, false, NULL },
    { "wps.data", 0, kWpsData, PROP_NOTIFY, 1, { 0 }, 244, 0, false, NULL },
};

#define PROFILE(key, title, u16, u128, chars) \
    { key, title, u16, u128, chars, sizeof(chars) / sizeof((chars)[0]), false }
#define PROFILE_ENC(key, title, u16, u128, chars) \
    { key, title, u16, u128, chars, sizeof(chars) / sizeof((chars)[0]), true }

static const srvProfile_t kProfiles[] = {
    PROFILE("hrs", "Heart Rate",              0x180D, NULL,    kHrsChars),
    PROFILE("bas", "Battery",                 0x180F, NULL,    kBasChars),
    PROFILE("ess", "Environmental Sensing",   0x181A, NULL,    kEssChars),
    PROFILE("aio", "Automation IO",           0x1815, NULL,    kAioChars),
    PROFILE("lns", "Location and Navigation", 0x1819, NULL,    kLnsChars),
    PROFILE("cts", "Current Time",            0x1805, NULL,    kCtsChars),
    PROFILE_ENC("ras", "Ranging (Channel Sounding)", 0x185B, NULL, kRasChars),
    PROFILE("nus", "Nordic UART",             0,      kNusSvc, kNusChars),
    PROFILE("sps", "u-blox Serial Port",      0,      kSpsSvc, kSpsChars),
    PROFILE("wps", "Wi-Fi Provisioning",      0,      kWpsSvc, kWpsChars),
};

#define PROFILE_COUNT (sizeof(kProfiles) / sizeof(kProfiles[0]))

typedef struct {
    const char *name;
    int32_t value;
    int32_t cccd;
} srvRec_t;

/* Services stay defined in the module until it restarts, so a profile can be set up only once. */
static srvRec_t gSrv[MAX_SRV_CHARS];
static size_t gSrvCount;
static unsigned gSrvDone;

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

static void buildCtsTime(uint8_t out[10])
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    memset(out, 0, 10);
    if (t == NULL) {
        return;
    }
    put16(out, (unsigned)(t->tm_year + 1900));
    out[2] = (uint8_t)(t->tm_mon + 1);
    out[3] = (uint8_t)t->tm_mday;
    out[4] = (uint8_t)t->tm_hour;
    out[5] = (uint8_t)t->tm_min;
    out[6] = (uint8_t)t->tm_sec;
    out[7] = (uint8_t)(t->tm_wday == 0 ? 7 : t->tm_wday);
}

static void buildLocation(uint8_t out[10])
{
    put16(out, 0x0084);           /* Location Present, Position Status OK */
    put32(out + 2, 557174228u);   /* 55.7174228 deg */
    put32(out + 6, 132137795u);   /* 13.2137795 deg */
}

static void ensureAdvertising(ucxapp_t *app)
{
    uCxBtGetAdvertiseInformation_t info;

    if (uCxBluetoothGetAdvertiseInformation(app->ucx, &info) != 0) {
        ucxapp_print("WARNING: cannot read advertisement state\n");
        return;
    }
    if (info.legacy_adv) {
        return;
    }
    uCxBluetoothSetLegacyAdvertisementConfig(app->ucx, 160, 240);
    if (uCxBluetoothLegacyAdvertisementStart(app->ucx) != 0) {
        ucxapp_print("WARNING: cannot start advertising, remote devices will not be able to connect\n");
        return;
    }
    ucxapp_print("legacy advertising enabled\n");
}

static const srvRec_t *findSrv(const char *key)
{
    int32_t h = 0;
    bool numeric = ucxapp_parse_int(key, &h);

    for (size_t i = 0; i < gSrvCount; i++) {
        if (strcmp(gSrv[i].name, key) == 0 || (numeric && gSrv[i].value == h)) {
            return &gSrv[i];
        }
    }
    return NULL;
}

static const srvProfile_t *findProfile(const char *key, size_t *index)
{
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        if (strcmp(kProfiles[i].key, key) == 0) {
            *index = i;
            return &kProfiles[i];
        }
    }
    return NULL;
}

static int cmdServerSetup(ucxapp_t *app, int argc, char *argv[])
{
    size_t idx = 0;
    int32_t svcHandle;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    const srvProfile_t *p = findProfile(argv[1], &idx);
    if (p == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    if (gSrvDone & (1u << idx)) {
        ucxapp_print("ERROR: %s is already defined; restart the module to define it again\n", p->title);
        return UCXAPP_ERR_FAILED;
    }
    if (gSrvCount + p->nChars > MAX_SRV_CHARS) {
        ucxapp_print("ERROR: too many characteristics defined\n");
        return UCXAPP_ERR_FAILED;
    }

    ucxapp_bt_security_defaults(app);
    ensureAdvertising(app);

    uint8_t svcUuid16[2] = { (uint8_t)(p->u16 >> 8), (uint8_t)(p->u16 & 0xFF) };
    int32_t r = p->u128 ? uCxGattServerServiceDefine(app->ucx, p->u128, 16, &svcHandle)
                        : uCxGattServerServiceDefine(app->ucx, svcUuid16, 2, &svcHandle);
    if (r != 0) {
        ucxapp_print("ERROR: cannot define %s service (%d%s)\n", p->title, (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s service defined (handle %d)\n", p->title, (int)svcHandle);

    size_t first = gSrvCount;
    for (size_t i = 0; i < p->nChars; i++) {
        const srvChar_t *c = &p->chars[i];
        uint8_t init[MAX_INIT];
        size_t initLen = c->initLen;
        uint8_t uuid16[2] = { (uint8_t)(c->u16 >> 8), (uint8_t)(c->u16 & 0xFF) };
        uint8_t props = c->props;
        uCxGattServerCharDefine_t rsp;

        memset(init, 0, sizeof(init));
        memcpy(init, c->init, sizeof(c->init));
        if (c->text != NULL) {
            initLen = strlen(c->text);
            memcpy(init, c->text, initLen);
        }
        if (c->dyn == 1) {
            buildCtsTime(init);
        } else if (c->dyn == 2) {
            buildLocation(init);
        }
        r = uCxGattServerCharDefine6(app->ucx,
                                     c->u128 ? c->u128 : uuid16, c->u128 ? 16 : 2,
                                     &props, 1,
                                     p->encrypt ? U_GATT_SERVER_READ_SECURITY_UNAUTHENTICATED
                                                : U_GATT_SERVER_READ_SECURITY_NONE,
                                     (c->writeUnauth || p->encrypt) ? U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED
                                                                    : U_GATT_SERVER_WRITE_SECURITY_NONE,
                                     init, (int32_t)initLen, c->maxLen, &rsp);
        if (r != 0) {
            ucxapp_print("ERROR: cannot add %s (%d%s)\n", c->name, (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        if (c->u16 == 0x2A56) {
            static const uint8_t kNumDigitalsUuid[2] = { 0x29, 0x09 };
            static const uint8_t kNumDigitals = 4;   /* 2 bits per digital, one octet */
            int32_t dh;
            if (uCxGattServerDescriptorDefine4(app->ucx, kNumDigitalsUuid, 2, U_GATT_SERVER_READ_SECURITY_NONE,
                                               U_GATT_SERVER_WRITE_SECURITY_NONE, &kNumDigitals, 1, &dh) != 0) {
                ucxapp_print("WARNING: cannot add Number of Digitals descriptor\n");
            }
        }
        gSrv[gSrvCount].name = c->name;
        gSrv[gSrvCount].value = rsp.value_handle;
        gSrv[gSrvCount].cccd = rsp.cccd_handle;
        gSrvCount++;
        ucxapp_print("  %s: handle %d, CCCD %d\n", c->name, (int)rsp.value_handle, (int)rsp.cccd_handle);
    }

    r = uCxGattServerServiceActivate(app->ucx);
    if (r != 0) {
        gSrvCount = first;
        ucxapp_print("ERROR: cannot activate %s service (%d%s)\n", p->title, (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    gSrvDone |= 1u << idx;
    ucxapp_print("%s service active; connect a client, or use gatt.server.demo\n", p->title);
    return UCXAPP_OK;
}

static int cmdServerList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;

    for (size_t i = 0; i < gSrvCount; i++) {
        ucxapp_print("%-16s value handle %d, CCCD %d\n", gSrv[i].name, (int)gSrv[i].value, (int)gSrv[i].cccd);
    }
    ucxapp_print("%u characteristic(s)\n", (unsigned)gSrvCount);
    return UCXAPP_OK;
}

static int cmdServerSet(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t data[MAX_VALUE];
    size_t len = 0;

    if (argc != 3 || !parseData(argv[2], data, sizeof(data), &len)) {
        return UCXAPP_ERR_USAGE;
    }
    const srvRec_t *s = findSrv(argv[1]);
    int32_t handle = 0;
    if (s != NULL) {
        handle = s->value;
    } else if (!ucxapp_parse_int(argv[1], &handle) || handle < 0) {
        ucxapp_print("ERROR: unknown characteristic %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    int32_t r = uCxGattServerSetAttrValue(app->ucx, handle, data, (int32_t)len);
    if (r != 0) {
        ucxapp_print("ERROR: set value failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("value set (%u byte(s))\n", (unsigned)len);
    return UCXAPP_OK;
}

static int serverSend(ucxapp_t *app, int argc, char *argv[], bool indicate)
{
    int32_t conn;
    uint8_t data[MAX_VALUE];
    size_t len = 0;

    if (argc != 4 || !parseConn(argv[1], &conn) || !parseData(argv[3], data, sizeof(data), &len)) {
        return UCXAPP_ERR_USAGE;
    }
    const srvRec_t *s = findSrv(argv[2]);
    int32_t handle = 0;
    if (s != NULL) {
        handle = s->value;
    } else if (!ucxapp_parse_int(argv[2], &handle) || handle < 0) {
        ucxapp_print("ERROR: unknown characteristic %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    int32_t r = indicate ? uCxGattServerSendIndication(app->ucx, conn, handle, data, (int32_t)len)
                         : uCxGattServerSendNotification(app->ucx, conn, handle, data, (int32_t)len);
    if (r != 0) {
        ucxapp_print("ERROR: %s failed (%d); the client must have subscribed\n",
                     indicate ? "indication" : "notification", (int)r);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_GATT, len, 0);
    ucxapp_print("%s sent (%u byte(s))\n", indicate ? "indication" : "notification", (unsigned)len);
    return UCXAPP_OK;
}

static int cmdServerNotify(ucxapp_t *app, int argc, char *argv[])
{
    return serverSend(app, argc, argv, false);
}

static int cmdServerIndicate(ucxapp_t *app, int argc, char *argv[])
{
    return serverSend(app, argc, argv, true);
}

static bool notifyNamed(ucxapp_t *app, int32_t conn, const char *name, const uint8_t *d, int32_t n)
{
    const srvRec_t *s = findSrv(name);
    return s != NULL && uCxGattServerSendNotification(app->ucx, conn, s->value, d, n) == 0;
}

bool ucxapp_gatt_notify_nus(ucxapp_t *app, const uint8_t *data, size_t len)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    return conn >= 0 && notifyNamed(app, conn, "nus.tx", data, (int32_t)len);
}

bool ucxapp_gatt_notify_custom(ucxapp_t *app, const uint8_t *data, size_t len)
{
    int32_t conn = ucxapp_handle_current(UCXAPP_HANDLE_CONN);

    return conn >= 0 && notifyNamed(app, conn, "custom.tx", data, (int32_t)len);
}

/* Sends one second's worth of simulated values; false if a notification failed. */
static bool demoTick(ucxapp_t *app, int32_t conn, const srvProfile_t *p, int32_t t)
{
    uint8_t d[10];
    bool ok = true;

    if (strcmp(p->key, "hrs") == 0) {
        d[0] = 0;
        d[1] = (uint8_t)(58 + (t * 7) % 15);
        ok = notifyNamed(app, conn, "hrs.measurement", d, 2);
    } else if (strcmp(p->key, "bas") == 0) {
        d[0] = (uint8_t)(t < 90 ? 100 - t : 10);
        ok = notifyNamed(app, conn, "bas.level", d, 1);
    } else if (strcmp(p->key, "ess") == 0) {
        put16(d, (unsigned)(2300 + (t % 10) * 10));
        ok = notifyNamed(app, conn, "ess.temperature", d, 2);
        put16(d, (unsigned)(4500 + (t % 10) * 20));
        ok = notifyNamed(app, conn, "ess.humidity", d, 2) && ok;
    } else if (strcmp(p->key, "aio") == 0) {
        d[0] = (uint8_t)(t & 1);
        ok = notifyNamed(app, conn, "aio.digital", d, 1);
        put16(d, (unsigned)(500 + (t % 10) * 25));
        ok = notifyNamed(app, conn, "aio.analog", d, 2) && ok;
    } else if (strcmp(p->key, "lns") == 0) {
        buildLocation(d);
        ok = notifyNamed(app, conn, "lns.location", d, 10);
    } else if (strcmp(p->key, "cts") == 0) {
        buildCtsTime(d);
        ok = notifyNamed(app, conn, "cts.time", d, 10);
    } else if (strcmp(p->key, "ras") == 0) {
        const srvRec_t *s = findSrv("ras.ready");
        put16(d, (unsigned)(t & 0x0FFF));
        ok = s != NULL && uCxGattServerSendIndication(app->ucx, conn, s->value, d, 2) == 0;
    }
    return ok;
}

/* Sends simulated values once per second for a defined profile (nus and sps have no simulated data). */
static int cmdServerDemo(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t seconds = LISTEN_DEFAULT_S;
    size_t idx = 0;

    if (argc < 3 || argc > 4 || !parseConn(argv[1], &conn) ||
        (argc == 4 && (!ucxapp_parse_int(argv[3], &seconds) || seconds < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    const srvProfile_t *p = findProfile(argv[2], &idx);
    if (p == NULL || p->u128 != NULL) {
        return UCXAPP_ERR_USAGE;
    }
    if (!(gSrvDone & (1u << idx))) {
        ucxapp_print("ERROR: %s is not defined; run gatt.server.%s first\n", p->title, p->key);
        return UCXAPP_ERR_FAILED;
    }

    int failures = 0;
    for (int32_t t = 0; t < seconds; t++) {
        if (!demoTick(app, conn, p, t) && ++failures == 3) {
            ucxapp_print("ERROR: notifications are failing; has the client subscribed?\n");
            return UCXAPP_ERR_FAILED;
        }
        U_CX_PORT_SLEEP_MS(1000);
    }
    ucxapp_print("demo finished\n");
    return UCXAPP_OK;
}

/* ---- server: write events, Wi-Fi provisioning ---------------------------- */

#ifndef WPS_QUEUE
#define WPS_QUEUE   4
#endif
#define WPS_CMD_MAX 257
#define WPS_SCANS   8

typedef struct {
    int32_t conn;
    char text[WPS_CMD_MAX];
} wpsCmd_t;

/* The URC task queues commands; the foreground (gatt.listen or a server example) runs them. */
static wpsCmd_t gWpsQueue[WPS_QUEUE];
static volatile int gWpsHead;
static volatile int gWpsTail;

/* Called from the URC task when a remote client writes to a local characteristic. */
void ucxapp_gatt_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    ucxapp_stats_add(UCXAPP_STATS_GATT, 0, len);
    if (ucxapp_ft_server_write(conn, handle, data, len) || ucxapp_hid_server_write(conn, handle, data, len) ||
        ucxapp_notify_server_write(conn, handle, data, len)) {
        return;
    }
    const srvRec_t *ctrl = findSrv("wps.ctrl");
    const srvRec_t *nusRx = findSrv("nus.rx");
    if (nusRx != NULL && nusRx->value == handle) {
        ucxapp_stream_on_ble_rx(data, len);
        return;
    }
    const srvRec_t *customRx = findSrv("custom.rx");
    if (customRx != NULL && customRx->value == handle) {
        ucxapp_stream_on_ble_rx(data, len);
        return;
    }
    if (ctrl != NULL && ctrl->value == handle) {
        int next = (gWpsHead + 1) % WPS_QUEUE;
        if (next == gWpsTail) {
            ucxapp_print("[WPS] queue full, command dropped\n");
            return;
        }
        size_t n = len < WPS_CMD_MAX - 1 ? len : WPS_CMD_MAX - 1;
        memcpy(gWpsQueue[gWpsHead].text, data, n);
        gWpsQueue[gWpsHead].text[n] = '\0';
        gWpsQueue[gWpsHead].conn = conn;
        gWpsHead = next;
        ucxapp_print("[WPS] command received: %s\n", gWpsQueue[(next + WPS_QUEUE - 1) % WPS_QUEUE].text);
        return;
    }
    ucxapp_print("[write conn=%d handle=%d] ", (int)conn, (int)handle);
    printHex(data, len);
    ucxapp_print("  \"");
    printText(data, len);
    ucxapp_print("\"\n");
}

/* Called from the URC task when a remote client reads a host-managed characteristic. */
void ucxapp_gatt_server_read(int32_t conn, int32_t handle)
{
    ucxapp_print("[read conn=%d handle=%d] answer with gatt.server.readresp or gatt.server.error\n", (int)conn, (int)handle);
}

static void wpsRespond(ucxapp_t *app, int32_t conn, const char *text)
{
    const srvRec_t *data = findSrv("wps.data");

    if (data != NULL && uCxGattServerSendNotification(app->ucx, conn, data->value, (const uint8_t *)text,
                                                      (int32_t)strlen(text)) == 0) {
        ucxapp_print("[WPS] response: %s\n", text);
    } else {
        ucxapp_print("[WPS] response not sent (client not subscribed?): %s\n", text);
    }
}

static void wpsScan(ucxapp_t *app, int32_t conn)
{
    struct { int32_t rssi; char ssid[34]; } found[WPS_SCANS];
    uCxWifiStationScanDefault_t net;
    int count = 0;
    char resp[64];

    uCxAtClientSetCommandTimeout(app->ucx->pAtClient, 60000, false);
    uCxWifiStationScanDefaultBegin(app->ucx);
    while (uCxWifiStationScanDefaultGetNext(app->ucx, &net)) {
        if (count < WPS_SCANS && net.ssid != NULL && net.ssid[0] != '\0') {
            found[count].rssi = net.rssi;
            snprintf(found[count].ssid, sizeof(found[count].ssid), "%s", net.ssid);
            count++;
        }
    }
    uCxEnd(app->ucx);
    for (int i = 0; i < count; i++) {
        snprintf(resp, sizeof(resp), "SCAN_RESULT:%d:%s", (int)found[i].rssi, found[i].ssid);
        wpsRespond(app, conn, resp);
    }
    wpsRespond(app, conn, "OK:SCAN_DONE");
}

static void wpsHandle(ucxapp_t *app, int32_t conn, char *cmd)
{
    uCxHandle_t *ucx = app->ucx;
    char resp[200];
    int32_t r;

    resp[0] = '\0';
    if (strncmp(cmd, "SET_CONFIG:", 11) == 0) {
        char *ssid = cmd + 11;
        char *first = strchr(ssid, ':');
        char *last = strrchr(ssid, ':');

        if (first == NULL || first == last || first == ssid) {
            snprintf(resp, sizeof(resp), "ERROR:INVALID_CONFIG");
        } else {
            *first = '\0';
            *last = '\0';
            const char *password = first + 1;
            const char *auth = last + 1;

            r = uCxWifiStationSetConnectionParams(ucx, 0, ssid);
            if (r != 0) {
                snprintf(resp, sizeof(resp), "ERROR:SET_SSID_FAILED:%d", (int)r);
            } else {
                r = strcmp(auth, "OPEN") == 0
                        ? uCxWifiStationSetSecurityOpen(ucx, 0)
                        : uCxWifiStationSetSecurityWpa(ucx, 0, password,
                                                       strcmp(auth, "WPA3") == 0 ? U_WIFI_WPA_THRESHOLD_WPA3
                                                                                 : U_WIFI_WPA_THRESHOLD_WPA2);
                if (r != 0) {
                    snprintf(resp, sizeof(resp), "ERROR:AUTH_FAILED:%d", (int)r);
                } else if ((r = uCxWifiStationConnect(ucx, 0)) != 0) {
                    snprintf(resp, sizeof(resp), "ERROR:CONNECT_FAILED:%d", (int)r);
                } else {
                    snprintf(resp, sizeof(resp), "OK:CONNECTING:%.64s", ssid);
                }
            }
        }
    } else if (strcmp(cmd, "GET_STATUS") == 0) {
        char ssid[40];
        char ip[40];

        if (ucxapp_wifi_get_status(ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
            snprintf(resp, sizeof(resp), "STATUS:CONNECTED:%s:%s", ssid, ip);
        } else {
            snprintf(resp, sizeof(resp), "STATUS:DISCONNECTED");
        }
    } else if (strcmp(cmd, "START_SCAN") == 0) {
        wpsRespond(app, conn, "OK:SCAN_STARTED");
        wpsScan(app, conn);
        return;
    } else if (strcmp(cmd, "CONNECT") == 0) {
        r = uCxWifiStationConnect(ucx, 0);
        if (r == 0) {
            snprintf(resp, sizeof(resp), "OK:CONNECTING");
        } else {
            snprintf(resp, sizeof(resp), "ERROR:CONNECT_FAILED:%d", (int)r);
        }
    } else if (strcmp(cmd, "FORGET") == 0) {
        uCxWifiStationDisconnect(ucx);
        snprintf(resp, sizeof(resp), "OK:FORGOTTEN");
    } else {
        snprintf(resp, sizeof(resp), "ERROR:UNKNOWN_CMD");
    }
    wpsRespond(app, conn, resp);
}

static void wpsPoll(ucxapp_t *app)
{
    while (gWpsTail != gWpsHead) {
        wpsCmd_t cmd = gWpsQueue[gWpsTail];
        gWpsTail = (gWpsTail + 1) % WPS_QUEUE;
        wpsHandle(app, cmd.conn, cmd.text);
    }
}

void ucxapp_gatt_ensure_advertising(ucxapp_t *app)
{
    ucxapp_bt_security_defaults(app);
    ensureAdvertising(app);
}

int ucxapp_gatt_server_ensure(ucxapp_t *app, const char *key)
{
    size_t idx = 0;
    char *av[2] = { "gatt.server.setup", (char *)key };

    if (findProfile(key, &idx) == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    return (gSrvDone & (1u << idx)) ? UCXAPP_OK : cmdServerSetup(app, 2, av);
}

static bool gCustomDone;
static uint8_t gCustomUuid[3][16];   /* service, rx, tx */

int ucxapp_gatt_server_custom(ucxapp_t *app, const uint8_t svc[16], const uint8_t rx[16], const uint8_t tx[16])
{
    int32_t svcHandle;
    int32_t r;
    size_t first = gSrvCount;

    if (gCustomDone) {
        if (memcmp(gCustomUuid[0], svc, 16) == 0 && memcmp(gCustomUuid[1], rx, 16) == 0 &&
            memcmp(gCustomUuid[2], tx, 16) == 0) {
            return UCXAPP_OK;
        }
        ucxapp_print("ERROR: a custom service is already defined; restart the module to define another\n");
        return UCXAPP_ERR_FAILED;
    }
    if (gSrvCount + 2 > MAX_SRV_CHARS) {
        ucxapp_print("ERROR: too many characteristics defined\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_bt_security_defaults(app);
    ensureAdvertising(app);
    r = uCxGattServerServiceDefine(app->ucx, svc, 16, &svcHandle);
    if (r != 0) {
        ucxapp_print("ERROR: cannot define the custom service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 0; i < 2; i++) {
        uint8_t props = i == 0 ? PROP_NOTIFY : (PROP_WRITE | PROP_WRITE_NR);
        uint8_t init = 0;
        uCxGattServerCharDefine_t rsp;

        r = uCxGattServerCharDefine6(app->ucx, i == 0 ? tx : rx, 16, &props, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                     i == 0 ? U_GATT_SERVER_WRITE_SECURITY_NONE
                                            : U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED,
                                     &init, 1, 244, &rsp);
        if (r != 0) {
            gSrvCount = first;
            ucxapp_print("ERROR: cannot add a custom characteristic (%d%s)\n", (int)r, ucxapp_errname(r));
            return UCXAPP_ERR_FAILED;
        }
        gSrv[gSrvCount].name = i == 0 ? "custom.tx" : "custom.rx";
        gSrv[gSrvCount].value = rsp.value_handle;
        gSrv[gSrvCount].cccd = rsp.cccd_handle;
        gSrvCount++;
    }
    r = uCxGattServerServiceActivate(app->ucx);
    if (r != 0) {
        gSrvCount = first;
        ucxapp_print("ERROR: cannot activate the custom service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    memcpy(gCustomUuid[0], svc, 16);
    memcpy(gCustomUuid[1], rx, 16);
    memcpy(gCustomUuid[2], tx, 16);
    gCustomDone = true;
    return UCXAPP_OK;
}

/* ---- per-service example commands --------------------------------------- */

/* gatt.client.<svc> <conn> [flow] */
static int clientExample(ucxapp_t *app, int argc, char *argv[], const char *key)
{
    char *av[4] = { argv[0], argv[1], (char *)key, argc > 2 ? argv[2] : NULL };

    if (argc < 2 || argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    return cmdGattExample(app, argc + 1, av);
}

/* gatt.server.<svc> [seconds]: define the service, then simulate values for any connected client. */
static int serverExample(ucxapp_t *app, int argc, char *argv[], const char *key)
{
    char *av[2] = { argv[0], (char *)key };
    int32_t seconds = 0;
    size_t idx = 0;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &seconds) || seconds < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    const srvProfile_t *p = findProfile(key, &idx);
    if (!(gSrvDone & (1u << idx))) {
        int rc = cmdServerSetup(app, 2, av);
        if (rc != UCXAPP_OK) {
            return rc;
        }
    }
    if (seconds == 0) {
        return UCXAPP_OK;
    }
    if (p->u128 != NULL) {
        ucxapp_print("serving %s for %d s\n", p->title, (int)seconds);
        for (int32_t t = 0; t < seconds; t++) {
            U_CX_PORT_SLEEP_MS(1000);
            wpsPoll(app);
        }
        ucxapp_print("example finished\n");
        return UCXAPP_OK;
    }
    ucxapp_print("simulating %s for %d s; connect a client and subscribe\n", p->title, (int)seconds);
    for (int32_t t = 0; t < seconds; t++) {
        uCxBtListConnections_t c;
        int32_t conn = -1;

        uCxBluetoothListConnectionsBegin(app->ucx);
        while (uCxBluetoothListConnectionsGetNext(app->ucx, &c)) {
            if (conn < 0) {
                conn = c.conn_handle;
            }
        }
        uCxEnd(app->ucx);
        if (conn >= 0) {
            demoTick(app, conn, p, t);
        }
        U_CX_PORT_SLEEP_MS(1000);
    }
    ucxapp_print("example finished\n");
    return UCXAPP_OK;
}

#define EXAMPLE_CMDS(k)                                                                    \
    static int cmdClientEx_##k(ucxapp_t *app, int argc, char *argv[])                       \
    {                                                                                       \
        return clientExample(app, argc, argv, #k);                                          \
    }                                                                                       \
    static int cmdServerEx_##k(ucxapp_t *app, int argc, char *argv[])                       \
    {                                                                                       \
        return serverExample(app, argc, argv, #k);                                          \
    }

EXAMPLE_CMDS(hrs)
EXAMPLE_CMDS(cts)
EXAMPLE_CMDS(ess)
EXAMPLE_CMDS(lns)
EXAMPLE_CMDS(aio)
EXAMPLE_CMDS(bas)
EXAMPLE_CMDS(nus)
EXAMPLE_CMDS(sps)
EXAMPLE_CMDS(wps)
EXAMPLE_CMDS(ras)

static int cmdClientEx_wit(ucxapp_t *app, int argc, char *argv[])
{
    return clientExample(app, argc, argv, "wit");
}

static int cmdClientEx_dis(ucxapp_t *app, int argc, char *argv[])
{
    return clientExample(app, argc, argv, "dis");
}

/* ---- Wi-Fi provisioning client and advanced operations -------------------- */

/* gatt.wps <conn> <status|scan|connect|forget|config <ssid> <password> [OPEN|WPA2|WPA3]|raw <text>> */
static int cmdGattWps(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    char text[WPS_CMD_MAX];
    bool scan = false;

    if (argc < 3 || !parseConn(argv[1], &conn)) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[2], "status") == 0 && argc == 3) {
        snprintf(text, sizeof(text), "GET_STATUS");
    } else if (strcmp(argv[2], "scan") == 0 && argc == 3) {
        snprintf(text, sizeof(text), "START_SCAN");
        scan = true;
    } else if (strcmp(argv[2], "connect") == 0 && argc == 3) {
        snprintf(text, sizeof(text), "CONNECT");
    } else if (strcmp(argv[2], "forget") == 0 && argc == 3) {
        snprintf(text, sizeof(text), "FORGET");
    } else if (strcmp(argv[2], "config") == 0 && (argc == 5 || argc == 6)) {
        const char *auth = argc == 6 ? argv[5] : "WPA2";
        if (strcmp(auth, "OPEN") != 0 && strcmp(auth, "WPA2") != 0 && strcmp(auth, "WPA3") != 0) {
            return UCXAPP_ERR_USAGE;
        }
        snprintf(text, sizeof(text), "SET_CONFIG:%s:%s:%s", argv[3], argv[4], auth);
    } else if (strcmp(argv[2], "raw") == 0 && argc == 4) {
        snprintf(text, sizeof(text), "%s", argv[3]);
    } else {
        return UCXAPP_ERR_USAGE;
    }

    int rc = ensureCache(app, conn);
    if (rc != UCXAPP_OK) {
        return rc;
    }
    const chr_t *ctrl = findChar128(kWpsCtrl);
    if (ctrl == NULL) {
        ucxapp_print("ERROR: Wi-Fi provisioning service not found on connection %d\n", (int)conn);
        return UCXAPP_ERR_FAILED;
    }
    int before = gWpsResponses;
    int32_t r = uCxGattClientWrite(app->ucx, conn, ctrl->handle, (const uint8_t *)text, (int32_t)strlen(text));
    if (r != 0) {
        ucxapp_print("ERROR: write failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("sent: %s\n", argc > 2 && strcmp(argv[2], "config") == 0 ? "SET_CONFIG:..." : text);
    for (int i = 0; i < (scan ? 20 : 10); i++) {
        U_CX_PORT_SLEEP_MS(500);
        if (gWpsResponses != before && !scan && i >= 1) {
            break;
        }
    }
    if (gWpsResponses == before) {
        ucxapp_print("no response (is the notification subscription active? run gatt.client.wps first)\n");
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static bool parseHex(const char *s, uint32_t max, uint32_t *out)
{
    char *end;
    unsigned long v;

    if (s[0] == '\0') {
        return false;
    }
    v = strtoul(s, &end, 16);
    if (*end != '\0' || v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* gatt.writelong <conn> <handle> <data> [offset] [reliable] [more] */
static int cmdGattWriteLong(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t handle;
    int32_t offset = 0;
    uint8_t data[MAX_VALUE];
    size_t len = 0;
    bool reliable = false;
    bool more = false;

    if (argc < 4 || argc > 7 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &handle) || handle < 0 ||
        !parseData(argv[3], data, sizeof(data), &len) ||
        (argc > 4 && (!ucxapp_parse_int(argv[4], &offset) || offset < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "reliable") == 0) {
            reliable = true;
        } else if (strcmp(argv[i], "more") == 0) {
            more = true;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    int32_t r = uCxGattClientWriteLong(app->ucx, conn, handle, data, (int32_t)len,
                                       reliable ? U_GATT_CLIENT_RELIABLE_YES : U_GATT_CLIENT_RELIABLE_NO,
                                       more ? U_GATT_CLIENT_FLAG_MORE_DATA : U_GATT_CLIENT_FLAG_FINAL_DATA, offset);
    if (r != 0) {
        ucxapp_print("ERROR: write long failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("wrote %u byte(s) at offset %d%s\n", (unsigned)len, (int)offset, more ? " (more data follows)" : "");
    return UCXAPP_OK;
}

/* gatt.server.servicechanged <conn> <start-handle> <end-handle> */
static int cmdServerServiceChanged(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t start;
    int32_t end;

    if (argc != 4 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[2], &start) || start <= 0 ||
        !ucxapp_parse_int(argv[3], &end) || end < start) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = uCxGattServerSendServiceChangedInd(app->ucx, conn, start, end);
    if (r != 0) {
        ucxapp_print("ERROR: service changed indication failed (%d%s); the client must be connected and bonded\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("service changed indication sent for handles 0x%04X-0x%04X\n", (unsigned)start, (unsigned)end);
    return UCXAPP_OK;
}

/* gatt.server.error <conn> <read|write> <att-error-code> */
static int cmdServerError(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    int32_t code;
    bool isRead;

    if (argc != 4 || !parseConn(argv[1], &conn) || !ucxapp_parse_int(argv[3], &code) || code < 1 || code > 255) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[2], "read") == 0) {
        isRead = true;
    } else if (strcmp(argv[2], "write") == 0) {
        isRead = false;
    } else {
        return UCXAPP_ERR_USAGE;
    }
    uint8_t b = (uint8_t)code;
    int32_t r = isRead ? uCxGattServerReadReqError(app->ucx, conn, &b, 1) : uCxGattServerWriteReqError(app->ucx, conn, &b, 1);
    if (r != 0) {
        ucxapp_print("ERROR: error response failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s error 0x%02X sent\n", isRead ? "read" : "write", (unsigned)b);
    return UCXAPP_OK;
}

/* gatt.server.readresp <conn> <hex|text:abc> */
static int cmdServerReadResp(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;
    uint8_t data[MAX_VALUE];
    size_t len = 0;

    if (argc != 3 || !parseConn(argv[1], &conn) || !parseData(argv[2], data, sizeof(data), &len)) {
        return UCXAPP_ERR_USAGE;
    }
    int32_t r = uCxGattServerReadReqResponse(app->ucx, conn, data, (int32_t)len);
    if (r != 0) {
        ucxapp_print("ERROR: read response failed (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("read response sent (%u byte(s))\n", (unsigned)len);
    return UCXAPP_OK;
}

/* gatt.server.writeresp <conn> */
static int cmdServerWriteResp(ucxapp_t *app, int argc, char *argv[])
{
    int32_t conn;

    if (argc != 2 || !parseConn(argv[1], &conn)) {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxGattServerWriteReqResponse(app->ucx, conn) != 0) {
        ucxapp_print("ERROR: write response failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("write response sent\n");
    return UCXAPP_OK;
}

static bool parseSecurity(const char *s, int32_t *out)
{
    return ucxapp_parse_int(s, out) && *out >= 0 && *out <= 2;
}

/* gatt.server.hostchar <service-uuid16-hex> <char-uuid16-hex> <props-hex> [read-sec 0-2] [write-sec 0-2] */
static int cmdServerHostChar(ucxapp_t *app, int argc, char *argv[])
{
    uint32_t svc;
    uint32_t chr;
    uint32_t props;
    int32_t rsec = 0;
    int32_t wsec = 0;
    static const uGattServerReadSecurity_t kRead[] = {
        U_GATT_SERVER_READ_SECURITY_NONE, U_GATT_SERVER_READ_SECURITY_UNAUTHENTICATED,
        U_GATT_SERVER_READ_SECURITY_AUTHENTICATED };
    static const uGattServerWriteSecurity_t kWrite[] = {
        U_GATT_SERVER_WRITE_SECURITY_NONE, U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED,
        U_GATT_SERVER_WRITE_SECURITY_AUTHENTICATED };

    if (argc < 4 || argc > 6 || !parseHex(argv[1], 0xFFFF, &svc) || !parseHex(argv[2], 0xFFFF, &chr) ||
        !parseHex(argv[3], 0xFF, &props) || (argc > 4 && !parseSecurity(argv[4], &rsec)) ||
        (argc > 5 && !parseSecurity(argv[5], &wsec))) {
        return UCXAPP_ERR_USAGE;
    }
    uint8_t svcUuid[2] = { (uint8_t)(svc >> 8), (uint8_t)(svc & 0xFF) };
    uint8_t chrUuid[2] = { (uint8_t)(chr >> 8), (uint8_t)(chr & 0xFF) };
    uint8_t p = (uint8_t)props;
    int32_t svcHandle;
    uCxGattServerHostCharDefine_t rsp;

    ucxapp_gatt_ensure_advertising(app);
    int32_t r = uCxGattServerServiceDefine(app->ucx, svcUuid, 2, &svcHandle);
    if (r != 0) {
        ucxapp_print("ERROR: cannot define service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    r = uCxGattServerHostCharDefine(app->ucx, chrUuid, 2, &p, 1, kRead[rsec], kWrite[wsec], &rsp);
    if (r != 0) {
        ucxapp_print("ERROR: cannot define host characteristic (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    r = uCxGattServerServiceActivate(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: cannot activate service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("host characteristic: value handle %d, CCCD %d\n", (int)rsp.value_handle, (int)rsp.cccd_handle);
    ucxapp_print("client reads and writes are reported; answer with gatt.server.readresp, gatt.server.writeresp or gatt.server.error\n");
    return UCXAPP_OK;
}

/* ---- registration ------------------------------------------------------ */

static const ucxapp_cmd_t gGattCmds[] = {
    { "gatt.services",   "[conn]",                               "Discover and list the remote GATT services and characteristics", UCXAPP_CMD_NEEDS_DEVICE, cmdGattServices },
    { "gatt.chars",      "[conn] [service-index]",               "List the remote characteristics (discovers if needed)",         UCXAPP_CMD_NEEDS_DEVICE, cmdGattChars },
    { "gatt.read",       "[conn] <handle>",                      "Read a remote characteristic and decode known types",           UCXAPP_CMD_NEEDS_DEVICE, cmdGattRead },
    { "gatt.write",      "[conn] <handle> <hex|text:abc> [norsp]", "Write a remote characteristic",                            UCXAPP_CMD_NEEDS_DEVICE, cmdGattWrite },
    { "gatt.subscribe",  "[conn] <handle> [notify|indicate|off]", "Set the CCCD (assumed at value handle + 1)",                  UCXAPP_CMD_NEEDS_DEVICE, cmdGattSubscribe },
    { "gatt.client.hrs", "[conn]", "Heart Rate client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_hrs },
    { "gatt.client.cts", "[conn]", "Current Time client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_cts },
    { "gatt.client.ess", "[conn]", "Environmental Sensing client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_ess },
    { "gatt.client.lns", "[conn]", "Location and Navigation client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_lns },
    { "gatt.client.aio", "[conn]", "Automation IO client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_aio },
    { "gatt.client.bas", "[conn]", "Battery client: discover, read, decode and subscribe", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_bas },
    { "gatt.client.dis", "[conn]", "Device Information client: discover and read", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_dis },
    { "gatt.client.ras", "[conn]", "Ranging Service (Channel Sounding) client: read features, subscribe to ranging data", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_ras },
    { "gatt.client.nus", "[conn]", "Nordic UART client: discover and subscribe (send with gatt.send)", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_nus },
    { "gatt.client.sps", "[conn] [flow]", "u-blox Serial Port client: discover and subscribe (flow: SPS credits)", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_sps },
    { "gatt.client.wit", "[conn]", "WitMotion IMU client: subscribe and print acceleration, gyro and angles", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_wit },
    { "gatt.client.wps", "[conn]", "Wi-Fi Provisioning client: discover, read info and subscribe (control with gatt.wps)", UCXAPP_CMD_NEEDS_DEVICE, cmdClientEx_wps },
    { "gatt.wps", "[conn] <status|scan|connect|forget|config <ssid> <pw> [OPEN|WPA2|WPA3]|raw <text>>", "Send a Wi-Fi provisioning command and wait for the response", UCXAPP_CMD_NEEDS_DEVICE, cmdGattWps },
    { "gatt.writelong", "[conn] <handle> <hex|text:abc> [offset] [reliable] [more]", "Write a long characteristic value (prepare/execute write)", UCXAPP_CMD_NEEDS_DEVICE, cmdGattWriteLong },
    { "gatt.server.hrs", "[seconds]", "Heart Rate server; with seconds, simulate values for a connected client", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_hrs },
    { "gatt.server.cts", "[seconds]", "Current Time server; with seconds, simulate values for a connected client", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_cts },
    { "gatt.server.ess", "[seconds]", "Environmental Sensing server; with seconds, simulate values", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_ess },
    { "gatt.server.lns", "[seconds]", "Location and Navigation server; with seconds, simulate values", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_lns },
    { "gatt.server.aio", "[seconds]", "Automation IO server; with seconds, simulate values", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_aio },
    { "gatt.server.bas", "[seconds]", "Battery server; with seconds, simulate values", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_bas },
    { "gatt.server.ras", "[seconds]", "Ranging Service (Channel Sounding) server; with seconds, simulate Data Ready", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_ras },
    { "gatt.server.nus", "[seconds]", "Nordic UART server (writes from the client are printed)", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_nus },
    { "gatt.server.sps", "[seconds]", "u-blox Serial Port server (writes from the client are printed)", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_sps },
    { "gatt.server.wps", "[seconds]", "Wi-Fi Provisioning server; with seconds, run client commands for that long", UCXAPP_CMD_NEEDS_DEVICE, cmdServerEx_wps },
    { "gatt.server.servicechanged", "[conn] <start> <end>", "Send a Service Changed indication for a handle range", UCXAPP_CMD_NEEDS_DEVICE, cmdServerServiceChanged },
    { "gatt.server.error", "[conn] <read|write> <att-code>", "Answer a pending read or write request with an ATT error", UCXAPP_CMD_NEEDS_DEVICE, cmdServerError },
    { "gatt.server.readresp", "[conn] <hex|text:abc>", "Answer a pending host-characteristic read request", UCXAPP_CMD_NEEDS_DEVICE, cmdServerReadResp },
    { "gatt.server.writeresp", "[conn]", "Acknowledge a pending host-characteristic write request", UCXAPP_CMD_NEEDS_DEVICE, cmdServerWriteResp },
    { "gatt.server.hostchar", "<svc-uuid> <char-uuid> <props-hex> [rsec] [wsec]", "Define a service with a host-managed characteristic", UCXAPP_CMD_NEEDS_DEVICE, cmdServerHostChar },
    { "gatt.send",       "[conn] <nus|sps> <text>",              "Send text to the remote NUS or SPS service",                    UCXAPP_CMD_NEEDS_DEVICE, cmdGattSend },
    { "gatt.credits",    "[conn] <1-127>",                       "Send SPS credits to the remote device",                         UCXAPP_CMD_NEEDS_DEVICE, cmdGattCredits },
    { "gatt.listen",     "[seconds|off]",                        "Show listen state; with seconds wait for notifications; off unsubscribes", UCXAPP_CMD_NEEDS_DEVICE, cmdGattListen },
    { "gatt.server.list",     "",                                "List the local characteristics defined by the server examples", UCXAPP_CMD_NEEDS_DEVICE, cmdServerList },
    { "gatt.server.set",      "<name|handle> <hex|text:abc>",    "Set a local characteristic value",                              UCXAPP_CMD_NEEDS_DEVICE, cmdServerSet },
    { "gatt.server.notify",   "[conn] <name|handle> <hex|text:abc>", "Send a notification to a connected client",                UCXAPP_CMD_NEEDS_DEVICE, cmdServerNotify },
    { "gatt.server.indicate", "[conn] <name|handle> <hex|text:abc>", "Send an indication to a connected client",                 UCXAPP_CMD_NEEDS_DEVICE, cmdServerIndicate },
    { "gatt.server.demo",     "[conn] <hrs|bas|ess|aio|lns|cts|ras> [seconds]", "Send simulated values once per second",           UCXAPP_CMD_NEEDS_DEVICE, cmdServerDemo },
};

void ucxapp_register_gatt_cmds(void)
{
    ucxapp_cmd_register(gGattCmds, sizeof(gGattCmds) / sizeof(gGattCmds[0]));
}
