/* Simulated serial devices, see ucxapp_vport.h. Each device produces its output when it is read, so it needs no
 * thread of its own; a device is opened by one user at a time, like a real serial port. */

#include "ucxapp_vport.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_port.h"
#include "ucxapp_platform.h"

#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define RING_SIZE 8192
#define VPI       3.14159265358979323846

typedef struct vdev vdev_t;

struct vdev {
    const char *name;
    const char *desc;
    void (*reset)(vdev_t *d, uint32_t now);
    void (*input)(vdev_t *d, const uint8_t *data, size_t len, uint32_t now);
    void (*tick)(vdev_t *d, uint32_t now);
    bool open;
    uint8_t ring[RING_SIZE];   /* device to host */
    size_t head, tail;
};

static U_CX_MUTEX_HANDLE gLock;
static bool gLockInit;

static void put(vdev_t *d, const void *data, size_t len)
{
    const uint8_t *p = data;

    for (size_t i = 0; i < len; i++) {
        size_t next = (d->head + 1) % RING_SIZE;
        if (next == d->tail) {
            return;   /* nobody reads: drop, like an overrun UART */
        }
        d->ring[d->head] = p[i];
        d->head = next;
    }
}

/* ---- WitMotion IMU: accel, gyro, angle, magnetometer and barometer frames at the configured output rate ---- */

static const double kWitRates[] = { 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200 };

static struct {
    double hz;           /* kept over close and open, like the saved setting of a real sensor */
    uint32_t start, next;
    uint8_t cfg[5];
    size_t nCfg;
} gImu = { 10.0, 0, 0, { 0 }, 0 };

static int16_t scale(double v, double range)
{
    double r = v / range * 32768.0;

    r = r > 32767.0 ? 32767.0 : r < -32768.0 ? -32768.0 : r;
    return (int16_t)(r < 0 ? r - 0.5 : r + 0.5);
}

static void witFrame(vdev_t *d, uint8_t type, const uint8_t data[8])
{
    uint8_t f[11];
    uint8_t sum = 0;

    f[0] = 0x55;
    f[1] = type;
    memcpy(f + 2, data, 8);
    for (int i = 0; i < 10; i++) {
        sum = (uint8_t)(sum + f[i]);
    }
    f[10] = sum;
    put(d, f, sizeof(f));
}

static void witWords(vdev_t *d, uint8_t type, int16_t a, int16_t b, int16_t c, int16_t e)
{
    const int16_t w[4] = { a, b, c, e };
    uint8_t data[8];

    for (int i = 0; i < 4; i++) {
        data[2 * i] = (uint8_t)((uint16_t)w[i] & 0xFF);
        data[2 * i + 1] = (uint8_t)((uint16_t)w[i] >> 8);
    }
    witFrame(d, type, data);
}

/* Someone walking (about 1.8 steps a second) while slowly turning around. */
static void imuSample(vdev_t *d, uint32_t now)
{
    double t = (double)(now - gImu.start) / 1000.0;
    double step = 2.0 * VPI * 1.8 * t;
    double yaw = fmod(10.0 * t + 180.0, 360.0) - 180.0;
    double yawRad = yaw * VPI / 180.0;
    int32_t pa = 101205;
    int32_t cm = 1000;
    uint8_t baro[8];

    witWords(d, 0x51, scale(0.05 * sin(step / 2.0), 16.0), scale(0.03 * cos(step), 16.0),
             scale(1.0 + 0.35 * sin(step), 16.0), 2350);
    witWords(d, 0x52, scale(2.5 * cos(0.5 * t), 2000.0), scale(0.9 * cos(0.3 * t), 2000.0), scale(10.0, 2000.0), 0);
    witWords(d, 0x53, scale(5.0 * sin(0.5 * t), 180.0), scale(3.0 * sin(0.3 * t), 180.0), scale(yaw, 180.0), 0x6C);
    witWords(d, 0x54, (int16_t)(300.0 * cos(yawRad)), (int16_t)(-300.0 * sin(yawRad)), -400, 2350);
    for (int i = 0; i < 4; i++) {
        baro[i] = (uint8_t)((uint32_t)pa >> (8 * i));
        baro[4 + i] = (uint8_t)((uint32_t)cm >> (8 * i));
    }
    witFrame(d, 0x56, baro);
}

