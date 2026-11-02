/* perf.* - round-trip time and throughput through the module against an echo peer (perf.echo or sock.echo on a
 * second module, or any echo server), over TCP, UDP or Bluetooth SPS. Times include the AT link to the module, so
 * they show what an application on this host gets. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_stats.h"
#include "ucxapp_xplorer.h"
#include "u_cx_bluetooth.h"
#include "u_cx_sps.h"

#define PERF_MAX_SIZE   1000   /* fits one socket or UDP read on every firmware */
#define PERF_POLL_MS    20     /* read without a data URC this often */
#define PERF_WINDOW     8      /* tput: packets in flight */
#define PERF_STALL_MS   500    /* tput: no echo for this long counts the packets in flight as lost */

typedef enum { LINK_TCP, LINK_UDP, LINK_SPS } link_kind_t;

typedef struct {
    ucxapp_t *app;
    link_kind_t kind;
    int32_t handle;            /* socket or connection handle */
    uSockIpAddress_t addr;     /* UDP peer */
    int32_t port;
    uint32_t lastRead;
    bool closed;               /* the peer closed the TCP connection or SPS link */
} link_t;

static uint8_t gTx[PERF_MAX_SIZE];
static uint8_t gRx[PERF_MAX_SIZE];

static ucxapp_stats_ch_t linkStats(const link_t *l)
{
    return l->kind == LINK_TCP ? UCXAPP_STATS_TCP : l->kind == LINK_UDP ? UCXAPP_STATS_UDP : UCXAPP_STATS_SPS;
}

static bool linkSend(link_t *l, const uint8_t *data, int32_t len)
{
    int32_t rc;

    if (l->kind == LINK_TCP) {
        rc = uCxSocketWrite(l->app->ucx, l->handle, data, len);
    } else if (l->kind == LINK_UDP) {
        rc = uCxSocketSendTo(l->app->ucx, l->handle, &l->addr, l->port, data, len);
    } else {
        rc = uCxSpsWrite(l->app->ucx, l->handle, data, len);
    }
    if (rc >= 0) {
        ucxapp_stats_add(linkStats(l), (size_t)len, 0);
    }
    return rc >= 0;
}

/* Bytes read (0 when nothing is waiting). Reads when the data URC came, or every PERF_POLL_MS in case it was
 * missed; the next call reads straight away after data, so a backlog drains without waiting. */
static int32_t linkRecv(link_t *l, uint8_t *buf, int32_t max)
{
    uint32_t now = ucxapp_platform_tick_ms();
    uint32_t flag = l->kind == LINK_SPS ? URC_FLAG_SPS_DATA : URC_FLAG_SOCK_DATA;
    int32_t n;

    if (!ucxapp_events_take(flag) && now - l->lastRead < PERF_POLL_MS) {
        return 0;
    }
    l->lastRead = now;
    if (l->kind == LINK_TCP) {
        n = uCxSocketRead(l->app->ucx, l->handle, max, buf);
    } else if (l->kind == LINK_UDP) {
        uCxSocketReceiveFrom_t from;
        n = ucxapp_udp_recvfrom(l->app->ucx, l->handle, max, buf, &from);
    } else {
        n = uCxSpsRead(l->app->ucx, l->handle, max, buf);
    }
    if (n <= 0) {
        return 0;
    }
    l->lastRead = now - PERF_POLL_MS;
    ucxapp_stats_add(linkStats(l), 0, (size_t)n);
    return n;
}

/* True once the peer has closed the connection (UESOCL / SPS disconnect); some echo servers close after one reply.
 * A +STARTUP means the module itself restarted, which ends every link. */
static bool linkClosed(link_t *l)
{
    if (!l->closed && ucxapp_events_take(URC_FLAG_STARTUP)) {
        ucxapp_print("ERROR: the module restarted (+STARTUP) during the run; the link is gone\n");
        l->closed = true;
    }
    if (!l->closed && l->kind != LINK_UDP) {
        l->closed = ucxapp_events_take(l->kind == LINK_TCP ? URC_FLAG_SOCK_CLOSED : URC_FLAG_SPS_DISCONNECTED);
    }
    return l->closed;
}

/* After a failed send: the close URC can come just after the error. */
static bool linkClosedSoon(link_t *l)
{
    uint32_t t0 = ucxapp_platform_tick_ms();

    while (!linkClosed(l) && l->kind != LINK_UDP && ucxapp_platform_tick_ms() - t0 < 300u) {
        ucxapp_platform_sleep_ms(10);
    }
    return l->closed;
}

static void closedNote(const link_t *l, uint32_t echoes)
{
    ucxapp_print("the peer closed the %s after %u echo%s; use UDP or an echo server that keeps the connection "
                 "(perf.echo on a second module)\n", l->kind == LINK_TCP ? "connection" : "SPS link",
                 (unsigned)echoes, echoes == 1 ? "" : "es");
}

