#include "ucxapp.h"

#include <stdio.h>
#include <string.h>

#define UCXAPP_PRINT_BUF_SIZE 512

static const ucxapp_io_t *gIo = NULL;

#define UCXAPP_MAX_SINKS 4
static ucxapp_sink_fn_t gSinks[UCXAPP_MAX_SINKS];

bool ucxapp_io_add_sink(ucxapp_sink_fn_t sink)
{
    for (int i = 0; i < UCXAPP_MAX_SINKS; i++) {
        if (gSinks[i] == NULL) {
            gSinks[i] = sink;
            return true;
        }
    }
    return false;
}

void ucxapp_io_remove_sink(ucxapp_sink_fn_t sink)
{
    for (int i = 0; i < UCXAPP_MAX_SINKS; i++) {
        if (gSinks[i] == sink) {
            gSinks[i] = NULL;
        }
    }
}

void ucxapp_io_set(const ucxapp_io_t *io)
{
    gIo = io;
}

const ucxapp_io_t *ucxapp_io_get(void)
{
    return gIo;
}

void ucxapp_print(const char *fmt, ...)
{
    char buf[UCXAPP_PRINT_BUF_SIZE];
    va_list args;

    if (gIo == NULL || gIo->write == NULL) {
        return;
    }
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;   /* truncated */
    }
    gIo->write(buf, (size_t)n);
    for (int i = 0; i < UCXAPP_MAX_SINKS; i++) {
        if (gSinks[i] != NULL) {
            gSinks[i](buf, (size_t)n);
        }
    }
}

int ucxapp_readline(const char *prompt, char *buf, size_t size)
{
    if (gIo == NULL || gIo->readline == NULL || size == 0) {
        return -1;
    }
    if (prompt != NULL) {
        ucxapp_print("%s", prompt);
    }
    return gIo->readline(buf, size);
}