static void imuReset(vdev_t *d, uint32_t now)
{
    (void)d;
    gImu.start = now;
    gImu.next = now;
    gImu.nCfg = 0;
}

static void imuTick(vdev_t *d, uint32_t now)
{
    uint32_t period = (uint32_t)(1000.0 / gImu.hz);

    if ((int32_t)(now - gImu.next) > 1000) {
        gImu.next = now;   /* not read for a while: no burst of old samples */
    }
    while ((int32_t)(now - gImu.next) >= 0) {
        imuSample(d, gImu.next);
        gImu.next += period;
    }
}

/* Configuration frames FF AA <register> <lo> <hi>; only the output rate (register 0x03) changes anything. */
static void imuInput(vdev_t *d, const uint8_t *data, size_t len, uint32_t now)
{
    (void)d;
    (void)now;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if ((gImu.nCfg == 0 && b != 0xFF) || (gImu.nCfg == 1 && b != 0xAA)) {
            gImu.nCfg = b == 0xFF ? 1 : 0;
            continue;
        }
        gImu.cfg[gImu.nCfg++] = b;
        if (gImu.nCfg == sizeof(gImu.cfg)) {
            if (gImu.cfg[2] == 0x03 && gImu.cfg[3] >= 1 && gImu.cfg[3] <= 10) {
                gImu.hz = kWitRates[gImu.cfg[3] - 1];
            }
            gImu.nCfg = 0;
        }
    }
}

/* ---- u-blox SARA-R5 modem: 3GPP status, ping, sockets and a direct link to an NTRIP caster ---- */

static struct {
    bool attached;
    bool sock;
    bool data;           /* direct link (transparent) mode; kept over close and open like on the modem */
    bool caster;         /* the NTRIP request was answered: RTCM follows */
    char apn[64];
    char line[256];
    size_t nLine;
    char req[512];
    size_t nReq;
    uint32_t nextRtcm;
} gCell = { true, false, false, false, "internet", { 0 }, 0, { 0 }, 0, 0 };

static const struct {
    const char *cmd;
    const char *resp;
} kCellAt[] = {
    { "ATI", "Manufacturer: u-blox\r\nModel: SARA-R510M8S" },
    { "AT+CGMI", "u-blox" },
    { "AT+CGMM", "SARA-R510M8S" },
    { "AT+CGMR", "03.15" },
    { "AT+CGSN", "351234567890123" },
    { "AT+CIMI", "240011234567890" },
    { "AT+CCID", "+CCID: 89460112345678901234" },
    { "AT+CPIN?", "+CPIN: READY" },
    { "AT+CSQ", "+CSQ: 20,99" },
    { "AT+CESQ", "+CESQ: 99,99,255,255,20,50" },
    { "AT+CREG?", "+CREG: 0,1" },
    { "AT+CEREG?", "+CEREG: 0,1" },
    { "AT+CGREG?", "+CGREG: 0,1" },
    { "AT+COPS?", "+COPS: 0,0,\"vucx telecom\",7" },
    { "AT+COPS=?", "+COPS: (2,\"vucx telecom\",\"vucx\",\"24099\",7),(1,\"Other net\",\"Other\",\"24001\",9),,(0-4),(0-2)" },
    { "AT+CFUN?", "+CFUN: 1" },
    { "AT+URAT?", "+URAT: 7,8" },
    { "AT+UMNOPROF?", "+UMNOPROF: 90" },
    { "AT+UPSND=0,8", "+UPSND: 0,8,1" },
};

static void say(vdev_t *d, const char *fmt, ...)
{
    char text[300];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n > 0) {
        put(d, "\r\n", 2);
        put(d, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
        put(d, "\r\n", 2);
    }
}

