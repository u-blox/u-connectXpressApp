/* at.trace / at.decode / at.replay - record the AT traffic with timestamps (like u-center's message view), show it
 * decoded with command durations and a summary, and send a recorded trace again to compare the results. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_store.h"

#define TRACE_MAX_BYTES (4u * 1024u * 1024u)
#define TRACE_LINE_MAX  1024

typedef struct {
    const char *name;     /* without '+' */
    const char *meaning;
    const char *params;   /* comma separated names; extra values are shown as p<n> */
} urc_info_t;

static const urc_info_t kUrcs[] = {
    { "STARTUP", "module started", "" },
    { "UEBTC", "Bluetooth connected", "conn_handle,bd_addr" },
    { "UEBTDC", "Bluetooth disconnected", "conn_handle" },
    { "UEBTB", "Bluetooth bonding", "bd_addr,bond_status" },
    { "UEBTBGD", "Bluetooth background discovery", "bd_addr,rssi,device_name,data_type,data" },
    { "UEBTPHYU", "Bluetooth PHY update", "conn_handle,phy_status,tx_phy,rx_phy" },
    { "UEBTUC", "Bluetooth user confirmation", "bd_addr,numeric_value" },
    { "UEBTUPD", "Bluetooth passkey display", "bd_addr,numeric_value" },
    { "UEBTUPE", "Bluetooth passkey entry", "bd_addr" },
    { "UEBTCSS", "Bluetooth connection settings", "conn_handle,interval,latency,timeout" },
    { "UEBTGCN", "GATT notification", "conn_handle,value_handle,hex_data" },
    { "UEBTGCI", "GATT indication", "conn_handle,value_handle,hex_data" },
    { "UEBTGCW", "GATT server write", "conn_handle,value_handle,value,options" },
    { "UEBTGIC", "GATT indication confirmed", "conn_handle,char_handle" },
    { "UEBTGRR", "GATT server read request", "conn_handle,value_handle" },
    { "UEDGI", "iperf output", "iperf_output" },
    { "UEDGP", "ping response", "ping_response,response_time" },
    { "UEDGPC", "ping complete", "transmitted_packets,received_packets,packet_loss_rate,avg_response_time" },
    { "UEHTCDC", "HTTP disconnected", "session_id" },
    { "UEHTCRS", "HTTP response", "session_id,status_code,description" },
    { "UEMDD", "mDNS service down", "" },
    { "UEMDU", "mDNS service up", "" },
    { "UEMQC", "MQTT connected", "mqtt_id" },
    { "UEMQDA", "MQTT data available", "mqtt_id,message_len" },
    { "UEMQDC", "MQTT disconnected", "mqtt_id,disconnect_reason" },
    { "UEMQDD", "MQTT data (direct)", "mqtt_id,topic,data" },
    { "UEMQPC", "MQTT publish completed", "mqtt_id,packet_id" },
    { "UEMQSC", "MQTT subscribe completed", "mqtt_id,subscribe_action,packet_id" },
    { "UESOC", "socket connected", "socket_handle" },
    { "UESOCL", "socket closed", "socket_handle" },
    { "UESODA", "socket data available", "socket_handle,number_bytes" },
    { "UESODB", "socket binary data", "socket_handle,data" },
    { "UESODBF", "socket binary data from", "socket_handle,remote_ip,remote_port,data" },
    { "UESODS", "socket string data", "socket_handle,data" },
    { "UESODSF", "socket string data from", "socket_handle,remote_ip,remote_port,data" },
    /* Firmware 3.2/3.5 sends <listen>,<ip>,<new>, the reverse of the AT reference. */
    { "UESOIC", "socket incoming connection", "listening_socket,remote_ip,socket_handle" },
    { "UESPSC", "SPS connected", "conn_handle" },
    { "UESPSDA", "SPS data available", "conn_handle,number_bytes" },
    { "UESPSDC", "SPS disconnected", "conn_handle" },
    { "UESPSDB", "SPS binary data", "conn_handle,data" },
    { "UESPSDS", "SPS string data", "conn_handle,data" },
    { "UEWAPU", "Wi-Fi access point up", "" },
    { "UEWAPD", "Wi-Fi access point down", "" },
    { "UEWAPNU", "Wi-Fi access point network up", "" },
    { "UEWAPND", "Wi-Fi access point network down", "" },
    { "UEWAPSA", "Wi-Fi station associated to the access point", "mac" },
    { "UEWAPSDA", "Wi-Fi station left the access point", "mac" },
    { "UEWLU", "Wi-Fi link up", "wlan_handle,bssid,channel" },
    { "UEWLD", "Wi-Fi link down", "wlan_handle,reason" },
    { "UEWSNU", "Wi-Fi network up (IP assigned)", "" },
    { "UEWSND", "Wi-Fi network down", "" },
    { "UEWSRSI", "Wi-Fi roaming started", "" },
    { "UEWSRSC", "Wi-Fi roaming completed", "wlan_handle,bssid,channel" },
    { "UEWSRSF", "Wi-Fi roaming failed", "" },
};

