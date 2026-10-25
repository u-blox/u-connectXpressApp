#include "ucxapp_cmd.h"
#include "ucxapp_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_bluetooth.h"
#include "u_cx_gatt_client.h"
#include "u_cx_gatt_server.h"
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/* HIDS 1.0: https://www.bluetooth.com/specifications/specs/human-interface-device-service-1-0/
 * HOGP 1.1: https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile/
 * DIS 1.2:  https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/DIS_v1.2/out/en/index-en.html */

#define PROP_READ      0x02
#define PROP_WRITE_NR  0x04
#define PROP_WRITE     0x08
#define PROP_NOTIFY    0x10

#define HIDMOD_CTRL  0x01
#define HIDMOD_SHIFT 0x02
#define HIDMOD_ALT   0x04
#define HIDMOD_GUI   0x08

#define RID_KBD   1
#define RID_MOUSE 2
#define RID_MEDIA 3

/* Keyboard with LEDs (ID 1), 3-button mouse with wheel (ID 2), consumer control (ID 3). */
static const uint8_t kReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, RID_KBD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0x05, 0x08, 0x95, 0x05, 0x75, 0x01, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, RID_MOUSE, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, RID_MEDIA,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0
};

/* Version 1.11, country 0, normally connectable + remote wake. */
static const uint8_t kHidInfo[] = { 0x11, 0x01, 0x00, 0x03 };

/* Input reports; the boot ones are used while the host has selected the boot protocol. */
enum { IN_BOOT_KBD, IN_BOOT_MOUSE, IN_KBD, IN_MOUSE, IN_MEDIA, IN_COUNT };

typedef struct {
    const char *name;
    int32_t value;
    int32_t cccd;
    volatile bool on;
} hidIn_t;

static hidIn_t gIn[IN_COUNT] = {
    { "boot keyboard", -1, -1, false }, { "boot mouse", -1, -1, false }, { "keyboard", -1, -1, false },
    { "mouse", -1, -1, false },         { "media", -1, -1, false },
};

static bool gDefined;
static int32_t gService = -1;
static int32_t gBootOut = -1;
static int32_t gKbdOut = -1;
static int32_t gProtocolMode = -1;
static int32_t gControlPoint = -1;
static volatile int32_t gConn = -1;
static volatile bool gProtocolBoot;

bool ucxapp_hid_server_write(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    if (!gDefined || len == 0) {
        return false;
    }
    for (int i = 0; i < IN_COUNT; i++) {
        if (handle == gIn[i].cccd) {
            bool on = len >= 2 && (data[0] & 0x01) != 0;
            gIn[i].on = on;
            if (on) {
                gConn = conn;
            }
            ucxapp_print("[HID] %s input report notifications %s (conn %d)\n", gIn[i].name, on ? "enabled" : "disabled",
                         (int)conn);
            return true;
        }
    }
    if (handle == gProtocolMode) {
        gProtocolBoot = data[0] == 0;
        ucxapp_print("[HID] protocol mode: %s\n", gProtocolBoot ? "boot" : "report");
        return true;
    }
    if (handle == gKbdOut || handle == gBootOut) {
        ucxapp_print("[HID] keyboard LEDs 0x%02X (num=%d caps=%d scroll=%d)\n", (unsigned)data[0], (data[0] & 1) != 0,
                     (data[0] & 2) != 0, (data[0] & 4) != 0);
        return true;
    }
    if (handle == gControlPoint) {
        ucxapp_print("[HID] control point: %s\n", data[0] == 0 ? "suspend" : "exit suspend");
        return true;
    }
    return false;
}

/* Defines a characteristic and, if the firmware did not add one, its CCCD. Returns the value handle or -1. */
static uGattServerReadSecurity_t gRsec = U_GATT_SERVER_READ_SECURITY_UNAUTHENTICATED;
static uGattServerWriteSecurity_t gWsec = U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED;

static int32_t defineChar(ucxapp_t *app, uint16_t uuid, uint8_t props, const uint8_t *init, int32_t initLen,
                          int32_t *cccd)
{
    uint8_t u[2] = { (uint8_t)(uuid >> 8), (uint8_t)(uuid & 0xFF) };
    uCxGattServerCharDefine_t rsp;

    int32_t r = uCxGattServerCharDefine6(app->ucx, u, 2, &props, 1, gRsec, gWsec, init, initLen, initLen, &rsp);
    if (r != 0) {
        ucxapp_print("ERROR: cannot add characteristic 0x%04X (%d%s)\n", (unsigned)uuid, (int)r, ucxapp_errname(r));
        return -1;
    }
    if (cccd != NULL) {
        *cccd = rsp.cccd_handle;
        if (rsp.cccd_handle <= 0) {
            static const uint8_t cccdUuid[2] = { 0x29, 0x02 };
            static const uint8_t off[2] = { 0, 0 };
            if (uCxGattServerDescriptorDefine4(app->ucx, cccdUuid, 2, gRsec, gWsec, off, 2, cccd) != 0) {
                ucxapp_print("WARNING: cannot add CCCD for 0x%04X\n", (unsigned)uuid);
            }
        }
    }
    return rsp.value_handle;
}

static bool defineDescriptor(ucxapp_t *app, uint16_t uuid, const uint8_t *value, int32_t len)
{
    uint8_t u[2] = { (uint8_t)(uuid >> 8), (uint8_t)(uuid & 0xFF) };
    int32_t handle;
    int32_t r = uCxGattServerDescriptorDefine4(app->ucx, u, 2, gRsec, U_GATT_SERVER_WRITE_SECURITY_NONE, value, len,
                                               &handle);
    if (r != 0) {
        ucxapp_print("WARNING: cannot add descriptor 0x%04X (%d%s)\n", (unsigned)uuid, (int)r, ucxapp_errname(r));
    }
    return r == 0;
}

