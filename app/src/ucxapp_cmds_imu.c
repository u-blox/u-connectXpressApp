/* IMU tools (host only): detect, monitor, log and bridge a WitMotion (serial, 11-byte frames) or Bosch BNO055 (UART)
 * inertial sensor. Bosch BMI160/BMI270/BMI323 and BNO08x have no PC serial port; they need an I2C/SPI host and are
 * not covered here. */

#include "ucxapp_cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_port.h"
#include "u_port_uart.h"
#include "ucxapp_platform.h"
#include "ucxapp_stream.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

enum { PROTO_NONE, PROTO_WIT, PROTO_BNO };
static const char *const kProtoName[] = { "none", "WitMotion serial", "Bosch BNO055" };

#define WIT_SHORT     11
#define WIT_LONG      20
#define STEP_HIGH_G   1.25
#define STEP_LOW_G    1.10
#define STEP_MIN_MS   300u
#define BNO_READ_LEN  32      /* registers 0x08..0x27: accel, mag, gyro, euler, quaternion */
#define BNO_POLL_MS   20u
#define G_MS2         9.80665

typedef struct {
    int proto;
    double accel[3];      /* g */
    double gyro[3];       /* deg/s */
    double angle[3];      /* roll, pitch, yaw in degrees */
    double mag[3];        /* raw (WitMotion) or uT (BNO055) */
    double quat[4];       /* w x y z */
    double pressure;      /* Pa */
    double altitude;      /* m */
    double temp;          /* deg C */
    bool hasAccel, hasGyro, hasAngle, hasMag, hasQuat, hasBaro, hasTemp;
    uint32_t frames, accelFrames, bad, lastMs;
    uint32_t steps, lastStepMs;
    bool stepArmed;
    /* parser state */
    uint8_t w[WIT_LONG];
    size_t wn;
    int bnoStage;         /* 0 idle, 1 length, 2 data, 3 error status */
    uint8_t bnoLen, bnoPos;
    uint8_t bnoBuf[BNO_READ_LEN];
    bool bnoId;
    bool bnoModeSent;
} imu_t;

/* ---- Decoding ---- */

static int16_t word(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void stepDetect(imu_t *s)
{
    double mag = sqrt(s->accel[0] * s->accel[0] + s->accel[1] * s->accel[1] + s->accel[2] * s->accel[2]);
    uint32_t now = ucxapp_platform_tick_ms();

    if (mag > STEP_HIGH_G && s->stepArmed && now - s->lastStepMs >= STEP_MIN_MS) {
        s->steps++;
        s->lastStepMs = now;
        s->stepArmed = false;
    } else if (mag < STEP_LOW_G) {
        s->stepArmed = true;
    }
}

static void markFrame(imu_t *s)
{
    s->frames++;
    s->lastMs = ucxapp_platform_tick_ms();
}

/* Frame with 0x55 header, type at w[1], data words from w[2]. */
static void witFrame(imu_t *s)
{
    const uint8_t *d = s->w + 2;

    markFrame(s);
    switch (s->w[1]) {
        case 0x51:
            for (int i = 0; i < 3; i++) {
                s->accel[i] = word(d + 2 * i) / 32768.0 * 16.0;
            }
            s->temp = word(d + 6) / 100.0;
            s->hasAccel = s->hasTemp = true;
            s->accelFrames++;
            stepDetect(s);
            break;
        case 0x52:
            for (int i = 0; i < 3; i++) {
                s->gyro[i] = word(d + 2 * i) / 32768.0 * 2000.0;
            }
            s->hasGyro = true;
            break;
        case 0x53:
            for (int i = 0; i < 3; i++) {
                s->angle[i] = word(d + 2 * i) / 32768.0 * 180.0;
            }
            s->hasAngle = true;
            break;
        case 0x54:
            for (int i = 0; i < 3; i++) {
                s->mag[i] = word(d + 2 * i);
            }
            s->hasMag = true;
            break;
        case 0x56:
            s->pressure = (double)(int32_t)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
                                            ((uint32_t)d[3] << 24));
            s->altitude = (double)(int32_t)((uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) |
                                            ((uint32_t)d[7] << 24)) / 100.0;
            s->hasBaro = true;
            break;
        case 0x59:
            for (int i = 0; i < 4; i++) {
                s->quat[i] = word(d + 2 * i) / 32768.0;
            }
            s->hasQuat = true;
            break;
        case 0x61:   /* WitMotion BLE 5.0 packet: accel, gyro, angle in one 20-byte frame */
            for (int i = 0; i < 3; i++) {
                s->accel[i] = word(d + 2 * i) / 32768.0 * 16.0;
                s->gyro[i] = word(d + 6 + 2 * i) / 32768.0 * 2000.0;
                s->angle[i] = word(d + 12 + 2 * i) / 32768.0 * 180.0;
            }
            s->hasAccel = s->hasGyro = s->hasAngle = true;
            s->accelFrames++;
            stepDetect(s);
            break;
        default:
            break;
    }
}

