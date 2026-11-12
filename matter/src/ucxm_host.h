/* The plug-in's view of the module: the u-connectXpress API (uCx*) on the app's AT client. */
#ifndef UCXM_HOST_H
#define UCXM_HOST_H

#include "ucxapp_matter.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "u_cx.h"

extern const ucxapp_matter_host_t *gUcxmHost;

/* The handle for the uCx* calls, with no URC callbacks registered yet. */
uCxHandle_t *ucxm_host_open(const ucxapp_matter_host_t *host);

/* The handle of ucxm_host_open, NULL before it. */
uCxHandle_t *ucxm_host_handle(void);

/* One URC line to the callbacks registered on the handle; the line is cut up in place. */
void ucxm_host_urc(char *line, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* UCXM_HOST_H */
