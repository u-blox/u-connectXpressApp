/* ppk.*: Nordic Power Profiler Kit II (PPK2) over its USB CDC ACM port. Measures the current a u-connectXpress
 * module draws, either powered by the PPK2 (source meter) or in series with its own supply (ampere meter).
 * The protocol follows Nordic's pc-nrfconnect-ppk: one-byte opcodes, a metadata text block with the calibration
 * that ends with "END", then 4-byte little-endian samples at 100 kHz (14-bit ADC, 3-bit range, 8 logic bits). */

#include "ucxapp_cmd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_port.h"
#include "u_port_uart.h"
#include "ucxapp_platform.h"
#include "ucxapp_vport.h"

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#define PPK_VID           0x1915
#define PPK_PID           0xC00A
#define OP_AVERAGE_START  0x06
#define OP_AVERAGE_STOP   0x07
#define OP_DEVICE_RUNNING 0x0C
#define OP_REGULATOR_SET  0x0D
#define OP_SET_POWER_MODE 0x11
#define OP_GET_META_DATA  0x19
#define OP_RESET          0x20
#define VDD_MIN_MV        800
#define VDD_MAX_MV        5000
#define META_SIZE         2048
#define RANGES            5
#define ADC_MULT          (1.8 / 163840.0)
#define SAMPLE_HZ         100000
#define MEASURE_MAX_MS    600000
#define PPK_READ_SIZE     65536   /* 100 kS/s is 400 kB/s */

typedef struct {
    double r[RANGES], gs[RANGES], gi[RANGES], o[RANGES], s[RANGES], i[RANGES], ug[RANGES];
    int calibrated, hw, ia;
} ppkCal_t;

static const ppkCal_t kDefaultCal = {
    { 1031.64, 101.65, 10.15, 0.94, 0.043 }, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 }, { 0 }, { 0 }, { 0 },
    { 1, 1, 1, 1, 1 }, -1, 0, 0
};

static struct {
    uPortUartHandle_t h;
    char port[32];
    ppkCal_t cal;
    bool source;        /* source meter (PPK2 powers the DUT) or ampere meter */
    int32_t vddMv;      /* 0 until ppk.vdd */
    bool dutOn;
} gPpk;

static void put(uPortUartHandle_t h, const uint8_t *cmd, size_t len)
{
    (void)ucxapp_uart_write(h, cmd, len);
}

/* Drops what an earlier session left in the pipe (samples after a bad shutdown). */
static void drain(uPortUartHandle_t h)
{
    uint8_t buf[512];
    uint32_t start = ucxapp_platform_tick_ms();

    while (ucxapp_uart_read(h, buf, sizeof(buf), 20) > 0 && ucxapp_platform_tick_ms() - start < 500) {
    }
}

/* Sends GET_META_DATA and collects the reply up to "END"; returns its length, 0 if none came. */
static size_t readMeta(uPortUartHandle_t h, char *meta, size_t size, uint32_t ms)
{
    static const uint8_t cmd[] = { OP_GET_META_DATA };
    uint32_t start = ucxapp_platform_tick_ms();
    size_t len = 0;

    put(h, cmd, sizeof(cmd));
    while (ucxapp_platform_tick_ms() - start < ms && len + 1 < size) {
        int32_t n = ucxapp_uart_read(h, meta + len, size - 1 - len, 50);
        if (n > 0) {
            len += (size_t)n;
            meta[len] = '\0';
            if (strstr(meta, "END") != NULL) {
                return len;
            }
        }
    }
    return 0;
}

/* "R0: 1031.6" lines into the calibration; R values of 0 (seen on some units) keep the default. */
static void parseMeta(const char *meta, ppkCal_t *cal)
{
    static const char *const kKeys[] = { "R", "GS", "GI", "O", "S", "I", "UG" };
    const char *p = meta;

    *cal = kDefaultCal;
    while (*p != '\0') {
        char key[16];
        double v;
        int used = 0;

        if (sscanf(p, "%15[A-Za-z0-9]: %lf%n", key, &v, &used) == 2) {
            size_t kl = strlen(key);
            if (strcmp(key, "Calibrated") == 0) {
                cal->calibrated = (int)v;
            } else if (strcmp(key, "HW") == 0) {
                cal->hw = (int)v;
            } else if (strcmp(key, "IA") == 0) {
                cal->ia = (int)v;
            } else if (kl >= 2 && key[kl - 1] >= '0' && key[kl - 1] < '0' + RANGES) {
                int r = key[kl - 1] - '0';
                double *tbl[] = { cal->r, cal->gs, cal->gi, cal->o, cal->s, cal->i, cal->ug };
                key[kl - 1] = '\0';
                for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
                    if (strcmp(key, kKeys[k]) == 0 && !(k == 0 && v == 0.0)) {
                        tbl[k][r] = v;
                    }
                }
            }
        }
        p = strchr(p, '\n');
        if (p == NULL) {
            break;
        }
        p++;
    }
}