static void witByte(imu_t *s, uint8_t b)
{
    if (s->wn == 0 && b != 0x55) {
        return;
    }
    s->w[s->wn++] = b;
    if (s->wn < 2) {
        return;
    }
    size_t need = s->w[1] == 0x61 ? WIT_LONG : WIT_SHORT;
    if (s->wn < need) {
        return;
    }
    bool ok = true;
    if (need == WIT_SHORT) {
        uint8_t sum = 0;
        for (size_t i = 0; i < WIT_SHORT - 1; i++) {
            sum = (uint8_t)(sum + s->w[i]);
        }
        ok = sum == s->w[WIT_SHORT - 1];
    }
    if (ok) {
        witFrame(s);
        s->wn = 0;
        return;
    }
    s->bad++;
    for (size_t i = 1; i < s->wn; i++) {   /* resynchronise on the next 0x55 */
        if (s->w[i] == 0x55) {
            memmove(s->w, s->w + i, s->wn - i);
            s->wn -= i;
            return;
        }
    }
    s->wn = 0;
}

static void bnoFrame(imu_t *s)
{
    const uint8_t *d = s->bnoBuf;

    if (s->bnoLen == 1) {
        s->bnoId = d[0] == 0xA0;
        return;
    }
    if (s->bnoLen != BNO_READ_LEN) {
        return;
    }
    markFrame(s);
    for (int i = 0; i < 3; i++) {
        s->accel[i] = word(d + 2 * i) / 100.0 / G_MS2;
        s->mag[i] = word(d + 6 + 2 * i) / 16.0;
        s->gyro[i] = word(d + 12 + 2 * i) / 16.0;
    }
    s->angle[2] = word(d + 18) / 16.0;   /* heading is reported first, then roll and pitch */
    s->angle[0] = word(d + 20) / 16.0;
    s->angle[1] = word(d + 22) / 16.0;
    for (int i = 0; i < 4; i++) {
        s->quat[i] = word(d + 24 + 2 * i) / 16384.0;
    }
    s->hasAccel = s->hasGyro = s->hasAngle = s->hasMag = s->hasQuat = true;
    s->accelFrames++;
    stepDetect(s);
}

static void bnoByte(imu_t *s, uint8_t b)
{
    switch (s->bnoStage) {
        case 0:
            if (b == 0xBB) {
                s->bnoStage = 1;
            } else if (b == 0xEE) {
                s->bnoStage = 3;
            }
            break;
        case 1:
            if (b == 0 || b > BNO_READ_LEN) {
                s->bnoStage = 0;
            } else {
                s->bnoLen = b;
                s->bnoPos = 0;
                s->bnoStage = 2;
            }
            break;
        case 2:
            s->bnoBuf[s->bnoPos++] = b;
            if (s->bnoPos == s->bnoLen) {
                bnoFrame(s);
                s->bnoStage = 0;
            }
            break;
        default:   /* 0x01 is "write success"; anything else is a bus or protocol error */
            if (b != 0x01) {
                s->bad++;
            }
            s->bnoStage = 0;
            break;
    }
}

static void feed(imu_t *s, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s->proto == PROTO_WIT) {
            witByte(s, d[i]);
        } else if (s->proto == PROTO_BNO) {
            bnoByte(s, d[i]);
        }
    }
}

/* ---- Detection ---- */

static const int32_t kBauds[] = { 115200, 9600, 230400, 460800, 921600 };
static const uint8_t kBnoIdReq[] = { 0xAA, 0x01, 0x00, 0x01 };

/* Listens for ms milliseconds, optionally after sending a request; fills s from the received bytes.
 * Returns the byte count, -1 if the port did not open. */
