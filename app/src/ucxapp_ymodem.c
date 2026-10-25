#include "ucxapp_ymodem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOH  0x01
#define STX  0x02
#define EOT  0x04
#define ACK  0x06
#define NAK  0x15
#define CAN  0x18
#define CTRL_C 0x03
#define CRC_REQ 'C'
#define PAD  0x1A

#define BYTE_MS      1000
#define START_TRIES  60      /* 'C' once a second for a minute while the user starts the other side */
#define MAX_RETRIES  10

#define PKT_OK       1
#define PKT_EOT      0
#define PKT_TIMEOUT  (-1)
#define PKT_CANCEL   (-2)
#define PKT_BAD      (-3)

uint16_t ucxapp_ymodem_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t ucxapp_ymodem_packet(uint8_t *pkt, uint8_t seq, const uint8_t *data, size_t len, size_t blockSize)
{
    if (len > blockSize) {
        len = blockSize;
    }
    pkt[0] = blockSize == 1024 ? STX : SOH;
    pkt[1] = seq;
    pkt[2] = (uint8_t)~seq;
    memcpy(pkt + 3, data, len);
    memset(pkt + 3 + len, PAD, blockSize - len);
    uint16_t crc = ucxapp_ymodem_crc16(pkt + 3, blockSize);
    pkt[3 + blockSize] = (uint8_t)(crc >> 8);
    pkt[4 + blockSize] = (uint8_t)crc;
    return blockSize + 5;
}

size_t ucxapp_ymodem_header(uint8_t *pkt, const char *name, long size)
{
    uint8_t data[128];

    memset(data, 0, sizeof(data));
    if (name != NULL && name[0] != '\0') {
        size_t n = strlen(name);
        if (n > UCXAPP_YMODEM_NAME_MAX) {
            n = UCXAPP_YMODEM_NAME_MAX;
        }
        memcpy(data, name, n);
        snprintf((char *)data + n + 1, sizeof(data) - n - 1, "%ld", size);
    }
    return ucxapp_ymodem_packet(pkt, 0, data, sizeof(data), 128);
}

bool ucxapp_ymodem_parse_header(const uint8_t *data, size_t len, char *name, size_t nameSize, long *size)
{
    const uint8_t *end = (const uint8_t *)memchr(data, 0, len);
    size_t n = end != NULL ? (size_t)(end - data) : len;

    *size = -1;
    if (n == 0 || n >= len) {
        return false;
    }
    snprintf(name, nameSize, "%.*s", (int)n, (const char *)data);
    const char *p = (const char *)data + n + 1;
    if ((size_t)(p - (const char *)data) < len && *p >= '0' && *p <= '9') {
        *size = strtol(p, NULL, 10);
    }
    return true;
}

static void putByte(const ucxapp_ymodem_io_t *io, uint8_t b)
{
    io->put(io->ctx, &b, 1);
}

static void cancel(const ucxapp_ymodem_io_t *io)
{
    static const uint8_t kCan[] = { CAN, CAN, CAN, CAN, CAN };
    io->put(io->ctx, kCan, sizeof(kCan));
}

/* Reads one packet into pkt; *dataLen is 128 or 1024. */
static int readPacket(const ucxapp_ymodem_io_t *io, uint8_t *pkt, size_t *dataLen)
{
    int c;

    for (;;) {
        c = io->getbyte(io->ctx, BYTE_MS);
        if (c < 0) {
            return PKT_TIMEOUT;
        }
        if (c == SOH || c == STX) {
            break;
        }
        if (c == EOT) {
            return PKT_EOT;
        }
        if (c == CTRL_C || (c == CAN && io->getbyte(io->ctx, BYTE_MS) == CAN)) {
            return PKT_CANCEL;
        }
    }
    size_t n = c == SOH ? 128 : 1024;
    pkt[0] = (uint8_t)c;
    for (size_t i = 1; i < n + 5; i++) {
        c = io->getbyte(io->ctx, BYTE_MS);
        if (c < 0) {
            return PKT_TIMEOUT;
        }
        pkt[i] = (uint8_t)c;
    }
    *dataLen = n;
    uint16_t crc = (uint16_t)((pkt[3 + n] << 8) | pkt[4 + n]);
    if (pkt[1] != (uint8_t)~pkt[2] || crc != ucxapp_ymodem_crc16(pkt + 3, n)) {
        return PKT_BAD;
    }
    return PKT_OK;
}