/* Regulator bytes: 800 mV is { 3, 32 }, then one step per mV. */
static void vddBytes(int32_t mv, uint8_t out[2])
{
    int32_t d = mv - VDD_MIN_MV + 32;

    out[0] = (uint8_t)(3 + d / 256);
    out[1] = (uint8_t)(d % 256);
}

typedef struct {
    double avg, avg4;
    int prevRange;
    int afterSpike, sameRange;
    bool first;
} spike_t;

/* One raw sample to amperes: calibration per range, then the spike filter the PPK2 app uses at range changes. */
static double toAmps(const ppkCal_t *c, uint32_t raw, double vddV, spike_t *f, int *rangeOut)
{
    int range = (int)((raw >> 14) & 7);
    double adc = (double)(raw & 0x3FFF) * 4.0;
    double r0, a, prevAvg = f->avg, prevAvg4 = f->avg4;

    range = range < RANGES ? range : RANGES - 1;
    *rangeOut = range;
    r0 = (adc - c->o[range]) * (ADC_MULT / c->r[range]);
    a = c->ug[range] * (r0 * (c->gs[range] * r0 + c->gi[range]) + (c->s[range] * vddV + c->i[range]));
    if (f->first) {
        f->avg = f->avg4 = a;
        f->prevRange = range;
        f->first = false;
    } else {
        f->avg = 0.18 * a + 0.82 * f->avg;
        f->avg4 = 0.06 * a + 0.94 * f->avg4;
    }
    if (f->prevRange != range || f->afterSpike > 0) {
        if (f->prevRange != range) {
            f->sameRange = 0;
            f->afterSpike = 3;
        } else {
            f->sameRange++;
        }
        if (range == 4) {
            if (f->sameRange < 2) {
                f->avg = prevAvg;
                f->avg4 = prevAvg4;
            }
            a = f->avg4;
        } else {
            a = f->avg;
        }
        f->afterSpike--;
    }
    f->prevRange = range;
    return a;
}

/* ---- Detection ---- */

static bool probe(const char *port, ppkCal_t *cal, char *meta, size_t size)
{
    uPortUartHandle_t h = ucxapp_uart_open(port, 9600);
    size_t n;

    if (h == NULL) {
        return false;
    }
    drain(h);
    n = readMeta(h, meta, size, 800);
    ucxapp_uart_close(h);
    if (n > 0) {
        parseMeta(meta, cal);
    }
    return n > 0;
}

/* Prints every PPK2 found; the first one's port goes to outPort. */
static int detect(const char *only, bool virt, char *outPort, size_t outSize)
{
    static ucxapp_serial_port_t ports[32];
    char *meta = (char *)malloc(META_SIZE);
    bool onlyVirt = only != NULL && ucxapp_vport_is(only);
    int count = onlyVirt ? 0 : ucxapp_platform_list_serial_ports(ports, 32);
    int found = 0;
    ppkCal_t cal;

    if (meta == NULL) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        const ucxapp_serial_port_t *sp = &ports[i];
        bool want = only != NULL ? strcasecmp(sp->name, only) == 0 : ucxapp_port_is_ppk(sp) && sp->available;
        /* the PPK2 also has a second CDC port that does not answer; probing it is harmless */
        if (!want || (gPpk.h != NULL && strcasecmp(sp->name, gPpk.port) == 0) || !probe(sp->name, &cal, meta, META_SIZE)) {
            continue;
        }
        ucxapp_print("  %s%s%s%s: Power Profiler Kit II, HW %d, %s\n", sp->name, sp->desc[0] ? " (" : "", sp->desc,
                     sp->desc[0] ? ")" : "", cal.hw, cal.calibrated == 1 ? "user calibration" : "factory calibration");
        if (found++ == 0 && outPort != NULL) {
            snprintf(outPort, outSize, "%s", sp->name);
        }
    }
    if (onlyVirt || (only == NULL && virt)) {
        if (probe(UCXAPP_VPORT_PPK, &cal, meta, META_SIZE)) {
            ucxapp_print("  %s (%s): Power Profiler Kit II, HW %d\n", UCXAPP_VPORT_PPK,
                         ucxapp_vport_desc(UCXAPP_VPORT_PPK), cal.hw);
            if (found++ == 0 && outPort != NULL) {
                snprintf(outPort, outSize, "%s", UCXAPP_VPORT_PPK);
            }
        }
    }
    free(meta);
    return found;
}

