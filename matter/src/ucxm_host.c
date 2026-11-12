/* Stands in for u-connectClient's u_cx.c: that one hooks the handle into an AT client it owns, here the AT client
 * is the app's and the URCs arrive as copies (ucxapp_matter.h). */
#include "ucxm_host.h"

#include <string.h>

#include "u_cx_urc.h"

const ucxapp_matter_host_t *gUcxmHost;
static uCxHandle_t gHandle;

uCxHandle_t *ucxm_host_open(const ucxapp_matter_host_t *host)
{
    gUcxmHost = host;
    memset(&gHandle, 0, sizeof(gHandle));
    gHandle.pAtClient = host->client;
    return &gHandle;
}

void ucxm_host_urc(char *line, size_t length)
{
    char *params = memchr(line, ':', length);
    size_t paramsLength = 0;

    if (params != NULL) {
        *params++ = '\0';
        paramsLength = length - (size_t)(params - line);
    } else {
        params = line + length;
    }
    (void)uCxUrcParse(&gHandle, line, params, paramsLength);
}

int32_t uCxEnd(uCxHandle_t *puCxHandle)
{
    return uCxAtClientCmdEnd(puCxHandle->pAtClient);
}
