/* hw.*: GPIO, I2C, SPI, ADC, PWM and CAN behind one backend table. The host build runs an emulated backend so every
 * command and the bridge work without hardware; a board port replaces gBe with real drivers.
 * hw.bridge exposes the same commands as a line protocol over TCP, UDP, BLE or a PC UART ("gpio.read 3" -> "OK ..."),
 * and pushes "EVT ..." lines for GPIO input changes and received CAN frames. */

#include "ucxapp_cmd.h"
#include "ucxapp_hwcfg.h"
#include "ucxapp_platform.h"
#include "ucxapp_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_port.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

/* Defaults for the emulation; hw.load replaces them with a board profile. */
#define DEF_GPIO  16
#define DEF_ADC   8
#define DEF_PWM   4
#define MAX_BYTES 32
#define VREF_MV   3300

enum { GPIO_IN, GPIO_OUT, GPIO_PULLUP, GPIO_PULLDOWN };

typedef struct {
    const char *name;
    bool (*gpioMode)(int pin, int mode);
    bool (*gpioWrite)(int pin, int level);
    int (*gpioRead)(int pin);
    /* write wl bytes, then read rl bytes (repeated start); returns 0 on ACK, negative on NACK */
    int (*i2cXfer)(int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl);
    bool (*spiXfer)(const uint8_t *tx, uint8_t *rx, size_t n);
    int (*adcRead)(int ch);   /* 12-bit raw, negative on error */
    bool (*pwmSet)(int ch, uint32_t hz, int permille);
} hwBackend_t;

/* ---- emulated backend ---- */

static int gNGpio = DEF_GPIO;
static int gNAdc = DEF_ADC;
static int gNPwm = DEF_PWM;
static hwcfg_t *gCfg;   /* NULL until hw.load succeeds */
static int gEmuMode[HWCFG_MAX_GPIO];
static int gEmuLevel[HWCFG_MAX_GPIO];
static uint8_t gEmuRegs[HWCFG_MAX_EMU][256];
static int gEmuPtr[HWCFG_MAX_EMU];
static int gEmuAddr[HWCFG_MAX_EMU];
static int gEmuN;

static void emuInit(void)
{
    static const int kAddr[3] = { 0x48, 0x68, 0x76 };

    memset(gEmuRegs, 0, sizeof(gEmuRegs));
    memset(gEmuPtr, 0, sizeof(gEmuPtr));
    memset(gEmuLevel, 0, sizeof(gEmuLevel));
    memset(gEmuMode, 0, sizeof(gEmuMode));
    gEmuN = 3;
    memcpy(gEmuAddr, kAddr, sizeof(kAddr));
    gEmuRegs[0][0] = 0x19;    /* TMP102-like: 25.0 C */
    gEmuRegs[1][0x75] = 0x68; /* MPU-like WHO_AM_I */
    gEmuRegs[2][0xD0] = 0x60; /* BME280-like chip id */
}

static bool emuGpioMode(int pin, int mode)
{
    gEmuMode[pin] = mode;
    if (mode == GPIO_PULLUP) {
        gEmuLevel[pin] = 1;
    } else if (mode == GPIO_PULLDOWN) {
        gEmuLevel[pin] = 0;
    }
    return true;
}

/* Writing to an input pin sets the simulated external level. */
static bool emuGpioWrite(int pin, int level)
{
    gEmuLevel[pin] = level;
    return true;
}

static int emuGpioRead(int pin)
{
    return gEmuLevel[pin];
}

static int emuI2cXfer(int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    int d = -1;

    for (int i = 0; i < gEmuN; i++) {
        if (gEmuAddr[i] == addr) {
            d = i;
        }
    }
    if (d < 0) {
        return -1;
    }
    if (wl > 0) {
        gEmuPtr[d] = w[0];
        for (size_t i = 1; i < wl; i++) {
            gEmuRegs[d][(gEmuPtr[d]++) & 0xFF] = w[i];
        }
    }
    for (size_t i = 0; i < rl; i++) {
        r[i] = gEmuRegs[d][(gEmuPtr[d]++) & 0xFF];
    }
    return 0;
}

