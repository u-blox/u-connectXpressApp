/* coap.* - minimal CoAP (RFC 7252) over UDP on the host, using a module UDP socket. No DTLS (coaps), blockwise or observe.
 * Host builds only. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_socket.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define COAP_MAX          256
#define COAP_TRIES        3
#define COAP_WAIT_POLLS   200     /* 2 s per try at 10 ms per poll */
#define COAP_POLL_MS      10
#define COAP_SEG_MAX      12      /* longer Uri-Path segments need extended option lengths, not supported */
#define COAP_CODE_GET     0x01
#define COAP_HDR_CON      0x40    /* version 1, type confirmable, no token */
#define COAP_HDR_ACK      0x60    /* version 1, type acknowledgement, no token */
#define COAP_TYPE_ACK     2
#define COAP_OPT_URI_PATH 11
#define COAP_PAYLOAD_MARK 0xFF
#define COAP_MID          0x4B1D  /* fixed message id, enough for one request at a time */

/* Encodes a confirmable request with Uri-Path options from "/a/b". Returns the length, 0 on error. */
static size_t encodeRequest(uint8_t code, uint16_t mid, const char *path, uint8_t *out, size_t max)
{
    size_t n = 4;
    unsigned prev = 0;
    const char *p = path;

    if (max < 4) {
        return 0;
    }
    out[0] = COAP_HDR_CON;
    out[1] = code;
    out[2] = (uint8_t)(mid >> 8);
    out[3] = (uint8_t)(mid & 0xFF);
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char *s = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t len = (size_t)(p - s);
        if (len == 0) {
            break;
        }
        if (len > COAP_SEG_MAX || n + 1 + len > max) {
            return 0;
        }
        out[n++] = (uint8_t)(((COAP_OPT_URI_PATH - prev) << 4) | len);
        memcpy(out + n, s, len);
        n += len;
        prev = COAP_OPT_URI_PATH;
    }
    return n;
}

/* Parses a response to message id mid. Returns the payload length, or -1 if the datagram is not a matching response. */
static int parseResponse(const uint8_t *buf, size_t len, uint16_t mid, uint8_t *code, const uint8_t **payload)
{
    if (len < 4 || (buf[0] >> 6) != 1) {
        return -1;
    }
    unsigned type = (buf[0] >> 4) & 0x03;
    size_t tkl = buf[0] & 0x0F;
    if (tkl > 8 || (type == COAP_TYPE_ACK && (buf[2] != (uint8_t)(mid >> 8) || buf[3] != (uint8_t)(mid & 0xFF)))) {
        return -1;
    }
    size_t p = 4 + tkl;
    if (p > len) {
        return -1;
    }
    while (p < len && buf[p] != COAP_PAYLOAD_MARK) {
        unsigned delta = buf[p] >> 4;
        unsigned optLen = buf[p] & 0x0F;
        if (delta == 15 || optLen == 15) {
            return -1;
        }
        p++;
        if (delta == 13) {
            p += 1;
        } else if (delta == 14) {
            p += 2;
        }
        if (optLen == 13) {
            p += 1;
        } else if (optLen == 14) {
            p += 2;
        }
        p += optLen;
        if (p > len) {
            return -1;
        }
    }
    *code = buf[1];
    if (p < len && buf[p] == COAP_PAYLOAD_MARK) {
        *payload = buf + p + 1;
        return (int)(len - p - 1);
    }
    *payload = buf + len;
    return 0;
}

/* Sends the request up to COAP_TRIES times and waits for the matching response. Returns the payload length or < 0. */
static int exchange(uCxHandle_t *ucx, int32_t sock, const uint8_t *req, size_t reqLen, uint16_t mid,
                    uint8_t *buf, size_t bufMax, uint8_t *code, const uint8_t **payload)
{
    for (int t = 0; t < COAP_TRIES; t++) {
        if (uCxSocketWrite(ucx, sock, req, (int32_t)reqLen) < 0) {
            return -2;
        }
        for (int i = 0; i < COAP_WAIT_POLLS; i++) {
            uCxSocketReceiveFrom_t from;
            memset(&from, 0, sizeof(from));
            int32_t rc = ucxapp_udp_recvfrom(ucx, sock, (int32_t)bufMax, buf, &from);
            if (rc > 0) {
                int pl = parseResponse(buf, (size_t)rc, mid, code, payload);
                if (pl >= 0) {
                    return pl;
                }
            } else {
                ucxapp_platform_sleep_ms(COAP_POLL_MS);
            }
        }
    }
    return -1;
}