/* "+UEWLU:0,..." -> table entry; NULL if not a known URC. */
static const urc_info_t *findUrc(const char *line)
{
    if (*line == '+') {
        line++;
    }
    size_t len = strcspn(line, ":");
    for (size_t i = 0; i < sizeof(kUrcs) / sizeof(kUrcs[0]); i++) {
        if (strlen(kUrcs[i].name) == len && strncmp(kUrcs[i].name, line, len) == 0) {
            return &kUrcs[i];
        }
    }
    return NULL;
}

static bool isUrc(const char *line)
{
    return strncmp(line, "+UE", 3) == 0 || strncmp(line, "+STARTUP", 8) == 0;
}

/* "Wi-Fi link up: wlan_handle=0 bssid=... channel=6" into out. False if the line is not a known URC. */
static bool decodeUrc(const char *line, char *out, size_t size)
{
    const urc_info_t *u = findUrc(line);

    if (u == NULL) {
        return false;
    }
    size_t o = (size_t)snprintf(out, size, "%s", u->meaning);
    const char *p = strchr(line, ':');
    const char *names = u->params;
    int n = 0;
    while (p != NULL && *p != '\0' && o < size) {
        const char *v = p + 1;
        bool quote = false;
        for (p = v; *p != '\0' && (quote || *p != ','); p++) {
            if (*p == '"') {
                quote = !quote;
            }
        }
        size_t nameLen = strcspn(names, ",");
        char fallback[8];
        const char *name = names;
        if (nameLen == 0) {
            snprintf(fallback, sizeof(fallback), "p%d", n);
            name = fallback;
            nameLen = strlen(fallback);
        }
        o += (size_t)snprintf(out + o, size - o, "%s %.*s=%.*s", n == 0 ? ":" : "", (int)nameLen, name,
                              (int)(p - v), v);
        names += nameLen;
        if (*names == ',') {
            names++;
        }
        n++;
        if (*p == '\0') {
            break;
        }
    }
    return true;
}

static bool savePcapng(const char *text, const char *name, unsigned *frames, bool *dated);

/* ---- recording (host builds: the library prints the AT trace through ucxappLibPrintf) ---- */

#if defined(_WIN32) || defined(U_PORT_POSIX)
#define TRACE_CAPTURE 1
#endif

#ifdef TRACE_CAPTURE
#include <time.h>

static bool isPcapngName(const char *name)
{
    size_t n = strlen(name);
    return n >= 7 && strcmp(name + n - 7, ".pcapng") == 0;
}
static char *gBuf;
static size_t gLen;
static size_t gCap;
static bool gFull;
static uint32_t gStartMs;
static char gFile[128];

static void append(const char *s)
{
    size_t n = strlen(s);

    if (gFull) {
        return;
    }
    if (gLen + n + 1 > TRACE_MAX_BYTES) {
        gFull = true;
        return;
    }
    if (gLen + n + 1 > gCap) {
        size_t cap = gCap == 0 ? 16384 : gCap * 2;
        while (cap < gLen + n + 1) {
            cap *= 2;
        }
        char *p = realloc(gBuf, cap);
        if (p == NULL) {
            gFull = true;
            return;
        }
        gBuf = p;
        gCap = cap;
    }
    memcpy(gBuf + gLen, s, n + 1);
    gLen += n;
}

