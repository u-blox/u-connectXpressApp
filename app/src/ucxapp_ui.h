#ifndef UCXAPP_UI_H
#define UCXAPP_UI_H

/* Shared core for the web page and the text UI: output and event streams, a one-at-a-time command
 * runner, and the command catalog (groups, usage fields) both front ends are generated from. */

#include "ucxapp_cmd.h"

#define UCXAPP_UI_MAX_GROUPS     48
#define UCXAPP_UI_GROUP_LEN      12
#define UCXAPP_UI_MAX_FIELDS     8
#define UCXAPP_UI_MAX_GROUP_CMDS 64
#define UCXAPP_UI_MAX_VARIANTS   24

/* Field kinds: a plain value, a fixed choice (select), a value or one of the choices (open),
 * a keyword that is there or not (flag), or the rest of the line as words (rest). */
#define UCXAPP_UI_TEXT  't'
#define UCXAPP_UI_PICK  's'
#define UCXAPP_UI_OPEN  'o'
#define UCXAPP_UI_FLAG  'f'
#define UCXAPP_UI_REST  'r'

typedef struct {
    char name[48];       /* placeholder text, e.g. "ssid" */
    bool optional;
    char choices[160];   /* "tcp|udp" for a choice, else empty */
    char def[24];        /* default ([seconds=10]) or suggested value (<hz=1>), else empty */
    char key[12];        /* "id=" for [id=<client>]: sent in front of the value */
    char kind;           /* UCXAPP_UI_TEXT ... UCXAPP_UI_REST */
} ucxapp_ui_field_t;

/* One form of a command: "tcpc <host> <port>" is literal "tcpc" plus the fields host and port. */
typedef struct {
    char label[48];
    char literal[32];
    int count;
    ucxapp_ui_field_t fields[UCXAPP_UI_MAX_FIELDS];
} ucxapp_ui_variant_t;

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
/* Cellular live card cached by the command worker (see ucxapp_cell_card); false when no modem is open. */
bool ucxapp_ui_cell_card(char *out, size_t size);
/* The links snapshot (ucxapp_conn_snapshot) cached by the command worker; asking keeps it refreshed every
   2 s for 12 s. Empty until the first refresh or when no module is open. */
size_t ucxapp_ui_conn_text(char *out, size_t size);
/* The same snapshot for the web event stream; false (nothing to send) while nobody asks ucxapp_ui_conn_text. */
bool ucxapp_ui_conn_card(char *out, size_t size);
void ucxapp_ui_wifi_set(bool up);   /* URC context safe */
void ucxapp_ui_thread_changed(void);   /* URC context safe: re-read the Thread role */
void ucxapp_ui_thread_text(char *out, size_t size);   /* role, "off", or empty without Thread firmware */

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

/* Catalog. Groups come in menu order (module groups first, each side in sections); commands keep their
 * registration order unless listed in the most-used-first table. */
int ucxapp_ui_groups(char names[][UCXAPP_UI_GROUP_LEN], int max);
int ucxapp_ui_group_cmds(const char *group, const ucxapp_cmd_t **out, int max);
/* "host" when this PC does the work, "module" when the NORA module executes it. */
const char *ucxapp_ui_runs_on(const char *group);
const char *ucxapp_ui_cmd_runs_on(const ucxapp_cmd_t *cmd);

/* One-line description of a command group; "" if none. */
const char *ucxapp_ui_group_help(const char *group);
/* Menu section of a group ("Device", "Bluetooth", ... "Peripherals"); "" for an unlisted group. */
const char *ucxapp_ui_group_section(const char *group);

/* Splits a usage string into fields; -1 when it has more than one form or is too complex. */
int ucxapp_ui_parse_usage(const char *usage, ucxapp_ui_field_t *fields, int max);

/* Splits a usage string into its forms ("tcps [port] | udps <port>", "[a|b c]") and each form into a
 * literal and fields; returns the number of forms, -1 when it is too complex (use one free-text field). */
int ucxapp_ui_parse_variants(const char *usage, ucxapp_ui_variant_t *out, int max);

/* Builds a command line from field values: trailing empties dropped, values quoted as needed.
 * raw: values[0] is appended unchanged. */
void ucxapp_ui_compose(const char *name, const char *const *values, int count, bool raw, char *out, size_t size);

#endif /* UCXAPP_UI_H */
