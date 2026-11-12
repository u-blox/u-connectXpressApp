/* The AT client calls of the generated u-connectXpress API go to the app: the plug-in has no AT client and no UART
 * of its own (app/src/ucxapp_matter.h). Force-included into the C files that make those calls, so the real
 * declarations are read first and only the calls are redirected. */
#ifndef UCXM_AT_H
#define UCXM_AT_H

#ifdef __EMSCRIPTEN__
/* the node's C names get their prefix before anything here declares them (force-included after this file) */
#include "../compat/wasm/ucxm_symbols.h"
#endif
#include "u_cx_at_client.h"
#include "ucxapp_matter.h"

extern const ucxapp_matter_host_t *gUcxmHost;

#define uCxAtClientExecSimpleCmd       (gUcxmHost->at->execSimpleCmd)
#define uCxAtClientExecSimpleCmdF      (gUcxmHost->at->execSimpleCmdF)
#define uCxAtClientCmdBeginF           (gUcxmHost->at->cmdBeginF)
#define uCxAtClientCmdGetRspParamLine  (gUcxmHost->at->cmdGetRspParamLine)
#define uCxAtClientCmdGetRspParamsF    (gUcxmHost->at->cmdGetRspParamsF)
#define uCxAtClientCmdEnd              (gUcxmHost->at->cmdEnd)
#define uCxAtClientSetCommandTimeout   (gUcxmHost->at->setCommandTimeout)
#define uCxAtClientGetLastIoError      (gUcxmHost->at->getLastIoError)

#endif /* UCXM_AT_H */