/* "[12:00:00.123] [AT TX][0] AT+GMM" -> "0.012 TX AT+GMM". */
static void traceSink(const char *line)
{
    const char *tx = strstr(line, "[AT TX]");
    const char *rx = strstr(line, "[AT RX]");
    const char *p = tx != NULL ? tx : rx;
    char out[TRACE_LINE_MAX + 32];

    if (p == NULL) {
        return;
    }
    p += 7;
    if (*p == '[') {
        const char *end = strchr(p, ']');
        p = end != NULL ? end + 1 : p;
    }
    while (*p == ' ') {
        p++;
    }
    uint32_t ms = ucxapp_platform_tick_ms() - gStartMs;
    snprintf(out, sizeof(out), "%u.%03u %s %s\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000),
             tx != NULL ? "TX" : "RX", p);
    append(out);
}

static int cmdTrace(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gFile[0] != '\0') {
        ucxapp_print("ERROR: already tracing to %s (at.trace.stop first)\n", gFile);
        return UCXAPP_ERR_FAILED;
    }
    if (strlen(argv[1]) >= sizeof(gFile)) {
        ucxapp_print("ERROR: file name too long\n");
        return UCXAPP_ERR_FAILED;
    }
    snprintf(gFile, sizeof(gFile), "%s", argv[1]);
    gLen = 0;
    gFull = false;
    gStartMs = ucxapp_platform_tick_ms();
    append("# u-connectXpressApp AT trace: <seconds> <TX|RX> <line>; passwords are written as \"****\"\n");
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
        /* Wall-clock start, so at.trace.pcap can line the trace up with sniffer captures in Wireshark. */
        time_t sec = now.tv_sec;
        struct tm *tm = gmtime(&sec);
        unsigned ms = (unsigned)(now.tv_nsec / 1000000);
        char line[128];
        if (tm != NULL) {
            snprintf(line, sizeof(line), "# start %llu ms since 1970 UTC (%04d-%02d-%02d %02d:%02d:%02d.%03u UTC)\n",
                     (unsigned long long)sec * 1000u + ms, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec, ms);
            append(line);
        }
    }
    ucxappLogAtSetTrace(traceSink);
    ucxapp_print("tracing AT traffic to %s until at.trace.stop%s\n", gFile,
                 isPcapngName(gFile) ? " (Wireshark pcapng)" : "");
    return UCXAPP_OK;
}

static int cmdTraceStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gFile[0] == '\0') {
        ucxapp_print("ERROR: no trace running (at.trace <file>)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxappLogAtSetTrace(NULL);
    if (gFull) {
        append("# trace truncated: buffer limit reached\n");
    }
    bool ok;
    size_t lines = 0;
    if (isPcapngName(gFile)) {
        unsigned frames = 0;
        bool dated;
        ok = savePcapng(gBuf != NULL ? gBuf : "", gFile, &frames, &dated);
        lines = frames;
    } else {
        ok = ucxapp_store_save(gFile, gBuf != NULL ? gBuf : "", gLen);
        for (size_t i = 0; i < gLen; i++) {
            lines += (i == 0 || gBuf[i - 1] == '\n') && gBuf[i] != '#';
        }
    }
    if (ok) {
        ucxapp_print("%u lines written to %s%s\n", (unsigned)lines, gFile, gFull ? " (truncated)" : "");
    } else {
        ucxapp_print("ERROR: cannot write %s\n", gFile);
    }
    free(gBuf);
    gBuf = NULL;
    gLen = gCap = 0;
    gFile[0] = '\0';
    return ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}
#else
static int cmdTrace(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("at.trace needs Windows, Linux or the browser; use app.log.at.on on this target\n");
    return UCXAPP_ERR_UNSUPPORTED;
}

static int cmdTraceStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    return argc != 1 ? UCXAPP_ERR_USAGE : UCXAPP_ERR_UNSUPPORTED;
}
#endif

/* ---- reading a trace ---- */

typedef struct {
    uint32_t ms;
    bool tx;
    const char *text;   /* points into the line buffer */
} trace_line_t;

/* Next "<s.mmm> <TX|RX> <text>" line; NULL at the end. Comments and other lines are skipped. */
static const char *nextTraceLine(const char *p, char *buf, size_t size, trace_line_t *out)
{
    while (p != NULL && *p != '\0') {
        size_t len = strcspn(p, "\r\n");
        const char *next = p + len;
        while (*next == '\r' || *next == '\n') {
            next++;
        }
        snprintf(buf, size, "%.*s", (int)len, p);
        p = next;
        unsigned s;
        unsigned ms;
        char dir[3];
        int used = 0;
        if (buf[0] != '#' && sscanf(buf, "%u.%3u %2s %n", &s, &ms, dir, &used) == 3 && used > 0 &&
            (strcmp(dir, "TX") == 0 || strcmp(dir, "RX") == 0)) {
            out->ms = s * 1000u + ms;
            out->tx = dir[0] == 'T';
            out->text = buf + used;
            return p;
        }
    }
    return NULL;
}

