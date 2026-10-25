#include "ucxapp_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_cx_error_codes.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

static int32_t gModErr;
static const void *gCapsFor;
static bool gCapsWifi = true;
static ucxapp_recorder_fn_t gRecorder;

void ucxapp_cmd_set_recorder(ucxapp_recorder_fn_t fn)
{
    gRecorder = fn;
}

typedef struct {
    const ucxapp_cmd_t *table;
    size_t count;
} pack_t;

static pack_t gPacks[UCXAPP_MAX_PACKS];
static size_t gPackCount = 0;

bool ucxapp_cmd_register(const ucxapp_cmd_t *table, size_t count)
{
    if (table == NULL || gPackCount >= UCXAPP_MAX_PACKS) {
        return false;
    }
    gPacks[gPackCount].table = table;
    gPacks[gPackCount].count = count;
    gPackCount++;
    return true;
}

static const ucxapp_cmd_t *findCmd(const char *name)
{
    for (size_t p = 0; p < gPackCount; p++) {
        for (size_t i = 0; i < gPacks[p].count; i++) {
            if (strcmp(gPacks[p].table[i].name, name) == 0) {
                return &gPacks[p].table[i];
            }
        }
    }
    return NULL;
}

const ucxapp_cmd_t *ucxapp_cmd_find(const char *name)
{
    return findCmd(name);
}

const char *ucxapp_errname(int32_t code)
{
    static char text[64];
    const char *name = uCxGetErrorName(code);

    if (name == NULL) {
        return "";
    }
    gModErr = code;
    snprintf(text, sizeof(text), ": %s", name);
    return text;
}

int32_t ucxapp_last_module_error(void)
{
    return gModErr;
}

void ucxapp_caps_reset(void)
{
    gCapsFor = NULL;
}

static bool needsWifi(const char *name)
{
    static const char *const kGroups[] = { "wifi.", "sock.", "mqtt.", "http.", "net.", "cert.", "tftp.", "web.",
                                           "coap.", "dtls." };

    for (size_t i = 0; i < sizeof(kGroups) / sizeof(kGroups[0]); i++) {
        if (strncmp(name, kGroups[i], strlen(kGroups[i])) == 0) {
            return true;
        }
    }
    return false;
}

/* Model is asked once per connection; an unknown model is not blocked. */
static bool moduleHasWifi(ucxapp_t *app)
{
    if (gCapsFor != (const void *)app->ucx) {
        const char *text = NULL;

        gCapsWifi = true;
        if (uCxGeneralGetDeviceModelIdentificationBegin(app->ucx, &text) && text != NULL) {
            gCapsWifi = strstr(text, "B26") == NULL;
        }
        uCxEnd(app->ucx);
        gCapsFor = app->ucx;
    }
    return gCapsWifi;
}

bool ucxapp_parse_int(const char *s, int32_t *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0') {
        return false;
    }
    v = strtol(s, &end, 0);
    if (*end != '\0') {
        return false;
    }
    *out = (int32_t)v;
    return true;
}

static int tokenize(char *line, char *argv[], int maxArgs)
{
    int argc = 0;
    char *r = line;
    char *w = line;

    for (;;) {
        while (*r == ' ' || *r == '\t') {
            r++;
        }
        if (*r == '\0') {
            break;
        }
        if (argc >= maxArgs) {
            return -1;
        }
        argv[argc++] = w;
        if (*r == '"') {
            r++;
            while (*r != '\0' && *r != '"') {
                if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                    r++;
                }
                *w++ = *r++;
            }
            if (*r == '"') {
                r++;
            }
        } else {
            while (*r != '\0' && *r != ' ' && *r != '\t') {
                *w++ = *r++;
            }
        }
        if (*r != '\0') {
            r++;
        }
        *w++ = '\0';
    }
    return argc;
}

void ucxapp_cmd_foreach(void (*visit)(const ucxapp_cmd_t *cmd, void *user), void *user)
{
    for (size_t p = 0; p < gPackCount; p++) {
        for (size_t i = 0; i < gPacks[p].count; i++) {
            visit(&gPacks[p].table[i], user);
        }
    }
}