static bool emuSpiXfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    memcpy(rx, tx, n);   /* MISO wired to MOSI */
    return true;
}

static int emuAdcRead(int ch)
{
    uint32_t period = 4000u + 500u * (uint32_t)ch;
    uint32_t phase = ucxapp_platform_tick_ms() % period;
    uint32_t half = period / 2;
    uint32_t tri = phase < half ? phase : period - phase;

    return (int)(tri * 4095u / half);
}

static bool emuPwmSet(int ch, uint32_t hz, int permille)
{
    (void)ch;
    (void)hz;
    (void)permille;
    return true;
}

static const hwBackend_t kEmu = {
    "emulated", emuGpioMode, emuGpioWrite, emuGpioRead, emuI2cXfer, emuSpiXfer, emuAdcRead, emuPwmSet
};

static const hwBackend_t *gBe = &kEmu;

static U_CX_MUTEX_HANDLE gLock;
static bool gLockInit;

/* ---- shared state kept by the command layer ---- */

static int gSpiMode;
static int32_t gSpiHz = 1000000;
static struct { uint32_t hz; int permille; } gPwm[HWCFG_MAX_PWM];
static ucxapp_stream_t *gStream;

typedef struct {
    char buf[1024];
    size_t n;
} hwOut_t;

static void outf(hwOut_t *o, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (o->n < sizeof(o->buf)) {
        int w = vsnprintf(o->buf + o->n, sizeof(o->buf) - o->n, fmt, ap);
        if (w > 0) {
            o->n += (size_t)w < sizeof(o->buf) - o->n ? (size_t)w : sizeof(o->buf) - o->n - 1;
        }
    }
    va_end(ap);
}

static bool parseRange(const char *s, int lo, int hi, int *out)
{
    int32_t v;

    if (!ucxapp_parse_int(s, &v) || v < lo || v > hi) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool isHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexVal(char c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

/* A GPIO pin by index or, once a board profile is loaded, by name such as PB0. */
static bool parsePin(const char *s, int *out)
{
    if (gCfg != NULL) {
        for (int i = 0; i < gNGpio; i++) {
            if (strcasecmp(s, gCfg->gpio[i]) == 0) {
                *out = i;
                return true;
            }
        }
    }
    return parseRange(s, 0, gNGpio - 1, out);
}

/* Hex bytes from argv[first..]: "01 02", "0x01" or "0102"; -1 on a bad token, else the count. */
static int parseBytes(int argc, char *argv[], int first, uint8_t *out, int max)
{
    int n = 0;

    for (int a = first; a < argc; a++) {
        const char *s = argv[a];
        size_t len;

        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
        }
        len = strlen(s);
        if (len == 0 || (len > 2 && (len & 1))) {
            return -1;
        }
        for (size_t i = 0; i < len; i += (len == 1 ? 1 : 2)) {
            if (!isHex(s[i]) || (len > 1 && !isHex(s[i + 1])) || n >= max) {
                return -1;
            }
            out[n++] = (uint8_t)(len == 1 ? hexVal(s[i]) : hexVal(s[i]) * 16 + hexVal(s[i + 1]));
        }
    }
    return n;
}

static void outBytes(hwOut_t *o, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        outf(o, "%s%02X", i ? " " : "", d[i]);
    }
}

/* ---- operations ---- */

static int opGpioMode(int argc, char *argv[], hwOut_t *o)
{
    static const char *const kModes[] = { "in", "out", "pullup", "pulldown" };
    int pin;

    if (argc != 3 || !parsePin(argv[1], &pin)) {
        return UCXAPP_ERR_USAGE;
    }
    for (int m = 0; m < 4; m++) {
        if (strcasecmp(argv[2], kModes[m]) == 0) {
            if (!gBe->gpioMode(pin, m)) {
                outf(o, "cannot set pin %d to %s", pin, kModes[m]);
                return UCXAPP_ERR_FAILED;
            }
            outf(o, "pin %d is %s", pin, kModes[m]);
            return UCXAPP_OK;
        }
    }
    return UCXAPP_ERR_USAGE;
}