static bool isFinal(const char *text, bool *ok)
{
    *ok = strcmp(text, "OK") == 0;
    return *ok || strncmp(text, "ERROR", 5) == 0 || strncmp(text, "+CME ERROR", 10) == 0;
}

typedef struct {
    char name[16];
    unsigned count;
} urc_count_t;

typedef struct {
    char cmd[64];
    uint32_t ms;
} slow_t;

static int cmdTraceShow(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "errors") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    bool errorsOnly = argc == 3;
    size_t len;
    char *text = (char *)ucxapp_store_load(argv[1], &len);
    if (text == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    char buf[TRACE_LINE_MAX];
    char cmd[TRACE_LINE_MAX];
    char decoded[512];
    trace_line_t t;
    const char *p = text;
    bool pending = false;
    uint32_t txMs = 0;
    unsigned commands = 0;
    unsigned errors = 0;
    unsigned urcs = 0;
    urc_count_t counts[16];
    int nCounts = 0;
    slow_t slow[5];
    int nSlow = 0;
    char rsp[4][128];
    int nRsp = 0;

    memset(slow, 0, sizeof(slow));
    while ((p = nextTraceLine(p, buf, sizeof(buf), &t)) != NULL) {
        bool ok;
        if (t.tx) {
            snprintf(cmd, sizeof(cmd), "%s", t.text);
            pending = true;
            txMs = t.ms;
            nRsp = 0;
            commands++;
        } else if (isUrc(t.text) && !(pending && strncmp(t.text + 1, cmd + 3, strcspn(t.text + 1, ":")) == 0)) {
            urcs++;
            size_t n = strcspn(t.text, ":");
            int i;
            for (i = 0; i < nCounts; i++) {
                if (strlen(counts[i].name) == n && strncmp(counts[i].name, t.text, n) == 0) {
                    break;
                }
            }
            if (i == nCounts && nCounts < (int)(sizeof(counts) / sizeof(counts[0]))) {
                snprintf(counts[nCounts].name, sizeof(counts[0].name), "%.*s", (int)n, t.text);
                counts[nCounts++].count = 0;
            }
            if (i < nCounts) {
                counts[i].count++;
            }
            if (!errorsOnly) {
                bool known = decodeUrc(t.text, decoded, sizeof(decoded));
                ucxapp_print("%5u.%03u  URC   %s%s%s\n", (unsigned)(t.ms / 1000), (unsigned)(t.ms % 1000), t.text,
                             known ? "\n                   = " : "", known ? decoded : "");
            }
        } else if (pending && isFinal(t.text, &ok)) {
            uint32_t d = t.ms - txMs;
            pending = false;
            errors += !ok;
            if (!errorsOnly || !ok) {
                ucxapp_print("%5u.%03u  %5u ms %-5s %s\n", (unsigned)(txMs / 1000), (unsigned)(txMs % 1000),
                             (unsigned)d, ok ? "OK" : "ERROR", cmd);
                for (int i = 0; i < nRsp; i++) {
                    ucxapp_print("                   %s\n", rsp[i]);
                }
                if (!ok && strcmp(t.text, "ERROR") != 0) {
                    ucxapp_print("                   %s\n", t.text);
                }
            }
            bool fresh = nSlow < 5;
            int at = fresh ? nSlow++ : 4;
            if (fresh || d > slow[4].ms) {
                snprintf(slow[at].cmd, sizeof(slow[at].cmd), "%s", cmd);
                slow[at].ms = d;
                for (int i = at; i > 0 && slow[i].ms > slow[i - 1].ms; i--) {
                    slow_t s = slow[i];
                    slow[i] = slow[i - 1];
                    slow[i - 1] = s;
                }
            }
        } else if (pending && nRsp < 4) {
            snprintf(rsp[nRsp++], sizeof(rsp[0]), "%s", t.text);
        }
    }
    free(text);
    ucxapp_print("\n%u commands, %u failed, %u URCs\n", commands, errors, urcs);
    for (int i = 0; i < nCounts; i++) {
        const urc_info_t *u = findUrc(counts[i].name);
        ucxapp_print("  %-10s %5u  %s\n", counts[i].name, counts[i].count, u != NULL ? u->meaning : "");
    }
    if (nSlow > 0) {
        ucxapp_print("slowest:\n");
        for (int i = 0; i < nSlow; i++) {
            ucxapp_print("  %5u ms  %s\n", (unsigned)slow[i].ms, slow[i].cmd);
        }
    }
    return UCXAPP_OK;
}