int ucxapp_cmd_complete(const char *prefix, char *out, size_t size)
{
    size_t plen = strlen(prefix);
    int count = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (size_t p = 0; p < gPackCount; p++) {
        for (size_t i = 0; i < gPacks[p].count; i++) {
            const char *name = gPacks[p].table[i].name;

            if (strncmp(name, prefix, plen) != 0) {
                continue;
            }
            if (count++ == 0) {
                snprintf(out, size, "%s", name);
            } else {
                size_t k = 0;

                while (out[k] != '\0' && out[k] == name[k]) {
                    k++;
                }
                out[k] = '\0';
            }
        }
    }
    return count;
}

/* Print usage one top-level alternative per line ("a|b" or "<a|b>"), each line prefixed by indent. */
void ucxapp_print_usage_lines(const char *indent, const char *usage)
{
    size_t len = strlen(usage);
    const char *p = usage;
    const char *end = usage + len;
    int depth = 0;

    if (len > 2 && usage[0] == '<' && usage[len - 1] == '>') {
        const char *q = usage;

        do {
            depth += (*q == '<' || *q == '[') - (*q == '>' || *q == ']');
            q++;
        } while (depth > 0 && q < end);
        if (q == end) {   /* one <...> around everything */
            p++;
            end--;
        }
        depth = 0;
    }
    for (const char *q = p; q <= end; q++) {
        if (q == end || (*q == '|' && depth == 0)) {
            const char *a = p, *b = q;

            while (a < b && *a == ' ') {
                a++;
            }
            while (b > a && b[-1] == ' ') {
                b--;
            }
            ucxapp_print("%s%.*s\n", indent, (int)(b - a), a);
            p = q + 1;
        } else if (*q == '<' || *q == '[') {
            depth++;
        } else if (*q == '>' || *q == ']') {
            depth--;
        }
    }
}

void ucxapp_cmd_print_help(const char *prefix)
{
    size_t prefixLen = prefix != NULL ? strlen(prefix) : 0;
    bool any = false;

    for (size_t p = 0; p < gPackCount; p++) {
        for (size_t i = 0; i < gPacks[p].count; i++) {
            const ucxapp_cmd_t *c = &gPacks[p].table[i];
            if (strncmp(c->name, prefix != NULL ? prefix : "", prefixLen) != 0) {
                continue;
            }
            if (strlen(c->usage) > 24) {
                ucxapp_print("  %-16s %-24s %s\n", c->name, "", c->help);
                ucxapp_print_usage_lines("                     ", c->usage);
            } else {
                ucxapp_print("  %-16s %-24s %s\n", c->name, c->usage, c->help);
            }
            any = true;
        }
    }
    if (!any) {
        ucxapp_print("No commands match '%s'\n", prefix);
    }
}

void ucxapp_print_json_string(const char *s)
{
    char chunk[64];
    size_t n = 0;

    chunk[n++] = '"';
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (n > sizeof(chunk) - 8) {
            chunk[n] = '\0';
            ucxapp_print("%s", chunk);
            n = 0;
        }
        if (c == '"' || c == '\\') {
            chunk[n++] = '\\';
            chunk[n++] = (char)c;
        } else if (c == '\n') {
            chunk[n++] = '\\';
            chunk[n++] = 'n';
        } else if (c == '\t') {
            chunk[n++] = '\\';
            chunk[n++] = 't';
        } else if (c < 0x20) {
            n += (size_t)snprintf(&chunk[n], sizeof(chunk) - n, "\\u%04x", c);
        } else {
            chunk[n++] = (char)c;
        }
    }
    chunk[n++] = '"';
    chunk[n] = '\0';
    ucxapp_print("%s", chunk);
}

void ucxapp_cmd_print_json(const char *prefix)
{
    size_t prefixLen = prefix != NULL ? strlen(prefix) : 0;
    bool first = true;

    ucxapp_print("[");
    for (size_t p = 0; p < gPackCount; p++) {
        for (size_t i = 0; i < gPacks[p].count; i++) {
            const ucxapp_cmd_t *c = &gPacks[p].table[i];
            if (prefixLen > 0 && strncmp(c->name, prefix, prefixLen) != 0) {
                continue;
            }
            ucxapp_print("%s\n{\"name\":", first ? "" : ",");
            ucxapp_print_json_string(c->name);
            ucxapp_print(",\"usage\":");
            ucxapp_print_json_string(c->usage);
            ucxapp_print(",\"help\":");
            ucxapp_print_json_string(c->help);
            ucxapp_print(",\"needsDevice\":%s,\"danger\":%s}", (c->flags & UCXAPP_CMD_NEEDS_DEVICE) ? "true" : "false",
                         (c->flags & UCXAPP_CMD_DANGER) ? "true" : "false");
            first = false;
        }
    }
    ucxapp_print("\n]\n");
}