static int opGpioWrite(int argc, char *argv[], hwOut_t *o)
{
    int pin;
    int level;

    if (argc != 3 || !parsePin(argv[1], &pin) || !parseRange(argv[2], 0, 1, &level)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gBe->gpioWrite(pin, level)) {
        outf(o, "cannot write pin %d", pin);
        return UCXAPP_ERR_FAILED;
    }
    outf(o, "pin %d = %d", pin, level);
    return UCXAPP_OK;
}

static int opGpioRead(int argc, char *argv[], hwOut_t *o)
{
    int pin;

    if (argc != 2 || !parsePin(argv[1], &pin)) {
        return UCXAPP_ERR_USAGE;
    }
    outf(o, "pin %d = %d", pin, gBe->gpioRead(pin));
    return UCXAPP_OK;
}

static int opGpioToggle(int argc, char *argv[], hwOut_t *o)
{
    int pin;
    int level;

    if (argc != 2 || !parsePin(argv[1], &pin)) {
        return UCXAPP_ERR_USAGE;
    }
    level = gBe->gpioRead(pin) ? 0 : 1;
    if (!gBe->gpioWrite(pin, level)) {
        outf(o, "cannot write pin %d", pin);
        return UCXAPP_ERR_FAILED;
    }
    outf(o, "pin %d = %d", pin, level);
    return UCXAPP_OK;
}

static int opGpioList(int argc, char *argv[], hwOut_t *o)
{
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (int p = 0; p < gNGpio; p++) {
        if (gCfg != NULL) {
            outf(o, "%s%d:%s=%d", p ? " " : "", p, gCfg->gpio[p], gBe->gpioRead(p));
        } else {
            outf(o, "%s%d=%d", p ? " " : "", p, gBe->gpioRead(p));
        }
    }
    return UCXAPP_OK;
}

/* A user LED is a GPIO output: configure the pin as out, then drive it. */
static int opLed(int argc, char *argv[], hwOut_t *o)
{
    int pin;
    int level;

    if (argc != 3 || !parsePin(argv[1], &pin)) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcasecmp(argv[2], "on") == 0) {
        level = 1;
    } else if (strcasecmp(argv[2], "off") == 0) {
        level = 0;
    } else if (strcasecmp(argv[2], "toggle") == 0) {
        level = -1;
    } else {
        return UCXAPP_ERR_USAGE;
    }
    if (!gBe->gpioMode(pin, GPIO_OUT)) {
        outf(o, "cannot drive LED on pin %d", pin);
        return UCXAPP_ERR_FAILED;
    }
    if (level < 0) {
        level = gBe->gpioRead(pin) ? 0 : 1;
    }
    if (!gBe->gpioWrite(pin, level)) {
        outf(o, "cannot drive LED on pin %d", pin);
        return UCXAPP_ERR_FAILED;
    }
    outf(o, "led on pin %d = %d", pin, level);
    return UCXAPP_OK;
}

static int opI2cScan(int argc, char *argv[], hwOut_t *o)
{
    int found = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (int a = 0x08; a < 0x78; a++) {
        if (gBe->i2cXfer(a, NULL, 0, NULL, 0) == 0) {
            outf(o, "%s0x%02X", found++ ? " " : "found ", a);
        }
    }
    if (found == 0) {
        outf(o, "no devices");
    }
    return UCXAPP_OK;
}

