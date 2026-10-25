#include "ucxapp.h"

#include <stdio.h>
#include <string.h>

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

const ucxapp_io_t ucxapp_io_stdio = { stdioWrite, stdioReadline, NULL, NULL };