static int cmdHidSetup(ucxapp_t *app, int argc, char *argv[])
{
    static const uint8_t kSvcUuid[2] = { 0x18, 0x12 };
    static const uint8_t kDisUuid[2] = { 0x18, 0x0A };
    static const uint8_t kExtRef[2] = { 0x2A, 0x19 };   /* External Report Reference: Battery Level */
    static const uint8_t kKbdInRef[2] = { RID_KBD, 0x01 };
    static const uint8_t kKbdOutRef[2] = { RID_KBD, 0x02 };
    static const uint8_t kMouseInRef[2] = { RID_MOUSE, 0x01 };
    static const uint8_t kMediaInRef[2] = { RID_MEDIA, 0x01 };
    static const uint8_t kProtoReport[1] = { 0x01 };
    /* Vendor ID source USB-IF, VID 0x1546 (u-blox), PID 0, version 1.0. */
    static const uint8_t kPnpId[7] = { 0x02, 0x46, 0x15, 0x00, 0x00, 0x00, 0x01 };
    /* Flags, Appearance (Generic HID), complete 16-bit UUIDs (HID), complete local name. */
    static const uint8_t kAdv[] = { 0x02, 0x01, 0x06, 0x03, 0x19, 0xC0, 0x03, 0x03, 0x03, 0x12, 0x18,
                                    0x0B, 0x09, 'u', '-', 'b', 'l', 'o', 'x', ' ', 'H', 'I', 'D' };
    static const uint8_t kZero8[8] = { 0 };
    uint8_t props;
    int32_t svc;
    int32_t r;
    bool auth = argc > 1 && strcmp(argv[1], "auth") == 0;

    if (argc > 1 && !auth) {
        return UCXAPP_ERR_USAGE;
    }
    if (gDefined) {
        ucxapp_print("HID service is already defined; restart the module to define it again\n");
        return UCXAPP_ERR_FAILED;
    }
    /* HOGP: every HIDS characteristic needs LE security mode 1, level 2 (or 3 with auth). */
    gRsec = auth ? U_GATT_SERVER_READ_SECURITY_AUTHENTICATED : U_GATT_SERVER_READ_SECURITY_UNAUTHENTICATED;
    gWsec = auth ? U_GATT_SERVER_WRITE_SECURITY_AUTHENTICATED : U_GATT_SERVER_WRITE_SECURITY_UNAUTHENTICATED;

    ucxapp_bt_security_defaults(app);
    uCxBluetoothSetSecurityMode(app->ucx, auth ? U_BT_SECURITY_MODE_AUTHENTICATED : U_BT_SECURITY_MODE_UNAUTHENTICATED);
    uCxBluetoothSetLocalName(app->ucx, "u-blox HID");
    if (uCxBluetoothSetLegacyAdvertiseDataAndType1(app->ucx, kAdv, sizeof(kAdv)) != 0) {
        ucxapp_print("WARNING: cannot advertise the HID UUID and appearance\n");
    }
    ucxapp_gatt_ensure_advertising(app);

    r = uCxGattServerServiceDefine(app->ucx, kSvcUuid, 2, &gService);
    if (r != 0) {
        ucxapp_print("ERROR: cannot define HID service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    if (defineChar(app, 0x2A4A, PROP_READ, kHidInfo, sizeof(kHidInfo), NULL) < 0 ||
        defineChar(app, 0x2A4B, PROP_READ, kReportMap, sizeof(kReportMap), NULL) < 0) {
        return UCXAPP_ERR_FAILED;
    }
    defineDescriptor(app, 0x2907, kExtRef, 2);
    gProtocolMode = defineChar(app, 0x2A4E, PROP_READ | PROP_WRITE_NR, kProtoReport, 1, NULL);

    /* Boot characteristics carry no Report Reference descriptor (HIDS 1.0). */
    gIn[IN_BOOT_KBD].value = defineChar(app, 0x2A22, PROP_READ | PROP_NOTIFY, kZero8, 8, &gIn[IN_BOOT_KBD].cccd);
    gBootOut = defineChar(app, 0x2A32, PROP_READ | PROP_WRITE_NR | PROP_WRITE, kZero8, 1, NULL);
    gIn[IN_BOOT_MOUSE].value = defineChar(app, 0x2A33, PROP_READ | PROP_NOTIFY, kZero8, 3, &gIn[IN_BOOT_MOUSE].cccd);

    gIn[IN_KBD].value = defineChar(app, 0x2A4D, PROP_READ | PROP_NOTIFY, kZero8, 8, &gIn[IN_KBD].cccd);
    defineDescriptor(app, 0x2908, kKbdInRef, 2);
    gKbdOut = defineChar(app, 0x2A4D, PROP_READ | PROP_WRITE_NR | PROP_WRITE, kZero8, 1, NULL);
    defineDescriptor(app, 0x2908, kKbdOutRef, 2);
    gIn[IN_MOUSE].value = defineChar(app, 0x2A4D, PROP_READ | PROP_NOTIFY, kZero8, 4, &gIn[IN_MOUSE].cccd);
    defineDescriptor(app, 0x2908, kMouseInRef, 2);
    gIn[IN_MEDIA].value = defineChar(app, 0x2A4D, PROP_READ | PROP_NOTIFY, kZero8, 2, &gIn[IN_MEDIA].cccd);
    defineDescriptor(app, 0x2908, kMediaInRef, 2);

    gControlPoint = defineChar(app, 0x2A4C, PROP_WRITE_NR, kZero8, 1, NULL);

    r = uCxGattServerServiceActivate(app->ucx);
    if (r != 0) {
        ucxapp_print("ERROR: cannot activate HID service (%d%s)\n", (int)r, ucxapp_errname(r));
        return UCXAPP_ERR_FAILED;
    }
    gDefined = true;
    ucxapp_print("HID service active: keyboard, mouse and media keys\n");

    /* HOGP also requires Device Information (PnP ID) and Battery. */
    props = PROP_READ;
    uCxGattServerCharDefine_t rsp;
    if (uCxGattServerServiceDefine(app->ucx, kDisUuid, 2, &svc) != 0 ||
        uCxGattServerCharDefine6(app->ucx, (const uint8_t[]){ 0x2A, 0x50 }, 2, &props, 1, U_GATT_SERVER_READ_SECURITY_NONE,
                                 U_GATT_SERVER_WRITE_SECURITY_NONE, kPnpId, sizeof(kPnpId), sizeof(kPnpId), &rsp) != 0 ||
        uCxGattServerServiceActivate(app->ucx) != 0) {
        ucxapp_print("WARNING: device information (PnP ID) not available\n");
    }
    if (ucxapp_gatt_server_ensure(app, "bas") != UCXAPP_OK) {
        ucxapp_print("WARNING: battery service not available\n");
    }
    ucxapp_print("pair from the host, wait for '[HID] ... notifications enabled', then use hid.key, hid.text, hid.mouse, "
                 "hid.click or hid.media\n");
    return UCXAPP_OK;
}

static int cmdHidStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;

    if (!gDefined) {
        ucxapp_print("hid: not set up (run hid.setup)\n");
        return UCXAPP_OK;
    }
    ucxapp_print("hid: service %d, protocol %s, host conn %d\n", (int)gService, gProtocolBoot ? "boot" : "report",
                 (int)gConn);
    for (int i = 0; i < IN_COUNT; i++) {
        ucxapp_print("  %-13s handle %d, CCCD %d, notifications %s\n", gIn[i].name, (int)gIn[i].value, (int)gIn[i].cccd,
                     gIn[i].on ? "on" : "off");
    }
    return UCXAPP_OK;
}

