#include "ucxapp_hwcfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8
#define PATH_LEN  128

typedef struct {
    hwcfg_t *c;
    char *err;
    size_t errSize;
} ctx_t;

static void fail(ctx_t *x, const char *fmt, const char *a)
{
    if (x->err[0] == '\0') {
        snprintf(x->err, x->errSize, fmt, a);
    }
}

static bool validPin(const char *s)
{
    size_t n = strlen(s);

    return n >= 3 && n < HWCFG_PIN_LEN && s[0] == 'P' && s[1] >= 'A' && s[1] <= 'K' && isdigit((unsigned char)s[2]) &&
           (n == 3 || (isdigit((unsigned char)s[3]) && n == 4));
}

static bool setPin(ctx_t *x, char *dst, const char *path, const char *v)
{
    if (!validPin(v)) {
        fail(x, "bad pin name '%s'", v);
        (void)path;
        return false;
    }
    snprintf(dst, HWCFG_PIN_LEN, "%s", v);
    return true;
}

/* Matches "name[N]" followed by rest; returns true and sets *idx and *rest. */
static bool indexed(const char *path, const char *name, int *idx, const char **rest)
{
    size_t n = strlen(name);
    int used = 0;

    if (strncmp(path, name, n) != 0 || path[n] != '[' || sscanf(path + n, "[%d]%n", idx, &used) != 1 || *idx < 0) {
        return false;
    }
    *rest = path + n + used;
    return true;
}

static bool applyArray(ctx_t *x, char (*arr)[HWCFG_PIN_LEN], int *count, int max, int idx, const char *v)
{
    if (idx >= max) {
        fail(x, "too many entries in '%s'", v);
        return false;
    }
    if (!setPin(x, arr[idx], "", v)) {
        return false;
    }
    if (idx + 1 > *count) {
        *count = idx + 1;
    }
    return true;
}

static bool applyEmu(ctx_t *x, const char *path, const char *v)
{
    hwcfg_t *c = x->c;
    int idx;
    const char *rest;
    char *end;
    long n;

    if (!indexed(path, "emu.i2c", &idx, &rest) || idx >= HWCFG_MAX_EMU) {
        return false;
    }
    n = strtol(v, &end, 0);
    if (idx + 1 > c->nEmuI2c) {
        c->nEmuI2c = idx + 1;
    }
    if (strcmp(rest, ".addr") == 0 && *end == '\0' && n >= 0x08 && n < 0x78) {
        c->emuI2c[idx].addr = (int)n;
        return true;
    }
    if (strncmp(rest, ".regs.", 6) == 0 && *end == '\0' && n >= 0 && n <= 255) {
        long reg = strtol(rest + 6, &end, 0);
        if (*end == '\0' && reg >= 0 && reg <= 255) {
            c->emuI2c[idx].regs[reg] = (uint8_t)n;
            return true;
        }
    }
    return false;
}

static bool apply(ctx_t *x, const char *path, const char *v)
{
    hwcfg_t *c = x->c;
    int idx;
    const char *rest;

    if (strcmp(path, "board") == 0) {
        snprintf(c->board, sizeof(c->board), "%s", v);
        return true;
    }
    if (indexed(path, "gpio", &idx, &rest) && *rest == '\0') {
        return applyArray(x, c->gpio, &c->nGpio, HWCFG_MAX_GPIO, idx, v);
    }
    if (indexed(path, "adc", &idx, &rest) && *rest == '\0') {
        return applyArray(x, c->adc, &c->nAdc, HWCFG_MAX_ADC, idx, v);
    }
    if (indexed(path, "reserved", &idx, &rest) && *rest == '\0') {
        return applyArray(x, c->reserved, &c->nReserved, HWCFG_MAX_RES, idx, v);
    }
    if (indexed(path, "pwm", &idx, &rest) && idx < HWCFG_MAX_PWM) {
        if (idx + 1 > c->nPwm) {
            c->nPwm = idx + 1;
        }
        if (strcmp(rest, ".pin") == 0) {
            return setPin(x, c->pwm[idx].pin, path, v);
        }
        if (strcmp(rest, ".timer") == 0) {
            snprintf(c->pwm[idx].timer, sizeof(c->pwm[idx].timer), "%s", v);
            return true;
        }
    }
    if (strcmp(path, "i2c.scl") == 0) {
        return setPin(x, c->i2cScl, path, v);
    }
    if (strcmp(path, "i2c.sda") == 0) {
        return setPin(x, c->i2cSda, path, v);
    }
    if (strcmp(path, "spi.sck") == 0) {
        return setPin(x, c->spiSck, path, v);
    }
    if (strcmp(path, "spi.miso") == 0) {
        return setPin(x, c->spiMiso, path, v);
    }
    if (strcmp(path, "spi.mosi") == 0) {
        return setPin(x, c->spiMosi, path, v);
    }
    if (strcmp(path, "spi.cs") == 0) {
        return setPin(x, c->spiCs, path, v);
    }
    if (strcmp(path, "can.rx") == 0) {
        return setPin(x, c->canRx, path, v);
    }
    if (strcmp(path, "can.tx") == 0) {
        return setPin(x, c->canTx, path, v);
    }
    if (strcmp(path, "button.pin") == 0) {
        return setPin(x, c->button, path, v);
    }
    if (strcmp(path, "button.pressed") == 0 && (strcmp(v, "0") == 0 || strcmp(v, "1") == 0)) {
        c->buttonPressed = v[0] - '0';
        return true;
    }
    if (strcmp(path, "led") == 0) {
        return setPin(x, c->led, path, v);
    }
    if (strncmp(path, "emu.", 4) == 0 && applyEmu(x, path, v)) {
        return true;
    }
    fail(x, "unknown or invalid key '%s'", path);
    return false;
}

