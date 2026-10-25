/* file.send / file.recv: YMODEM over the console, for consoles that are a serial line (STM32). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_cmd.h"
#include "ucxapp_store.h"
#include "ucxapp_ymodem.h"

/* Messages printed right before a transfer avoid a capital C: senders take 'C' as the start signal. */

static int consoleGet(void *ctx, int timeoutMs)
{
    return ((const ucxapp_io_t *)ctx)->getbyte(timeoutMs);
}

static void consolePut(void *ctx, const uint8_t *data, size_t len)
{
    ((const ucxapp_io_t *)ctx)->putraw(data, len);
}

static bool rawConsole(ucxapp_ymodem_io_t *io)
{
    const ucxapp_io_t *con = ucxapp_io_get();

    if (con == NULL || con->getbyte == NULL || con->putraw == NULL) {
        ucxapp_print("ERROR: this console has no raw serial access (use the STM32 console)\n");
        return false;
    }
    io->getbyte = consoleGet;
    io->put = consolePut;
    io->ctx = (void *)con;
    while (con->getbyte(50) >= 0) {
        /* drop the rest of the command line (LF after CR) */
    }
    return true;
}

static const char *errText(int rc)
{
    switch (rc) {
        case UCXAPP_YMODEM_ERR_TIMEOUT: return "timeout";
        case UCXAPP_YMODEM_ERR_CANCEL:  return "cancelled";
        case UCXAPP_YMODEM_ERR_IO:      return "file error";
        case UCXAPP_YMODEM_ERR_MEMORY:  return "out of memory";
        default:                        return "protocol error";
    }
}

/* Plain file name: no folders, no "." or "..". */
static const char *baseName(const char *path)
{
    const char *p = path;

    for (const char *s = path; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\' || *s == ':') {
            p = s + 1;
        }
    }
    if (*p == '\0' || strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
        return NULL;
    }
    return p;
}

typedef struct {
    const char *forced;
    char name[UCXAPP_YMODEM_NAME_MAX + 1];
    ucxapp_store_writer_t *w;
} recvCtx_t;

static bool recvSink(void *ctx, const char *name, long size, const uint8_t *data, size_t len)
{
    recvCtx_t *r = (recvCtx_t *)ctx;

    (void)size;
    if (data == NULL) {
        const char *base = r->forced != NULL ? r->forced : baseName(name);
        if (base == NULL || r->w != NULL) {
            return false;
        }
        snprintf(r->name, sizeof(r->name), "%s", base);
        r->w = ucxapp_store_begin(r->name);
        return r->w != NULL;
    }
    return ucxapp_store_append(r->w, data, len);
}