static bool is(const char *cmd, const char *prefix)
{
    return strncasecmp(cmd, prefix, strlen(prefix)) == 0;
}

/* The n-th (1 based) double quoted string of s into out. */
static bool quoted(const char *s, int n, char *out, size_t size)
{
    for (int i = 1; (s = strchr(s, '"')) != NULL; i++) {
        const char *e = strchr(++s, '"');
        if (e == NULL) {
            return false;
        }
        if (i == n) {
            size_t len = (size_t)(e - s) < size - 1 ? (size_t)(e - s) : size - 1;
            memcpy(out, s, len);
            out[len] = '\0';
            return true;
        }
        s = e + 1;
    }
    return false;
}

static void cellCommand(vdev_t *d, const char *cmd)
{
    char text[104];
    bool ok = true;

    for (size_t i = 0; i < sizeof(kCellAt) / sizeof(kCellAt[0]); i++) {
        if (strlen(cmd) == strlen(kCellAt[i].cmd) && is(cmd, kCellAt[i].cmd)) {
            say(d, "%s", kCellAt[i].resp);
            say(d, "OK");
            return;
        }
    }
    if (is(cmd, "AT+CGATT?")) {
        say(d, "+CGATT: %d", gCell.attached ? 1 : 0);
    } else if (is(cmd, "AT+CGATT=")) {
        gCell.attached = cmd[9] == '1';
    } else if (is(cmd, "AT+CGACT?")) {
        say(d, "+CGACT: 1,%d", gCell.attached ? 1 : 0);
    } else if (is(cmd, "AT+CGDCONT?")) {
        say(d, "+CGDCONT: 1,\"IP\",\"%s\",\"%s\",0,0", gCell.apn, gCell.attached ? "10.0.0.5" : "0.0.0.0");
    } else if (is(cmd, "AT+CGDCONT=1,")) {
        if (quoted(cmd, 2, text, sizeof(text))) {
            snprintf(gCell.apn, sizeof(gCell.apn), "%s", text);
        }
    } else if (is(cmd, "AT+CGPADDR")) {
        say(d, gCell.attached ? "+CGPADDR: 1,\"10.0.0.5\"" : "+CGPADDR: 1");
    } else if (is(cmd, "AT+UPING=")) {
        ok = gCell.attached && quoted(cmd, 1, text, sizeof(text));
        if (ok) {
            say(d, "OK");
            for (int i = 1; i <= 4; i++) {
                say(d, "+UUPING: %d,32,\"%s\",\"93.184.216.34\",55,%d", i, text, 18 + 3 * i);
            }
            return;
        }
    } else if (is(cmd, "AT+USOCR=")) {
        ok = gCell.attached && !gCell.sock;
        if (ok) {
            gCell.sock = true;
            say(d, "+USOCR: 0");
        }
    } else if (is(cmd, "AT+USOCO=")) {
        ok = gCell.sock && quoted(cmd, 1, text, sizeof(text));
    } else if (is(cmd, "AT+USOCL=")) {
        gCell.sock = false;
    } else if (is(cmd, "AT+USODL=")) {
        ok = gCell.sock;
        if (ok) {
            gCell.data = true;
            gCell.caster = false;
            gCell.nReq = 0;
            say(d, "CONNECT");
            return;
        }
    } else {
        ok = is(cmd, "AT");
    }
    say(d, ok ? "OK" : "ERROR");
}

static void bits(uint8_t *buf, unsigned *pos, unsigned n, int64_t v)
{
    for (unsigned i = 0; i < n; i++, (*pos)++) {
        if (((uint64_t)v >> (n - 1 - i)) & 1u) {
            buf[*pos / 8] |= (uint8_t)(0x80u >> (*pos % 8));
        }
    }
}

