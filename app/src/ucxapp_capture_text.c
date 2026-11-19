/* A text view of the sniff.* and wlanpi.* captures: one line per packet (time, channel, signal, frame type,
 * addresses, name or SSID) or a per-device summary, so a quick look needs no Wireshark. Reads classic pcap in
 * either byte order: Nordic BLE (link type 272, nrfutil ble-sniffer), 802.11 with radiotap (127, tcpdump in
 * monitor mode) and Ethernet (1). PC builds only. */

#include "ucxapp_cmd.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_capture.h"

#ifndef __EMSCRIPTEN__

#define LINK_ETHERNET 1
#define LINK_RADIOTAP 127
#define LINK_NORDIC_BLE 272
#define ADV_AA 0x8E89BED6u
#define MAX_DEVICES 256

typedef struct {
    char addr[24];
    char label[40];
    uint32_t count;
    int best;        /* dBm, 0 when unknown */
    int channel;
} device_t;

typedef struct {
    char addr[24];   /* the device the line is about, for the summary */
    char label[40];  /* its name or SSID if the packet carries one */
    int rssi;
    int channel;
} pkt_t;

static uint32_t rd32(const uint8_t *p, bool be)
{
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void cat(char *out, size_t size, const char *fmt, ...)
{
    size_t len = strlen(out);
    va_list ap;

    if (len + 1 >= size) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(out + len, size - len, fmt, ap);
    va_end(ap);
}

/* addr[5..0], most significant first as Bluetooth addresses are written. */
static void bleAddr(const uint8_t *a, bool random, char *out, size_t size)
{
    (void)snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X%s", a[5], a[4], a[3], a[2], a[1], a[0],
                   random ? " r" : "");
}

