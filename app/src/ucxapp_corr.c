/* Correction stream checker: frames RTCM3 (CRC-24Q), SPARTN (frame CRC-4 and message CRC-8/16/24/32, as in ubxlib's
   u_spartn.c, Apache 2.0) and UBX (Fletcher-8) and counts the good and the corrupt ones. */

#include "ucxapp_corr.h"

#include <stdio.h>
#include <string.h>

#define NEED_MORE 0
#define NO_FRAME  (-1)

/* SPARTN CRC-4 is linear in its input byte: the XOR of these per set bit gives ubxlib's 256-entry table. */
static const uint8_t kCrc4Bit[8] = { 0x0B, 0x05, 0x0A, 0x07, 0x0E, 0x0F, 0x0D, 0x09 };

static uint8_t crc4(const uint8_t *d, size_t n)
{
    uint8_t r = 0;

    for (size_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)(d[i] ^ r);
        r = 0;
        for (int b = 0; b < 8; b++) {
            if (((x >> b) & 1) != 0) {
                r = (uint8_t)(r ^ kCrc4Bit[b]);
            }
        }
    }
    return r;
}

/* MSB-first CRC without reflection: CRC-8 (0x07), CRC-16 (0x1021), CRC-24Q (0x864CFB), CRC-32 (0x04C11DB7). */
static uint32_t crcMsb(const uint8_t *d, size_t n, unsigned width, uint32_t poly, uint32_t init)
{
    uint32_t top = 1u << (width - 1);
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1;
    uint32_t crc = init;

    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << (width - 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & top) != 0 ? (crc << 1) ^ poly : crc << 1;
        }
        crc &= mask;
    }
    return crc;
}

static uint32_t be(const uint8_t *d, size_t n)
{
    uint32_t v = 0;

    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | d[i];
    }
    return v;
}

static int spartnLen(const uint8_t *p, size_t n)
{
    static const uint8_t kAuth[] = { 8, 12, 16, 32, 64 };
    uint8_t head[3];
    size_t hdr = 8;
    size_t body;

    if (n < 5) {
        return NEED_MORE;
    }
    head[0] = p[1];
    head[1] = p[2];
    head[2] = (uint8_t)(p[3] & 0xF0);
    if (crc4(head, 3) != (p[3] & 0x0F)) {
        return NO_FRAME;
    }
    hdr += (p[4] & 0x08) != 0 ? 2 : 0;                                      /* 32-bit time tag */
    body = ((size_t)(p[1] & 0x01) << 9) | ((size_t)p[2] << 1) | (p[3] >> 7);  /* payload length */
    body += ((p[3] >> 4) & 3) + 1u;                                          /* message CRC */
    if ((p[3] & 0x40) != 0) {                                                /* encrypted and authenticated */
        if (n < hdr + 2) {
            return NEED_MORE;
        }
        hdr += 2;
        if (((p[hdr - 1] >> 3) & 7) > 1) {
            if ((p[hdr - 1] & 7) >= sizeof(kAuth)) {
                return NO_FRAME;
            }
            body += kAuth[p[hdr - 1] & 7];
        }
    }
    return (int)(hdr + body);
}

/* Frame length at p, NEED_MORE or NO_FRAME; *type tells which protocol. */
static int frameLen(const uint8_t *p, size_t n, int *type)
{
    if (p[0] == 0xD3) {
        *type = UCXAPP_CORR_RTCM;
        if (n < 3) {
            return NEED_MORE;
        }
        return (p[1] & 0xFC) != 0 ? NO_FRAME : (int)(3 + (((size_t)(p[1] & 3) << 8) | p[2]) + 3);
    }
    if (p[0] == 0xB5) {
        *type = UCXAPP_CORR_UBX;
        if (n < 2) {
            return NEED_MORE;
        }
        if (p[1] != 0x62) {
            return NO_FRAME;
        }
        if (n < 6) {
            return NEED_MORE;
        }
        size_t len = 8u + (p[4] | ((size_t)p[5] << 8));
        return len > UCXAPP_CORR_BUF ? NO_FRAME : (int)len;
    }
    if (p[0] == 0x73) {
        *type = UCXAPP_CORR_SPARTN;
        return spartnLen(p, n);
    }
    return NO_FRAME;
}

static int frameOk(const uint8_t *p, size_t len, int type)
{
    if (type == UCXAPP_CORR_RTCM) {
        return crcMsb(p, len - 3, 24, 0x864CFB, 0) == be(p + len - 3, 3);
    }
    if (type == UCXAPP_CORR_UBX) {
        uint8_t a = 0, b = 0;
        for (size_t i = 2; i < len - 2; i++) {
            a = (uint8_t)(a + p[i]);
            b = (uint8_t)(b + a);
        }
        return a == p[len - 2] && b == p[len - 1];
    }
    size_t crcLen = ((p[3] >> 4) & 3) + 1u;
    static const uint32_t kPoly[] = { 0x07, 0x1021, 0x864CFB, 0x04C11DB7 };
    uint32_t init = crcLen == 4 ? 0xFFFFFFFFu : 0;
    uint32_t crc = crcMsb(p + 1, len - 1 - crcLen, (unsigned)crcLen * 8, kPoly[crcLen - 1], init) ^ init;
    return crc == be(p + len - crcLen, crcLen);
}

void ucxapp_corr_reset(ucxapp_corr_t *c)
{
    memset(c, 0, sizeof(*c));
}

static void scan(ucxapp_corr_t *c)
{
    size_t pos = 0;

    while (pos < c->len) {
        int type = 0;
        int len = frameLen(c->buf + pos, c->len - pos, &type);

        if (len == NEED_MORE || (len > 0 && (size_t)len > c->len - pos)) {
            break;
        }
        if (len > 0 && frameOk(c->buf + pos, (size_t)len, type)) {
            c->good[type]++;
            c->resync = 0;
            pos += (size_t)len;
            continue;
        }
        if (len > 0 && !c->resync) {
            c->bad[type]++;
            c->resync = 1;
        }
        c->junk++;
        pos++;
    }
    memmove(c->buf, c->buf + pos, c->len - pos);
    c->len -= pos;
}

void ucxapp_corr_feed(ucxapp_corr_t *c, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = sizeof(c->buf) - c->len < len ? sizeof(c->buf) - c->len : len;

        memcpy(c->buf + c->len, data, n);
        c->len += n;
        data += n;
        len -= n;
        scan(c);
    }
}

void ucxapp_corr_describe(const ucxapp_corr_t *c, char *out, size_t size)
{
    static const char *const kName[UCXAPP_CORR_TYPES] = { "RTCM", "SPARTN", "UBX" };
    size_t n = 0;

    out[0] = '\0';
    for (int t = 0; t < UCXAPP_CORR_TYPES; t++) {
        if (c->good[t] == 0 && c->bad[t] == 0) {
            continue;
        }
        n += (size_t)snprintf(out + n, n < size ? size - n : 0, "%s%s %u", n > 0 ? ", " : "", kName[t],
                              (unsigned)c->good[t]);
        if (c->bad[t] > 0 && n < size) {
            n += (size_t)snprintf(out + n, size - n, " (%u bad)", (unsigned)c->bad[t]);
        }
    }
    if (n == 0) {
        n = (size_t)snprintf(out, size, "no frames yet");
    }
    if (c->junk > 0 && n < size) {
        snprintf(out + n, size - n, ", %u bytes outside frames", (unsigned)c->junk);
    }
}