const char *ucxapp_err_code(int rc)
{
    switch (rc) {
    case UCXAPP_OK:                return "OK";
    case UCXAPP_ERR_UNKNOWN_CMD:   return "UNKNOWN_CMD";
    case UCXAPP_ERR_USAGE:         return "USAGE";
    case UCXAPP_ERR_NOT_CONNECTED: return "NOT_CONNECTED";
    case UCXAPP_ERR_UNSUPPORTED:   return "UNSUPPORTED";
    default:                       return "FAILED";
    }
}

/* Bytes up to 10 MB, then M or G, so 32-bit printf (newlib nano) is enough. */
static void fmtSize(char *out, size_t size, uint64_t v)
{
    if (v < 10ull * 1024 * 1024) {
        snprintf(out, size, "%lu", (unsigned long)v);
    } else if (v < 10ull * 1024 * 1024 * 1024) {
        snprintf(out, size, "%luM", (unsigned long)(v >> 20));
    } else {
        snprintf(out, size, "%luG", (unsigned long)(v >> 30));
    }
}

void ucxapp_print_usage_row(const char *kind, const char *name, const char *sizeLabel,
                            uint64_t size, uint64_t used)
{
    char label[40];
    char sizeText[16];
    char usedText[16];
    char pctText[8] = "?";

    snprintf(label, sizeof(label), "%s:%s", kind, name);
    fmtSize(usedText, sizeof(usedText), used);
    if (size == 0) {
        snprintf(sizeText, sizeof(sizeText), "?");
    } else {
        fmtSize(sizeText, sizeof(sizeText), size);
        snprintf(pctText, sizeof(pctText), "%u", (unsigned)((used * 100u) / size));
    }
    ucxapp_print("%-24s %s: %-8s used: %-8s usage(%%): %s\n", label, sizeLabel, sizeText, usedText, pctText);
}

bool ucxapp_expand_env(const char *in, char *out, size_t size)
{
    size_t n = 0;

    while (*in != '\0') {
        const char *end = in[0] == '$' && in[1] == '{' ? strchr(in + 2, '}') : NULL;
        const char *value = NULL;
        size_t len;

        if (end != NULL && end > in + 2 && (size_t)(end - in - 2) < 64) {
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(end - in - 2), in + 2);
            value = getenv(name);
            if (value == NULL) {
                ucxapp_print("ERROR: environment variable %s is not set\n", name);
                return false;
            }
            in = end + 1;
        } else {
            end = NULL;
        }
        len = value != NULL ? strlen(value) : 1;
        if (n + len >= size) {
            ucxapp_print("ERROR: command too long\n");
            return false;
        }
        if (value != NULL) {
            memcpy(out + n, value, len);
        } else {
            out[n] = *in++;
        }
        n += len;
    }
    out[n] = '\0';
    return true;
}

#define HANDLE_MAX 8

typedef struct {
    int32_t ids[HANDLE_MAX];
    int count;
} handleSet_t;

static handleSet_t gHandles[2];
static volatile uint32_t gHandleVersion;

uint32_t ucxapp_handle_version(void)
{
    return gHandleVersion;
}

int ucxapp_handle_list(ucxapp_handle_kind_t kind, int32_t *out, int max)
{
    handleSet_t *s = &gHandles[kind];
    int n;

    ucxapp_events_lock();
    n = s->count < max ? s->count : max;
    memcpy(out, s->ids, sizeof(out[0]) * (size_t)n);
    ucxapp_events_unlock();
    return n;
}

void ucxapp_handle_remove(ucxapp_handle_kind_t kind, int32_t id)
{
    handleSet_t *s = &gHandles[kind];

    ucxapp_events_lock();
    for (int i = 0; i < s->count; i++) {
        if (s->ids[i] == id) {
            memmove(&s->ids[i], &s->ids[i + 1], sizeof(s->ids[0]) * (size_t)(s->count - i - 1));
            s->count--;
            gHandleVersion++;
            break;
        }
    }
    ucxapp_events_unlock();
}