static int32_t listen(const char *port, int32_t baud, int proto, const uint8_t *req, size_t reqLen, uint32_t ms,
                      imu_t *s)
{
    uPortUartHandle_t h = uPortUartOpen(port, baud, false);
    uint8_t buf[256];
    uint32_t start = ucxapp_platform_tick_ms();
    int32_t total = 0;

    if (h == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->proto = proto;
    if (req != NULL) {
        (void)uPortUartWrite(h, req, reqLen);
    }
    while (ucxapp_platform_tick_ms() - start < ms) {
        int32_t n = uPortUartRead(h, buf, sizeof(buf), 50);
        if (n > 0) {
            feed(s, buf, (size_t)n);
            total += n;
        }
        if ((proto == PROTO_WIT && s->frames >= 3) || (proto == PROTO_BNO && s->bnoId)) {
            break;
        }
    }
    uPortUartClose(h);
    return total;
}

/* Probes one port; on a hit stores protocol and baud. Thread safe, so ports can be probed at once. */
static bool probePort(const char *port, int *proto, int32_t *baud)
{
    imu_t *p = (imu_t *)malloc(sizeof(*p));
    char lastPort[32];
    int32_t lastBaud = 0;
    bool hit = false;

    if (p == NULL) {
        return false;
    }
    if (ucxapp_last_port_load("imu", lastPort, sizeof(lastPort), &lastBaud) && strcasecmp(lastPort, port) == 0 &&
        listen(port, lastBaud, PROTO_WIT, NULL, 0, 400, p) > 0 && p->frames >= 3) {
        *proto = PROTO_WIT;
        *baud = lastBaud;
        hit = true;
    }
    for (size_t b = 0; !hit && b < sizeof(kBauds) / sizeof(kBauds[0]); b++) {
        int32_t got = listen(port, kBauds[b], PROTO_WIT, NULL, 0, 400, p);
        if (got > 0 && p->frames >= 3) {
            *proto = PROTO_WIT;
            *baud = kBauds[b];
            hit = true;
        }
        if (got <= 0) {
            break;   /* a WitMotion streams by itself: silence at any baud means none is attached */
        }
    }
    if (!hit && listen(port, 115200, PROTO_BNO, kBnoIdReq, sizeof(kBnoIdReq), 400, p) > 0 && p->bnoId) {
        *proto = PROTO_BNO;
        *baud = 115200;
        hit = true;
    }
    free(p);
    return hit;
}

typedef struct {
    int proto;
    int32_t baud;
} portProbe_t;

static bool probeEntry(const char *port, void *result)
{
    portProbe_t *r = (portProbe_t *)result;

    return probePort(port, &r->proto, &r->baud);
}

static int detect(const char *only, char *outPort, size_t outSize, int *outProto, int32_t *outBaud)
{
    static ucxapp_serial_port_t ports[32];
    static char names[32][32];
    portProbe_t results[32];
    int map[32];
    bool hits[32];
    int count = ucxapp_platform_list_serial_ports(ports, 32);
    int n = 0;
    int found = 0;

    for (int i = 0; i < count; i++) {
        if (only != NULL ? strcasecmp(ports[i].name, only) == 0
                         : !(ucxapp_port_skip(&ports[i]) || ucxapp_port_is_gnss(&ports[i]))) {
            snprintf(names[n], sizeof(names[n]), "%.31s", ports[i].name);
            map[n++] = i;
        }
    }
    ucxapp_stream_probe_ports((const char (*)[32])names, n, probeEntry, results, sizeof(results[0]), hits);

    for (int k = 0; k < n; k++) {
        const ucxapp_serial_port_t *sp = &ports[map[k]];
        if (!hits[k]) {
            continue;
        }
        ucxapp_print("  %s%s%s%s: ", sp->name, sp->desc[0] ? " (" : "", sp->desc, sp->desc[0] ? ")" : "");
        ucxapp_print("%s IMU at %d baud\n", kProtoName[results[k].proto], (int)results[k].baud);
        if (found == 0) {
            ucxapp_last_port_save("imu", sp->name, results[k].baud);
            if (outPort != NULL) {
                snprintf(outPort, outSize, "%s", sp->name);
                *outProto = results[k].proto;
                *outBaud = results[k].baud;
            }
        }
        found++;
    }
    return found;
}

/* ---- Commands ---- */

static ucxapp_stream_t *gStream;
static imu_t gImu;
static imu_t gSnap;

static void onData(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    feed(&gImu, d, n);
}

/* Dashboard text such as "IMU WitMotion serial"; empty when no IMU is open. */
size_t ucxapp_imu_summary(char *out, size_t size)
{
    ucxapp_stream_t *s = gStream;

    out[0] = '\0';
    if (s == NULL) {
        return 0;
    }
    ucxapp_stream_lock(s);
    int proto = gImu.proto;
    uint32_t frames = gImu.frames;
    uint32_t last = gImu.lastMs;
    ucxapp_stream_unlock(s);
    bool stalled = frames == 0 || ucxapp_platform_tick_ms() - last > 2000u;
    int n = snprintf(out, size, "IMU %s%s", proto == PROTO_BNO ? "BNO055" : proto == PROTO_WIT ? "WIT" : "?",
                     stalled ? " no data" : "");
    if (ucxapp_stream_bridge_active(s) && n > 0 && (size_t)n < size) {
        n += snprintf(out + n, size - (size_t)n, " bridged");
    }
    return n > 0 ? (size_t)n : 0;
}

bool ucxapp_imu_dash(char *out, size_t size)
{
    ucxapp_stream_t *s = gStream;
    imu_t v;

    out[0] = '\0';
    if (s == NULL) {
        return false;
    }
    ucxapp_stream_lock(s);
    memcpy(&v, &gImu, sizeof(v));
    ucxapp_stream_unlock(s);
    const char *name = v.proto == PROTO_BNO ? "BNO055" : v.proto == PROTO_WIT ? "WIT" : "?";
    if (v.frames == 0 || ucxapp_platform_tick_ms() - v.lastMs > 2000u) {
        snprintf(out, size, "%s  no data on %s", name, ucxapp_stream_port(s));
    } else if (v.hasAngle) {
        snprintf(out, size, "%s  roll %.0f  pitch %.0f  yaw %.0f deg", name, v.angle[0], v.angle[1], v.angle[2]);
    } else if (v.hasAccel) {
        snprintf(out, size, "%s  |a| %.2f g", name,
                 sqrt(v.accel[0] * v.accel[0] + v.accel[1] * v.accel[1] + v.accel[2] * v.accel[2]));
    } else {
        snprintf(out, size, "%s  %u frames", name, (unsigned)v.frames);
    }
    return true;
}

/* BNO055 does not stream: set NDOF fusion mode once, then request one burst per poll. */
static void onPoll(void *ctx, ucxapp_stream_t *s)
{
    static const uint8_t kSetNdof[] = { 0xAA, 0x00, 0x3D, 0x01, 0x0C };
    static const uint8_t kRead[] = { 0xAA, 0x01, 0x08, BNO_READ_LEN };

    (void)ctx;
    if (!gImu.bnoModeSent) {
        gImu.bnoModeSent = true;
        (void)ucxapp_stream_write(s, kSetNdof, sizeof(kSetNdof));
        return;
    }
    (void)ucxapp_stream_write(s, kRead, sizeof(kRead));
}

static bool needOpen(void)
{
    if (gStream == NULL) {
        ucxapp_print("ERROR: no IMU open (use imu.open)\n");
        return false;
    }
    return true;
}

static void snapshot(void)
{
    ucxapp_stream_lock(gStream);
    memcpy(&gSnap, &gImu, sizeof(gSnap));
    ucxapp_stream_unlock(gStream);
}

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("Scanning serial ports for a WitMotion or Bosch BNO055 IMU (all ports at once)...\n");
    int n = detect(argc == 2 ? argv[1] : NULL, NULL, 0, NULL, NULL);
    ucxapp_print("%d IMU(s) found\n", n);
    return n > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    char port[32] = "";
    int proto = PROTO_NONE;
    int32_t baud = 0;
    ucxapp_stream_cfg_t cfg = { "imu", onData, NULL, 0, NULL, NULL };
    (void)app;

    if (argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        if (strcasecmp(argv[i], "wit") == 0) {
            proto = PROTO_WIT;
        } else if (strcasecmp(argv[i], "bno055") == 0) {
            proto = PROTO_BNO;
        } else if (!ucxapp_parse_int(argv[i], &baud) || baud < 300) {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (gStream != NULL) {
        ucxapp_print("ERROR: %s is already open (imu.close first)\n", ucxapp_stream_port(gStream));
        return UCXAPP_ERR_FAILED;
    }
    if (argc >= 2) {
        snprintf(port, sizeof(port), "%s", argv[1]);
    }
    if (proto == PROTO_NONE || baud == 0) {
        int dp = PROTO_NONE;
        int32_t db = 0;

        ucxapp_print("Detecting...\n");
        if (detect(argc >= 2 ? port : NULL, port, sizeof(port), &dp, &db) == 0) {
            ucxapp_print("ERROR: no IMU found\n");
            return UCXAPP_ERR_FAILED;
        }
        proto = proto != PROTO_NONE ? proto : dp;
        baud = baud != 0 ? baud : db;
    }
    memset(&gImu, 0, sizeof(gImu));
    gImu.proto = proto;
    if (proto == PROTO_BNO) {
        cfg.poll = onPoll;
        cfg.pollMs = BNO_POLL_MS;
    }
    gStream = ucxapp_stream_open(port, baud, &cfg);
    if (gStream == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", port);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s IMU open on %s at %d baud\n", kProtoName[proto], port, (int)baud);
    return UCXAPP_OK;
}

static int cmdClose(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stream_close(gStream);
    gStream = NULL;
    ucxapp_print("closed\n");
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    static uint32_t prevCount;
    static uint32_t prevMs;
    static double hz;
    uint32_t now = ucxapp_platform_tick_ms();
    (void)app;
    (void)argc;
    (void)argv;

    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    snapshot();
    if (prevMs == 0 || now - prevMs >= 500u) {
        hz = prevMs == 0 || gSnap.accelFrames < prevCount ? 0.0
                                                          : (gSnap.accelFrames - prevCount) * 1000.0 / (now - prevMs);
        prevCount = gSnap.accelFrames;
        prevMs = now;
    }
    ucxapp_print("Port:     %s at %d baud, %s, %u bytes received\n", ucxapp_stream_port(gStream),
                 (int)ucxapp_stream_baud(gStream), kProtoName[gSnap.proto], (unsigned)ucxapp_stream_rx_bytes(gStream));
    ucxapp_print("Frames:   %u good, %u bad, about %.0f Hz%s\n", (unsigned)gSnap.frames, (unsigned)gSnap.bad, hz,
                 gSnap.frames == 0 ? " (no data yet)" : (now - gSnap.lastMs > 2000u ? " (stalled)" : ""));
    if (gSnap.hasAccel) {
        double m = sqrt(gSnap.accel[0] * gSnap.accel[0] + gSnap.accel[1] * gSnap.accel[1] +
                        gSnap.accel[2] * gSnap.accel[2]);
        ucxapp_print("Accel:    %7.3f %7.3f %7.3f g   |a| %.3f g\n", gSnap.accel[0], gSnap.accel[1], gSnap.accel[2], m);
    }
    if (gSnap.hasGyro) {
        ucxapp_print("Gyro:     %7.1f %7.1f %7.1f deg/s\n", gSnap.gyro[0], gSnap.gyro[1], gSnap.gyro[2]);
    }
    if (gSnap.hasAngle) {
        ucxapp_print("Angle:    roll %.1f  pitch %.1f  yaw %.1f deg\n", gSnap.angle[0], gSnap.angle[1], gSnap.angle[2]);
    }
    if (gSnap.hasMag) {
        ucxapp_print("Mag:      %7.1f %7.1f %7.1f %s\n", gSnap.mag[0], gSnap.mag[1], gSnap.mag[2],
                     gSnap.proto == PROTO_BNO ? "uT" : "(raw)");
    }
    if (gSnap.hasQuat) {
        ucxapp_print("Quat:     w %.3f x %.3f y %.3f z %.3f\n", gSnap.quat[0], gSnap.quat[1], gSnap.quat[2], gSnap.quat[3]);
    }
    if (gSnap.hasBaro) {
        ucxapp_print("Baro:     %.0f Pa, altitude %.2f m\n", gSnap.pressure, gSnap.altitude);
    }
    if (gSnap.hasTemp) {
        ucxapp_print("Temp:     %.1f C\n", gSnap.temp);
    }
    ucxapp_print("Steps:    %u\n", (unsigned)gSnap.steps);
    ucxapp_print("Log:      %s\n", ucxapp_stream_log_name(gStream)[0] ? ucxapp_stream_log_name(gStream) : "off");
    ucxapp_print("Bridge:   ");
    ucxapp_stream_bridge_status(gStream);
    return UCXAPP_OK;
}

static int cmdLog(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    bool off = strcasecmp(argv[1], "off") == 0;
    if (!ucxapp_stream_log(gStream, off ? NULL : argv[1])) {
        ucxapp_print("ERROR: cannot create %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print(off ? "logging stopped\n" : "logging raw IMU data to %s\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdSend(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t buf[64];
    size_t n = 0;
    int hi = -1;
    (void)app;

    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    for (int i = 1; i < argc; i++) {
        for (const char *c = argv[i]; *c != '\0'; c++) {
            int v = (*c >= '0' && *c <= '9') ? *c - '0' : ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'f') ? (*c | 0x20) - 'a' + 10 : -1;
            if (v < 0) {
                return UCXAPP_ERR_USAGE;
            }
            if (hi < 0) {
                hi = v;
            } else if (n < sizeof(buf)) {
                buf[n++] = (uint8_t)(hi << 4 | v);
                hi = -1;
            } else {
                return UCXAPP_ERR_USAGE;
            }
        }
    }
    if (n == 0 || hi >= 0 || !ucxapp_stream_write(gStream, buf, n)) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("sent %u byte(s)\n", (unsigned)n);
    return UCXAPP_OK;
}

/* WitMotion output rate (RRATE register 0x03); the sensor must be unlocked first and the setting saved. */
static int cmdRate(ucxapp_t *app, int argc, char *argv[])
{
    static const struct { double hz; uint8_t code; } kRates[] = {
        { 0.2, 0x01 }, { 0.5, 0x02 }, { 1, 0x03 }, { 2, 0x04 }, { 5, 0x05 },
        { 10, 0x06 }, { 20, 0x07 }, { 50, 0x08 }, { 100, 0x09 }, { 200, 0x0A },
    };
    double hz;
    uint8_t unlock[] = { 0xFF, 0xAA, 0x69, 0x88, 0xB5 };
    uint8_t set[] = { 0xFF, 0xAA, 0x03, 0x00, 0x00 };
    uint8_t save[] = { 0xFF, 0xAA, 0x00, 0x00, 0x00 };
    bool known = false;
    (void)app;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    hz = atof(argv[1]);
    for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
        if (fabs(kRates[i].hz - hz) < 0.01) {
            set[3] = kRates[i].code;
            known = true;
        }
    }
    if (!known) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (gImu.proto != PROTO_WIT) {
        ucxapp_print("ERROR: the output rate can only be set on WitMotion sensors (BNO055 is polled at 50 Hz)\n");
        return UCXAPP_ERR_FAILED;
    }
    (void)ucxapp_stream_write(gStream, unlock, sizeof(unlock));
    ucxapp_platform_sleep_ms(200);
    (void)ucxapp_stream_write(gStream, set, sizeof(set));
    ucxapp_platform_sleep_ms(200);
    (void)ucxapp_stream_write(gStream, save, sizeof(save));
    ucxapp_print("output rate set to %g Hz\n", hz);
    return UCXAPP_OK;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (gImu.proto == PROTO_BNO && strcasecmp(argv[1], "off") != 0 && strcasecmp(argv[1], "status") != 0) {
        ucxapp_print("note: the BNO055 stream is the sensor's register read-outs, not a continuous frame stream\n");
    }
    return ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 5005);
}

static const ucxapp_cmd_t kCmds[] = {
    { "imu.detect", "[port]", "Find a WitMotion or Bosch BNO055 IMU on the serial ports", 0, cmdDetect },
    { "imu.open", "[port] [baud] [wit|bno055]", "Open an IMU (detects port, baud and type when omitted)", 0, cmdOpen },
    { "imu.close", "", "Close the IMU", 0, cmdClose },
    { "imu.status", "", "Accel, gyro, angle, magnetometer, pressure, steps and counters", 0, cmdStatus },
    { "imu.log", "<file|off>", "Record the raw IMU byte stream to a file", 0, cmdLog },
    { "imu.send", "<hex..>", "Send raw bytes to the sensor (e.g. WitMotion configuration frames)", 0, cmdSend },
    { "imu.rate", "<hz>", "Set the WitMotion output rate (0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200 Hz)", 0, cmdRate },
    { "imu.bridge", "<tcp [port]|tls [port] [cert] [clientauth]|tcpc <host> <port>|udp <host> <port> [local]|udps <port>|ble [nus|sps|custom <uuid> [rx tx]]|uart <port> [baud]|cell|off|status>", "Two-way bridge of the IMU stream (TCP server/client, UDP, BLE NUS/SPS, PC UART)", 0, cmdBridge },
};

void ucxapp_register_imu_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
