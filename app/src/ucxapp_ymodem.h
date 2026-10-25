#ifndef UCXAPP_YMODEM_H
#define UCXAPP_YMODEM_H

/* YMODEM (1K blocks, CRC-16, file name and size in block 0) over any byte stream, one file per batch. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UCXAPP_YMODEM_OK          0
#define UCXAPP_YMODEM_ERR_TIMEOUT (-1)
#define UCXAPP_YMODEM_ERR_CANCEL  (-2)
#define UCXAPP_YMODEM_ERR_IO      (-3)   /* source or sink failed */
#define UCXAPP_YMODEM_ERR_PROTO   (-4)
#define UCXAPP_YMODEM_ERR_MEMORY  (-5)

#define UCXAPP_YMODEM_PKT_MAX     (3 + 1024 + 2)
#define UCXAPP_YMODEM_NAME_MAX    96

typedef struct {
    int (*getbyte)(void *ctx, int timeoutMs);            /* byte, or -1 on timeout */
    void (*put)(void *ctx, const uint8_t *data, size_t len);
    void *ctx;
} ucxapp_ymodem_io_t;

/* Fills buf; returns bytes read, 0 at end, < 0 on error. */
typedef int32_t (*ucxapp_ymodem_src_t)(void *ctx, uint8_t *buf, size_t len);
/* Called once with name/size (data NULL) when block 0 arrives, then with each piece of file data. */
typedef bool (*ucxapp_ymodem_sink_t)(void *ctx, const char *name, long size, const uint8_t *data, size_t len);

uint16_t ucxapp_ymodem_crc16(const uint8_t *data, size_t len);
/* Builds a packet (SOH for 128, STX for 1024 bytes; short data is padded with 0x1A); returns its length. */
size_t ucxapp_ymodem_packet(uint8_t *pkt, uint8_t seq, const uint8_t *data, size_t len, size_t blockSize);
/* Block 0 with "name\0size"; an empty name builds the end-of-batch block. */
size_t ucxapp_ymodem_header(uint8_t *pkt, const char *name, long size);
/* Name and size from block 0 data; size is -1 when not given. False for the end-of-batch block. */
bool ucxapp_ymodem_parse_header(const uint8_t *data, size_t len, char *name, size_t nameSize, long *size);

int ucxapp_ymodem_send(const ucxapp_ymodem_io_t *io, const char *name, long size,
                       ucxapp_ymodem_src_t src, void *srcCtx);
/* *outLen: bytes stored. */
int ucxapp_ymodem_recv(const ucxapp_ymodem_io_t *io, ucxapp_ymodem_sink_t sink, void *sinkCtx, long *outLen);

#endif /* UCXAPP_YMODEM_H */
