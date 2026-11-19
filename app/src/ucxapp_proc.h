#ifndef UCXAPP_PROC_H
#define UCXAPP_PROC_H

/* Child processes for the PC builds: the external capture tools (nrfutil, ssh) and Wireshark. Not in the browser
 * build (every call fails there) nor on the boards. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ucxapp_proc ucxapp_proc_t;

#define UCXAPP_PROC_STDIN    0x01   /* a pipe to the child's stdin (ucxapp_proc_write) */
#define UCXAPP_PROC_DETACHED 0x02   /* may outlive the app (Wireshark): no output pipe, ucxapp_proc_release */

/** Runs cmdline in the OS shell (cmd.exe on Windows, /bin/sh elsewhere) as its own process group. stdout goes to
 *  outFile when given (binary, created or truncated), else to the output pipe with stderr. NULL if it cannot start. */
ucxapp_proc_t *ucxapp_proc_start(const char *cmdline, const char *outFile, int flags);

/** Output without blocking: the byte count, 0 when nothing is waiting, -1 once the pipe is closed. */
int32_t ucxapp_proc_read(ucxapp_proc_t *p, char *buf, size_t size);

/** Writes everything to the child's stdin; false once the child has closed it. */
bool ucxapp_proc_write(ucxapp_proc_t *p, const void *data, size_t len);

/** True while the child runs; when it has ended, its exit code goes to *code (may be NULL). */
bool ucxapp_proc_running(ucxapp_proc_t *p, int *code);

/** Waits up to ms for the child to end on its own, then kills its process tree; frees p. Returns the exit code
 *  (-1 when it had to be killed). */
int ucxapp_proc_wait(ucxapp_proc_t *p, uint32_t ms);

/** Interrupts the process tree (Ctrl-Break / SIGINT, so a capture tool can finish its file), then as wait. */
int ucxapp_proc_stop(ucxapp_proc_t *p, uint32_t graceMs);

/** Lets a detached child go on alone: closes its stdin and our handles; frees p. */
void ucxapp_proc_release(ucxapp_proc_t *p);

/** Quotes one argument for the shell ucxapp_proc_start uses; false if it cannot be passed safely
 *  (on Windows: ", %, ! and control characters; elsewhere: control characters). */
bool ucxapp_proc_quote(const char *arg, char *out, size_t size);

/** Full path of a program on PATH (".exe" is added on Windows); false if it is not there. */
bool ucxapp_proc_which(const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