/* RTCM 1005 (reference station position): u-blox in Malmo, once a second while the NTRIP stream runs. */
static void rtcm1005(vdev_t *d)
{
    static const double kA = 6378137.0;
    static const double kE2 = 6.69437999014e-3;
    double lat = 55.6125 * VPI / 180.0;
    double lon = 12.9925 * VPI / 180.0;
    double n = kA / sqrt(1.0 - kE2 * sin(lat) * sin(lat));
    double xyz[3] = { (n + 10.0) * cos(lat) * cos(lon), (n + 10.0) * cos(lat) * sin(lon),
                      (n * (1.0 - kE2) + 10.0) * sin(lat) };
    uint8_t f[3 + 19 + 3] = { 0xD3, 0x00, 19 };
    unsigned pos = 24;
    uint32_t crc = 0;

    bits(f, &pos, 12, 1005);
    bits(f, &pos, 12, 0);    /* station id */
    bits(f, &pos, 6, 0);     /* ITRF year */
    bits(f, &pos, 4, 0xE);   /* GPS, GLONASS, Galileo, reference station */
    bits(f, &pos, 38, (int64_t)llround(xyz[0] * 10000.0));
    bits(f, &pos, 2, 0);
    bits(f, &pos, 38, (int64_t)llround(xyz[1] * 10000.0));
    bits(f, &pos, 2, 0);
    bits(f, &pos, 38, (int64_t)llround(xyz[2] * 10000.0));
    for (size_t i = 0; i < 22; i++) {
        crc ^= (uint32_t)f[i] << 16;
        for (int b = 0; b < 8; b++) {
            crc = (crc << 1) ^ ((crc & 0x800000u) != 0 ? 0x1864CFBu : 0u);
        }
    }
    f[22] = (uint8_t)(crc >> 16);
    f[23] = (uint8_t)(crc >> 8);
    f[24] = (uint8_t)crc;
    put(d, f, sizeof(f));
}

static void cellReset(vdev_t *d, uint32_t now)
{
    (void)d;
    (void)now;
    gCell.nLine = 0;
}

static void cellInput(vdev_t *d, const uint8_t *data, size_t len, uint32_t now)
{
    if (gCell.data) {
        if (len == 3 && memcmp(data, "+++", 3) == 0) {
            gCell.data = false;
            gCell.caster = false;
            say(d, "OK");
            return;
        }
        /* the far side: an NTRIP caster answers a GET; anything else (NMEA, IMU frames, GGA) is taken silently */
        for (size_t i = 0; i < len && !gCell.caster && gCell.nReq < sizeof(gCell.req) - 1; i++) {
            gCell.req[gCell.nReq++] = (char)data[i];
            gCell.req[gCell.nReq] = '\0';
            if (strncmp(gCell.req, "GET ", gCell.nReq < 4 ? gCell.nReq : 4) != 0) {
                gCell.nReq = sizeof(gCell.req) - 1;
            } else if (strstr(gCell.req, "\r\n\r\n") != NULL) {
                put(d, "ICY 200 OK\r\n", 12);
                gCell.caster = true;
                gCell.nextRtcm = now;
            }
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (gCell.nLine > 0) {
                gCell.line[gCell.nLine] = '\0';
                cellCommand(d, gCell.line);
            }
            gCell.nLine = 0;
        } else if (gCell.nLine < sizeof(gCell.line) - 1) {
            gCell.line[gCell.nLine++] = c;
        }
    }
}

static void cellTick(vdev_t *d, uint32_t now)
{
    if (gCell.data && gCell.caster && (int32_t)(now - gCell.nextRtcm) >= 0) {
        rtcm1005(d);
        gCell.nextRtcm = (int32_t)(now - gCell.nextRtcm) > 2000 ? now + 1000 : gCell.nextRtcm + 1000;
    }
}

/* ---- Port functions ---- */

/* virt:pc: a PC tool (u-center, a terminal) on the far end of a null-modem pair; it takes whatever it is sent. */
static void pcReset(vdev_t *d, uint32_t now)
{
    (void)d;
    (void)now;
}

static void pcInput(vdev_t *d, const uint8_t *data, size_t len, uint32_t now)
{
    (void)d;
    (void)data;
    (void)len;
    (void)now;
}