/* ---- Commands ---- */

static bool needOpen(void)
{
    if (gPpk.h == NULL) {
        ucxapp_print("ERROR: no PPK2 open (ppk.open)\n");
        return false;
    }
    return true;
}

static int cmdDetect(ucxapp_t *app, int argc, char *argv[])
{
    int open = 0;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("Looking for a Nordic Power Profiler Kit II (USB %04X:%04X)...\n", PPK_VID, PPK_PID);
    if (gPpk.h != NULL && (argc == 1 || strcasecmp(argv[1], gPpk.port) == 0)) {
        ucxapp_print("  %s: Power Profiler Kit II, HW %d, open now (ppk.status)\n", gPpk.port, gPpk.cal.hw);
        open = 1;
    }
    int n = open + (open == 1 && argc == 2 ? 0 :
                    detect(argc == 2 ? argv[1] : NULL,
                           ucxapp_module_is_virtual(app) && !(open && ucxapp_vport_is(gPpk.port)), NULL, 0));
    ucxapp_print("%d PPK2(s) found\n", n);
    return n > 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdOpen(ucxapp_t *app, int argc, char *argv[])
{
    char port[32] = "";
    char *meta;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gPpk.h != NULL) {
        if (argc == 1 || strcasecmp(argv[1], gPpk.port) == 0) {
            ucxapp_print("PPK2 already open on %s\n", gPpk.port);
            return UCXAPP_OK;
        }
        ucxapp_print("ERROR: a PPK2 is already open on %s (ppk.close first)\n", gPpk.port);
        return UCXAPP_ERR_FAILED;
    }
    if (argc == 2) {
        snprintf(port, sizeof(port), "%s", argv[1]);
    } else if (detect(NULL, ucxapp_module_is_virtual(app), port, sizeof(port)) == 0) {
        ucxapp_print("ERROR: no PPK2 found\n");
        return UCXAPP_ERR_FAILED;
    }
    meta = (char *)malloc(META_SIZE);
    if (meta == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    gPpk.h = ucxapp_uart_open(port, 9600);
    if (gPpk.h == NULL) {
        free(meta);
        ucxapp_print("ERROR: cannot open %s\n", port);
        return UCXAPP_ERR_FAILED;
    }
    drain(gPpk.h);
    if (readMeta(gPpk.h, meta, META_SIZE, 1500) == 0) {
        free(meta);
        ucxapp_uart_close(gPpk.h);
        gPpk.h = NULL;
        ucxapp_print("ERROR: %s does not answer like a PPK2 (no metadata)\n", port);
        return UCXAPP_ERR_FAILED;
    }
    parseMeta(meta, &gPpk.cal);
    free(meta);
    snprintf(gPpk.port, sizeof(gPpk.port), "%s", port);
    gPpk.source = false;
    gPpk.vddMv = 0;
    gPpk.dutOn = false;
    {
        /* start safe: ampere meter, DUT power off, whatever the last session left */
        const uint8_t amp[] = { OP_SET_POWER_MODE, 1 };
        const uint8_t off[] = { OP_DEVICE_RUNNING, 0 };
        put(gPpk.h, amp, sizeof(amp));
        put(gPpk.h, off, sizeof(off));
    }
    ucxapp_print("PPK2 open on %s: HW %d, %s, ampere meter, DUT power off\n", port, gPpk.cal.hw,
                 gPpk.cal.calibrated == 1 ? "user calibration" : "factory calibration");
    return UCXAPP_OK;
}

static int cmdClose(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gPpk.h == NULL) {
        ucxapp_print("no PPK2 open\n");
        return UCXAPP_OK;
    }
    {
        const uint8_t stop[] = { OP_AVERAGE_STOP };
        put(gPpk.h, stop, sizeof(stop));
    }
    ucxapp_uart_close(gPpk.h);
    gPpk.h = NULL;
    ucxapp_print("closed (the DUT power stays as it was)\n");
    return UCXAPP_OK;
}