static bool keyFor(char c, uint8_t *mod, uint8_t *code)
{
    *mod = 0;
    if (c >= 'a' && c <= 'z') {
        *code = (uint8_t)(0x04 + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
        *code = (uint8_t)(0x04 + (c - 'A'));
        *mod = HIDMOD_SHIFT;
    } else if (c >= '1' && c <= '9') {
        *code = (uint8_t)(0x1E + (c - '1'));
    } else {
        switch (c) {
        case '0':  *code = 0x27; break;
        case ' ':  *code = 0x2C; break;
        case '\n': *code = 0x28; break;
        case '.':  *code = 0x37; break;
        case ',':  *code = 0x36; break;
        case '-':  *code = 0x2D; break;
        case '/':  *code = 0x38; break;
        case ';':  *code = 0x33; break;
        case '\'': *code = 0x34; break;
        case '!':  *code = 0x1E; *mod = HIDMOD_SHIFT; break;
        case '@':  *code = 0x1F; *mod = HIDMOD_SHIFT; break;
        case '?':  *code = 0x38; *mod = HIDMOD_SHIFT; break;
        case ':':  *code = 0x33; *mod = HIDMOD_SHIFT; break;
        case '_':  *code = 0x2D; *mod = HIDMOD_SHIFT; break;
        default:   return false;
        }
    }
    return true;
}

static bool subscribed(int report, int boot)
{
    return gConn >= 0 && (gIn[report].on || (boot >= 0 && gIn[boot].on));
}

/* Waits for the host to subscribe; boot is the matching boot input or -1. */
static bool reportsEnabled(int report, int boot)
{
    if (!gDefined) {
        ucxapp_print("ERROR: HID not set up (run hid.setup)\n");
        return false;
    }
    if (!subscribed(report, boot)) {
        ucxapp_print("waiting up to 60 s for the host to connect and subscribe...\n");
        int connectedTicks = 0;
        for (int i = 0; i < 120 && !subscribed(report, boot); i++) {
            U_CX_PORT_SLEEP_MS(500);
            /* A bonded host reconnects without rewriting its CCCDs, so no URC reports the subscription. */
            if (gConn >= 0 && ++connectedTicks >= 10) {
                ucxapp_print("host connected without a subscribe event; sending anyway\n");
                gIn[report].on = true;
                break;
            }
        }
    }
    if (!subscribed(report, boot)) {
        ucxapp_print("ERROR: the host has not enabled notifications on the %s input report yet\n", gIn[report].name);
        return false;
    }
    return true;
}

/* Boot protocol hosts get the boot input report. */
static int pickInput(int report, int boot)
{
    return (boot >= 0 && gIn[boot].on && (gProtocolBoot || !gIn[report].on)) ? boot : report;
}

static bool sendReport(ucxapp_t *app, int in, const uint8_t *d, size_t n)
{
    if (uCxGattServerSendNotification(app->ucx, gConn, gIn[in].value, d, (int32_t)n) != 0) {
        ucxapp_print("ERROR: %s report notification failed\n", gIn[in].name);
        return false;
    }
    return true;
}

/* Press and release one key. */
static bool sendKey(ucxapp_t *app, uint8_t mod, uint8_t code)
{
    int in = pickInput(IN_KBD, IN_BOOT_KBD);
    uint8_t down[8] = { mod, 0, code, 0, 0, 0, 0, 0 };
    uint8_t up[8] = { 0 };

    if (!sendReport(app, in, down, sizeof(down))) {
        return false;
    }
    U_CX_PORT_SLEEP_MS(50);
    return sendReport(app, in, up, sizeof(up));
}

/* Buttons bit0 left, bit1 right, bit2 middle; the boot report has no wheel. */
static bool sendMouse(ucxapp_t *app, uint8_t buttons, int dx, int dy, int wheel)
{
    int in = pickInput(IN_MOUSE, IN_BOOT_MOUSE);
    uint8_t r[4] = { buttons, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(int8_t)wheel };

    return sendReport(app, in, r, in == IN_BOOT_MOUSE ? 3 : 4);
}

static int clampStep(int v)
{
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

/* hid.mouse <dx> <dy> [wheel]: large moves are split into steps of at most 127. */
static int cmdHidMouse(ucxapp_t *app, int argc, char *argv[])
{
    int32_t dx;
    int32_t dy;
    int32_t wheel = 0;

    if (argc < 3 || argc > 4 || !ucxapp_parse_int(argv[1], &dx) || !ucxapp_parse_int(argv[2], &dy) ||
        (argc == 4 && !ucxapp_parse_int(argv[3], &wheel)) || abs(dx) > 10000 || abs(dy) > 10000 || abs(wheel) > 127) {
        return UCXAPP_ERR_USAGE;
    }
    if (!reportsEnabled(IN_MOUSE, IN_BOOT_MOUSE)) {
        return UCXAPP_ERR_FAILED;
    }
    int w = wheel;
    do {
        int sx = clampStep(dx);
        int sy = clampStep(dy);
        if (!sendMouse(app, 0, sx, sy, w)) {
            return UCXAPP_ERR_FAILED;
        }
        dx -= sx;
        dy -= sy;
        w = 0;
        U_CX_PORT_SLEEP_MS(10);
    } while (dx != 0 || dy != 0);
    ucxapp_print("mouse moved\n");
    return UCXAPP_OK;
}

/* hid.click [left|right|middle] [double] */
static int cmdHidClick(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t button = 0x01;
    int clicks = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "left") == 0) {
            button = 0x01;
        } else if (strcmp(argv[i], "right") == 0) {
            button = 0x02;
        } else if (strcmp(argv[i], "middle") == 0) {
            button = 0x04;
        } else if (strcmp(argv[i], "double") == 0) {
            clicks = 2;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (!reportsEnabled(IN_MOUSE, IN_BOOT_MOUSE)) {
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 0; i < clicks; i++) {
        if (!sendMouse(app, button, 0, 0, 0)) {
            return UCXAPP_ERR_FAILED;
        }
        U_CX_PORT_SLEEP_MS(40);
        if (!sendMouse(app, 0, 0, 0, 0)) {
            return UCXAPP_ERR_FAILED;
        }
        U_CX_PORT_SLEEP_MS(80);
    }
    ucxapp_print("clicked\n");
    return UCXAPP_OK;
}

/* Consumer page usages (HID Usage Tables, page 0x0C). */
static const struct {
    const char *name;
    uint16_t usage;
} kMedia[] = {
    { "play", 0x00CD },     { "next", 0x00B5 },       { "prev", 0x00B6 },     { "stop", 0x00B7 },
    { "mute", 0x00E2 },     { "volup", 0x00E9 },      { "voldown", 0x00EA },  { "brightup", 0x006F },
    { "brightdown", 0x0070 }, { "home", 0x0223 },     { "back", 0x0224 },     { "search", 0x0221 },
};

static const char *mediaName(uint16_t usage)
{
    for (size_t i = 0; i < sizeof(kMedia) / sizeof(kMedia[0]); i++) {
        if (kMedia[i].usage == usage) {
            return kMedia[i].name;
        }
    }
    return NULL;
}

static int cmdHidMedia(ucxapp_t *app, int argc, char *argv[])
{
    uint16_t usage = 0;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < sizeof(kMedia) / sizeof(kMedia[0]); i++) {
        if (strcmp(argv[1], kMedia[i].name) == 0) {
            usage = kMedia[i].usage;
        }
    }
    if (usage == 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!reportsEnabled(IN_MEDIA, -1)) {
        return UCXAPP_ERR_FAILED;
    }
    uint8_t down[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t up[2] = { 0, 0 };
    if (!sendReport(app, IN_MEDIA, down, 2)) {
        return UCXAPP_ERR_FAILED;
    }
    U_CX_PORT_SLEEP_MS(50);
    if (!sendReport(app, IN_MEDIA, up, 2)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s sent\n", argv[1]);
    return UCXAPP_OK;
}

/* hid.key <key> [ctrl|shift|alt|gui ...]: key is one character or enter, esc, tab, backspace, space */
static int cmdHidKey(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t mod = 0;
    uint8_t code = 0;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "enter") == 0) {
        code = 0x28;
    } else if (strcmp(argv[1], "esc") == 0) {
        code = 0x29;
    } else if (strcmp(argv[1], "backspace") == 0) {
        code = 0x2A;
    } else if (strcmp(argv[1], "tab") == 0) {
        code = 0x2B;
    } else if (strcmp(argv[1], "space") == 0) {
        code = 0x2C;
    } else if (strlen(argv[1]) != 1 || !keyFor(argv[1][0], &mod, &code)) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "ctrl") == 0) {
            mod |= HIDMOD_CTRL;
        } else if (strcmp(argv[i], "shift") == 0) {
            mod |= HIDMOD_SHIFT;
        } else if (strcmp(argv[i], "alt") == 0) {
            mod |= HIDMOD_ALT;
        } else if (strcmp(argv[i], "gui") == 0) {
            mod |= HIDMOD_GUI;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (!reportsEnabled(IN_KBD, IN_BOOT_KBD) || !sendKey(app, mod, code)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("key sent\n");
    return UCXAPP_OK;
}

/* hid.text <text...>: types the text; words are joined with spaces. */
static int cmdHidText(ucxapp_t *app, int argc, char *argv[])
{
    char text[200];
    size_t pos = 0;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 1; i < argc; i++) {
        int n = snprintf(text + pos, sizeof(text) - pos, "%s%s", i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - pos) {
            ucxapp_print("ERROR: text too long\n");
            return UCXAPP_ERR_FAILED;
        }
        pos += (size_t)n;
    }
    for (size_t i = 0; i < pos; i++) {
        uint8_t m;
        uint8_t k;
        if (!keyFor(text[i], &m, &k)) {
            ucxapp_print("ERROR: no key mapping for '%c'\n", text[i]);
            return UCXAPP_ERR_FAILED;
        }
    }
    if (!reportsEnabled(IN_KBD, IN_BOOT_KBD)) {
        return UCXAPP_ERR_FAILED;
    }
    for (size_t i = 0; i < pos; i++) {
        uint8_t m;
        uint8_t k;
        keyFor(text[i], &m, &k);
        if (!sendKey(app, m, k)) {
            return UCXAPP_ERR_FAILED;
        }
        U_CX_PORT_SLEEP_MS(50);
    }
    ucxapp_print("typed %u character(s)\n", (unsigned)pos);
    return UCXAPP_OK;
}