static int opI2cWrite(int argc, char *argv[], hwOut_t *o)
{
    uint8_t buf[MAX_BYTES];
    int addr;
    int n = 0;

    if (argc < 3 || !parseRange(argv[1], 0x08, 0x77, &addr) || (n = parseBytes(argc, argv, 2, buf, MAX_BYTES)) <= 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (gBe->i2cXfer(addr, buf, (size_t)n, NULL, 0) != 0) {
        outf(o, "no ACK from 0x%02X", addr);
        return UCXAPP_ERR_FAILED;
    }
    outf(o, "wrote %d byte(s) to 0x%02X", n, addr);
    return UCXAPP_OK;
}

static int opI2cRead(int argc, char *argv[], hwOut_t *o)
{
    uint8_t buf[MAX_BYTES];
    int addr;
    int len;

    if (argc != 3 || !parseRange(argv[1], 0x08, 0x77, &addr) || !parseRange(argv[2], 1, MAX_BYTES, &len)) {
        return UCXAPP_ERR_USAGE;
    }
    if (gBe->i2cXfer(addr, NULL, 0, buf, (size_t)len) != 0) {
        outf(o, "no ACK from 0x%02X", addr);
        return UCXAPP_ERR_FAILED;
    }
    outBytes(o, buf, (size_t)len);
    return UCXAPP_OK;
}

static int opI2cReg(int argc, char *argv[], hwOut_t *o)
{
    uint8_t buf[MAX_BYTES];
    int addr;
    int reg;
    int len = 1;

    if (argc < 3 || argc > 4 || !parseRange(argv[1], 0x08, 0x77, &addr) || !parseRange(argv[2], 0, 255, &reg) ||
        (argc == 4 && !parseRange(argv[3], 1, MAX_BYTES, &len))) {
        return UCXAPP_ERR_USAGE;
    }
    buf[0] = (uint8_t)reg;
    if (gBe->i2cXfer(addr, buf, 1, buf, (size_t)len) != 0) {
        outf(o, "no ACK from 0x%02X", addr);
        return UCXAPP_ERR_FAILED;
    }
    outBytes(o, buf, (size_t)len);
    return UCXAPP_OK;
}

static int opSpiCfg(int argc, char *argv[], hwOut_t *o)
{
    int mode;
    int32_t hz;

    if (argc != 3 || !parseRange(argv[1], 0, 3, &mode) || !ucxapp_parse_int(argv[2], &hz) || hz < 1000) {
        return UCXAPP_ERR_USAGE;
    }
    gSpiMode = mode;
    gSpiHz = hz;
    outf(o, "spi mode %d at %d Hz", mode, (int)hz);
    return UCXAPP_OK;
}

static int opSpiXfer(int argc, char *argv[], hwOut_t *o)
{
    uint8_t tx[MAX_BYTES];
    uint8_t rx[MAX_BYTES];
    int n = 0;

    if (argc < 2 || (n = parseBytes(argc, argv, 1, tx, MAX_BYTES)) <= 0) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gBe->spiXfer(tx, rx, (size_t)n)) {
        outf(o, "spi transfer failed");
        return UCXAPP_ERR_FAILED;
    }
    outBytes(o, rx, (size_t)n);
    return UCXAPP_OK;
}

static int opAdcRead(int argc, char *argv[], hwOut_t *o)
{
    int ch;
    int raw;

    if (argc != 2 || !parseRange(argv[1], 0, gNAdc - 1, &ch)) {
        return UCXAPP_ERR_USAGE;
    }
    raw = gBe->adcRead(ch);
    if (raw < 0) {
        outf(o, "adc %d is not available", ch);
        return UCXAPP_ERR_FAILED;
    }
    outf(o, "adc %d = %d (%d mV)", ch, raw, raw * VREF_MV / 4095);
    return UCXAPP_OK;
}

static int opPwmSet(int argc, char *argv[], hwOut_t *o)
{
    int ch;
    int32_t hz;
    char *end;
    double duty;

    if (argc != 4 || !parseRange(argv[1], 0, gNPwm - 1, &ch) || !ucxapp_parse_int(argv[2], &hz) || hz < 1) {
        return UCXAPP_ERR_USAGE;
    }
    duty = strtod(argv[3], &end);
    if (*end != '\0' || duty < 0 || duty > 100) {
        return UCXAPP_ERR_USAGE;
    }
    if (!gBe->pwmSet(ch, (uint32_t)hz, (int)(duty * 10 + 0.5))) {
        outf(o, "cannot set pwm %d", ch);
        return UCXAPP_ERR_FAILED;
    }
    gPwm[ch].hz = (uint32_t)hz;
    gPwm[ch].permille = (int)(duty * 10 + 0.5);
    outf(o, "pwm %d: %d Hz, %d.%d %%", ch, (int)hz, gPwm[ch].permille / 10, gPwm[ch].permille % 10);
    return UCXAPP_OK;
}

