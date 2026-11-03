#ifndef UCXAPP_XPLORER_H
#define UCXAPP_XPLORER_H

/* Remote control of the u-connectXplorer phone app (Settings > Remote control): JSON-RPC 2.0 over a WebSocket.
 * ucxapp drives the module over AT and the phone over this link, so one command can run both ends of a test. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if (defined(_WIN32) || defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) && \
    !defined(__ZEPHYR__)
#define UCXAPP_XPLORER 1
#else
#define UCXAPP_XPLORER 0
#endif

#define UCXAPP_XPLORER_PORT 8765

/** The phone saved by xplorer.connect; false (message printed) if none. */
bool ucxapp_xplorer_saved(char *host, size_t hostSize, uint16_t *port, char *token, size_t tokenSize);

/** Saves the phone; the token is sealed with the install key. */
bool ucxapp_xplorer_save(const char *host, uint16_t port, const char *token);

/** Connects and authenticates; false with the reason printed. */
bool ucxapp_xplorer_open_to(const char *host, uint16_t port, const char *token);

/** Opens the saved phone (does nothing if already open). */
bool ucxapp_xplorer_open(void);

void ucxapp_xplorer_close(void);

/** Host of the open connection, "" when closed. */
const char *ucxapp_xplorer_host(void);

/**
 * Calls method with params (a JSON object text, or NULL). Returns the "result" value as malloc'ed JSON text
 * (caller frees), or NULL with "ERROR: <method>: <message>" printed. Events that come meanwhile are queued.
 */
char *ucxapp_xplorer_call(const char *method, const char *params, uint32_t timeoutMs);

/** Next event with this method name (others are dropped): its params as malloc'ed JSON, NULL after timeoutMs. */
char *ucxapp_xplorer_event(const char *method, uint32_t timeoutMs);

/* Small JSON readers. path is a key or dotted keys ("features.socketEcho") of nested objects. */

/** The raw value text (strings keep their quotes) as malloc'ed text; NULL if missing. */
char *ucxapp_json_get(const char *json, const char *path);

/** A string value without quotes and escapes; false if missing or not a string. */
bool ucxapp_json_str(const char *json, const char *path, char *out, size_t size);

/** A number (fractions dropped); false if missing or not a number. */
bool ucxapp_json_int(const char *json, const char *path, int64_t *out);

/** true/false; false if missing or not a boolean. */
bool ucxapp_json_bool(const char *json, const char *path, bool *out);

/** Element index of a JSON array as malloc'ed text; NULL past the end. */
char *ucxapp_json_item(const char *array, int index);

/** s as a JSON string literal with quotes; false if out is too small. */
bool ucxapp_json_quote(const char *s, char *out, size_t size);

/* Phone echo peer (socket.startServer with echo) for perf.latency / perf.throughput; the connection must be open. */

/** Starts the phone's TCP or UDP echo server on port; false (printed) if the app cannot echo. */
bool ucxapp_xplorer_echo_start(bool udp, int32_t port);

/** Prints what the phone received and echoed; *breaks gets its 0x00-0xFF pattern breaks (-1 if unknown). */
bool ucxapp_xplorer_echo_report(int64_t *breaks);

void ucxapp_xplorer_echo_stop(void);

/** True if app.getCapabilities lists features.<name> as true (e.g. "socketEcho", "perfSps"). */
bool ucxapp_xplorer_has(const char *feature);

#endif /* UCXAPP_XPLORER_H */