/* ---- HID host (HOGP Report Host and Boot Host) --------------------------- */

#define CLI_MAX_REPORTS 12
#define CLI_MAX_IDS     8
#define CLI_MAP_MAX     512

enum { KIND_OTHER, KIND_KBD, KIND_MOUSE, KIND_MEDIA };

/* Input field layout of one report ID, taken from the report map (bit offsets, -1 if absent). */
typedef struct {
    uint8_t id;
    uint8_t kind;
    uint16_t pos;
    int16_t btnOff, btnBits;
    int16_t xOff, yOff, xyBits;
    int16_t wOff, wBits;
    int16_t modOff;
    int16_t keysOff, keysCount, keyBits;
    int16_t mediaOff, mediaBits;
} cliLayout_t;

typedef struct {
    int32_t handle;
    int32_t cccd;
    uint8_t id;
    bool boot;
    uint8_t kind;
    uint8_t prevKeys[8];
} cliReport_t;

static volatile bool gCliActive;
static int32_t gCliConn = -1;
static cliReport_t gCliRep[CLI_MAX_REPORTS];
static int gCliRepCount;
static cliLayout_t gCliLay[CLI_MAX_IDS];
static int gCliLayCount;

static cliLayout_t *layoutFor(uint8_t id, uint8_t kind)
{
    for (int i = 0; i < gCliLayCount; i++) {
        if (gCliLay[i].id == id) {
            return &gCliLay[i];
        }
    }
    if (gCliLayCount >= CLI_MAX_IDS) {
        return NULL;
    }
    cliLayout_t *l = &gCliLay[gCliLayCount++];
    memset(l, 0, sizeof(*l));
    l->id = id;
    l->kind = kind;
    l->btnOff = l->xOff = l->yOff = l->wOff = l->modOff = l->keysOff = l->mediaOff = -1;
    return l;
}