static void printPrintable(const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        ucxapp_print("%c", (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' ? (char)c : '.');
    }
    ucxapp_print("\n");
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argc;
    (void)argv;
    /* GET /hello/world, message id 0x1234: Uri-Path "hello" (delta 11, length 5), "world" (delta 0, length 5). */
    static const uint8_t expect[] = { 0x40, 0x01, 0x12, 0x34, 0xB5, 'h', 'e', 'l', 'l', 'o', 0x05, 'w', 'o', 'r', 'l', 'd' };
    /* 2.05 Content, message id 0x1234, payload "ok". */
    static const uint8_t reply[] = { 0x60, 0x45, 0x12, 0x34, 0xFF, 'o', 'k' };
    uint8_t req[COAP_MAX];
    uint8_t code = 0;
    const uint8_t *payload = NULL;

    (void)app;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    size_t n = encodeRequest(COAP_CODE_GET, 0x1234, "/hello/world", req, sizeof(req));
    if (n != sizeof(expect) || memcmp(req, expect, n) != 0) {
        ucxapp_print("ERROR: coap selftest: request encoding mismatch\n");
        return UCXAPP_ERR_FAILED;
    }
    int pl = parseResponse(reply, sizeof(reply), 0x1234, &code, &payload);
    if (pl != 2 || code != 0x45 || memcmp(payload, "ok", 2) != 0) {
        ucxapp_print("ERROR: coap selftest: response parsing mismatch\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("coap selftest: GET encoding and 2.05 response parsing ok\n");
    return UCXAPP_OK;
}

static int cmdGet(ucxapp_t *app, int argc, char *argv[])
{
    int32_t port;
    uint8_t req[COAP_MAX];
    uint8_t buf[COAP_MAX];
    uint8_t code = 0;
    const uint8_t *payload = NULL;
    int32_t sock = 0;

    if (argc < 3 || argc > 4 || !ucxapp_parse_int(argv[2], &port) || port < 1 || port > 65535) {
        return UCXAPP_ERR_USAGE;
    }
    const char *path = argc == 4 ? argv[3] : "/";
    size_t n = encodeRequest(COAP_CODE_GET, COAP_MID, path, req, sizeof(req));
    if (n == 0) {
        ucxapp_print("ERROR: path too long, or a segment is longer than %d bytes\n", COAP_SEG_MAX);
        return UCXAPP_ERR_USAGE;
    }
    if (uCxSocketCreate1(app->ucx, U_SOCKET_PROTOCOL_UDP, &sock) != 0) {
        ucxapp_print("ERROR: failed to create a UDP socket\n");
        return UCXAPP_ERR_FAILED;
    }
    if (uCxSocketConnect(app->ucx, sock, argv[1], port) != 0) {
        ucxapp_print("ERROR: connect to %s:%d failed\n", argv[1], (int)port);
        uCxSocketClose(app->ucx, sock);
        return UCXAPP_ERR_FAILED;
    }
    int pl = exchange(app->ucx, sock, req, n, COAP_MID, buf, sizeof(buf), &code, &payload);
    uCxSocketClose(app->ucx, sock);
    if (pl < 0) {
        ucxapp_print("ERROR: no CoAP reply from %s:%d%s\n", argv[1], (int)port, path);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("coap %d.%02d from %s:%d%s\n", code >> 5, code & 0x1F, argv[1], (int)port, path);
    printPrintable(payload, pl);
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gCoapCmds[] = {
    { "coap.get",      "<host> <port> [path]", "CoAP GET over UDP via a module socket (confirmable, 3 tries, 2 s each)", UCXAPP_CMD_NEEDS_DEVICE, cmdGet },
    { "coap.selftest", "",                     "Offline check of CoAP request encoding and response parsing", 0, cmdSelftest },
};

void ucxapp_register_coap_cmds(void)
{
    ucxapp_cmd_register(gCoapCmds, sizeof(gCoapCmds) / sizeof(gCoapCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_coap_cmds(void)
{
}

#endif