static int cmdStatus(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Port:        %s\n", gPpk.port);
    ucxapp_print("Hardware:    %d (IA %d)\n", gPpk.cal.hw, gPpk.cal.ia);
    ucxapp_print("Calibration: %s\n", gPpk.cal.calibrated == 1 ? "user calibration" : "factory values from the PPK2");
    ucxapp_print("Mode:        %s\n", gPpk.source ? "source meter (PPK2 powers the DUT)" : "ampere meter (DUT on its own supply)");
    if (gPpk.vddMv > 0) {
        ucxapp_print("VDD:         %d mV\n", (int)gPpk.vddMv);
    } else {
        ucxapp_print("VDD:         not set (ppk.vdd)\n");
    }
    ucxapp_print("DUT power:   %s\n", gPpk.dutOn ? "on" : "off");
    for (int r = 0; r < RANGES; r++) {
        ucxapp_print("Range %d:     R %.3f ohm, offset %.1f\n", r, gPpk.cal.r[r], gPpk.cal.o[r]);
    }
    return UCXAPP_OK;
}

static int cmdMode(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t cmd[2] = { OP_SET_POWER_MODE, 0 };
    (void)app;

    if (argc != 2 || (strcasecmp(argv[1], "amp") != 0 && strcasecmp(argv[1], "source") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    gPpk.source = strcasecmp(argv[1], "source") == 0;
    cmd[1] = gPpk.source ? 2 : 1;
    put(gPpk.h, cmd, sizeof(cmd));
    ucxapp_print("%s\n", gPpk.source ? "source meter: the PPK2 powers the DUT at ppk.vdd when ppk.power on"
                                     : "ampere meter: put the PPK2 in series with the DUT supply");
    return UCXAPP_OK;
}

static int cmdVdd(ucxapp_t *app, int argc, char *argv[])
{
    int32_t mv;
    uint8_t cmd[3] = { OP_REGULATOR_SET, 0, 0 };
    (void)app;

    if (argc != 2 || !ucxapp_parse_int(argv[1], &mv)) {
        return UCXAPP_ERR_USAGE;
    }
    if (mv < VDD_MIN_MV || mv > VDD_MAX_MV) {
        ucxapp_print("ERROR: VDD must be %d..%d mV (NORA-W36 and NORA-B26 run on 3300)\n", VDD_MIN_MV, VDD_MAX_MV);
        return UCXAPP_ERR_FAILED;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    vddBytes(mv, cmd + 1);
    put(gPpk.h, cmd, sizeof(cmd));
    gPpk.vddMv = mv;
    ucxapp_print("VDD %d mV%s\n", (int)mv, gPpk.source ? "" : " (used for the calibration; ampere meter mode)");
    return UCXAPP_OK;
}

static int cmdPower(ucxapp_t *app, int argc, char *argv[])
{
    uint8_t cmd[2] = { OP_DEVICE_RUNNING, 0 };
    bool on;
    (void)app;

    if (argc != 2 || (strcasecmp(argv[1], "on") != 0 && strcasecmp(argv[1], "off") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    on = strcasecmp(argv[1], "on") == 0;
    if (on && gPpk.source && gPpk.vddMv == 0) {
        ucxapp_print("ERROR: set the output voltage first (ppk.vdd 3300)\n");
        return UCXAPP_ERR_FAILED;
    }
    cmd[1] = on ? 1 : 0;
    put(gPpk.h, cmd, sizeof(cmd));
    gPpk.dutOn = on;
    ucxapp_print("DUT power %s\n", on ? "on" : "off");
    return UCXAPP_OK;
}

static void fmtAmps(double a, char *out, size_t size)
{
    double m = fabs(a);

    if (m >= 1.0) {
        snprintf(out, size, "%.3f A", a);
    } else if (m >= 1e-3) {
        snprintf(out, size, "%.3f mA", a * 1e3);
    } else {
        snprintf(out, size, "%.2f uA", a * 1e6);
    }
}

#define CHART_POINTS 200

#define CHART_POINTS 200

/* Collects ms of samples; prints average, min, max, charge and energy, and writes a CSV when asked. */
static int cmdMeasure(ucxapp_t *app, int argc, char *argv[])
{
    static const uint8_t start[] = { OP_AVERAGE_START };
    static const uint8_t stop[] = { OP_AVERAGE_STOP };
    int32_t ms = 1000;
    FILE *csv = NULL;
    uint8_t *buf;
    uint8_t rem[4];
    size_t nRem = 0;
    uint64_t n = 0;
    uint64_t perRange[RANGES] = { 0 };
    double sum = 0, mn = 1e9, mx = -1e9;
    uint8_t logic = 0;
    spike_t f = { 0, 0, 0, 0, 0, true };
    double vddV;
    uint32_t t0;
    char a[32], b[32], c[32];
    static double chMin[CHART_POINTS], chMax[CHART_POINTS], chSum[CHART_POINTS];
    static uint32_t chN[CHART_POINTS];
    uint64_t expect;
    (void)app;

    if (argc > 3 || (argc >= 2 && (!ucxapp_parse_int(argv[1], &ms) || ms < 1 || ms > MEASURE_MAX_MS))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    if (gPpk.vddMv == 0) {
        ucxapp_print("ERROR: set VDD first (ppk.vdd 3300): the calibration depends on it\n");
        return UCXAPP_ERR_FAILED;
    }
    buf = (uint8_t *)malloc(PPK_READ_SIZE);
    if (buf == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    if (argc == 3) {
        csv = fopen(argv[2], "w");
        if (csv == NULL) {
            free(buf);
            ucxapp_print("ERROR: cannot write %s\n", argv[2]);
            return UCXAPP_ERR_FAILED;
        }
        fprintf(csv, "t_us,current_uA,range,logic\n");
    }
    vddV = gPpk.vddMv / 1000.0;
    expect = (uint64_t)ms * (SAMPLE_HZ / 1000);
    memset(chN, 0, sizeof(chN));
    drain(gPpk.h);
    put(gPpk.h, start, sizeof(start));
    t0 = ucxapp_platform_tick_ms();
    /* the window starts at the first sample, so the USB start-up delay does not count as lost samples */
    for (bool first = true; n == 0 ? ucxapp_platform_tick_ms() - t0 < 2000u
                                   : ucxapp_platform_tick_ms() - t0 < (uint32_t)ms;) {
        int32_t got = ucxapp_uart_read(gPpk.h, buf, PPK_READ_SIZE, 20);
        if (got > 0 && first) {
            first = false;
            t0 = ucxapp_platform_tick_ms();
        }
        for (int32_t i = 0; i < got; i++) {
            rem[nRem++] = buf[i];
            if (nRem == 4) {
                uint32_t raw = (uint32_t)rem[0] | ((uint32_t)rem[1] << 8) | ((uint32_t)rem[2] << 16) |
                               ((uint32_t)rem[3] << 24);
                int range;
                double amps = toAmps(&gPpk.cal, raw, vddV, &f, &range);
                nRem = 0;
                perRange[range]++;
                sum += amps;
                mn = amps < mn ? amps : mn;
                mx = amps > mx ? amps : mx;
                logic = (uint8_t)(raw >> 24);
                {
                    uint64_t k = n * CHART_POINTS / expect;
                    size_t j = k < CHART_POINTS ? (size_t)k : CHART_POINTS - 1;
                    if (chN[j]++ == 0) {
                        chMin[j] = chMax[j] = chSum[j] = amps;
                    } else {
                        chMin[j] = amps < chMin[j] ? amps : chMin[j];
                        chMax[j] = amps > chMax[j] ? amps : chMax[j];
                        chSum[j] += amps;
                    }
                }
                if (csv != NULL) {
                    fprintf(csv, "%llu,%.3f,%d,%u\n", (unsigned long long)(n * 1000000ull / SAMPLE_HZ), amps * 1e6,
                            range, logic);
                }
                n++;
            }
        }
    }
    put(gPpk.h, stop, sizeof(stop));
    drain(gPpk.h);
    free(buf);
    if (csv != NULL) {
        fclose(csv);
    }
    if (n == 0) {
        ucxapp_print("ERROR: no samples in %d ms (is the PPK2 connected and powered over USB DATA/POWER?)\n", (int)ms);
        return UCXAPP_ERR_FAILED;
    }
    double avg = sum / (double)n;
    double secs = (double)n / SAMPLE_HZ;
    fmtAmps(avg, a, sizeof(a));
    fmtAmps(mn, b, sizeof(b));
    fmtAmps(mx, c, sizeof(c));
    ucxapp_print("Samples:  %llu (%.3f s of signal at 100 kS/s, read in %d ms)\n", (unsigned long long)n, secs, (int)ms);
    ucxapp_print("Average:  %s\n", a);
    ucxapp_print("Min/max:  %s / %s\n", b, c);
    ucxapp_print("Charge:   %.3f uC over %.3f s\n", avg * secs * 1e6, secs);
    ucxapp_print("Energy:   %.3f uJ at %d mV (%.3f mW average)\n", avg * secs * vddV * 1e6, (int)gPpk.vddMv,
                 avg * vddV * 1e3);
    ucxapp_print("Ranges:   %llu %llu %llu %llu %llu samples (0 = uA .. 4 = A)\n", (unsigned long long)perRange[0],
                 (unsigned long long)perRange[1], (unsigned long long)perRange[2], (unsigned long long)perRange[3],
                 (unsigned long long)perRange[4]);
    ucxapp_print("Logic:    0x%02X (D0-D7 at the last sample)\n", logic);
    if (csv != NULL) {
        ucxapp_print("CSV:      %s\n", argv[2]);
    }
    /* min,avg,max in uA per time slot, drawn as a chart by the web page */
    ucxapp_print("Chart:    %.3f s uA avg %.2f", secs, avg * 1e6);
    for (size_t j = 0; j < CHART_POINTS; j++) {
        if (chN[j] > 0) {
            ucxapp_print(" %.2f,%.2f,%.2f", chMin[j] * 1e6, chSum[j] / chN[j] * 1e6, chMax[j] * 1e6);
        }
    }
    ucxapp_print("\n");
    return UCXAPP_OK;
}

static int cmdMeta(ucxapp_t *app, int argc, char *argv[])
{
    char *meta;
    (void)app;
    (void)argv;

    if (argc > 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needOpen()) {
        return UCXAPP_ERR_FAILED;
    }
    meta = (char *)malloc(META_SIZE);
    if (meta == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    drain(gPpk.h);
    if (readMeta(gPpk.h, meta, META_SIZE, 1500) == 0) {
        free(meta);
        ucxapp_print("ERROR: no metadata from %s\n", gPpk.port);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s", meta);
    if (meta[strlen(meta) - 1] != '\n') {
        ucxapp_print("\n");
    }
    free(meta);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t kCmds[] = {
    { "ppk.detect", "[port]", "Find a Nordic Power Profiler Kit II on USB (a virtual module also offers virt:ppk, a simulated one)", 0, cmdDetect },
    { "ppk.open", "[port]", "Open the PPK2 (detects it when omitted); starts as ampere meter with the DUT power off", 0, cmdOpen },
    { "ppk.close", "", "Close the PPK2", 0, cmdClose },
    { "ppk.status", "", "Port, hardware, calibration, mode, VDD and DUT power", 0, cmdStatus },
    { "ppk.mode", "amp|source", "Ampere meter (in series with the DUT supply) or source meter (the PPK2 powers the DUT)", 0, cmdMode },
    { "ppk.vdd", "<mV>", "Source voltage 800..5000 mV (3300 for NORA-W36 / NORA-B26); also used by the calibration", 0, cmdVdd },
    { "ppk.power", "on|off", "Switch the DUT power output of the PPK2", 0, cmdPower },
    { "ppk.measure", "[ms=1000] [file.csv]", "Sample at 100 kHz: average, min, max, charge and energy; the CSV gets every sample", 0, cmdMeasure },
    { "ppk.meta", "", "Raw calibration metadata from the PPK2", 0, cmdMeta },
};

void ucxapp_register_ppk_cmds(void)
{
    (void)ucxapp_cmd_register(kCmds, sizeof(kCmds) / sizeof(kCmds[0]));
}