static void parseReportMap(const uint8_t *m, size_t n)
{
    uint16_t page = 0;
    uint32_t size = 0;
    uint32_t count = 0;
    uint8_t id = 0;
    uint8_t appKind = KIND_OTHER;
    int depth = 0;
    uint32_t usages[16];
    int nUsages = 0;
    uint32_t usageMin = 0;
    bool haveMin = false;

    gCliLayCount = 0;
    for (size_t i = 0; i < n;) {
        uint8_t b = m[i];
        if (b == 0xFE) {   /* long item */
            i += 3u + (i + 1 < n ? m[i + 1] : 0u);
            continue;
        }
        size_t len = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + len > n) {
            break;
        }
        uint32_t v = 0;
        for (size_t k = 0; k < len; k++) {
            v |= (uint32_t)m[i + 1 + k] << (8 * k);
        }
        uint8_t type = (uint8_t)((b >> 2) & 3);
        uint8_t tag = (uint8_t)(b >> 4);
        i += 1 + len;

        if (type == 1) {   /* global */
            if (tag == 0) {
                page = (uint16_t)v;
            } else if (tag == 7) {
                size = v;
            } else if (tag == 9) {
                count = v;
            } else if (tag == 8) {
                id = (uint8_t)v;
                layoutFor(id, appKind);
            }
        } else if (type == 2) {   /* local */
            uint32_t full = len == 4 ? v : ((uint32_t)page << 16) | v;
            if (tag == 0 && nUsages < 16) {
                usages[nUsages++] = full;
            } else if (tag == 1) {
                usageMin = full;
                haveMin = true;
            }
        } else if (type == 0) {   /* main */
            if (tag == 0xA) {
                if (depth == 0 && v == 1) {
                    uint32_t u = nUsages > 0 ? usages[0] : 0;
                    appKind = (u == 0x10006 || u == 0x10007) ? KIND_KBD
                            : (u == 0x10002 || u == 0x10001) ? KIND_MOUSE
                            : (u == 0xC0001) ? KIND_MEDIA : KIND_OTHER;
                }
                depth++;
            } else if (tag == 0xC) {
                depth = depth > 0 ? depth - 1 : 0;
            } else if (tag == 8) {   /* Input */
                cliLayout_t *l = layoutFor(id, appKind);
                if (l != NULL && (v & 1) == 0) {
                    for (uint32_t f = 0; f < count && f < 64; f++) {
                        uint32_t u = nUsages > 0 ? usages[f < (uint32_t)nUsages ? f : (uint32_t)nUsages - 1]
                                   : haveMin ? usageMin + f : ((uint32_t)page << 16);
                        int16_t at = (int16_t)(l->pos + f * size);
                        uint16_t up = (uint16_t)(u >> 16);
                        if (up == 0x09 && l->btnOff < 0) {
                            l->btnOff = at;
                            l->btnBits = (int16_t)(size * count);
                        } else if (u == 0x10030) {
                            l->xOff = at;
                            l->xyBits = (int16_t)size;
                        } else if (u == 0x10031) {
                            l->yOff = at;
                        } else if (u == 0x10038) {
                            l->wOff = at;
                            l->wBits = (int16_t)size;
                        } else if (up == 0x07 && (v & 2) != 0 && (u & 0xFFFF) >= 0xE0 && l->modOff < 0) {
                            l->modOff = at;
                        } else if (up == 0x07 && (v & 2) == 0 && l->keysOff < 0) {
                            l->keysOff = at;
                            l->keysCount = (int16_t)count;
                            l->keyBits = (int16_t)size;
                        } else if (up == 0x0C && l->mediaOff < 0) {
                            l->mediaOff = at;
                            l->mediaBits = (int16_t)size;
                        }
                    }
                }
                if (l != NULL) {
                    l->pos = (uint16_t)(l->pos + size * count);
                }
            }
            nUsages = 0;
            haveMin = false;
        }
    }
}