/* The newest handle becomes the current one; the oldest is dropped when the list is full. */
void ucxapp_handle_add(ucxapp_handle_kind_t kind, int32_t id)
{
    handleSet_t *s = &gHandles[kind];

    ucxapp_handle_remove(kind, id);
    ucxapp_events_lock();
    if (s->count == HANDLE_MAX) {
        memmove(&s->ids[0], &s->ids[1], sizeof(s->ids[0]) * (HANDLE_MAX - 1));
        s->count--;
    }
    s->ids[s->count++] = id;
    gHandleVersion++;
    ucxapp_events_unlock();
}

int32_t ucxapp_handle_current(ucxapp_handle_kind_t kind)
{
    handleSet_t *s = &gHandles[kind];

    ucxapp_events_lock();
    int32_t id = s->count > 0 ? s->ids[s->count - 1] : -1;
    ucxapp_events_unlock();
    return id;
}

/* A usage starting with "[handle]" (socket) or "[conn]" (BLE) takes the current one when it is left out. */
static void insertCurrentHandle(const ucxapp_cmd_t *cmd, int *argc, char *argv[])
{
    static char text[16];
    ucxapp_handle_kind_t kind;
    const char *u = cmd->usage;
    int required = 0;
    int total = 0;
    int depth = 0;
    int32_t id;

    if (strncmp(u, "[handle]", 8) == 0) {
        kind = UCXAPP_HANDLE_SOCK;
        u += 8;
    } else if (strncmp(u, "[conn]", 6) == 0) {
        kind = UCXAPP_HANDLE_CONN;
        u += 6;
    } else {
        return;
    }
    for (; *u != '\0'; u++) {
        if (*u == '<' || *u == '[') {
            required += (*u == '<' && depth == 0);
            total += (depth == 0);
            depth++;
        } else if (*u == '>' || *u == ']') {
            depth--;
        }
    }
    if (*argc >= UCXAPP_MAX_ARGS) {
        return;
    }
    /* A leading number is the connection/socket only if it is an open one or the rest would not fit. */
    if (*argc - 1 > required && ucxapp_parse_int(argv[1], &id)) {
        int32_t open[HANDLE_MAX];
        int n = ucxapp_handle_list(kind, open, HANDLE_MAX);
        bool known = false;
        for (int i = 0; i < n; i++) {
            known = known || open[i] == id;
        }
        if (known || *argc - 1 > total) {
            return;
        }
    }
    id = ucxapp_handle_current(kind);
    if (id < 0) {
        return;
    }
    snprintf(text, sizeof(text), "%d", (int)id);
    memmove(&argv[2], &argv[1], (size_t)(*argc - 1) * sizeof(argv[0]));
    argv[1] = text;
    (*argc)++;
}

static bool gAtMode;

void ucxapp_at_mode(bool on)
{
    gAtMode = on;
}

bool ucxapp_at_mode_active(void)
{
    return gAtMode;
}

