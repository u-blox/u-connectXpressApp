#ifndef UCXAPP_LOGCFG_H
#define UCXAPP_LOGCFG_H

#include <stdbool.h>
#include <stddef.h>

/* Included through U_CX_AT_CONFIG_FILE: makes the AT TX/RX trace a runtime switch. */
bool ucxappLogAtEnabled(void);
#define U_CX_LOG_AT ucxappLogAtEnabled()

/* at.trace: every whole AT TX/RX line (ANSI colours stripped, secrets masked) also goes to sink; NULL stops it.
 * Host builds only, where the library prints through ucxappLibPrintf. */
void ucxappLogAtSetTrace(void (*sink)(const char *line));

/* The library prints warnings straight to stdout, which garbles the text UI. */
bool ucxappLogWarnEnabled(void);
void ucxappLogWarnSet(bool on);
void ucxappLogWarnLock(bool locked);
bool ucxappLogEventsEnabled(void);
#define U_CX_LOG_WARNING ucxappLogWarnEnabled()

/* Host builds route the library's printf (AT trace, warnings) through ucxapp_print, so the text UI log shows it. */
int ucxappLibPrintf(const char *format, ...);

/* Copies one piece of the AT trace to out with Wi-Fi, MQTT and key passwords replaced by "****". */
void ucxappLogMask(const char *in, char *out, size_t size);

#endif