static int opPwmOff(int argc, char *argv[], hwOut_t *o)
{
    int ch;

    if (argc != 2 || !parseRange(argv[1], 0, gNPwm - 1, &ch)) {
        return UCXAPP_ERR_USAGE;
    }
    (void)gBe->pwmSet(ch, 0, 0);
    gPwm[ch].hz = 0;
    gPwm[ch].permille = 0;
    outf(o, "pwm %d off", ch);
    return UCXAPP_OK;
}

static int opBoard(int argc, char *argv[], hwOut_t *o)
{
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gCfg == NULL) {
        outf(o, "no board profile loaded (hw.load <file.json>)");
        return UCXAPP_OK;
    }
    outf(o, "%s\ni2c scl %s sda %s\nspi sck %s miso %s mosi %s cs %s\ncan rx %s tx %s\n", gCfg->board, gCfg->i2cScl,
         gCfg->i2cSda, gCfg->spiSck, gCfg->spiMiso, gCfg->spiMosi, gCfg->spiCs, gCfg->canRx, gCfg->canTx);
    outf(o, "adc");
    for (int i = 0; i < gNAdc; i++) {
        outf(o, " %d:%s", i, gCfg->adc[i]);
    }
    outf(o, "\npwm");
    for (int i = 0; i < gNPwm; i++) {
        outf(o, " %d:%s(%s)", i, gCfg->pwm[i].pin, gCfg->pwm[i].timer);
    }
    return UCXAPP_OK;
}

static int opStatus(int argc, char *argv[], hwOut_t *o)
{
    int active = 0;

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    outf(o, "backend %s, board %s\n", gBe->name, gCfg != NULL ? gCfg->board : "(none loaded)");
    outf(o, "gpio %d pins, i2c, spi mode %d at %d Hz, adc %d ch\n", gNGpio, gSpiMode, (int)gSpiHz, gNAdc);
    for (int c = 0; c < gNPwm; c++) {
        if (gPwm[c].hz > 0) {
            active++;
        }
    }
    outf(o, "pwm %d of %d channels running", active, gNPwm);
    return UCXAPP_OK;
}

/* name, usage, help */
#define HW_OPS(X) \
    X(opGpioMode, "gpio.mode", "<pin> <in|out|pullup|pulldown>", "Set a GPIO pin mode") \
    X(opGpioWrite, "gpio.write", "<pin> <0|1>", "Drive an output (or simulate an input level)") \
    X(opGpioRead, "gpio.read", "<pin>", "Read a GPIO pin") \
    X(opGpioToggle, "gpio.toggle", "<pin>", "Invert a GPIO output") \
    X(opGpioList, "gpio.list", "", "Levels of all GPIO pins") \
    X(opLed, "led", "<pin> <on|off|toggle>", "Drive a user LED (configures the pin as an output)") \
    X(opI2cScan, "i2c.scan", "", "Scan the I2C bus for devices") \
    X(opI2cWrite, "i2c.write", "<addr> <hex...>", "Write bytes to an I2C device") \
    X(opI2cRead, "i2c.read", "<addr> <len>", "Read bytes from an I2C device") \
    X(opI2cReg, "i2c.reg", "<addr> <reg> [len]", "Read a register (write pointer, then read)") \
    X(opSpiCfg, "spi.cfg", "<mode 0-3> <hz>", "Set the SPI mode and clock") \
    X(opSpiXfer, "spi.xfer", "<hex...>", "Full duplex SPI transfer, prints the bytes read") \
    X(opAdcRead, "adc.read", "<ch>", "Read an ADC channel") \
    X(opPwmSet, "pwm.set", "<ch> <hz> <duty%>", "Start PWM on a channel") \
    X(opPwmOff, "pwm.off", "<ch>", "Stop PWM on a channel") \
    X(opStatus, "status", "", "Backend and peripheral summary") \
    X(opBoard, "board", "", "Pin map of the loaded board profile")