static vdev_t gDevs[] = {
    { UCXAPP_VPORT_IMU, "WitMotion IMU (virtual)", imuReset, imuInput, imuTick, false, { 0 }, 0, 0 },
    { UCXAPP_VPORT_CELL, "u-blox SARA-R510M8S cellular modem (virtual)", cellReset, cellInput, cellTick, false, { 0 }, 0, 0 },
    { UCXAPP_VPORT_PC, "PC tool on a null-modem pair (virtual)", pcReset, pcInput, pcReset, false, { 0 }, 0, 0 },
};

static vdev_t *find(const char *port)
{
    for (size_t i = 0; port != NULL && i < sizeof(gDevs) / sizeof(gDevs[0]); i++) {
        if (strncasecmp(port, gDevs[i].name, strlen(gDevs[i].name) + 1) == 0) {
            return &gDevs[i];
        }
    }
    return NULL;
}

static vdev_t *dev(uPortUartHandle_t h)
{
    for (size_t i = 0; i < sizeof(gDevs) / sizeof(gDevs[0]); i++) {
        if (h == (uPortUartHandle_t)&gDevs[i]) {
            return &gDevs[i];
        }
    }
    return NULL;
}

bool ucxapp_vport_is(const char *port)
{
    return find(port) != NULL;
}

const char *ucxapp_vport_desc(const char *port)
{
    vdev_t *d = find(port);

    return d != NULL ? d->desc : "";
}

uPortUartHandle_t ucxapp_uart_open(const char *port, int32_t baud)
{
    vdev_t *d = find(port);

    if (d == NULL) {
        return uPortUartOpen(port, baud, false);
    }
    if (!gLockInit) {
        U_CX_MUTEX_CREATE(gLock);
        gLockInit = true;
    }
    U_CX_MUTEX_LOCK(gLock);
    bool busy = d->open;
    if (!busy) {
        d->open = true;
        d->head = d->tail = 0;
        d->reset(d, ucxapp_platform_tick_ms());
    }
    U_CX_MUTEX_UNLOCK(gLock);
    return busy ? NULL : (uPortUartHandle_t)d;
}

void ucxapp_uart_close(uPortUartHandle_t h)
{
    vdev_t *d = dev(h);

    if (d == NULL) {
        uPortUartClose(h);
        return;
    }
    U_CX_MUTEX_LOCK(gLock);
    d->open = false;
    U_CX_MUTEX_UNLOCK(gLock);
}

int32_t ucxapp_uart_read(uPortUartHandle_t h, void *buf, size_t size, int32_t timeoutMs)
{
    vdev_t *d = dev(h);
    uint8_t *out = buf;
    uint32_t start = ucxapp_platform_tick_ms();

    if (d == NULL) {
        return uPortUartRead(h, buf, size, timeoutMs);
    }
    for (;;) {
        size_t n = 0;

        U_CX_MUTEX_LOCK(gLock);
        d->tick(d, ucxapp_platform_tick_ms());
        while (n < size && d->tail != d->head) {
            out[n++] = d->ring[d->tail];
            d->tail = (d->tail + 1) % RING_SIZE;
        }
        U_CX_MUTEX_UNLOCK(gLock);
        if (n > 0) {
            return (int32_t)n;
        }
        if (timeoutMs >= 0 && ucxapp_platform_tick_ms() - start >= (uint32_t)timeoutMs) {
            return 0;
        }
        ucxapp_platform_sleep_ms(1);
    }
}

int32_t ucxapp_uart_write(uPortUartHandle_t h, const void *data, size_t len)
{
    vdev_t *d = dev(h);

    if (d == NULL) {
        return uPortUartWrite(h, data, len);
    }
    U_CX_MUTEX_LOCK(gLock);
    d->input(d, data, len, ucxapp_platform_tick_ms());
    U_CX_MUTEX_UNLOCK(gLock);
    return (int32_t)len;
}