int ucxapp_ymodem_recv(const ucxapp_ymodem_io_t *io, ucxapp_ymodem_sink_t sink, void *sinkCtx, long *outLen)
{
    uint8_t *pkt = (uint8_t *)malloc(UCXAPP_YMODEM_PKT_MAX);
    char name[UCXAPP_YMODEM_NAME_MAX + 1];
    bool expectHeader = true;
    bool started = false;
    bool afterEot = false;
    uint8_t seq = 0;
    long remaining = -1;
    long total = 0;
    int errors = 0;
    int rc;

    *outLen = 0;
    if (pkt == NULL) {
        return UCXAPP_YMODEM_ERR_MEMORY;
    }
    putByte(io, CRC_REQ);
    for (;;) {
        size_t n = 0;
        int r = readPacket(io, pkt, &n);
        if (r == PKT_CANCEL) {
            rc = UCXAPP_YMODEM_ERR_CANCEL;
            break;
        }
        if (r == PKT_TIMEOUT || r == PKT_BAD) {
            if (++errors >= (started ? MAX_RETRIES : START_TRIES)) {
                cancel(io);
                rc = r == PKT_TIMEOUT ? UCXAPP_YMODEM_ERR_TIMEOUT : UCXAPP_YMODEM_ERR_PROTO;
                break;
            }
            putByte(io, expectHeader && r == PKT_TIMEOUT ? CRC_REQ : NAK);
            continue;
        }
        if (r == PKT_EOT) {
            putByte(io, ACK);
            if (!expectHeader) {
                putByte(io, CRC_REQ);
                expectHeader = true;
                afterEot = true;
                seq = 0;
            }
            continue;
        }
        const uint8_t *data = pkt + 3;
        if (expectHeader) {
            long size;
            if (pkt[1] != 0) {
                putByte(io, NAK);
                continue;
            }
            if (!ucxapp_ymodem_parse_header(data, n, name, sizeof(name), &size)) {
                putByte(io, ACK);
                rc = afterEot ? UCXAPP_YMODEM_OK : UCXAPP_YMODEM_ERR_PROTO;
                break;
            }
            if (afterEot) {
                cancel(io);   /* one file per transfer; the first one is stored */
                rc = UCXAPP_YMODEM_OK;
                break;
            }
            if (!sink(sinkCtx, name, size, NULL, 0)) {
                cancel(io);
                rc = UCXAPP_YMODEM_ERR_IO;
                break;
            }
            remaining = size;
            expectHeader = false;
            started = true;
            seq = 1;
            errors = 0;
            putByte(io, ACK);
            putByte(io, CRC_REQ);
            continue;
        }
        if (pkt[1] == (uint8_t)(seq - 1)) {
            putByte(io, ACK);   /* our ACK was lost: the sender repeated the block */
            continue;
        }
        if (pkt[1] != seq) {
            cancel(io);
            rc = UCXAPP_YMODEM_ERR_PROTO;
            break;
        }
        if (remaining >= 0 && (long)n > remaining) {
            n = (size_t)remaining;
        }
        if (n > 0 && !sink(sinkCtx, NULL, 0, data, n)) {
            cancel(io);
            rc = UCXAPP_YMODEM_ERR_IO;
            break;
        }
        if (remaining >= 0) {
            remaining -= (long)n;
        }
        total += (long)n;
        seq++;
        errors = 0;
        putByte(io, ACK);
    }
    free(pkt);
    *outLen = total;
    return rc;
}

/* Waits for wanted, ACK/NAK or a cancel; other bytes (repeated 'C', line noise) are skipped. */
static int waitFor(const ucxapp_ymodem_io_t *io, int wanted, int tries)
{
    int noise = 0;

    for (int i = 0; i < tries; i++) {
        int c = io->getbyte(io->ctx, BYTE_MS);
        if (c < 0) {
            continue;
        }
        if (c == wanted || c == NAK) {
            return c;
        }
        if (c == CAN || c == CTRL_C) {
            return CAN;
        }
        if (++noise < 4096) {
            i--;
        }
    }
    return -1;
}

static int sendPacket(const ucxapp_ymodem_io_t *io, const uint8_t *pkt, size_t len)
{
    for (int i = 0; i < MAX_RETRIES; i++) {
        io->put(io->ctx, pkt, len);
        int c = waitFor(io, ACK, MAX_RETRIES);
        if (c == ACK) {
            return UCXAPP_YMODEM_OK;
        }
        if (c == CAN) {
            return UCXAPP_YMODEM_ERR_CANCEL;
        }
    }
    cancel(io);
    return UCXAPP_YMODEM_ERR_TIMEOUT;
}

static int32_t fill(ucxapp_ymodem_src_t src, void *ctx, uint8_t *buf, size_t len)
{
    size_t have = 0;

    while (have < len) {
        int32_t n = src(ctx, buf + have, len - have);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            break;
        }
        have += (size_t)n;
    }
    return (int32_t)have;
}

int ucxapp_ymodem_send(const ucxapp_ymodem_io_t *io, const char *name, long size,
                       ucxapp_ymodem_src_t src, void *srcCtx)
{
    uint8_t *pkt = (uint8_t *)malloc(UCXAPP_YMODEM_PKT_MAX + 1024);
    uint8_t *data;
    uint8_t seq = 1;
    int rc;
    int c;

    if (pkt == NULL) {
        return UCXAPP_YMODEM_ERR_MEMORY;
    }
    data = pkt + UCXAPP_YMODEM_PKT_MAX;
    c = waitFor(io, CRC_REQ, START_TRIES);
    if (c != CRC_REQ) {
        free(pkt);
        return c == CAN ? UCXAPP_YMODEM_ERR_CANCEL : UCXAPP_YMODEM_ERR_TIMEOUT;
    }
    rc = sendPacket(io, pkt, ucxapp_ymodem_header(pkt, name, size));
    if (rc == UCXAPP_YMODEM_OK && waitFor(io, CRC_REQ, MAX_RETRIES) != CRC_REQ) {
        rc = UCXAPP_YMODEM_ERR_PROTO;
    }
    while (rc == UCXAPP_YMODEM_OK) {
        int32_t n = fill(src, srcCtx, data, 1024);
        if (n < 0) {
            cancel(io);
            rc = UCXAPP_YMODEM_ERR_IO;
            break;
        }
        if (n == 0) {
            break;
        }
        rc = sendPacket(io, pkt, ucxapp_ymodem_packet(pkt, seq++, data, (size_t)n, n <= 128 ? 128 : 1024));
    }
    if (rc == UCXAPP_YMODEM_OK) {
        static const uint8_t kEot = EOT;
        rc = sendPacket(io, &kEot, 1);
    }
    if (rc == UCXAPP_YMODEM_OK) {
        if (waitFor(io, CRC_REQ, MAX_RETRIES) == CRC_REQ) {
            rc = sendPacket(io, pkt, ucxapp_ymodem_header(pkt, NULL, 0));
        }
        /* a receiver that stops after the file is fine too */
    }
    free(pkt);
    return rc;
}