typedef struct {
    const char *name;
    const char *usage;
    int (*fn)(int argc, char *argv[], hwOut_t *o);
} hwOp_t;

#define X_OP(opf, name, usage, help) { name, usage, opf },
static const hwOp_t kOps[] = { HW_OPS(X_OP) };

static void lockHw(void)
{
    if (gLockInit) {
        U_CX_MUTEX_LOCK(gLock);
    }
}

static void unlockHw(void)
{
    if (gLockInit) {
        U_CX_MUTEX_UNLOCK(gLock);
    }
}

static int runOp(const hwOp_t *op, int argc, char *argv[], hwOut_t *o)
{
    int r;

    lockHw();
    r = op->fn(argc, argv, o);
    unlockHw();
    return r;
}

/* ---- shell commands ---- */

#define X_SHELL(opf, name, usage, help) \
    static int sh_##opf(ucxapp_t *app, int argc, char *argv[]) \
    { \
        hwOut_t o = { { 0 }, 0 }; \
        int r; \
        (void)app; \
        for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++) { \
            if (kOps[i].fn == opf) { \
                r = runOp(&kOps[i], argc, argv, &o); \
                if (o.n > 0) { \
                    ucxapp_print("%s\n", o.buf); \
                } \
                return r; \
            } \
        } \
        return UCXAPP_ERR_FAILED; \
    }
HW_OPS(X_SHELL)

/* One request line in, one reply out ("OK ..." or "ERR ..."); also behind hw.exec. */
static void execLine(const char *line, hwOut_t *reply)
{
    char text[200];
    char *argv[UCXAPP_MAX_ARGS];
    int argc = 0;
    hwOut_t o = { { 0 }, 0 };

    snprintf(text, sizeof(text), "%s", line);
    for (char *p = strtok(text, " \t\r"); p != NULL && argc < UCXAPP_MAX_ARGS; p = strtok(NULL, " \t\r")) {
        argv[argc++] = p;
    }
    if (argc == 0) {
        return;
    }
    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++) {
        if (strcasecmp(argv[0], kOps[i].name) == 0) {
            int r = runOp(&kOps[i], argc, argv, &o);
            if (r == UCXAPP_ERR_USAGE) {
                outf(reply, "ERR usage: %s %s", kOps[i].name, kOps[i].usage);
            } else {
                outf(reply, "%s %s", r == UCXAPP_OK ? "OK" : "ERR", o.buf);
            }
            return;
        }
    }
    outf(reply, "ERR unknown command '%s'", argv[0]);
}

static int cmdExec(ucxapp_t *app, int argc, char *argv[])
{
    char line[200] = "";
    hwOut_t reply = { { 0 }, 0 };

    (void)app;
    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 1; i < argc; i++) {
        size_t used = strlen(line);
        snprintf(line + used, sizeof(line) - used, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    execLine(line, &reply);
    ucxapp_print("%s\n", reply.buf);
    return strncmp(reply.buf, "OK", 2) == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* ---- bridge ---- */

static char gRx[200];
static size_t gRxLen;
static int gLastLevel[HWCFG_MAX_GPIO];

static void pushText(const char *text)
{
    if (gStream != NULL) {
        ucxapp_stream_push(gStream, (const uint8_t *)text, strlen(text));
    }
}

/* Bytes from the far side: split into lines and answer each. */
static bool bridgeWrite(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n' || data[i] == '\r') {
            if (gRxLen > 0) {
                hwOut_t reply = { { 0 }, 0 };
                gRx[gRxLen] = '\0';
                gRxLen = 0;
                execLine(gRx, &reply);
                outf(&reply, "\n");
                pushText(reply.buf);
            }
        } else if (gRxLen < sizeof(gRx) - 1) {
            gRx[gRxLen++] = (char)data[i];
        } else {
            gRxLen = 0;   /* overlong line: drop it */
        }
    }
    return true;
}