int ucxapp_cmd_exec(ucxapp_t *app, char *line)
{
    char *argv[UCXAPP_MAX_ARGS];
    int argc;

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line == '\0' || *line == '#') {
        return UCXAPP_OK;
    }
    if (gAtMode) {
        if (strcmp(line, "back") == 0) {
            gAtMode = false;
            ucxapp_print("command mode\n");
            return UCXAPP_OK;
        }
        return app->ucx != NULL ? ucxapp_at_send(app, line) : UCXAPP_ERR_NOT_CONNECTED;
    }
    {
        /* Raw AT ("at <cmd>" or "AT+..." typed directly) goes to the module verbatim, keeping its quotes. */
        char first[32];
        size_t n = strcspn(line, " \t");
        const char *rest = line + n + strspn(line + n, " \t");
        bool atWord = n == 2 && (line[0] == 'a' || line[0] == 'A') && (line[1] == 't' || line[1] == 'T');
        bool atDirect = n > 2 && n < sizeof(first) && (line[0] == 'a' || line[0] == 'A') &&
                        (line[1] == 't' || line[1] == 'T') && memchr(line, '.', n) == NULL;

        if (atDirect) {
            memcpy(first, line, n);
            first[n] = '\0';
            atDirect = findCmd(first) == NULL;
        }
        if ((atWord && *rest != '\0' && strchr(line, '"') != NULL) || (atDirect && strchr(line, '"') != NULL)) {
            if (app->ucx == NULL) {
                ucxapp_print("ERROR: not connected to a module\n");
                return UCXAPP_ERR_NOT_CONNECTED;
            }
            return ucxapp_at_send(app, atWord ? rest : line);
        }
    }
    argc = tokenize(line, argv, UCXAPP_MAX_ARGS);
    if (argc < 0) {
        ucxapp_print("ERROR: too many arguments (max %d)\n", UCXAPP_MAX_ARGS);
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 0) {
        return UCXAPP_OK;
    }

    if (strcmp(argv[0], "help") == 0 && argc > 1 && strcmp(argv[1], "--json") == 0) {
        ucxapp_cmd_print_json(argc > 2 ? argv[2] : NULL);
        return UCXAPP_OK;
    }
    if (strcmp(argv[0], "help") == 0) {
        ucxapp_cmd_print_help(argc > 1 ? argv[1] : NULL);
        return UCXAPP_OK;
    }

    const ucxapp_cmd_t *cmd = findCmd(argv[0]);
    if (cmd == NULL && argc < UCXAPP_MAX_ARGS && strlen(argv[0]) > 2 && strchr(argv[0], '.') == NULL &&
        (argv[0][0] == 'a' || argv[0][0] == 'A') && (argv[0][1] == 't' || argv[0][1] == 'T')) {
        /* "ati9" or "AT+GMR" typed directly is sent as a raw AT command */
        static char atName[] = "at";

        memmove(&argv[1], &argv[0], (size_t)argc * sizeof(argv[0]));
        argv[0] = atName;
        argc++;
        cmd = findCmd("at");
    }
    if (cmd == NULL) {
        ucxapp_print("ERROR: unknown command '%s' (try 'help')\n", argv[0]);
        return UCXAPP_ERR_UNKNOWN_CMD;
    }
    if ((cmd->flags & UCXAPP_CMD_NEEDS_DEVICE) && app->ucx == NULL) {
        ucxapp_print("ERROR: not connected to a module\n");
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if ((cmd->flags & UCXAPP_CMD_NEEDS_DEVICE) && needsWifi(cmd->name) && !moduleHasWifi(app)) {
        ucxapp_print("ERROR: %s needs Wi-Fi; this module has Bluetooth LE only (see device.caps)\n", cmd->name);
        return UCXAPP_ERR_UNSUPPORTED;
    }

    int result;
    char *typed[UCXAPP_MAX_ARGS];
    int typedCount = argc;
    ucxapp_recorder_fn_t rec = gRecorder;

    memcpy(typed, argv, (size_t)argc * sizeof(argv[0]));
    if (rec != NULL) {
        rec(cmd, typedCount, typed, false, 0);
    }
    gModErr = 0;
    insertCurrentHandle(cmd, &argc, argv);
    result = cmd->run(app, argc, argv);
    if (result == UCXAPP_ERR_USAGE) {
        if (strlen(cmd->usage) > 24) {
            char indent[48];

            snprintf(indent, sizeof(indent), "  %s ", cmd->name);
            ucxapp_print("usage: %s\n", cmd->name);
            ucxapp_print_usage_lines(indent, cmd->usage);
        } else {
            ucxapp_print("usage: %s %s\n", cmd->name, cmd->usage);
        }
    }
    if (rec != NULL) {
        rec(cmd, typedCount, typed, true, result);
    }
    return result;
}

void ucxapp_shell_run(ucxapp_t *app)
{
    char line[256];

    ucxapp_print("Command shell. Type 'help' for commands, 'exit' to return.\n");
    for (;;) {
        if (ucxapp_readline("ucxapp> ", line, sizeof(line)) < 0) {
            break;
        }
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            break;
        }
        char expanded[sizeof(line)];
        if (ucxapp_at_mode_active()) {
            ucxapp_cmd_exec(app, line);
        } else if (ucxapp_expand_env(line, expanded, sizeof(expanded))) {
            ucxapp_cmd_exec(app, expanded);
        }
    }
}
