#include "ucxapp.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "ucxapp_platform.h"
#include "ucxapp_thread.h"

static void stdioWrite(const char *text, size_t len)
{
    fwrite(text, 1, len, stdout);
    fflush(stdout);
}

static int stdioReadline(char *buf, size_t size)
{
    if (fgets(buf, (int)size, stdin) == NULL) {
        return -1;
    }
    size_t len = strcspn(buf, "\r\n");
    buf[len] = '\0';
    return (int)len;
}

ucxapp_io_t ucxapp_io_stdio = { stdioWrite, stdioReadline, NULL, NULL };

#ifndef __EMSCRIPTEN__
/* --raw-console: stdin and stdout become a serial line, as the STM32 console is, so YMODEM (file.recv, file.send)
   runs on a PC through a pipe (the tests' fake console). A reader thread fills a ring so getbyte can time out. */

#define RAW_RING 8192

static uint8_t gRing[RAW_RING];
static volatile size_t gHead;   /* moved only by the reader thread */
static volatile size_t gTail;   /* moved only by the console's reader */
static volatile bool gEof;

static void rawReader(void *arg)
{
    int c;

    (void)arg;
    while ((c = fgetc(stdin)) != EOF) {
        while ((gHead + 1) % RAW_RING == gTail) {
            ucxapp_platform_sleep_ms(1);
        }
        gRing[gHead] = (uint8_t)c;
        gHead = (gHead + 1) % RAW_RING;
    }
    gEof = true;
}

static int rawGetbyte(int timeoutMs)
{
    uint32_t start = ucxapp_platform_tick_ms();

    while (gTail == gHead) {
        if (gEof || (int32_t)(ucxapp_platform_tick_ms() - start) >= timeoutMs) {
            return -1;
        }
        ucxapp_platform_sleep_ms(1);
    }
    int c = gRing[gTail];
    gTail = (gTail + 1) % RAW_RING;
    return c;
}

static void rawPutraw(const uint8_t *data, size_t len)
{
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static int rawReadline(char *buf, size_t size)
{
    size_t len = 0;
    int c;

    for (;;) {
        while ((c = rawGetbyte(100)) < 0) {
            if (gEof && gTail == gHead) {
                buf[len] = '\0';
                return len > 0 ? (int)len : -1;
            }
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r' && len + 1 < size) {
            buf[len++] = (char)c;
        }
    }
    buf[len] = '\0';
    return (int)len;
}

bool ucxapp_io_stdio_raw(void)
{
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    if (ucxapp_thread_start(rawReader, NULL) == NULL) {
        return false;
    }
    ucxapp_io_stdio.readline = rawReadline;
    ucxapp_io_stdio.getbyte = rawGetbyte;
    ucxapp_io_stdio.putraw = rawPutraw;
    return true;
}
#endif
