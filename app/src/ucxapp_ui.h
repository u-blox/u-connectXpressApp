#ifndef UCXAPP_UI_H
#define UCXAPP_UI_H

/* Shared core for the web page and the text UI: output and event streams, a one-at-a-time command
 * runner, and the command catalog (groups, usage fields) both front ends are generated from. */

#include "ucxapp_cmd.h"

#define UCXAPP_UI_MAX_GROUPS     48
#define UCXAPP_UI_GROUP_LEN      12
#define UCXAPP_UI_MAX_FIELDS     6
#define UCXAPP_UI_MAX_GROUP_CMDS 64

typedef struct {
    char name[24];       /* placeholder text, e.g. "ssid" */
    bool optional;
    char choices[48];    /* "tcp|udp" when the field is a fixed choice, else empty */
} ucxapp_ui_field_t;

/* Starts the streams and the command runner; calls nest, the last release stops them. */
bool ucxapp_ui_acquire(ucxapp_t *app);
void ucxapp_ui_release(void);

/* One line for the events stream (URC context safe); ignored while no front end is active. */
void ucxapp_ui_event(const char *text);

/* Connection state line ("Wi-Fi up | BLE 0* 1 | Sockets -") for a status bar; the returned version
 * changes whenever Wi-Fi, a BLE connection or a socket comes or goes. */
uint32_t ucxapp_ui_state(char *out, size_t size);
int ucxapp_ui_wifi(void);   /* 1 up, 0 down, -1 unknown */
/* A module request that needs an answer (URC context safe): the text UI pre-fills the input with prefill and
   sends cancel on Esc. */
void ucxapp_ui_prompt(const char *text, const char *prefill, const char *cancel);
uint32_t ucxapp_ui_prompt_get(char *text, size_t textSize, char *prefill, size_t fillSize, char *cancel,
                              size_t cancelSize);
/* Wi-Fi ("ssid  rssi  ch\nip") and cellular status cached by the command worker; empty when unknown. */
void ucxapp_ui_dash_text(char *wifi, size_t wifiSize, char *cell, size_t cellSize);
void ucxapp_ui_wifi_set(bool up);   /* URC context safe */

/* Streams are read with a private cursor. Peek clamps the cursor if the reader fell behind and
 * never moves past the data it returns: advance the cursor by the consumed amount yourself. */
uint32_t ucxapp_ui_log_start(void);
size_t ucxapp_ui_log_peek(uint32_t *cursor, char *out, size_t size);   /* size >= 8; never splits UTF-8 */
uint32_t ucxapp_ui_event_start(void);
bool ucxapp_ui_event_peek(uint32_t *cursor, char *line, size_t size);   /* consume with (*cursor)++ */

/* Command runner: false from submit when a command is still running. */
bool ucxapp_ui_submit(const char *line);
bool ucxapp_ui_busy(void);
uint32_t ucxapp_ui_done_seq(int *rc);    /* increments when a command finishes; rc is its result */

/* Catalog. Groups follow the AT manual order; commands keep their registration order. */
int ucxapp_ui_groups(char names[][UCXAPP_UI_GROUP_LEN], int max);
int ucxapp_ui_group_cmds(const char *group, const ucxapp_cmd_t **out, int max);
/* "host" when this PC does the work, "module" when the NORA module executes it. */
const char *ucxapp_ui_runs_on(const char *group);
const char *ucxapp_ui_cmd_runs_on(const ucxapp_cmd_t *cmd);

/* One-line description of a command group; "" if none. */
const char *ucxapp_ui_group_help(const char *group);

/* Splits a usage string into fields; -1 when it is too complex (use one free-text field). */
int ucxapp_ui_parse_usage(const char *usage, ucxapp_ui_field_t *fields, int max);

/* Builds a command line from field values: trailing empties dropped, values quoted as needed.
 * raw: values[0] is appended unchanged. */
void ucxapp_ui_compose(const char *name, const char *const *values, int count, bool raw, char *out, size_t size);

#endif /* UCXAPP_UI_H */