/* ---- Wireshark export: pcapng, one exported-PDU record per AT line for Wireshark's own AT dissector ---- */

#define PCAPNG_SHB        0x0A0D0D0Au
#define PCAPNG_IDB        1u
#define PCAPNG_EPB        6u
#define LINKTYPE_UPPER_PDU 252u
#define EXP_PDU_NAME      12u
#define EXP_PDU_DIR       35u
#define EXP_PDU_INFO      36u

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool failed;
} bytes_t;

static void put(bytes_t *b, const void *data, size_t n)
{
    if (b->failed || n == 0) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap == 0 ? 16384 : b->cap;
        while (cap < b->len + n) {
            cap *= 2;
        }
        uint8_t *p = realloc(b->data, cap);
        if (p == NULL) {
            b->failed = true;
            return;
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

/* pcapng fields are in host byte order; the byte-order magic in the section header tells the reader. */
static void put32(bytes_t *b, uint32_t v)
{
    put(b, &v, 4);
}

static void put16(bytes_t *b, uint16_t v)
{
    put(b, &v, 2);
}

static void putPad(bytes_t *b)
{
    static const uint8_t zero[4] = { 0 };
    put(b, zero, (4u - b->len % 4u) % 4u);
}

static void putOption(bytes_t *b, uint16_t code, const void *value, size_t n)
{
    put16(b, code);
    put16(b, (uint16_t)n);
    put(b, value, n);
    putPad(b);
}

/* Exported-PDU tags are big-endian. */
static void putTag(bytes_t *b, uint16_t tag, const void *value, size_t n)
{
    uint8_t head[4] = { (uint8_t)(tag >> 8), (uint8_t)tag, (uint8_t)(n >> 8), (uint8_t)n };
    put(b, head, sizeof(head));
    put(b, value, n);
}

static size_t blockBegin(bytes_t *b, uint32_t type)
{
    size_t at = b->len;
    put32(b, type);
    put32(b, 0);
    return at;
}

static void blockEnd(bytes_t *b, size_t at)
{
    putPad(b);
    putOption(b, 0, NULL, 0);
    uint32_t total = (uint32_t)(b->len - at + 4u);
    put32(b, total);
    if (!b->failed) {
        memcpy(b->data + at + 4, &total, sizeof(total));
    }
}

/* Converts a text trace to pcapng. Each line becomes one frame for Wireshark's "at" dissector, with its direction,
 * its wall-clock time (from the "# start" line; without it the times count from 1970) and an Info text: the meaning
 * of a known URC, or the time from the command to OK/ERROR. */
static bool savePcapng(const char *text, const char *name, unsigned *frames, bool *dated)
{
    static const char kApp[] = "u-connectXpressApp";
    static const char kIf[] = "u-connectXpress AT";
    static const char kDissector[4] = { 'a', 't', 0, 0 };
    bytes_t b = { 0 };
    unsigned long long startMs = 0;
    const char *s = strncmp(text, "# start ", 8) == 0 ? text : strstr(text, "\n# start ");

    *frames = 0;
    *dated = false;
    if (s != NULL) {
        const char *num = s + (*s == '\n') + 8;
        char *end;
        startMs = strtoull(num, &end, 10);
        *dated = end != num;
    }

    size_t at = blockBegin(&b, PCAPNG_SHB);
    put32(&b, 0x1A2B3C4Du);
    put16(&b, 1);
    put16(&b, 0);
    put32(&b, 0xFFFFFFFFu);  /* section length: not given */
    put32(&b, 0xFFFFFFFFu);
    putOption(&b, 4, kApp, sizeof(kApp) - 1);
    blockEnd(&b, at);

    at = blockBegin(&b, PCAPNG_IDB);
    put16(&b, (uint16_t)LINKTYPE_UPPER_PDU);
    put16(&b, 0);
    put32(&b, 0);
    putOption(&b, 2, kIf, sizeof(kIf) - 1);
    uint8_t tsresol = 3;  /* milliseconds */
    putOption(&b, 9, &tsresol, 1);
    blockEnd(&b, at);

    char buf[TRACE_LINE_MAX];
    char cmd[TRACE_LINE_MAX] = "";
    char info[512];
    trace_line_t t;
    const char *p = text;
    bool pending = false;
    uint32_t txMs = 0;

    while ((p = nextTraceLine(p, buf, sizeof(buf), &t)) != NULL) {
        bool ok;
        info[0] = '\0';
        if (t.tx) {
            snprintf(cmd, sizeof(cmd), "%s", t.text);
            pending = true;
            txMs = t.ms;
        } else if (isUrc(t.text) && !(pending && strncmp(t.text + 1, cmd + 3, strcspn(t.text + 1, ":")) == 0)) {
            if (!decodeUrc(t.text, info, sizeof(info))) {
                info[0] = '\0';
            }
        } else if (pending && isFinal(t.text, &ok)) {
            snprintf(info, sizeof(info), "%u ms %.200s", (unsigned)(t.ms - txMs), cmd);
            pending = false;
        }
        size_t textLen = strlen(t.text);
        size_t infoLen = strlen(info);
        size_t eol = t.tx ? 1u : 2u;  /* commands end with CR, responses and URCs with CR LF */
        uint8_t dir[4] = { 0, 0, 0, t.tx ? 0 : 1 };  /* P2P_DIR_SENT, P2P_DIR_RECV */
        uint32_t pduLen = (uint32_t)(4u + sizeof(kDissector) + 4u + sizeof(dir) + (infoLen > 0 ? 4u + infoLen : 0u) +
                                     4u + textLen + eol);
        unsigned long long ts = startMs + t.ms;
        uint32_t flags = t.tx ? 2u : 1u;              /* outbound, inbound */

        at = blockBegin(&b, PCAPNG_EPB);
        put32(&b, 0);
        put32(&b, (uint32_t)(ts >> 32));
        put32(&b, (uint32_t)ts);
        put32(&b, pduLen);
        put32(&b, pduLen);
        putTag(&b, EXP_PDU_NAME, kDissector, sizeof(kDissector));
        putTag(&b, EXP_PDU_DIR, dir, sizeof(dir));
        if (infoLen > 0) {
            putTag(&b, EXP_PDU_INFO, info, infoLen);
        }
        putTag(&b, 0, NULL, 0);
        put(&b, t.text, textLen);
        put(&b, "\r\n", eol);
        putPad(&b);
        putOption(&b, 2, &flags, sizeof(flags));
        blockEnd(&b, at);
        (*frames)++;
    }
    bool ok = !b.failed && ucxapp_store_save(name, b.data, b.len);
    free(b.data);
    return ok;
}

static int cmdTracePcap(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc < 2 || argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    char out[256];
    if (argc == 3) {
        snprintf(out, sizeof(out), "%s", argv[2]);
    } else {
        const char *dot = strrchr(argv[1], '.');
        const char *slash = strpbrk(dot != NULL ? dot : "", "/\\");
        size_t base = dot != NULL && slash == NULL ? (size_t)(dot - argv[1]) : strlen(argv[1]);
        if (base + sizeof(".pcapng") > sizeof(out)) {
            ucxapp_print("ERROR: file name too long\n");
            return UCXAPP_ERR_FAILED;
        }
        snprintf(out, sizeof(out), "%.*s.pcapng", (int)base, argv[1]);
    }
    size_t len;
    char *text = (char *)ucxapp_store_load(argv[1], &len);
    if (text == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    unsigned frames;
    bool dated;
    bool ok = savePcapng(text, out, &frames, &dated);
    free(text);
    if (!ok) {
        ucxapp_print("ERROR: cannot write %s\n", out);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%u frames written to %s%s\n", frames, out,
                 dated ? "" : " (the trace has no '# start' line: times count from 1970-01-01)");
    return UCXAPP_OK;
}

static int cmdDecode(ucxapp_t *app, int argc, char *argv[])
{
    char line[TRACE_LINE_MAX];
    char out[512];
    size_t o = 0;

    (void)app;
    if (argc < 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 1; i < argc && o < sizeof(line); i++) {
        o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    if (line[0] != '+') {
        size_t n = strlen(line);
        if (n + 2 > sizeof(line)) {
            n = sizeof(line) - 2;
        }
        memmove(line + 1, line, n);
        line[0] = '+';
        line[n + 1] = '\0';
    }
    if (!decodeUrc(line, out, sizeof(out))) {
        ucxapp_print("ERROR: unknown URC %.*s\n", (int)strcspn(line, ":"), line);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s\n", out);
    return UCXAPP_OK;
}

static bool replaySkip(const char *cmd)
{
    static const char *const kSkip[] = { "AT+CPWROFF", "AT+USYFR", "AT+USYFWU", "AT+USYUS=" };

    if (strstr(cmd, "****") != NULL || cmd[0] == '[') {
        return true;
    }
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); i++) {
        if (strncmp(cmd, kSkip[i], strlen(kSkip[i])) == 0) {
            return true;
        }
    }
    return false;
}

static int cmdReplay(ucxapp_t *app, int argc, char *argv[])
{
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    size_t len;
    char *text = (char *)ucxapp_store_load(argv[1], &len);
    if (text == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    char buf[TRACE_LINE_MAX];
    char look[TRACE_LINE_MAX];
    trace_line_t t;
    trace_line_t r;
    const char *p = text;
    unsigned sent = 0;
    unsigned same = 0;
    unsigned skipped = 0;
    unsigned differ = 0;
    uCxAtClient_t *client = app->ucx->pAtClient;

    while ((p = nextTraceLine(p, buf, sizeof(buf), &t)) != NULL) {
        bool recordedOk = false;
        bool known = false;
        if (!t.tx) {
            continue;
        }
        for (const char *q = p; (q = nextTraceLine(q, look, sizeof(look), &r)) != NULL && !r.tx;) {
            if (isFinal(r.text, &recordedOk)) {
                known = true;
                break;
            }
        }
        if (replaySkip(t.text) || !known) {
            ucxapp_print("skip  %s\n", t.text);
            skipped++;
            continue;
        }
        uCxAtClientCmdBeginF(client, t.text, "", NULL);
        while (uCxAtClientCmdGetRspParamLine(client, NULL, NULL, NULL) != NULL) {
        }
        int32_t rc = uCxAtClientCmdEnd(client);
        bool ok = rc == 0;
        sent++;
        if (ok == recordedOk) {
            same++;
            ucxapp_print("same  %-5s %s\n", ok ? "OK" : "ERROR", t.text);
        } else {
            differ++;
            ucxapp_print("DIFF  %s: recorded %s, now %s (%d%s)\n", t.text, recordedOk ? "OK" : "ERROR",
                         ok ? "OK" : "ERROR", (int)rc, ucxapp_errname(rc));
        }
    }
    free(text);
    ucxapp_print("%u sent, %u same, %u different, %u skipped\n", sent, same, differ, skipped);
    return differ > 0 ? UCXAPP_ERR_FAILED : UCXAPP_OK;
}

static const ucxapp_cmd_t gTraceCmds[] = {
    { "at.trace", "<file>", "Record every AT command, response and URC with its time until at.trace.stop; "
      "passwords are written as \"****\"; a .pcapng name writes a Wireshark capture instead of text", 0, cmdTrace },
    { "at.trace.stop", "", "Stop at.trace and write the file", 0, cmdTraceStop },
    { "at.trace.show", "<file> [errors]", "Decode a trace: each command with its time to OK/ERROR and its responses, "
      "URCs with their meaning, then counts and the slowest commands; errors shows only failures",
      0, cmdTraceShow },
    { "at.trace.pcap", "<file> [out.pcapng]", "Convert a text trace to a Wireshark capture (default <file>.pcapng): "
      "wall-clock times to merge with sniffer captures, direction, URC meanings and command times in Info; "
      "scripts/wireshark/ucxapp_at.lua adds ucx.* fields", 0, cmdTracePcap },
    { "at.decode", "<urc...>", "Explain one URC line, e.g. at.decode +UEWLD:0,3", 0, cmdDecode },
    { "at.replay", "<file>", "Send the commands of a trace again and compare OK/ERROR with the recording; skips "
      "masked passwords, binary data, reboot, factory reset, firmware update and baud rate changes",
      UCXAPP_CMD_NEEDS_DEVICE, cmdReplay },
};

void ucxapp_register_trace_cmds(void)
{
    ucxapp_cmd_register(gTraceCmds, sizeof(gTraceCmds) / sizeof(gTraceCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_trace_cmds(void)
{
}

#endif