static int32_t getBits(const uint8_t *d, size_t n, int off, int bits, bool sign)
{
    uint32_t v = 0;

    if (off < 0 || bits <= 0 || bits > 32 || (size_t)(off + bits) > n * 8) {
        return 0;
    }
    for (int b = 0; b < bits; b++) {
        if (d[(off + b) / 8] & (1u << ((off + b) % 8))) {
            v |= 1u << b;
        }
    }
    if (sign && bits < 32 && (v & (1u << (bits - 1)))) {
        v |= ~0u << bits;
    }
    return (int32_t)v;
}

static void keyName(uint8_t code, char *out, size_t size)
{
    static const char kPunct[] = "\n\x1b\b\t -=[]\\#;'`,./";   /* 0x28..0x38 */
    static const char *const kNames[] = { "enter", "esc", "backspace", "tab", "space" };

    if (code >= 0x04 && code <= 0x1D) {
        snprintf(out, size, "%c", 'a' + (code - 0x04));
    } else if (code >= 0x1E && code <= 0x27) {
        snprintf(out, size, "%c", code == 0x27 ? '0' : '1' + (code - 0x1E));
    } else if (code >= 0x28 && code <= 0x2C) {
        snprintf(out, size, "%s", kNames[code - 0x28]);
    } else if (code >= 0x2D && code <= 0x38) {
        snprintf(out, size, "%c", kPunct[code - 0x28]);
    } else if (code >= 0x3A && code <= 0x45) {
        snprintf(out, size, "F%d", code - 0x39);
    } else if (code >= 0x4F && code <= 0x52) {
        static const char *const kArrows[] = { "right", "left", "down", "up" };
        snprintf(out, size, "%s", kArrows[code - 0x4F]);
    } else {
        snprintf(out, size, "0x%02X", (unsigned)code);
    }
}

static void showLine(const char *line)
{
    ucxapp_print("%s\n", line);
    ucxapp_ui_event(line);
}

static void decodeKeyboard(cliReport_t *r, const cliLayout_t *l, const uint8_t *d, size_t n)
{
    static const char *const kMods[] = { "ctrl", "shift", "alt", "gui" };
    uint8_t keys[8] = { 0 };
    int modOff = (l != NULL && l->modOff >= 0) ? l->modOff : 0;
    int keysOff = (l != NULL && l->keysOff >= 0) ? l->keysOff : 16;
    int keysCount = (l != NULL && l->keysOff >= 0) ? l->keysCount : 6;
    int keyBits = (l != NULL && l->keysOff >= 0) ? l->keyBits : 8;
    uint8_t mod = (uint8_t)getBits(d, n, modOff, 8, false);
    char line[160];
    int pos = snprintf(line, sizeof(line), "[HID] keys:");
    bool any = false;

    for (int i = 0; i < keysCount && i < 8; i++) {
        keys[i] = (uint8_t)getBits(d, n, keysOff + i * keyBits, keyBits, false);
    }
    for (int i = 0; i < 4; i++) {
        if (mod & (0x11 << i)) {   /* left or right modifier */
            pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s+", kMods[i]);
        }
    }
    for (int i = 0; i < 8 && pos < (int)sizeof(line) - 16; i++) {
        if (keys[i] <= 0x03 || memchr(r->prevKeys, keys[i], sizeof(r->prevKeys)) != NULL) {
            continue;   /* none, rollover error, or still held */
        }
        char name[16];
        keyName(keys[i], name, sizeof(name));
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s", name);
        any = true;
    }
    memcpy(r->prevKeys, keys, sizeof(keys));
    if (any) {
        showLine(line);
    }
}

static void decodeMouse(const cliLayout_t *l, const uint8_t *d, size_t n)
{
    char line[120];
    int32_t buttons;
    int32_t x;
    int32_t y;
    int32_t w = 0;

    if (l == NULL || l->xOff < 0) {   /* boot mouse: buttons, x, y (int8) */
        buttons = n > 0 ? d[0] : 0;
        x = n > 1 ? (int8_t)d[1] : 0;
        y = n > 2 ? (int8_t)d[2] : 0;
        w = n > 3 ? (int8_t)d[3] : 0;
    } else {
        buttons = getBits(d, n, l->btnOff, l->btnBits > 8 ? 8 : l->btnBits, false);
        x = getBits(d, n, l->xOff, l->xyBits, true);
        y = getBits(d, n, l->yOff, l->xyBits, true);
        w = getBits(d, n, l->wOff, l->wBits, true);
    }
    snprintf(line, sizeof(line), "[HID] mouse: buttons %c%c%c dx %d dy %d wheel %d", (buttons & 1) ? 'L' : '-',
             (buttons & 4) ? 'M' : '-', (buttons & 2) ? 'R' : '-', (int)x, (int)y, (int)w);
    showLine(line);
}

static void decodeMedia(const cliLayout_t *l, const uint8_t *d, size_t n)
{
    char line[80];
    int32_t usage = l != NULL && l->mediaOff >= 0 ? getBits(d, n, l->mediaOff, l->mediaBits, false)
                                                  : getBits(d, n, 0, n >= 2 ? 16 : 8, false);

    if (usage == 0) {
        return;   /* release */
    }
    const char *name = mediaName((uint16_t)usage);
    snprintf(line, sizeof(line), "[HID] media: %s (0x%04X)", name != NULL ? name : "usage", (unsigned)usage);
    showLine(line);
}