static void macAddr(const uint8_t *a, char *out, size_t size)
{
    (void)snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Printable text, at most size-1 characters; others become '.'. */
static void text(const uint8_t *p, size_t n, char *out, size_t size)
{
    size_t i;

    for (i = 0; i < n && i + 1 < size; i++) {
        out[i] = p[i] >= 0x20 && p[i] < 0x7F ? (char)p[i] : '.';
    }
    out[i] = '\0';
}

/* Advertising data: the name and the manufacturer's company ID. */
static void adData(const uint8_t *p, size_t n, char *line, size_t size, pkt_t *pk)
{
    size_t i = 0;

    while (i + 1 < n && p[i] != 0 && i + 1 + p[i] <= n) {
        uint8_t len = p[i], type = p[i + 1];

        if ((type == 0x08 || type == 0x09) && len > 1) {
            text(&p[i + 2], (size_t)len - 1, pk->label, sizeof(pk->label));
            cat(line, size, " '%s'", pk->label);
        } else if (type == 0xFF && len >= 3) {
            cat(line, size, " mfr 0x%04X", le16(&p[i + 2]));
        }
        i += (size_t)len + 1;
    }
}

static const char *llControl(uint8_t op)
{
    static const char *const kNames[] = {
        "CONNECTION_UPDATE_IND", "CHANNEL_MAP_IND", "TERMINATE_IND", "ENC_REQ", "ENC_RSP", "START_ENC_REQ",
        "START_ENC_RSP", "UNKNOWN_RSP", "FEATURE_REQ", "FEATURE_RSP", "PAUSE_ENC_REQ", "PAUSE_ENC_RSP",
        "VERSION_IND", "REJECT_IND", "PERIPHERAL_FEATURE_REQ", "CONNECTION_PARAM_REQ", "CONNECTION_PARAM_RSP",
        "REJECT_EXT_IND", "PING_REQ", "PING_RSP", "LENGTH_REQ", "LENGTH_RSP", "PHY_REQ", "PHY_RSP", "PHY_UPDATE_IND"
    };
    return op < sizeof(kNames) / sizeof(kNames[0]) ? kNames[op] : "LL control";
}

static const char *attOp(uint8_t op)
{
    switch (op) {
    case 0x01: return "ATT error";
    case 0x02: return "ATT MTU request";
    case 0x03: return "ATT MTU response";
    case 0x04: return "ATT find information";
    case 0x05: return "ATT information";
    case 0x08: return "ATT read by type";
    case 0x09: return "ATT read by type response";
    case 0x0A: return "ATT read";
    case 0x0B: return "ATT read response";
    case 0x10: return "ATT read by group type";
    case 0x11: return "ATT read by group type response";
    case 0x12: return "ATT write request";
    case 0x13: return "ATT write response";
    case 0x1B: return "ATT notification";
    case 0x1D: return "ATT indication";
    case 0x1E: return "ATT confirmation";
    case 0x52: return "ATT write command";
    default: return "ATT";
    }
}

/* Nordic BLE pseudo header (protocol 2 and 3): board, length(2), version, counter(2), id, then the packet header
 * (length, flags, channel, RSSI, event counter(2), time(4)) and the link layer packet. */
static bool bleLine(const uint8_t *d, size_t n, char *line, size_t size, pkt_t *pk)
{
    static const char *const kAdv[] = { "ADV_IND", "ADV_DIRECT_IND", "ADV_NONCONN_IND", "SCAN_REQ", "SCAN_RSP",
                                        "CONNECT_IND", "ADV_SCAN_IND", "ADV_EXT_IND" };
    size_t ll;
    uint8_t flags;
    const uint8_t *p;
    uint8_t len;

    if (n < 8 || d[3] < 2 || (ll = 7u + d[7]) + 6 > n) {
        return false;
    }
    flags = d[8];
    pk->channel = d[9];
    pk->rssi = -(int)d[10];
    p = &d[ll + 4];
    len = p[1];
    cat(line, size, "ch%-2d %4d dBm  ", pk->channel, pk->rssi);
    if (!(flags & 0x01)) {
        cat(line, size, "CRC error  ");
    }
    if (rd32(&d[ll], false) == ADV_AA) {
        uint8_t type = p[0] & 0x0F;
        bool tx = (p[0] & 0x40) != 0, rx = (p[0] & 0x80) != 0;
        const uint8_t *pl = &p[2];
        size_t avail = n - ll - 6 < len ? n - ll - 6 : len;
        char other[24];

        cat(line, size, "%-15s", type < 8 ? kAdv[type] : "ADV");
        if ((type == 3 || type == 5) && avail >= 12) {
            bleAddr(&pl[6], rx, pk->addr, sizeof(pk->addr));
            bleAddr(pl, tx, other, sizeof(other));
            cat(line, size, " %s <- %s", pk->addr, other);
        } else if (type == 7 && avail >= 2) {
            size_t ext = (size_t)(pl[0] & 0x3F);

            if (ext >= 7 && (pl[1] & 0x01) && avail >= 8) {
                bleAddr(&pl[2], tx, pk->addr, sizeof(pk->addr));
                cat(line, size, " %s", pk->addr);
            }
            if (1 + ext < avail) {
                adData(&pl[1 + ext], avail - 1 - ext, line, size, pk);
            }
        } else if (avail >= 6) {
            bleAddr(pl, tx, pk->addr, sizeof(pk->addr));
            cat(line, size, " %s", pk->addr);
            if (type != 1 && type != 3 && type != 5) {
                adData(&pl[6], avail - 6, line, size, pk);
            }
        }
        return true;
    }
    (void)snprintf(pk->addr, sizeof(pk->addr), "AA %08X", (unsigned)rd32(&d[ll], false));
    cat(line, size, "%-15s %s", (flags & 0x02) ? "data C->P" : "data P->C", pk->addr);
    if (flags & 0x04) {
        cat(line, size, " encrypted");
    }
    if (len == 0) {
        cat(line, size, " empty");
    } else if ((p[0] & 0x03) == 3 && ll + 7 <= n) {
        cat(line, size, " %s", llControl(p[2]));
    } else if ((p[0] & 0x03) == 2 && len >= 5 && ll + 11 <= n && !(flags & 0x04)) {
        uint16_t cid = le16(&p[4]);

        cat(line, size, " %s", cid == 4 ? attOp(p[6]) : cid == 6 ? "SMP" : cid == 5 ? "L2CAP signaling" : "L2CAP");
    } else {
        cat(line, size, " %u B", (unsigned)len);
    }
    return true;
}

static int wifiChannel(int mhz)
{
    if (mhz == 2484) {
        return 14;
    }
    if (mhz >= 2412 && mhz < 2484) {
        return (mhz - 2407) / 5;
    }
    if (mhz >= 5955) {
        return (mhz - 5950) / 5;
    }
    return mhz >= 5000 ? (mhz - 5000) / 5 : 0;
}

/* The SSID element among the information elements at p. */
static void ssid(const uint8_t *p, size_t n, char *line, size_t size, pkt_t *pk)
{
    size_t i = 0;

    while (i + 2 <= n && i + 2 + p[i + 1] <= n) {
        if (p[i] == 0) {
            size_t len = p[i + 1], z = 0;

            while (z < len && p[i + 2 + z] == 0) {
                z++;
            }
            if (len == 0 || z == len) {
                (void)snprintf(pk->label, sizeof(pk->label), "<hidden>");
            } else {
                text(&p[i + 2], len, pk->label, sizeof(pk->label));
            }
            cat(line, size, " '%s'", pk->label);
            return;
        }
        i += 2u + p[i + 1];
    }
}

/* Radiotap: the channel and the signal, then the 802.11 frame. */
static bool wifiLine(const uint8_t *d, size_t n, char *line, size_t size, pkt_t *pk)
{
    static const char *const kMgmt[] = { "Assoc request", "Assoc response", "Reassoc request", "Reassoc response",
                                         "Probe request", "Probe response", "Timing advert", "Mgmt 7", "Beacon",
                                         "ATIM", "Disassoc", "Auth", "Deauth", "Action", "Action no ack",
                                         "Mgmt 15" };
    size_t rt, off, hdr;
    uint32_t present;
    uint16_t fc;
    const uint8_t *f;
    uint8_t type, sub, rtFlags = 0;
    char a1[24], a2[24];

    if (n < 8 || (rt = le16(&d[2])) > n) {
        return false;
    }
    present = rd32(&d[4], false);
    off = 8;
    for (uint32_t w = present; (w & 0x80000000u) && off + 4 <= rt; off += 4) {
        w = rd32(&d[off], false);
    }
    for (int bit = 0; bit <= 5 && off <= rt; bit++) {
        static const uint8_t kSize[] = { 8, 1, 1, 4, 2, 1 }, kAlign[] = { 8, 1, 1, 2, 1, 1 };

        if (!(present & (1u << bit))) {
            continue;
        }
        off = (off + kAlign[bit] - 1) & ~(size_t)(kAlign[bit] - 1);
        if (off + kSize[bit] > rt) {
            break;
        }
        if (bit == 1) {
            rtFlags = d[off];
        } else if (bit == 3) {
            pk->channel = wifiChannel(le16(&d[off]));
        } else if (bit == 5) {
            pk->rssi = (int8_t)d[off];
        }
        off += kSize[bit];
    }
    f = &d[rt];
    n -= rt;
    if (rtFlags & 0x10) {
        n = n >= 4 ? n - 4 : 0;   /* FCS at the end */
    }
    if (n < 10) {
        return false;
    }
    cat(line, size, "ch%-3d %4d dBm  ", pk->channel, pk->rssi);
    fc = le16(f);
    type = (uint8_t)((fc >> 2) & 3);
    sub = (uint8_t)((fc >> 4) & 15);
    macAddr(&f[4], a1, sizeof(a1));
    if (type == 1) {
        static const char *const kCtl[] = { "Ctrl 0", "Ctrl 1", "Trigger", "TACK", "Beamforming poll", "NDP announce",
                                            "Ctrl ext", "Ctrl wrapper", "Block ack request", "Block ack", "PS-Poll",
                                            "RTS", "CTS", "ACK", "CF-End", "CF-End ack" };

        cat(line, size, "%-17s", kCtl[sub]);
        if (n >= 16 && sub != 12 && sub != 13) {
            macAddr(&f[10], pk->addr, sizeof(pk->addr));
            cat(line, size, " %s -> %s", pk->addr, a1);
        } else {
            cat(line, size, " -> %s", a1);
        }
        return true;
    }
    if (n < 24) {
        return false;
    }
    macAddr(&f[10], pk->addr, sizeof(pk->addr));
    macAddr(&f[10], a2, sizeof(a2));
    if (type == 0) {
        const uint8_t *body = &f[24];
        size_t len = n - 24;

        cat(line, size, "%-17s %s -> %s", kMgmt[sub], a2, a1);
        if ((sub == 8 || sub == 5) && len > 12) {
            ssid(&body[12], len - 12, line, size, pk);
        } else if (sub == 4) {
            ssid(body, len, line, size, pk);
        } else if (sub == 0 && len > 4) {
            ssid(&body[4], len - 4, line, size, pk);
        } else if (sub == 2 && len > 10) {
            ssid(&body[10], len - 10, line, size, pk);
        } else if ((sub == 10 || sub == 12) && len >= 2) {
            cat(line, size, " reason %u", le16(body));
        } else if (sub == 11 && len >= 6) {
            cat(line, size, " seq %u status %u", le16(&body[2]), le16(&body[4]));
        } else if ((sub == 1 || sub == 3) && len >= 4) {
            cat(line, size, " status %u", le16(&body[2]));
        }
        if (f[1] & 0x08) {
            cat(line, size, " retry");
        }
        return true;
    }
    hdr = 24 + ((f[1] & 0x03) == 0x03 ? 6 : 0) + ((sub & 0x08) ? 2 : 0) + ((sub & 0x08) && (f[1] & 0x80) ? 4 : 0);
    cat(line, size, "%-17s %s -> %s", (sub & 0x04) ? ((sub & 0x08) ? "QoS Null" : "Null")
                                                   : ((sub & 0x08) ? "QoS Data" : "Data"), a2, a1);
    if (f[1] & 0x40) {
        cat(line, size, " protected");
    } else if (hdr + 8 + 4 + 13 + 32 <= n && memcmp(&f[hdr], "\xAA\xAA\x03\x00\x00\x00\x88\x8E", 8) == 0 &&
               f[hdr + 9] == 3) {
        const uint8_t *k = &f[hdr + 12];   /* key descriptor */
        uint16_t info = (uint16_t)(k[1] << 8 | k[2]);
        bool ack = (info & 0x0080) != 0, mic = (info & 0x0100) != 0;
        size_t z = 0;

        while (z < 32 && k[13 + z] == 0) {
            z++;
        }
        cat(line, size, " EAPOL key %d/4", ack ? (mic ? 3 : 1) : (z == 32 ? 4 : 2));
    }
    if (n > hdr) {
        cat(line, size, " %u B", (unsigned)(n - hdr));
    }
    if (f[1] & 0x08) {
        cat(line, size, " retry");
    }
    return true;
}

static bool ethLine(const uint8_t *d, size_t n, char *line, size_t size, pkt_t *pk)
{
    char dst[24];

    if (n < 14) {
        return false;
    }
    macAddr(&d[6], pk->addr, sizeof(pk->addr));
    macAddr(d, dst, sizeof(dst));
    cat(line, size, "%s -> %s  type 0x%04X  %u B", pk->addr, dst, (unsigned)(d[12] << 8 | d[13]), (unsigned)n);
    return true;
}

static bool contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay != '\0'; hay++) {
        size_t i = 0;

        while (i < n && hay[i] != '\0' && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0;
}

static void addDevice(device_t *devs, int *count, const pkt_t *pk)
{
    int i;

    if (pk->addr[0] == '\0') {
        return;
    }
    for (i = 0; i < *count && strcmp(devs[i].addr, pk->addr) != 0; i++) {
    }
    if (i == *count) {
        if (*count == MAX_DEVICES) {
            return;
        }
        memset(&devs[i], 0, sizeof(devs[i]));
        (void)snprintf(devs[i].addr, sizeof(devs[i].addr), "%s", pk->addr);
        (*count)++;
    }
    devs[i].count++;
    if (pk->label[0] != '\0') {
        (void)snprintf(devs[i].label, sizeof(devs[i].label), "%s", pk->label);
    }
    if (pk->rssi != 0 && (devs[i].best == 0 || pk->rssi > devs[i].best)) {
        devs[i].best = pk->rssi;
    }
    if (pk->channel != 0) {
        devs[i].channel = pk->channel;
    }
}

static int byCount(const void *a, const void *b)
{
    const device_t *x = (const device_t *)a, *y = (const device_t *)b;

    return x->count != y->count ? (x->count < y->count ? 1 : -1) : strcmp(x->addr, y->addr);
}

int ucxapp_capture_show(int argc, char *argv[], const char *dflt)
{
    const char *path = NULL, *filter = "";
    int32_t count = 50, skip = 0;
    bool summary = false, be, nano;
    uint8_t hdr[24], rec[16];
    uint8_t *buf = NULL;
    device_t *devs = NULL;
    uint32_t link, total = 0, matched = 0, shown = 0, sec0 = 0, frac0 = 0;
    int ndev = 0, rc = UCXAPP_OK;
    FILE *f;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "count=", 6) == 0 && ucxapp_parse_int(argv[i] + 6, &count) && count >= 0) {
        } else if (strncmp(argv[i], "skip=", 5) == 0 && ucxapp_parse_int(argv[i] + 5, &skip) && skip >= 0) {
        } else if (strncmp(argv[i], "filter=", 7) == 0) {
            filter = argv[i] + 7;
        } else if (strcmp(argv[i], "summary") == 0) {
            summary = true;
        } else if (strchr(argv[i], '=') == NULL && path == NULL) {
            path = argv[i];
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    if (path == NULL) {
        path = dflt;
    }
    if (path == NULL || path[0] == '\0') {
        ucxapp_print("ERROR: no capture yet; give a file\n");
        return UCXAPP_ERR_FAILED;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", path);
        return UCXAPP_ERR_FAILED;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        hdr[0] = 0;
    }
    be = rd32(hdr, true) == 0xA1B2C3D4u || rd32(hdr, true) == 0xA1B23C4Du;
    if (!be && rd32(hdr, false) != 0xA1B2C3D4u && rd32(hdr, false) != 0xA1B23C4Du) {
        ucxapp_print("ERROR: %s is not a pcap file (pcapng: open it with sniff.view)\n", path);
        fclose(f);
        return UCXAPP_ERR_FAILED;
    }
    nano = rd32(hdr, be) == 0xA1B23C4Du;
    link = rd32(&hdr[20], be) & 0x0FFFFFFFu;
    buf = (uint8_t *)malloc(65536);
    devs = summary ? (device_t *)calloc(MAX_DEVICES, sizeof(device_t)) : NULL;
    if (buf == NULL || (summary && devs == NULL)) {
        ucxapp_print("ERROR: out of memory\n");
        rc = UCXAPP_ERR_FAILED;
        goto done;
    }
    while (fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
        uint32_t sec = rd32(rec, be), frac = rd32(&rec[4], be), cap = rd32(&rec[8], be);
        char line[200];
        pkt_t pk;
        bool ok;
        double t;

        if (cap > 65536 || fread(buf, 1, cap, f) != cap) {
            break;   /* the last packet is still being written */
        }
        if (total++ == 0) {
            sec0 = sec;
            frac0 = frac;
        }
        t = (double)(sec - sec0) + ((double)frac - (double)frac0) / (nano ? 1e9 : 1e6);
        memset(&pk, 0, sizeof(pk));
        (void)snprintf(line, sizeof(line), "%9.3f  ", t);
        ok = link == LINK_NORDIC_BLE ? bleLine(buf, cap, line, sizeof(line), &pk)
           : link == LINK_RADIOTAP   ? wifiLine(buf, cap, line, sizeof(line), &pk)
           : link == LINK_ETHERNET   ? ethLine(buf, cap, line, sizeof(line), &pk)
                                     : false;
        if (!ok) {
            cat(line, sizeof(line), "%u B (link type %u)", (unsigned)cap, (unsigned)link);
        }
        if (!contains(line, filter)) {
            continue;
        }
        if (summary) {
            addDevice(devs, &ndev, &pk);
        } else if (matched >= (uint32_t)skip && (count == 0 || shown < (uint32_t)count)) {
            ucxapp_print("%s\n", line);
            shown++;
        }
        matched++;
    }
    if (summary) {
        qsort(devs, (size_t)ndev, sizeof(devs[0]), byCount);
        ucxapp_print("%-22s %7s %8s %4s  %s\n", "Device", "Packets", "Best dBm", "Ch", "Name / SSID");
        for (int i = 0; i < ndev && (count == 0 || i < count); i++) {
            ucxapp_print("%-22s %7u %8d %4d  %s\n", devs[i].addr, (unsigned)devs[i].count, devs[i].best,
                         devs[i].channel, devs[i].label);
        }
        ucxapp_print("%d devices in %u packets%s%s\n", ndev, (unsigned)matched, filter[0] ? " matching " : "", filter);
    } else {
        ucxapp_print("%u of %u packets shown%s%s%s\n", (unsigned)shown, (unsigned)total,
                     filter[0] ? ", filter " : "", filter,
                     shown < matched ? " (skip=/count= for more, summary per device, view for Wireshark)" : "");
    }
done:
    free(buf);
    free(devs);
    fclose(f);
    return rc;
}

#endif