static int cmdFileRecv(ucxapp_t *app, int argc, char *argv[])
{
    recvCtx_t r = { NULL, "", NULL };
    ucxapp_ymodem_io_t io;
    long len = 0;

    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2 && (r.forced = baseName(argv[1])) == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    if (!rawConsole(&io)) {
        return UCXAPP_ERR_UNSUPPORTED;
    }
    ucxapp_print("ymodem: ready, start the ymodem upload now (ctrl-x ctrl-x to abort)\n");
    int rc = ucxapp_ymodem_recv(&io, recvSink, &r, &len);
    bool ok = ucxapp_store_end(r.w, rc == UCXAPP_YMODEM_OK);
    if (rc != UCXAPP_YMODEM_OK || !ok) {
        ucxapp_print("\nERROR: file.recv %s\n", rc != UCXAPP_YMODEM_OK ? errText(rc) : "cannot store the file");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("\nfile.recv: %s, %ld bytes\n", r.name, len);
    return UCXAPP_OK;
}

static int32_t sendSrc(void *ctx, uint8_t *buf, size_t len)
{
    return ucxapp_store_read(ctx, buf, len);
}

static int cmdFileSend(ucxapp_t *app, int argc, char *argv[])
{
    ucxapp_ymodem_io_t io;
    uint8_t buf[256];
    long size = 0;
    int32_t n;

    (void)app;
    if (argc != 2 || baseName(argv[1]) == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    void *h = ucxapp_store_open_read(argv[1]);
    if (h == NULL) {
        ucxapp_print("ERROR: cannot open %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    while ((n = ucxapp_store_read(h, buf, sizeof(buf))) > 0) {
        size += n;
    }
    ucxapp_store_close(h);
    if (n < 0 || (h = ucxapp_store_open_read(argv[1])) == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    if (!rawConsole(&io)) {
        ucxapp_store_close(h);
        return UCXAPP_ERR_UNSUPPORTED;
    }
    ucxapp_print("ymodem: ready, start the ymodem download now (%ld bytes, ctrl-x ctrl-x to abort)\n", size);
    int rc = ucxapp_ymodem_send(&io, baseName(argv[1]), size, sendSrc, h);
    ucxapp_store_close(h);
    if (rc != UCXAPP_YMODEM_OK) {
        ucxapp_print("\nERROR: file.send %s\n", errText(rc));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("\nfile.send: %s, %ld bytes\n", baseName(argv[1]), size);
    return UCXAPP_OK;
}

/* ---- file.selftest: sender and receiver run against each other through buffers ---- */

typedef struct {
    const uint8_t *in;
    size_t inLen;
    size_t inPos;
    uint8_t *out;
    size_t outLen;
    size_t outCap;
} loop_t;

static int loopGet(void *ctx, int timeoutMs)
{
    loop_t *l = (loop_t *)ctx;

    (void)timeoutMs;
    return l->inPos < l->inLen ? l->in[l->inPos++] : -1;
}

static void loopPut(void *ctx, const uint8_t *data, size_t len)
{
    loop_t *l = (loop_t *)ctx;

    if (l->outLen + len <= l->outCap) {
        memcpy(l->out + l->outLen, data, len);
        l->outLen += len;
    }
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} memSrc_t;

static int32_t memRead(void *ctx, uint8_t *buf, size_t len)
{
    memSrc_t *m = (memSrc_t *)ctx;
    size_t n = m->len - m->pos < len ? m->len - m->pos : len;

    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (int32_t)n;
}

typedef struct {
    char name[UCXAPP_YMODEM_NAME_MAX + 1];
    long size;
    uint8_t *data;
    size_t len;
    size_t cap;
} memSink_t;

static bool memWrite(void *ctx, const char *name, long size, const uint8_t *data, size_t len)
{
    memSink_t *m = (memSink_t *)ctx;

    if (data == NULL) {
        snprintf(m->name, sizeof(m->name), "%s", name);
        m->size = size;
        return true;
    }
    if (m->len + len > m->cap) {
        return false;
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return true;
}

#define ST_FILE_LEN  2100   /* two 1K blocks and a 128-byte tail */
#define ST_WIRE_CAP  8192

static bool recvCheck(const uint8_t *wire, size_t wireLen, const uint8_t *file, const uint8_t *expectAcks,
                      size_t expectLen, uint8_t *scratch)
{
    uint8_t acks[32];
    memSink_t sink = { "", 0, scratch, 0, ST_FILE_LEN };
    loop_t l = { wire, wireLen, 0, acks, 0, sizeof(acks) };
    ucxapp_ymodem_io_t io = { loopGet, loopPut, &l };
    long got = 0;

    int rc = ucxapp_ymodem_recv(&io, memWrite, &sink, &got);
    return rc == UCXAPP_YMODEM_OK && got == ST_FILE_LEN && sink.size == ST_FILE_LEN &&
           strcmp(sink.name, "test.bin") == 0 && memcmp(scratch, file, ST_FILE_LEN) == 0 &&
           l.outLen == expectLen && memcmp(acks, expectAcks, expectLen) == 0;
}

static int cmdFileSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const uint8_t kReplies[] = { 'C', 0x06, 'C', 0x06, 0x06, 0x06, 0x06, 'C', 0x06 };
    static const uint8_t kRetryReplies[] = { 'C', 0x06, 'C', 0x15, 0x06, 0x06, 0x06, 0x06, 'C', 0x06 };
    const char *fail = NULL;

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    uint8_t *file = (uint8_t *)malloc(ST_FILE_LEN * 2 + ST_WIRE_CAP * 2);
    if (file == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    uint8_t *got = file + ST_FILE_LEN;
    uint8_t *wire = got + ST_FILE_LEN;
    uint8_t *bad = wire + ST_WIRE_CAP;
    for (size_t i = 0; i < ST_FILE_LEN; i++) {
        file[i] = (uint8_t)(i * 7 + 3);
    }

    memSrc_t src = { file, ST_FILE_LEN, 0 };
    loop_t l = { kReplies, sizeof(kReplies), 0, wire, 0, ST_WIRE_CAP };
    ucxapp_ymodem_io_t io = { loopGet, loopPut, &l };
    if (ucxapp_ymodem_crc16((const uint8_t *)"123456789", 9) != 0x31C3) {
        fail = "crc16";
    } else if (ucxapp_ymodem_send(&io, "test.bin", ST_FILE_LEN, memRead, &src) != UCXAPP_YMODEM_OK ||
               l.outLen != 133 + 1029 * 2 + 133 + 1 + 133 || wire[133 + 1029 * 2] != 0x01) {
        fail = "send";
    } else if (!recvCheck(wire, l.outLen, file, kReplies, sizeof(kReplies), got)) {
        fail = "receive";
    } else {
        /* a corrupted copy of block 1 must be NAKed, then the good one accepted */
        memcpy(bad, wire, 133);
        memcpy(bad + 133, wire + 133, 1029);
        bad[133 + 500] ^= 0x40;
        memcpy(bad + 133 + 1029, wire + 133, l.outLen - 133);
        if (!recvCheck(bad, l.outLen + 1029, file, kRetryReplies, sizeof(kRetryReplies), got)) {
            fail = "retry";
        }
    }
    free(file);
    if (fail != NULL) {
        ucxapp_print("ymodem selftest: FAILED (%s)\n", fail);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("ymodem selftest: crc16, 1K and 128-byte blocks, retry ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gYmodemCmds[] = {
    { "file.recv", "[name]", "Receive a file over the console with YMODEM (Tera Term, sz, ucxapp --put)", 0, cmdFileRecv },
    { "file.send", "<file>", "Send a file over the console with YMODEM (Tera Term, rz, ucxapp --get)", 0, cmdFileSend },
    { "file.selftest", "", "Check the YMODEM encoder and decoder against each other (offline)", 0, cmdFileSelftest },
};

void ucxapp_register_ymodem_cmds(void)
{
    ucxapp_cmd_register(gYmodemCmds, sizeof(gYmodemCmds) / sizeof(gYmodemCmds[0]));
}