static void linkClose(link_t *l)
{
    if (l->kind != LINK_SPS && l->handle >= 0) {
        uCxSocketClose(l->app->ucx, l->handle);
    }
    l->handle = -1;
}

/* argv: <tcp|udp> <host> <port> [a] [b]  or  sps [a] [b]. *next is the index of the first optional number. */
static bool parseLink(int argc, char *argv[], link_kind_t *kind, int32_t *port, int *next)
{
    if (argc < 2) {
        return false;
    }
    if (strcmp(argv[1], "sps") == 0) {
        *kind = LINK_SPS;
        *next = 2;
        return argc <= 4;
    }
    if ((strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0) || argc < 4 || argc > 6 ||
        !ucxapp_parse_int(argv[3], port) || *port < 1 || *port > 65535) {
        return false;
    }
    *kind = argv[1][0] == 't' ? LINK_TCP : LINK_UDP;
    *next = 4;
    return true;
}

static bool linkOpen(ucxapp_t *app, link_t *l, link_kind_t kind, const char *host, int32_t port)
{
    memset(l, 0, sizeof(*l));
    l->app = app;
    l->kind = kind;
    l->handle = -1;
    l->port = port;
    ucxapp_events_clear(URC_FLAG_STARTUP);
    if (kind == LINK_SPS) {
        l->handle = ucxapp_handle_current(UCXAPP_HANDLE_CONN);
        if (l->handle < 0) {
            ucxapp_print("ERROR: no Bluetooth connection (bt.connect first, the peer runs perf.echo sps)\n");
            return false;
        }
        if (uCxSpsSetDataMode(app->ucx, U_READ_MODE_BUFFERED) != 0) {
            ucxapp_print("ERROR: cannot set SPS buffered mode\n");
            return false;
        }
        ucxapp_events_clear(URC_FLAG_SPS_CONNECTED | URC_FLAG_SPS_DISCONNECTED);
        if (uCxSpsConnect1(app->ucx, l->handle) == 0 && !ucxapp_events_wait(URC_FLAG_SPS_CONNECTED, 5)) {
            ucxapp_print("ERROR: SPS did not connect on connection %d (does the peer run perf.echo sps?)\n",
                         (int)l->handle);
            return false;
        }
        ucxapp_events_clear(URC_FLAG_SPS_DATA);
        return true;
    }
    if (kind == LINK_UDP) {
        if (uCxStringToIpAddress(host, &l->addr) != 0 && uCxSocketGetHostByName(app->ucx, host, &l->addr) != 0) {
            ucxapp_print("ERROR: cannot resolve %s\n", host);
            return false;
        }
    }
    if (uCxSocketCreate1(app->ucx, kind == LINK_UDP ? U_SOCKET_PROTOCOL_UDP : U_SOCKET_PROTOCOL_TCP,
                         &l->handle) != 0) {
        ucxapp_print("ERROR: failed to create socket\n");
        l->handle = -1;
        return false;
    }
    if (kind == LINK_TCP) {
        ucxapp_events_clear(URC_FLAG_SOCK_CONNECTED | URC_FLAG_SOCK_CLOSED);
        int32_t rc = uCxSocketConnect(app->ucx, l->handle, host, port);
        if (rc != 0 || !ucxapp_events_wait(URC_FLAG_SOCK_CONNECTED, 10)) {
            ucxapp_print("ERROR: cannot connect to %s:%d (%d%s)\n", host, (int)port, (int)rc, ucxapp_errname(rc));
            linkClose(l);
            return false;
        }
        (void)uCxSocketSetOption(app->ucx, l->handle, U_SOCKET_OPTION_NO_DELAY, 1);
    }
    ucxapp_events_clear(URC_FLAG_SOCK_DATA);
    return true;
}

/* The s-center data pump pattern: a 0x00-0xFF counter that runs on across packets, so packet seq of the given
 * size starts at (seq * size) & 0xFF. Every byte value is sent, which also proves the path is binary-safe. */
static void fillPacket(uint32_t seq, int32_t size)
{
    uint32_t b = seq * (uint32_t)size;

    for (int32_t i = 0; i < size; i++) {
        gTx[i] = (uint8_t)(b + (uint32_t)i);
    }
}

/* Received-stream check like the s-center verifier: lock onto the first byte, expect the counter to go on, and on
 * a break count one error and lock again at the byte that came. */
typedef struct {
    bool synced;
    uint8_t next;
    uint64_t errors;
} pattern_check_t;

static void patternCheck(pattern_check_t *v, const uint8_t *p, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (v->synced && p[i] != v->next) {
            v->errors++;
        }
        v->synced = true;
        v->next = (uint8_t)(p[i] + 1u);
    }
}

static const char *linkName(link_kind_t kind)
{
    return kind == LINK_TCP ? "TCP" : kind == LINK_UDP ? "UDP" : "SPS";
}

