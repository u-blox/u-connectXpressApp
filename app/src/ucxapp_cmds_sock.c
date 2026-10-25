#include "ucxapp_cmd.h"
#include "u_cx_system.h"
#include "ucxapp_platform.h"
#include "ucxapp_stats.h"

#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define SOCK_MAX_READ 1460

static bool parseHandle(const char *s, int32_t *handle)
{
    return ucxapp_parse_int(s, handle) && *handle >= 0;
}

static int cmdSockOpen(ucxapp_t *app, int argc, char *argv[])
{
    uSocketProtocol_t protocol;
    int32_t handle = -1;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (strcmp(argv[1], "tcp") == 0) {
        protocol = U_SOCKET_PROTOCOL_TCP;
    } else if (strcmp(argv[1], "udp") == 0) {
        protocol = U_SOCKET_PROTOCOL_UDP;
    } else {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxSocketCreate1(app->ucx, protocol, &handle) != 0) {
        ucxapp_print("ERROR: failed to create socket\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_handle_add(UCXAPP_HANDLE_SOCK, handle);
    ucxapp_print("socket %d (current)\n", (int)handle);
    return UCXAPP_OK;
}

static int cmdSockConnect(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t port;

    if (argc != 4 || !parseHandle(argv[1], &handle) ||
        !ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_clear(URC_FLAG_SOCK_CONNECTED);
    int32_t result = uCxSocketConnect(app->ucx, handle, argv[2], port);
    if (result != 0) {
        ucxapp_print("ERROR: connect failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_events_wait(URC_FLAG_SOCK_CONNECTED, 10)) {
        ucxapp_print("ERROR: timeout waiting for socket connection\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("socket %d connected to %s:%d\n", (int)handle, argv[2], (int)port);
    return UCXAPP_OK;
}

static int cmdSockTls(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t result;

    if ((argc != 3 && argc != 5) || !parseHandle(argv[1], &handle)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 5) {
        result = uCxSocketSetTLS5(app->ucx, handle, U_WIFI_TLS_VERSION_TLS1_2_OR_TLS1_3, argv[2], argv[3], argv[4]);
    } else {
        result = uCxSocketSetTLS3(app->ucx, handle, U_WIFI_TLS_VERSION_TLS1_2_OR_TLS1_3, argv[2]);
    }
    if (result != 0) {
        ucxapp_print("ERROR: TLS setup failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("socket %d uses TLS with ca %s%s%s\n", (int)handle, argv[2], argc == 5 ? ", client " : "",
                 argc == 5 ? argv[3] : "");
    return UCXAPP_OK;
}

/* Joins argv[first..] with spaces and converts \r, \n and \\ to the real characters; false if too long. */
static bool buildPayload(int argc, char *argv[], int first, char *text, size_t size, size_t *outLen)
{
    size_t len = 0;

    for (int i = first; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (len + n + 1 >= size) {
            ucxapp_print("ERROR: data too long (max %d bytes)\n", (int)size - 1);
            return false;
        }
        if (i > first) {
            text[len++] = ' ';
        }
        memcpy(&text[len], argv[i], n);
        len += n;
    }
    size_t out = 0;
    for (size_t in = 0; in < len; in++) {
        if (text[in] == '\\' && in + 1 < len && (text[in + 1] == 'r' || text[in + 1] == 'n' || text[in + 1] == '\\')) {
            in++;
            text[out++] = text[in] == 'r' ? '\r' : text[in] == 'n' ? '\n' : '\\';
        } else {
            text[out++] = text[in];
        }
    }
    *outLen = out;
    return true;
}

static void printable(uint8_t *buf, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7e) {
            buf[i] = '.';
        }
    }
    buf[n] = '\0';
}

static bool resolveHost(ucxapp_t *app, const char *host, uSockIpAddress_t *addr)
{
    if (uCxStringToIpAddress(host, addr) == 0) {
        return true;
    }
    if (uCxSocketGetHostByName(app->ucx, host, addr) != 0) {
        ucxapp_print("ERROR: cannot resolve %s\n", host);
        return false;
    }
    return true;
}

static int cmdSockSend(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    char text[SOCK_MAX_READ];
    size_t len;

    if (argc < 3 || !parseHandle(argv[1], &handle)) {
        return UCXAPP_ERR_USAGE;
    }
    if (!buildPayload(argc, argv, 2, text, sizeof(text), &len)) {
        return UCXAPP_ERR_FAILED;
    }
    int32_t result = uCxSocketWrite(app->ucx, handle, (const uint8_t *)text, (int32_t)len);
    if (result < 0) {
        ucxapp_print("ERROR: send failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_TCP, len, 0);
    ucxapp_print("sent %d bytes\n", (int)len);
    return UCXAPP_OK;
}

static int cmdSockRead(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t maxLen = SOCK_MAX_READ;
    int32_t timeoutS = 5;
    uint8_t buf[SOCK_MAX_READ + 1];

    if (argc < 2 || argc > 4 || !parseHandle(argv[1], &handle) ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &maxLen) || maxLen < 1 || maxLen > SOCK_MAX_READ)) ||
        (argc > 3 && (!ucxapp_parse_int(argv[3], &timeoutS) || timeoutS < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_wait(URC_FLAG_SOCK_DATA, (uint32_t)timeoutS);   /* a read below tells if data arrived */
    int32_t n = uCxSocketRead(app->ucx, handle, maxLen, buf);
    if (n < 0) {
        ucxapp_print("ERROR: read failed (%d%s)\n", (int)n, ucxapp_errname(n));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_TCP, 0, (size_t)n);
    printable(buf, n);
    ucxapp_print("received %d bytes: %s\n", (int)n, (const char *)buf);
    return UCXAPP_OK;
}

static int cmdSockBind(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t port;

    if (argc != 3 || !parseHandle(argv[1], &handle) || !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxSocketBind(app->ucx, handle, port) != 0) {
        ucxapp_print("ERROR: bind failed\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("socket %d bound to port %d\n", (int)handle, (int)port);
    return UCXAPP_OK;
}

static int cmdSockListen(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t port = 0;
    int32_t result;

    if (argc < 2 || argc > 3 || !parseHandle(argv[1], &handle) ||
        (argc == 3 && (!ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_sock_clear_incoming();
    result = argc == 3 ? uCxSocketListen2(app->ucx, handle, port) : uCxSocketListen1(app->ucx, handle);
    if (result != 0) {
        ucxapp_print("ERROR: listen failed (bind the socket first, or give a port)\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("socket %d listening\n", (int)handle);
    return UCXAPP_OK;
}

#define INCOMING_MAX 4
static int32_t gIncoming[INCOMING_MAX][2];
static int gIncomingCount;

void ucxapp_sock_on_incoming(int32_t sock, int32_t listenSock)
{
    ucxapp_events_lock();
    if (gIncomingCount == INCOMING_MAX) {
        memmove(gIncoming[0], gIncoming[1], sizeof(gIncoming[0]) * (INCOMING_MAX - 1));
        gIncomingCount--;
    }
    gIncoming[gIncomingCount][0] = sock;
    gIncoming[gIncomingCount][1] = listenSock;
    gIncomingCount++;
    ucxapp_events_unlock();
    ucxapp_events_signal(URC_FLAG_SOCK_INCOMING);
}

void ucxapp_sock_clear_incoming(void)
{
    ucxapp_events_lock();
    gIncomingCount = 0;
    ucxapp_events_unlock();
    ucxapp_events_clear(URC_FLAG_SOCK_INCOMING);
}

static bool popIncoming(int32_t *sock, int32_t *listenSock)
{
    bool have = false;

    ucxapp_events_lock();
    if (gIncomingCount > 0) {
        *sock = gIncoming[0][0];
        *listenSock = gIncoming[0][1];
        gIncomingCount--;
        memmove(gIncoming[0], gIncoming[1], sizeof(gIncoming[0]) * (size_t)gIncomingCount);
        have = true;
    }
    ucxapp_events_unlock();
    return have;
}

static int cmdSockAccept(ucxapp_t *app, int argc, char *argv[])
{
    int32_t waitS = 30;
    int32_t sock;
    int32_t listenSock;
    (void)app;

    if (argc > 2 || (argc == 2 && (!ucxapp_parse_int(argv[1], &waitS) || waitS < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    if (!popIncoming(&sock, &listenSock)) {
        ucxapp_events_wait(URC_FLAG_SOCK_INCOMING, (uint32_t)waitS);
        if (!popIncoming(&sock, &listenSock)) {
            ucxapp_print("ERROR: no incoming connection\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    ucxapp_handle_add(UCXAPP_HANDLE_SOCK, sock);
    ucxapp_print("accepted socket %d (listener %d, current)\n", (int)sock, (int)listenSock);
    return UCXAPP_OK;
}

static int cmdSockSendTo(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t port;
    uSockIpAddress_t addr;
    char text[SOCK_MAX_READ];
    size_t len;

    if (argc < 5 || !parseHandle(argv[1], &handle) || !ucxapp_parse_int(argv[3], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    if (!resolveHost(app, argv[2], &addr) || !buildPayload(argc, argv, 4, text, sizeof(text), &len)) {
        return UCXAPP_ERR_FAILED;
    }
    int32_t result = uCxSocketSendTo(app->ucx, handle, &addr, port, (const uint8_t *)text, (int32_t)len);
    if (result < 0) {
        ucxapp_print("ERROR: sendto failed (%d%s)\n", (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_UDP, len, 0);
    ucxapp_print("sent %d bytes to %s:%d\n", (int)len, argv[2], (int)port);
    return UCXAPP_OK;
}

static int cmdSockRecvFrom(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;
    int32_t maxLen = SOCK_MAX_READ;
    int32_t timeoutS = 5;
    uint8_t buf[SOCK_MAX_READ + 1];
    uCxSocketReceiveFrom_t from;
    char ip[64];

    if (argc < 2 || argc > 4 || !parseHandle(argv[1], &handle) ||
        (argc > 2 && (!ucxapp_parse_int(argv[2], &maxLen) || maxLen < 1 || maxLen > SOCK_MAX_READ)) ||
        (argc > 3 && (!ucxapp_parse_int(argv[3], &timeoutS) || timeoutS < 0))) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_events_wait(URC_FLAG_SOCK_DATA, (uint32_t)timeoutS);
    int32_t n = ucxapp_udp_recvfrom(app->ucx, handle, maxLen, buf, &from);
    if (n < 0) {
        ucxapp_print("ERROR: receive failed (%d%s)\n", (int)n, ucxapp_errname(n));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_stats_add(UCXAPP_STATS_UDP, 0, (size_t)n);
    if (uCxIpAddressToString(&from.remote_ip, ip, sizeof(ip)) <= 0) {
        ip[0] = '\0';
    }
    printable(buf, n);
    ucxapp_print("received %d bytes from %s:%d: %s\n", (int)n, ip, (int)from.remote_port, (const char *)buf);
    return UCXAPP_OK;
}

static int closeAll(ucxapp_t *app)
{
    uCxSocketListStatus_t status;
    int32_t handles[16];
    int count = 0;
    int failed = 0;

    uCxSocketListStatusBegin(app->ucx);
    while (uCxSocketListStatusGetNext(app->ucx, &status) && count < (int)(sizeof(handles) / sizeof(handles[0]))) {
        handles[count++] = status.socket_handle;
    }
    uCxEnd(app->ucx);
    for (int i = 0; i < count; i++) {
        if (uCxSocketClose(app->ucx, handles[i]) != 0) {
            failed++;
        }
        ucxapp_handle_remove(UCXAPP_HANDLE_SOCK, handles[i]);
    }
    ucxapp_print("closed %d socket(s)\n", count - failed);
    return failed == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

#define UDP_READ_MAX     1460   /* NORA-W36 3.4 and later */
#define UDP_READ_MAX_OLD 1000   /* NORA-W36 3.2 */
#define ERR_INVALID_INT_RANGE 36
static volatile int32_t gUdpReadMax;   /* 0: not probed yet, else the largest length the module accepted */

void ucxapp_udp_reset_limit(void)
{
    gUdpReadMax = 0;
}

static bool lengthRejected(uCxHandle_t *ucx)
{
    int32_t code = 0;
    return uCxSystemGetLastErrorCode(ucx, &code) == 0 && code == ERR_INVALID_INT_RANGE;
}

int32_t ucxapp_udp_recvfrom(uCxHandle_t *ucx, int32_t sock, int32_t len, uint8_t *buf, uCxSocketReceiveFrom_t *from)
{
    int32_t max = gUdpReadMax > 0 ? gUdpReadMax : UDP_READ_MAX;
    if (len > max) {
        len = max;
    }
    int32_t rc = uCxSocketReceiveFrom(ucx, sock, len, buf, from);
    if (gUdpReadMax == 0 && len > UDP_READ_MAX_OLD) {
        /* The binary read hides the error code; ask once whether the length itself was rejected. */
        if (rc < 0 && lengthRejected(ucx)) {
            gUdpReadMax = UDP_READ_MAX_OLD;
            rc = uCxSocketReceiveFrom(ucx, sock, UDP_READ_MAX_OLD, buf, from);
        } else {
            gUdpReadMax = UDP_READ_MAX;
        }
    }
    return rc;
}

int32_t ucxapp_udp_max(void)
{
    return gUdpReadMax > 0 ? gUdpReadMax : UDP_READ_MAX;
}

static int cmdSockClose(ucxapp_t *app, int argc, char *argv[])
{
    int32_t handle;

    if (argc == 2 && strcmp(argv[1], "all") == 0) {
        return closeAll(app);
    }
    if (argc != 2 || !parseHandle(argv[1], &handle)) {
        return UCXAPP_ERR_USAGE;
    }
    if (uCxSocketClose(app->ucx, handle) != 0) {
        ucxapp_print("ERROR: failed to close socket %d\n", (int)handle);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_handle_remove(UCXAPP_HANDLE_SOCK, handle);
    ucxapp_print("socket %d closed\n", (int)handle);
    return UCXAPP_OK;
}

static int cmdSockList(ucxapp_t *app, int argc, char *argv[])
{
    uCxSocketListStatus_t status;
    int count = 0;
    int32_t cur = ucxapp_handle_current(UCXAPP_HANDLE_SOCK);
    (void)argc;
    (void)argv;

    uCxSocketListStatusBegin(app->ucx);
    while (uCxSocketListStatusGetNext(app->ucx, &status)) {
        count++;
        ucxapp_print("%csocket %d: %s, %s\n", status.socket_handle == cur ? '*' : ' ', (int)status.socket_handle,
                     status.protocol == U_SOCKET_PROTOCOL_TCP ? "TCP" : "UDP",
                     status.status == 0 ? "not connected" :
                     status.status == 1 ? "listening" : "connected");
    }
    uCxEnd(app->ucx);
    ucxapp_print("%d socket(s)\n", count);
    return UCXAPP_OK;
}

/* Echo server for latency and throughput tests: everything received is sent straight back. */
static int cmdSockEcho(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port;
    int32_t seconds = 60;
    bool udp;
    int32_t sock = -1;
    int32_t client = -1;
    int32_t listenSock;
    uint8_t buf[SOCK_MAX_READ];
    size_t total = 0;

    if (argc < 3 || argc > 4 || (strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0) ||
        !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535 ||
        (argc == 4 && (!ucxapp_parse_int(argv[3], &seconds) || seconds < 1))) {
        return UCXAPP_ERR_USAGE;
    }
    udp = argv[1][0] == 'u';
    if (uCxSocketCreate1(app->ucx, udp ? U_SOCKET_PROTOCOL_UDP : U_SOCKET_PROTOCOL_TCP, &sock) != 0) {
        ucxapp_print("ERROR: failed to create socket\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_sock_clear_incoming();
    if ((udp ? uCxSocketBind(app->ucx, sock, port) : uCxSocketListen2(app->ucx, sock, port)) != 0) {
        ucxapp_print("ERROR: cannot open port %d\n", (int)port);
        uCxSocketClose(app->ucx, sock);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%s echo on port %d for %d s\n", udp ? "UDP" : "TCP", (int)port, (int)seconds);
    uint32_t end = ucxapp_platform_tick_ms() + (uint32_t)seconds * 1000u;
    while ((int32_t)(end - ucxapp_platform_tick_ms()) > 0) {
        int32_t n;
        if (udp) {
            uCxSocketReceiveFrom_t from;
            n = ucxapp_udp_recvfrom(app->ucx, sock, (int32_t)sizeof(buf), buf, &from);
            if (n > 0 && uCxSocketSendTo(app->ucx, sock, &from.remote_ip, from.remote_port, buf, n) < 0) {
                break;
            }
        } else {
            if (client < 0) {
                if (popIncoming(&client, &listenSock)) {
                    if (listenSock != sock) {
                        client = -1;   /* another listener's connection (e.g. a web or stream server) */
                    } else {
                        ucxapp_print("client connected\n");
                    }
                }
                n = 0;
            } else {
                n = uCxSocketRead(app->ucx, client, (int32_t)sizeof(buf), buf);
                if (n > 0 && uCxSocketWrite(app->ucx, client, buf, n) < 0) {
                    break;
                }
            }
        }
        if (n > 0) {
            total += (size_t)n;
            ucxapp_stats_add(udp ? UCXAPP_STATS_UDP : UCXAPP_STATS_TCP, (size_t)n, (size_t)n);
        } else {
            ucxapp_platform_sleep_ms(1);
        }
    }
    if (client >= 0) {
        uCxSocketClose(app->ucx, client);
    }
    uCxSocketClose(app->ucx, sock);
    ucxapp_print("echoed %u bytes\n", (unsigned)total);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gSockCmds[] = {
    { "sock.open",    "<tcp|udp>",                "Create a socket, prints its handle",   UCXAPP_CMD_NEEDS_DEVICE, cmdSockOpen },
    { "sock.connect", "[handle] <host> <port>",   "Connect a socket",                     UCXAPP_CMD_NEEDS_DEVICE, cmdSockConnect },
    { "sock.tls",     "[handle] <ca> [client key]", "Use TLS on a TCP socket before connect (certificates from cert.upload)", UCXAPP_CMD_NEEDS_DEVICE, cmdSockTls },
    { "sock.send",    "[handle] <text>",          "Send text on a connected socket",      UCXAPP_CMD_NEEDS_DEVICE, cmdSockSend },
    { "sock.read",    "[handle] [max] [wait_s]",  "Wait for data (default 5 s) and read", UCXAPP_CMD_NEEDS_DEVICE, cmdSockRead },
    { "sock.bind",    "[handle] <port>",          "Bind a socket to a local port (UDP receive)", UCXAPP_CMD_NEEDS_DEVICE, cmdSockBind },
    { "sock.listen",  "[handle] [port]",          "Listen for TCP connections",           UCXAPP_CMD_NEEDS_DEVICE, cmdSockListen },
    { "sock.accept",  "[wait_s]",                 "Wait for an incoming TCP connection (default 30 s)", UCXAPP_CMD_NEEDS_DEVICE, cmdSockAccept },
    { "sock.sendto",  "[handle] <host> <port> <text>", "Send a UDP datagram",           UCXAPP_CMD_NEEDS_DEVICE, cmdSockSendTo },
    { "sock.recvfrom", "[handle] [max] [wait_s]", "Receive a UDP datagram and show its sender", UCXAPP_CMD_NEEDS_DEVICE, cmdSockRecvFrom },
    { "sock.close",   "[handle] | all",           "Close a socket, or every socket on the module", UCXAPP_CMD_NEEDS_DEVICE, cmdSockClose },
    { "sock.list",    "",                         "List open sockets",                    UCXAPP_CMD_NEEDS_DEVICE, cmdSockList },
    { "sock.echo",    "<tcp|udp> <port> [seconds]", "Echo server for latency/throughput tests (default 60 s, one TCP client)", UCXAPP_CMD_NEEDS_DEVICE, cmdSockEcho },
};

void ucxapp_register_sock_cmds(void)
{
    ucxapp_cmd_register(gSockCmds, sizeof(gSockCmds) / sizeof(gSockCmds[0]));
}