/* Called from the URC task for every GATT notification; true if it was a subscribed HID report. */
bool ucxapp_hid_client_notification(int32_t conn, int32_t handle, const uint8_t *data, size_t len)
{
    if (!gCliActive || conn != gCliConn) {
        return false;
    }
    for (int i = 0; i < gCliRepCount; i++) {
        cliReport_t *r = &gCliRep[i];
        if (r->handle != handle) {
            continue;
        }
        const cliLayout_t *l = NULL;
        for (int k = 0; !r->boot && k < gCliLayCount; k++) {
            if (gCliLay[k].id == r->id) {
                l = &gCliLay[k];
            }
        }
        if (r->kind == KIND_KBD) {
            decodeKeyboard(r, l, data, len);
        } else if (r->kind == KIND_MOUSE) {
            decodeMouse(l, data, len);
        } else if (r->kind == KIND_MEDIA) {
            decodeMedia(l, data, len);
        } else {
            char line[120];
            int pos = snprintf(line, sizeof(line), "[HID] report %u:", (unsigned)r->id);
            for (size_t k = 0; k < len && pos < (int)sizeof(line) - 4; k++) {
                pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02X", (unsigned)data[k]);
            }
            showLine(line);
        }
        return true;
    }
    return false;
}

static bool cliRead(ucxapp_t *app, int32_t handle, uint8_t *buf, size_t max, size_t *len)
{
    uByteArray_t v;

    v.pData = buf;
    v.length = 0;
    bool ok = uCxGattClientReadBegin(app->ucx, gCliConn, handle, &v);
    int32_t r = uCxEnd(app->ucx);
    if (!ok || r != 0) {
        return false;
    }
    *len = v.length < max ? v.length : max;
    if (v.pData != buf) {
        memcpy(buf, v.pData, *len);
    }
    return true;
}

/* HIDS needs an encrypted link: on a failed read, bond with the peer and retry for up to 15 s. */
static bool cliReadSecure(ucxapp_t *app, int32_t handle, uint8_t *buf, size_t max, size_t *len)
{
    uCxBtListConnections_t c;
    uBtLeAddress_t addr;
    bool found = false;

    memset(&addr, 0, sizeof(addr));
    if (cliRead(app, handle, buf, max, len)) {
        return true;
    }
    uCxBluetoothListConnectionsBegin(app->ucx);
    while (uCxBluetoothListConnectionsGetNext(app->ucx, &c)) {
        if (c.conn_handle == gCliConn) {
            addr = c.bd_addr;
            found = true;
        }
    }
    uCxEnd(app->ucx);
    if (!found) {
        ucxapp_print("ERROR: connection %d not found\n", (int)gCliConn);
        return false;
    }
    ucxapp_print("read refused, bonding for an encrypted link...\n");
    ucxapp_bt_security_defaults(app);
    if (uCxBluetoothBond(app->ucx, &addr) != 0) {
        ucxapp_print("WARNING: bond request failed\n");
    }
    for (int i = 0; i < 15; i++) {
        U_CX_PORT_SLEEP_MS(1000);
        if (cliRead(app, handle, buf, max, len)) {
            return true;
        }
    }
    return false;
}

typedef struct {
    int32_t decl;
    int32_t value;
    uint16_t uuid;
    uint8_t props;
} cliChar_t;

static int cliStop(ucxapp_t *app)
{
    for (int i = 0; gCliActive && i < gCliRepCount; i++) {
        uCxGattClientConfigWrite(app->ucx, gCliConn, gCliRep[i].cccd, U_GATT_CLIENT_CONFIG_NONE);
    }
    gCliActive = false;
    gCliRepCount = 0;
    ucxapp_print("HID host stopped\n");
    return UCXAPP_OK;
}