/* "12.3" from tenths, without %f (newlib-nano on STM32 has none). */
static void tenths(uint32_t v10, char *out, size_t size)
{
    snprintf(out, size, "%u.%u", (unsigned)(v10 / 10), (unsigned)(v10 % 10));
}

static int rttRun(ucxapp_t *app, int argc, char *argv[])
{
    link_kind_t kind;
    int32_t port = 0;
    int next;
    int32_t count = 20;
    int32_t size = 64;
    link_t l;

    if (!parseLink(argc, argv, &kind, &port, &next) ||
        (argc > next && (!ucxapp_parse_int(argv[next], &count) || count < 1 || count > 100000)) ||
        (argc > next + 1 && (!ucxapp_parse_int(argv[next + 1], &size) || size < 10 || size > PERF_MAX_SIZE))) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (!linkOpen(app, &l, kind, kind == LINK_SPS ? NULL : argv[2], port)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("RTT over %s, %d packets of %d bytes\n", linkName(kind), (int)count, (int)size);
    uint32_t timeoutMs = kind == LINK_UDP ? 1000 : 3000;
    uint32_t received = 0;
    uint32_t corrupt = 0;
    uint32_t minMs = UINT32_MAX;
    uint32_t maxMs = 0;
    uint64_t sumMs = 0;
    uint64_t jitterSum = 0;
    uint32_t prevMs = 0;
    bool havePrev = false;
    bool lostSync = false;
    uint32_t sent = 0;

    for (int32_t seq = 0; seq < count && !lostSync; seq++) {
        fillPacket((uint32_t)seq, size);
        uint32_t t0 = ucxapp_platform_tick_ms();
        if (!linkSend(&l, gTx, size)) {
            if (linkClosedSoon(&l)) {
                closedNote(&l, received);
            } else {
                ucxapp_print("ERROR: send failed at packet %d\n", (int)seq);
            }
            break;
        }
        sent++;
        int32_t got = 0;
        bool done = false;
        while (!done && ucxapp_platform_tick_ms() - t0 < timeoutMs) {
            int32_t n = linkRecv(&l, gRx + got, kind == LINK_UDP ? (int32_t)sizeof(gRx) : size - got);
            if (n == 0) {
                if (linkClosed(&l)) {
                    break;
                }
                ucxapp_platform_sleep_ms(1);
                continue;
            }
            if (kind == LINK_UDP) {
                pattern_check_t v = { false, 0, 0 };
                patternCheck(&v, gRx, n);
                if (n == size && v.errors == 0 && gRx[0] != gTx[0]) {
                    continue;   /* a clean late echo of an earlier packet */
                }
                got = n;
            } else {
                got += n;
            }
            done = kind == LINK_UDP || got >= size;
        }
        uint32_t ms = ucxapp_platform_tick_ms() - t0;
        if (!done && l.closed) {
            closedNote(&l, received);
            break;
        }
        if (!done) {
            ucxapp_print("packet %d: no echo within %u ms\n", (int)seq, (unsigned)timeoutMs);
            /* A partial TCP/SPS echo would shift every later packet. */
            lostSync = kind != LINK_UDP && got > 0;
            continue;
        }
        if (got != size || memcmp(gRx, gTx, (size_t)size) != 0) {
            ucxapp_print("packet %d: echo differs (%d bytes back)\n", (int)seq, (int)got);
            corrupt++;
            continue;
        }
        received++;
        sumMs += ms;
        minMs = ms < minMs ? ms : minMs;
        maxMs = ms > maxMs ? ms : maxMs;
        if (havePrev) {
            jitterSum += ms > prevMs ? ms - prevMs : prevMs - ms;
        }
        prevMs = ms;
        havePrev = true;
    }
    linkClose(&l);
    uint32_t lost = sent - received - corrupt;
    ucxapp_print("%u sent, %u received, %u lost (%u%%), %u corrupt%s\n", (unsigned)sent, (unsigned)received,
                 (unsigned)lost, (unsigned)(sent > 0 ? lost * 100u / sent : 0), (unsigned)corrupt,
                 lostSync ? " (stopped: TCP/SPS stream out of step)" : "");
    if (received == 0) {
        return UCXAPP_ERR_FAILED;
    }
    char avg[16];
    char jit[16];
    tenths((uint32_t)(sumMs * 10u / received), avg, sizeof(avg));
    tenths(received > 1 ? (uint32_t)(jitterSum * 10u / (received - 1)) : 0, jit, sizeof(jit));
    ucxapp_print("rtt min %u ms, avg %s ms, max %u ms, jitter %s ms\n", (unsigned)minMs, avg, (unsigned)maxMs, jit);
    return lost > 0 || corrupt > 0 || sent < (uint32_t)count ? UCXAPP_ERR_FAILED : UCXAPP_OK;
}

static int tputRun(ucxapp_t *app, int argc, char *argv[])
{
    link_kind_t kind;
    int32_t port = 0;
    int next;
    int32_t seconds = 10;
    int32_t size = 512;
    link_t l;

    if (!parseLink(argc, argv, &kind, &port, &next) ||
        (argc > next && (!ucxapp_parse_int(argv[next], &seconds) || seconds < 1 || seconds > 3600)) ||
        (argc > next + 1 && (!ucxapp_parse_int(argv[next + 1], &size) || size < 10 || size > PERF_MAX_SIZE))) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (!linkOpen(app, &l, kind, kind == LINK_SPS ? NULL : argv[2], port)) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("throughput over %s for %d s, %d-byte packets, echoed back\n", linkName(kind), (int)seconds,
                 (int)size);
    uint64_t tx = 0;
    uint64_t rx = 0;
    uint64_t lost = 0;
    uint32_t seq = 0;
    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t lastRx = start;
    uint32_t lastReport = start;
    uint32_t endMs = (uint32_t)seconds * 1000u;
    uint32_t sendEnd = 0;
    bool sendFailed = false;
    pattern_check_t check = { false, 0, 0 };

    for (;;) {
        uint32_t now = ucxapp_platform_tick_ms();
        uint32_t elapsed = now - start;
        bool sending = elapsed < endMs && !sendFailed;
        if (!sending && sendEnd == 0) {
            sendEnd = elapsed;
        }
        if (!sending && (tx - lost <= rx || now - lastRx > 1000u)) {
            break;   /* everything echoed, or nothing more came for a second */
        }
        bool busy = false;
        if (sending && tx - rx - lost < (uint64_t)size * PERF_WINDOW) {
            fillPacket(seq++, size);
            if (!linkSend(&l, gTx, size)) {
                if (!linkClosedSoon(&l)) {
                    ucxapp_print("ERROR: send failed after %u packets\n", (unsigned)seq - 1);
                }
                sendFailed = true;
            } else {
                tx += (uint64_t)size;
                busy = true;
            }
        }
        int32_t n = linkRecv(&l, gRx, (int32_t)sizeof(gRx));
        if (n == 0 && !sendFailed && linkClosed(&l)) {
            sendFailed = true;
        }
        if (n > 0) {
            patternCheck(&check, gRx, n);
            rx += (uint64_t)n;
            lastRx = now;
            busy = true;
        } else if (sending && now - lastRx > PERF_STALL_MS && tx > rx + lost) {
            lost = tx - rx;   /* the window is stuck: count what is in flight as lost and go on */
            lastRx = now;
        }
        if (now - lastReport >= 1000u && sending) {
            lastReport = now;
            ucxapp_print("  %2u s  tx %u kbit/s  rx %u kbit/s\n", (unsigned)(elapsed / 1000),
                         (unsigned)(tx * 8u / (elapsed ? elapsed : 1)), (unsigned)(rx * 8u / (elapsed ? elapsed : 1)));
        }
        if (!busy) {
            ucxapp_platform_sleep_ms(1);
        }
    }
    if (l.closed) {
        closedNote(&l, (uint32_t)(rx / (uint64_t)size));
    }
    linkClose(&l);
    uint32_t ms = sendEnd > 0 ? sendEnd : 1;
    uint32_t lossPct10 = tx > 0 && tx > rx ? (uint32_t)((tx - rx) * 1000u / tx) : 0;
    char loss[16];
    tenths(lossPct10, loss, sizeof(loss));
    ucxapp_print("sent %u bytes (%u kbit/s), echoed %u bytes (%u kbit/s), %s%% not echoed\n", (unsigned)tx,
                 (unsigned)(tx * 8u / ms), (unsigned)rx, (unsigned)(rx * 8u / ms), loss);
    /* TCP and SPS must return the 0x00-0xFF counter unbroken; on UDP each lost datagram is one break. */
    ucxapp_print("pattern 0x00-0xFF: %u break%s%s\n", (unsigned)check.errors, check.errors == 1 ? "" : "s",
                 kind == LINK_UDP && check.errors > 0 ? " (lost or reordered datagrams)" : "");
    bool streamOk = kind == LINK_UDP || check.errors == 0;
    return rx > 0 && !sendFailed && streamOk ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int spsEcho(ucxapp_t *app, int32_t seconds)
{
    link_t l;
    size_t total = 0;

    if (uCxSpsSetServiceEnable(app->ucx, U_SPS_SERVICE_OPTION_ENABLE) != 0 ||
        uCxSpsSetDataMode(app->ucx, U_READ_MODE_BUFFERED) != 0) {
        ucxapp_print("ERROR: cannot enable the SPS service\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_gatt_ensure_advertising(app);
    memset(&l, 0, sizeof(l));
    l.app = app;
    l.kind = LINK_SPS;
    ucxapp_print("SPS echo for %d s: connect with perf.rtt sps or perf.tput sps from the other module\n",
                 (int)seconds);
    uint32_t end = ucxapp_platform_tick_ms() + (uint32_t)seconds * 1000u;
    while ((int32_t)(end - ucxapp_platform_tick_ms()) > 0) {
        l.handle = ucxapp_handle_current(UCXAPP_HANDLE_CONN);
        int32_t n = l.handle >= 0 ? linkRecv(&l, gRx, (int32_t)sizeof(gRx)) : 0;
        if (n > 0 && linkSend(&l, gRx, n)) {
            total += (size_t)n;
        } else {
            ucxapp_platform_sleep_ms(1);
        }
    }
    ucxapp_print("echoed %u bytes\n", (unsigned)total);
    return UCXAPP_OK;
}

static int cmdEcho(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 60;

    if (argc >= 2 && strcmp(argv[1], "sps") == 0) {
        if (argc > 3 || (argc == 3 && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1))) {
            return UCXAPP_ERR_USAGE;
        }
        return app->ucx == NULL ? UCXAPP_ERR_NOT_CONNECTED : spsEcho(app, seconds);
    }
    const ucxapp_cmd_t *echo = ucxapp_cmd_find("sock.echo");
    if (argc < 3 || argc > 4 || echo == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    char name[] = "sock.echo";
    char *args[4] = { name, argv[1], argv[2], argc == 4 ? argv[3] : NULL };
    return echo->run(app, argc, args);
}

#if UCXAPP_XPLORER

#define PHONE_CALL_MS 30000u

static char gPhoneHost[64];

/* perf.rtt / perf.tput <tcp|udp> xplorer <port>: the u-connectXplorer phone saved by xplorer.connect echoes. The
 * remote-control link is closed while the test runs (the phone keeps its echo server), so a long run cannot miss the
 * app's keep-alive pings; afterwards the phone reports what it got, including its own 0x00-0xFF check. */
static int withPhone(ucxapp_t *app, int argc, char *argv[], ucxapp_cmd_fn_t run)
{
    link_kind_t kind;
    int32_t port = 0;
    int next;
    int64_t breaks = -1;

    if (!parseLink(argc, argv, &kind, &port, &next) || kind == LINK_SPS || strcmp(argv[2], "xplorer") != 0) {
        return run(app, argc, argv);
    }
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (!ucxapp_xplorer_open() || !ucxapp_xplorer_echo_start(kind == LINK_UDP, port)) {
        ucxapp_xplorer_close();
        return UCXAPP_ERR_FAILED;
    }
    snprintf(gPhoneHost, sizeof(gPhoneHost), "%s", ucxapp_xplorer_host());
    ucxapp_xplorer_close();
    ucxapp_print("phone %s echoes %s on port %d\n", gPhoneHost, linkName(kind), (int)port);
    char *host = argv[2];
    argv[2] = gPhoneHost;
    int rc = run(app, argc, argv);
    argv[2] = host;
    if (!ucxapp_xplorer_open() || !ucxapp_xplorer_echo_report(&breaks)) {
        rc = rc == UCXAPP_OK ? UCXAPP_ERR_FAILED : rc;
    }
    ucxapp_xplorer_echo_stop();
    ucxapp_xplorer_close();
    if (kind == LINK_TCP && breaks > 0) {
        ucxapp_print("ERROR: the TCP stream reached the phone with gaps\n");
        rc = UCXAPP_ERR_FAILED;
    }
    return rc;
}

static int cmdRtt(ucxapp_t *app, int argc, char *argv[])
{
    return withPhone(app, argc, argv, rttRun);
}

static int cmdTput(ucxapp_t *app, int argc, char *argv[])
{
    return withPhone(app, argc, argv, tputRun);
}

/* The phone's deviceId of the module advertising this name (iOS hides the address, so match on the name). */
static bool phoneFind(const char *name, char *id, size_t size)
{
    bool found = false;
    char *r = ucxapp_xplorer_call("ble.startScan", NULL, PHONE_CALL_MS);

    if (r == NULL) {
        return false;
    }
    free(r);
    ucxapp_print("phone scans for \"%s\"\n", name);
    uint32_t t0 = ucxapp_platform_tick_ms();
    while (!found && ucxapp_platform_tick_ms() - t0 < 15000u) {
        ucxapp_platform_sleep_ms(500);
        char *d = ucxapp_xplorer_call("ble.getDevices", NULL, PHONE_CALL_MS);
        char *list = ucxapp_json_get(d, "devices");
        free(d);
        if (list == NULL) {
            break;
        }
        for (int i = 0; !found; i++) {
            char *item = ucxapp_json_item(list, i);
            char n[64];
            if (item == NULL) {
                break;
            }
            found = ucxapp_json_str(item, "name", n, sizeof(n)) && strcmp(n, name) == 0 &&
                    ucxapp_json_str(item, "id", id, size);
            free(item);
        }
        free(list);
    }
    free(ucxapp_xplorer_call("ble.stopScan", NULL, PHONE_CALL_MS));
    if (!found) {
        ucxapp_print("ERROR: the phone did not see \"%s\" advertising within 15 s\n", name);
    }
    return found;
}

static void phoneResult(const char *stats)
{
    int64_t tx = 0;
    int64_t rx = 0;
    int64_t kbps = 0;
    int64_t breaks = 0;
    int64_t count = 0;

    (void)ucxapp_json_int(stats, "txBytes", &tx);
    (void)ucxapp_json_int(stats, "rxBytes", &rx);
    (void)ucxapp_json_int(stats, "throughputKbps", &kbps);
    (void)ucxapp_json_int(stats, "patternBreaks", &breaks);
    ucxapp_print("phone:  sent %u bytes, received %u bytes, %u kbit/s, pattern 0x00-0xFF: %u break%s\n",
                 (unsigned)tx, (unsigned)rx, (unsigned)kbps, (unsigned)breaks, breaks == 1 ? "" : "s");
    if (ucxapp_json_int(stats, "rttMs.count", &count) && count > 0) {
        char *mn = ucxapp_json_get(stats, "rttMs.min");
        char *avg = ucxapp_json_get(stats, "rttMs.avg");
        char *mx = ucxapp_json_get(stats, "rttMs.max");
        char *jit = ucxapp_json_get(stats, "rttMs.jitter");
        ucxapp_print("phone:  rtt min %s ms, avg %s ms, max %s ms, jitter %s ms (%u echoes)\n", mn ? mn : "?",
                     avg ? avg : "?", mx ? mx : "?", jit ? jit : "?", (unsigned)count);
        free(mn);
        free(avg);
        free(mx);
        free(jit);
    }
}

typedef enum { PHONE_TX, PHONE_RX, PHONE_ECHO } phone_mode_t;

/* Module side of perf.phone while the phone runs perf.startThroughput; ends when the phone reports perf.onComplete
 * (or a few seconds after it should have). Returns the phone's stats (malloc'ed) or NULL. */
static char *phoneLoop(link_t *l, phone_mode_t mode, int32_t seconds, int32_t size, uint64_t *tx, uint64_t *rx,
                       pattern_check_t *check)
{
    uint32_t start = ucxapp_platform_tick_ms();
    uint32_t lastReport = start;
    uint32_t lastPoll = start;
    uint32_t sendMs = (uint32_t)seconds * 1000u;
    uint32_t seq = 0;
    char *stats = NULL;

    while (stats == NULL && ucxapp_platform_tick_ms() - start < sendMs + 5000u && !linkClosed(l)) {
        uint32_t now = ucxapp_platform_tick_ms();
        bool busy = false;
        if (mode == PHONE_TX && now - start < sendMs) {
            fillPacket(seq, size);
            if (linkSend(l, gTx, size)) {   /* a refused write is sent again with the same bytes */
                seq++;
                *tx += (uint64_t)size;
                busy = true;
            }
        } else {
            int32_t n = linkRecv(l, gRx, (int32_t)sizeof(gRx));
            if (n > 0) {
                *rx += (uint64_t)n;
                busy = true;
                if (mode == PHONE_RX) {
                    patternCheck(check, gRx, n);
                } else {
                    int tries = 0;
                    while (!linkSend(l, gRx, n) && ++tries < 50) {
                        ucxapp_platform_sleep_ms(2);
                    }
                    if (tries < 50) {
                        *tx += (uint64_t)n;
                    } else {
                        ucxapp_print("module: echo of %d bytes not sent (SPS write refused)\n", (int)n);
                    }
                }
            }
        }
        if (now - lastPoll >= 50u) {
            lastPoll = now;
            stats = ucxapp_xplorer_event("perf.onComplete", 0);
        }
        if (now - lastReport >= 1000u) {
            uint32_t ms = now - start;
            lastReport = now;
            ucxapp_print("  %2u s  module tx %u kbit/s  rx %u kbit/s\n", (unsigned)(ms / 1000),
                         (unsigned)(*tx * 8u / ms), (unsigned)(*rx * 8u / ms));
        }
        if (!busy) {
            ucxapp_platform_sleep_ms(1);
        }
    }
    if (stats == NULL) {
        stats = ucxapp_xplorer_call("perf.stop", NULL, PHONE_CALL_MS);
    }
    if (mode == PHONE_RX) {   /* what the phone sent last may still be on its way */
        uint32_t t0 = ucxapp_platform_tick_ms();
        while (ucxapp_platform_tick_ms() - t0 < 500u) {
            int32_t n = linkRecv(l, gRx, (int32_t)sizeof(gRx));
            if (n > 0) {
                *rx += (uint64_t)n;
                patternCheck(check, gRx, n);
            } else {
                ucxapp_platform_sleep_ms(1);
            }
        }
    }
    return stats;
}

static int phoneRun(ucxapp_t *app, phone_mode_t mode, int32_t seconds, int32_t size, const char *name)
{
    static const char *const kPhoneMode[] = { "rx", "tx", "echo" };
    char bt[24] = "?";
    char id[80];
    char qid[96];
    char params[256];
    link_t l;
    uint64_t tx = 0;
    uint64_t rx = 0;
    pattern_check_t check = { false, 0, 0 };

    char *p = ucxapp_xplorer_call("app.getPlatform", NULL, PHONE_CALL_MS);
    (void)ucxapp_json_str(p, "bluetoothState", bt, sizeof(bt));
    free(p);
    if (strcmp(bt, "poweredOn") != 0) {
        ucxapp_print("ERROR: the phone's Bluetooth is %s; turn it on\n", bt);
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_xplorer_has("perfSps")) {
        ucxapp_print("ERROR: this u-connectXplorer cannot run perf over SPS (needs perfSps); update the app\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxSpsSetServiceEnable(app->ucx, U_SPS_SERVICE_OPTION_ENABLE) != 0 ||
        uCxSpsSetDataMode(app->ucx, U_READ_MODE_BUFFERED) != 0) {
        ucxapp_print("ERROR: cannot enable the SPS service\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_gatt_ensure_advertising(app);
    if (!phoneFind(name, id, sizeof(id)) || !ucxapp_json_quote(id, qid, sizeof(qid))) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_events_clear(URC_FLAG_SPS_CONNECTED | URC_FLAG_SPS_DISCONNECTED | URC_FLAG_SPS_DATA | URC_FLAG_STARTUP);
    ucxapp_gatt_server_quiet(true);   /* the phone's CCCD writes are not news during the run */
    snprintf(params, sizeof(params), "{\"deviceId\":%s}", qid);
    char *c = ucxapp_xplorer_call("ble.connect", params, PHONE_CALL_MS);
    if (c == NULL) {
        ucxapp_gatt_server_quiet(false);
        return UCXAPP_ERR_FAILED;
    }
    int64_t mtu = 0;
    (void)ucxapp_json_int(c, "mtu", &mtu);
    free(c);
    int rc = UCXAPP_ERR_FAILED;
    snprintf(params, sizeof(params), "{\"deviceId\":%s,\"force\":true}", qid);
    char *s = ucxapp_xplorer_call("sps.connect", params, PHONE_CALL_MS);
    bool spsOk = s != NULL;
    free(s);
    memset(&l, 0, sizeof(l));
    l.app = app;
    l.kind = LINK_SPS;
    l.handle = -1;
    if (!spsOk) {
        ucxapp_print("the Bluetooth link dropped right after connecting; if bonding FAILED above, remove the module "
                     "from the phone's Bluetooth settings and try again\n");
        goto done;
    }
    if (!ucxapp_events_wait(URC_FLAG_SPS_CONNECTED, 10) || (l.handle = ucxapp_handle_current(UCXAPP_HANDLE_CONN)) < 0) {
        ucxapp_print("ERROR: the module saw no SPS connection from the phone\n");
        goto done;
    }
    snprintf(params, sizeof(params),
             "{\"deviceId\":%s,\"mode\":\"%s\",\"durationMs\":%u,\"packetSize\":%d,\"sps\":true}", qid,
             kPhoneMode[mode], (unsigned)seconds * 1000u, (int)size);
    char *started = ucxapp_xplorer_call("perf.startThroughput", params, PHONE_CALL_MS);
    if (started == NULL) {
        goto done;
    }
    int64_t phoneSize = size;
    (void)ucxapp_json_int(started, "packetSize", &phoneSize);
    free(started);
    ucxapp_print("SPS with the phone (MTU %d): module %s, phone %s, %d s, %d-byte packets\n", (int)mtu,
                 mode == PHONE_TX ? "sends" : mode == PHONE_RX ? "receives" : "echoes", kPhoneMode[mode],
                 (int)seconds, (int)(mode == PHONE_TX ? size : (int32_t)phoneSize));
    char *stats = phoneLoop(&l, mode, seconds, size, &tx, &rx, &check);
    uint32_t ms = (uint32_t)seconds * 1000u;
    ucxapp_print("module: sent %u bytes (%u kbit/s), received %u bytes (%u kbit/s)", (unsigned)tx,
                 (unsigned)(tx * 8u / ms), (unsigned)rx, (unsigned)(rx * 8u / ms));
    if (mode == PHONE_RX) {
        ucxapp_print(", pattern 0x00-0xFF: %u break%s", (unsigned)check.errors, check.errors == 1 ? "" : "s");
    }
    ucxapp_print("\n");
    if (l.closed) {
        ucxapp_print("ERROR: the SPS link closed during the run\n");
    }
    int64_t phoneRx = 0;
    int64_t phoneBreaks = 0;
    int64_t echoes = 0;
    bool haveStats = stats != NULL;
    if (haveStats) {
        phoneResult(stats);
        (void)ucxapp_json_int(stats, "rxBytes", &phoneRx);
        (void)ucxapp_json_int(stats, "patternBreaks", &phoneBreaks);
        (void)ucxapp_json_int(stats, "rttMs.count", &echoes);
        free(stats);
    }
    bool ok = haveStats && !l.closed &&
              (mode == PHONE_TX   ? phoneRx > 0 && phoneBreaks == 0
               : mode == PHONE_RX ? rx > 0 && check.errors == 0
                                  : echoes > 0);
    rc = ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
done:
    free(ucxapp_xplorer_call("sps.disconnect", NULL, PHONE_CALL_MS));
    snprintf(params, sizeof(params), "{\"deviceId\":%s}", qid);
    free(ucxapp_xplorer_call("ble.disconnect", params, PHONE_CALL_MS));
    ucxapp_gatt_server_quiet(false);
    return rc;
}

static int cmdPhone(ucxapp_t *app, int argc, char *argv[])
{
    int32_t seconds = 10;
    int32_t size = 244;
    phone_mode_t mode;
    const char *nm = NULL;
    char name[64];

    if (argc < 2 || argc > 4 ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &seconds) || seconds < 1 || seconds > 3600)) ||
        (argc > 3 && (!ucxapp_parse_int(argv[3], &size) || size < 20 || size > PERF_MAX_SIZE))) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "tx") == 0) {
        mode = PHONE_TX;
    } else if (strcmp(argv[1], "rx") == 0) {
        mode = PHONE_RX;
    } else if (strcmp(argv[1], "echo") == 0) {
        mode = PHONE_ECHO;
    } else {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx == NULL) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (!uCxBluetoothGetLocalNameBegin(app->ucx, &nm) || nm == NULL || nm[0] == '\0') {
        ucxapp_print("ERROR: cannot read the module's Bluetooth name (AT+UBTLN)\n");
        return UCXAPP_ERR_FAILED;
    }
    snprintf(name, sizeof(name), "%s", nm);
    uCxEnd(app->ucx);
    if (!ucxapp_xplorer_open()) {
        return UCXAPP_ERR_FAILED;
    }
    int rc = phoneRun(app, mode, seconds, size, name);
    ucxapp_xplorer_close();
    return rc;
}

#else

static int cmdRtt(ucxapp_t *app, int argc, char *argv[])
{
    return rttRun(app, argc, argv);
}

static int cmdTput(ucxapp_t *app, int argc, char *argv[])
{
    return tputRun(app, argc, argv);
}

#endif /* UCXAPP_XPLORER */

static const ucxapp_cmd_t gPerfCmds[] = {
    { "perf.rtt", "<tcp|udp> <host|xplorer> <port> [count] [size] | sps [count] [size]",
      "Round-trip time through the module to an echo peer: min/avg/max, jitter, lost and corrupt packets "
      "(default 20 packets of 64 bytes, max 1000 bytes); fails on any loss. Host xplorer makes the phone saved by "
      "xplorer.connect the echo peer", UCXAPP_CMD_NEEDS_DEVICE, cmdRtt },
    { "perf.tput", "<tcp|udp> <host|xplorer> <port> [seconds] [size] | sps [seconds] [size]",
      "Throughput to an echo peer: sends for the given time (default 10 s, 512-byte packets) with 8 packets in "
      "flight and reports kbit/s sent and echoed, once a second and at the end. Host xplorer makes the phone the "
      "echo peer, and it reports its own 0x00-0xFF check", UCXAPP_CMD_NEEDS_DEVICE, cmdTput },
    { "perf.echo", "<tcp|udp> <port> [seconds] | sps [seconds]",
      "Echo peer for perf.rtt and perf.tput on a second module (tcp/udp is sock.echo; sps advertises and echoes "
      "SPS data; default 60 s)", UCXAPP_CMD_NEEDS_DEVICE, cmdEcho },
#if UCXAPP_XPLORER
    { "perf.phone", "<tx|rx|echo> [seconds] [size]",
      "Bluetooth SPS between the module and the u-connectXplorer phone (xplorer.connect first): the phone finds "
      "and connects to the module, then the module sends (tx), receives (rx) or echoes the phone's packets (echo, "
      "phone measures RTT) for the given time (default 10 s, 244 bytes); both ends check the 0x00-0xFF pattern",
      UCXAPP_CMD_NEEDS_DEVICE, cmdPhone },
#endif
};

void ucxapp_register_perf_cmds(void)
{
    ucxapp_cmd_register(gPerfCmds, sizeof(gPerfCmds) / sizeof(gPerfCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_perf_cmds(void)
{
}

#endif