static const char *skipWs(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static const char *parseStr(const char *p, char *out, size_t n)
{
    size_t i = 0;

    for (p++; *p != '\0' && *p != '"'; p++) {
        if (*p == '\\' && *++p == '\0') {
            return NULL;
        }
        if (i + 1 < n) {
            out[i++] = *p;
        }
    }
    if (*p != '"') {
        return NULL;
    }
    out[i] = '\0';
    return p + 1;
}

/* Walks the document and calls apply() for each scalar with its dotted path, e.g. "pwm[0].pin". */
static const char *parseVal(const char *p, char *path, int depth, ctx_t *x)
{
    size_t base = strlen(path);
    char v[64];
    size_t n = 0;

    p = skipWs(p);
    if (depth > MAX_DEPTH) {
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        bool obj = *p == '{';
        int idx = 0;

        p = skipWs(p + 1);
        if (*p == (obj ? '}' : ']')) {
            return p + 1;
        }
        for (;;) {
            p = skipWs(p);
            if (obj) {
                char key[40];
                if (*p != '"' || (p = parseStr(p, key, sizeof(key))) == NULL || *(p = skipWs(p)) != ':') {
                    return NULL;
                }
                p++;
                snprintf(path + base, PATH_LEN - base, "%s%s", base ? "." : "", key);
            } else {
                snprintf(path + base, PATH_LEN - base, "[%d]", idx++);
            }
            p = parseVal(p, path, depth + 1, x);
            path[base] = '\0';
            if (p == NULL) {
                return NULL;
            }
            p = skipWs(p);
            if (*p == ',') {
                p++;
            } else if (*p == (obj ? '}' : ']')) {
                return p + 1;
            } else {
                return NULL;
            }
        }
    }
    if (*p == '"') {
        p = parseStr(p, v, sizeof(v));
        if (p == NULL) {
            return NULL;
        }
    } else {
        while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) {
            if (n + 1 < sizeof(v)) {
                v[n++] = *p;
            }
            p++;
        }
        v[n] = '\0';
        if (n == 0) {
            return NULL;
        }
    }
    return apply(x, path, v) ? p : NULL;
}

/* Collects every pin the profile assigns, to detect clashes with each other and with "reserved". */
static void checkPins(ctx_t *x)
{
    const hwcfg_t *c = x->c;
    const char *used[HWCFG_MAX_GPIO + HWCFG_MAX_ADC + HWCFG_MAX_PWM + 16];
    int n = 0;

    for (int i = 0; i < c->nGpio; i++) {
        used[n++] = c->gpio[i];
    }
    for (int i = 0; i < c->nAdc; i++) {
        used[n++] = c->adc[i];
    }
    for (int i = 0; i < c->nPwm; i++) {
        used[n++] = c->pwm[i].pin;
    }
    used[n++] = c->i2cScl;
    used[n++] = c->i2cSda;
    used[n++] = c->spiSck;
    used[n++] = c->spiMiso;
    used[n++] = c->spiMosi;
    used[n++] = c->spiCs;
    used[n++] = c->canRx;
    used[n++] = c->canTx;
    for (int i = 0; i < n; i++) {
        if (used[i][0] == '\0') {
            continue;
        }
        for (int j = i + 1; j < n; j++) {
            if (strcmp(used[i], used[j]) == 0) {
                fail(x, "pin %s is assigned twice", used[i]);
                return;
            }
        }
        for (int r = 0; r < c->nReserved; r++) {
            if (strcmp(used[i], c->reserved[r]) == 0) {
                fail(x, "pin %s is reserved", used[i]);
                return;
            }
        }
    }
    for (int i = 0; i < c->nGpio; i++) {
        if (c->gpio[i][0] == '\0') {
            fail(x, "%s: gpio list has a gap", c->board);
            return;
        }
    }
    for (int r = 0; r < 2; r++) {
        const char *pin = r == 0 ? c->button : c->led;
        bool found = pin[0] == '\0';

        for (int i = 0; i < c->nGpio && !found; i++) {
            found = strcmp(pin, c->gpio[i]) == 0;
        }
        if (!found) {
            fail(x, "%s is not one of the gpio pins", pin);
            return;
        }
    }
}

bool ucxapp_hwcfg_parse(const char *json, hwcfg_t *cfg, char *err, size_t errSize)
{
    ctx_t x = { cfg, err, errSize };
    char path[PATH_LEN] = "";
    const char *end;

    err[0] = '\0';
    memset(cfg, 0, sizeof(*cfg));
    cfg->buttonPressed = 1;
    end = skipWs(json);
    if (*end != '{') {
        snprintf(err, errSize, "not a JSON object");
        return false;
    }
    end = parseVal(end, path, 0, &x);
    if (end == NULL || *skipWs(end) != '\0') {
        if (err[0] == '\0') {
            snprintf(err, errSize, "JSON syntax error");
        }
        return false;
    }
    checkPins(&x);
    return err[0] == '\0';
}

bool ucxapp_hwcfg_load(const char *path, hwcfg_t *cfg, char *err, size_t errSize)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long size;
    bool ok;

    if (f == NULL) {
        snprintf(err, errSize, "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) {
        fclose(f);
        snprintf(err, errSize, "%s is empty or larger than 64 KB", path);
        return false;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        snprintf(err, errSize, "cannot read %s", path);
        return false;
    }
    fclose(f);
    buf[size] = '\0';
    ok = ucxapp_hwcfg_parse(buf, cfg, err, errSize);
    free(buf);
    return ok;
}