/* hid.client <conn> [boot] | off: read the report map, subscribe to every input report and decode it. */
static int cmdHidClient(ucxapp_t *app, int argc, char *argv[])
{
    static uint8_t map[CLI_MAP_MAX];
    uCxGattClientDiscoverPrimaryServices_t s;
    uCxGattClientDiscoverServiceChars_t ch;
    uCxGattClientDiscoverCharDescriptors_t ds;
    cliChar_t chars[24];
    int nChars = 0;
    int32_t svcStart = -1;
    int32_t svcEnd = -1;
    int32_t conn;
    size_t mapLen = 0;
    bool boot = false;

    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        return cliStop(app);
    }
    if (argc < 2 || argc > 3 || !ucxapp_parse_int(argv[1], &conn) || conn < 0 ||
        (argc == 3 && strcmp(argv[2], "boot") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    boot = argc == 3;
    if (gCliActive) {
        cliStop(app);
    }
    gCliConn = conn;

    uCxGattClientDiscoverPrimaryServicesBegin(app->ucx, conn);
    while (uCxGattClientDiscoverPrimaryServicesGetNext(app->ucx, &s)) {
        if (svcStart < 0 && s.uuid.length == 2 && s.uuid.pData[0] == 0x18 && s.uuid.pData[1] == 0x12) {
            svcStart = s.start_handle;
            svcEnd = s.end_handle;
        }
    }
    if (uCxEnd(app->ucx) != 0 || svcStart < 0) {
        ucxapp_print("ERROR: no HID service on connection %d\n", (int)conn);
        return UCXAPP_ERR_FAILED;
    }
    uCxGattClientDiscoverServiceCharsBegin(app->ucx, conn, svcStart, svcEnd);
    while (uCxGattClientDiscoverServiceCharsGetNext(app->ucx, &ch)) {
        if (nChars < (int)(sizeof(chars) / sizeof(chars[0])) && ch.uuid.length == 2) {
            chars[nChars].decl = ch.attr_handle;
            chars[nChars].value = ch.value_handle;
            chars[nChars].uuid = (uint16_t)((ch.uuid.pData[0] << 8) | ch.uuid.pData[1]);
            chars[nChars].props = ch.properties.length > 0 ? ch.properties.pData[0] : 0;
            nChars++;
        }
    }
    uCxEnd(app->ucx);

    for (int i = 0; i < nChars; i++) {
        if (chars[i].uuid == 0x2A4B) {
            if (!cliReadSecure(app, chars[i].value, map, sizeof(map), &mapLen)) {
                ucxapp_print("ERROR: cannot read the report map (pair or bond first)\n");
                return UCXAPP_ERR_FAILED;
            }
        }
    }
    parseReportMap(map, mapLen);
    ucxapp_print("report map: %u bytes, %d report ID(s)\n", (unsigned)mapLen, gCliLayCount);

    for (int i = 0; i < nChars; i++) {
        if (chars[i].uuid == 0x2A4E) {
            uint8_t mode = boot ? 0 : 1;
            uCxGattClientWriteNoRsp(app->ucx, conn, chars[i].value, &mode, 1);
        }
    }

    gCliRepCount = 0;
    for (int i = 0; i < nChars && gCliRepCount < CLI_MAX_REPORTS; i++) {
        const cliChar_t *c = &chars[i];
        bool isBoot = c->uuid == 0x2A22 || c->uuid == 0x2A33;
        if ((c->props & PROP_NOTIFY) == 0 || (c->uuid != 0x2A4D && !isBoot) || isBoot != boot) {
            continue;
        }
        int32_t end = i + 1 < nChars ? chars[i + 1].decl - 1 : svcEnd;
        int32_t cccd = -1;
        int32_t ref = -1;
        uCxGattClientDiscoverCharDescriptorsBegin(app->ucx, conn, c->value, end);
        while (uCxGattClientDiscoverCharDescriptorsGetNext(app->ucx, &ds)) {
            if (ds.uuid.length == 2 && ds.uuid.pData[0] == 0x29) {
                if (ds.uuid.pData[1] == 0x02) {
                    cccd = ds.desc_handle;
                } else if (ds.uuid.pData[1] == 0x08) {
                    ref = ds.desc_handle;
                }
            }
        }
        uCxEnd(app->ucx);

        cliReport_t *r = &gCliRep[gCliRepCount];
        memset(r, 0, sizeof(*r));
        r->handle = c->value;
        r->cccd = cccd;
        r->boot = isBoot;
        if (isBoot) {
            r->kind = c->uuid == 0x2A22 ? KIND_KBD : KIND_MOUSE;
        } else {
            uint8_t rr[2] = { 0, 0 };
            size_t n = 0;
            if (ref < 0 || !cliRead(app, ref, rr, sizeof(rr), &n) || n < 2 || rr[1] != 0x01) {
                continue;   /* not an input report */
            }
            r->id = rr[0];
            r->kind = KIND_OTHER;
            for (int k = 0; k < gCliLayCount; k++) {
                if (gCliLay[k].id == r->id) {
                    r->kind = gCliLay[k].kind;
                }
            }
        }
        if (cccd < 0 || uCxGattClientConfigWrite(app->ucx, conn, cccd, U_GATT_CLIENT_CONFIG_ENABLE_NOTIFICATIONS) != 0) {
            ucxapp_print("  cannot subscribe to handle 0x%04X\n", (unsigned)c->value);
            continue;
        }
        static const char *const kKinds[] = { "other", "keyboard", "mouse", "media" };
        ucxapp_print("  %s input%s, report ID %u, handle 0x%04X\n", kKinds[r->kind], r->boot ? " (boot)" : "",
                     (unsigned)r->id, (unsigned)r->handle);
        gCliRepCount++;
    }
    if (gCliRepCount == 0) {
        ucxapp_print("ERROR: no input reports to subscribe to\n");
        return UCXAPP_ERR_FAILED;
    }
    gCliActive = true;
    ucxapp_print("HID host on connection %d (%s protocol): %d input report(s); input is printed and goes to the "
                 "Events pane (hid.client off to stop)\n", (int)conn, boot ? "boot" : "report", gCliRepCount);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gHidCmds[] = {
    { "hid.setup",  "[auth]",                         "HID device (HOGP): keyboard, mouse and media keys, plus battery and PnP ID; 'auth' needs MITM pairing", UCXAPP_CMD_NEEDS_DEVICE, cmdHidSetup },
    { "hid.status", "",                               "Show HID handles, notification state and protocol mode",               UCXAPP_CMD_NEEDS_DEVICE, cmdHidStatus },
    { "hid.key",    "<key> [ctrl|shift|alt|gui ...]", "Send one key press to the connected host",                              UCXAPP_CMD_NEEDS_DEVICE, cmdHidKey },
    { "hid.text",   "<text...>",                      "Type text on the connected host",                                       UCXAPP_CMD_NEEDS_DEVICE, cmdHidText },
    { "hid.mouse",  "<dx> <dy> [wheel]",              "Move the mouse pointer (and scroll) on the connected host",             UCXAPP_CMD_NEEDS_DEVICE, cmdHidMouse },
    { "hid.click",  "[left|right|middle] [double]",   "Click a mouse button on the connected host",                            UCXAPP_CMD_NEEDS_DEVICE, cmdHidClick },
    { "hid.media",  "<play|next|prev|stop|mute|volup|voldown|brightup|brightdown|home|back|search>", "Send a media key to the connected host", UCXAPP_CMD_NEEDS_DEVICE, cmdHidMedia },
    { "hid.client", "<conn> [boot] | off",            "HID host: read a BLE keyboard, mouse or remote and print its input",     UCXAPP_CMD_NEEDS_DEVICE, cmdHidClient },
};

void ucxapp_register_hid_cmds(void)
{
    ucxapp_cmd_register(gHidCmds, sizeof(gHidCmds) / sizeof(gHidCmds[0]));
}