/* GPIO input changes become EVT lines. */
static void bridgePoll(void *ctx, ucxapp_stream_t *s)
{
    char evt[512];
    size_t n = 0;

    (void)ctx;
    (void)s;
    lockHw();
    for (int p = 0; p < gNGpio && n + 40 < sizeof(evt); p++) {
        int level = gBe->gpioRead(p);
        if (gLastLevel[p] >= 0 && level != gLastLevel[p]) {
            n += (size_t)snprintf(evt + n, sizeof(evt) - n, "EVT gpio %d %d\n", p, level);
        }
        gLastLevel[p] = level;
    }
    unlockHw();
    if (n > 0) {
        evt[n] = '\0';
        pushText(evt);
    }
}

/* Shell only: a remote peer must not be able to name files on this PC. */
static int cmdLoad(ucxapp_t *app, int argc, char *argv[])
{
    char err[96];
    hwcfg_t *c;

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    c = (hwcfg_t *)malloc(sizeof(*c));
    if (c == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_hwcfg_load(argv[1], c, err, sizeof(err))) {
        ucxapp_print("ERROR: %s\n", err);
        free(c);
        return UCXAPP_ERR_FAILED;
    }
    lockHw();
    emuInit();
    gNGpio = c->nGpio;
    gNAdc = c->nAdc;
    gNPwm = c->nPwm;
    memset(gPwm, 0, sizeof(gPwm));
    if (c->nEmuI2c > 0) {
        gEmuN = c->nEmuI2c;
        for (int i = 0; i < gEmuN; i++) {
            gEmuAddr[i] = c->emuI2c[i].addr;
            memcpy(gEmuRegs[i], c->emuI2c[i].regs, sizeof(gEmuRegs[i]));
        }
        memset(gEmuRegs[gEmuN], 0, sizeof(gEmuRegs) - sizeof(gEmuRegs[0]) * (size_t)gEmuN);
    }
    free(gCfg);
    gCfg = c;
    unlockHw();
    ucxapp_print("loaded %s: %d gpio, %d adc, %d pwm, %d emulated i2c devices\n", c->board, c->nGpio, c->nAdc,
                 c->nPwm, gEmuN);
    return UCXAPP_OK;
}

static int cmdBridge(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_stream_cfg_t cfg = { "hw", NULL, bridgePoll, 50, NULL, bridgeWrite };
    bool off;
    int r;

    if (argc < 2 || argc > 6) {
        return UCXAPP_ERR_USAGE;
    }
    off = strcasecmp(argv[1], "off") == 0;
    if (gStream == NULL && (off || strcasecmp(argv[1], "status") == 0)) {
        ucxapp_print("bridge: off\n");
        return UCXAPP_OK;
    }
    if (gStream == NULL) {
        gStream = ucxapp_stream_open("virt:hw", 0, &cfg);
        if (gStream == NULL) {
            return UCXAPP_ERR_FAILED;
        }
        gRxLen = 0;
        for (int p = 0; p < HWCFG_MAX_GPIO; p++) {
            gLastLevel[p] = -1;
        }
    }
    r = ucxapp_stream_bridge_cmd(gStream, app, argc, argv, 7000);
    if (!ucxapp_stream_bridge_active(gStream)) {
        ucxapp_stream_close(gStream);
        gStream = NULL;
    }
    return r;
}

#define X_ENTRY(opf, name, usage, help) { "hw." name, usage, help, 0, sh_##opf },
static const ucxapp_cmd_t kCmds[] = {
    HW_OPS(X_ENTRY)
    { "hw.load", "<file.json>", "Load a board profile (pin names, counts, emulated I2C devices)", 0, cmdLoad },
    { "hw.exec", "<command...>", "Run a bridge line such as 'gpio.read 3' and print the OK/ERR reply", 0, cmdExec },
    { "hw.bridge", "tcp [port] | tcpc <host> <port> | udp <host> <port> [local] | udps <port> | ble [nus|sps|custom <uuid> [rx tx]] | "
      "uart <port> [baud] | off [n] | status", "Expose these commands as a line protocol, with EVT lines for events; up to 4 bridges at once", 0,
      cmdBridge },
};

void ucxapp_register_hw_cmds(void)
{
    if (!gLockInit) {
        U_CX_MUTEX_CREATE(gLock);
        gLockInit = true;
    }
    emuInit();
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
