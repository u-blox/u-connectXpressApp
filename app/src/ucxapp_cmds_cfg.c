/* cfg.* - save the module's stored settings to a file of AT set commands, load them into another module and
 * compare a file with a module or another file (like u-center's configuration file). Passwords cannot be read
 * back from the module, so they are never in a saved file; written by hand they are loaded like any setting. */

#include "ucxapp_cmd.h"

#if !defined(UCXAPP_LITE)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_store.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#ifdef APP_VERSION_MAJOR
#define CFG_APP_VERSION STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR) "." STR(APP_VERSION_PATCH) "." STR(APP_VERSION_BUILD)
#else
#define CFG_APP_VERSION "dev"
#endif

#define CFG_LINE_MAX 400

typedef struct {
    const char *query;   /* read command */
    const char *name;    /* set command name, also the response prefix */
    int keep;            /* parameters written back (0: all) */
    int skipEmpty;       /* parameter that, when empty, means "not set" so no line is written (-1: none) */
    bool indexed;        /* the first parameter is an id: one setting per id */
} cfg_item_t;

/* Settings stored by AT&W that can be read back. Reads the module rejects (other model, older firmware) are
 * skipped. Not here on purpose: ATE, ATS2-ATS5 and USYEE (the AT client needs its own values) and USYUS (would
 * change the link baud rate; saved as a comment only). */
static const cfg_item_t kItems[] = {
    { "AT+UBTM?", "UBTM", 0, -1, false },
    { "AT+UBTLN?", "UBTLN", 0, -1, false },
    { "AT+UBTDFD?", "UBTDFD", 0, -1, true },
    { "AT+UBTADL?", "UBTADL", 0, 0, false },
    { "AT+UBTASD?", "UBTASD", 0, 0, false },
    { "AT+UBTCS0?", "UBTCS0", 0, -1, false },
    { "AT+UBTCS1?", "UBTCS1", 0, -1, false },
    { "AT+UBTCS2?", "UBTCS2", 0, -1, false },
    { "AT+UBTCS3?", "UBTCS3", 0, -1, false },
    { "AT+UBTCS4?", "UBTCS4", 0, -1, false },
    { "AT+UBTCS5?", "UBTCS5", 0, -1, false },
    { "AT+UBTSS2?", "UBTSS2", 0, -1, false },
    { "AT+UBTIOC?", "UBTIOC", 0, -1, false },
    { "AT+UBTBSM?", "UBTBSM", 0, -1, false },
    { "AT+UBTPM?", "UBTPM", 0, -1, false },
    { "AT+UBTCSM?", "UBTCSM", 0, -1, false },
    { "AT+UBTDIS?", "UBTDIS", 0, 1, true },
    { "AT+UMDE?", "UMDE", 0, -1, false },
    { "AT+UMDHN?", "UMDHN", 0, 0, false },
    { "AT+UMQCP=0", "UMQCP", 0, 1, true },
    { "AT+UMQKA=0", "UMQKA", 0, -1, true },
    { "AT+UMQLWT=0", "UMQLWT", 0, 1, true },
    { "AT+UMQTLS=0", "UMQTLS", 0, -1, true },
    { "AT+UNTE?", "UNTE", 0, -1, false },
    { "AT+UNTSC?", "UNTSC", 2, 1, true },
    { "AT+USETE0?", "USETE0", 0, -1, false },
    { "AT+USETE1?", "USETE1", 0, -1, false },
    { "AT+USPS?", "USPS", 0, -1, false },
    { "AT+USPSRM?", "USPSRM", 0, -1, false },
    { "AT+UWHN?", "UWHN", 0, 0, false },
    { "AT+UWSCP=0", "UWSCP", 0, 1, true },
    { "AT+UWRD?", "UWRD", 0, -1, false },
    { "AT+UWCL?", "UWCL", 0, 0, false },
    { "AT+UWAPCP?", "UWAPCP", 0, 0, false },
    { "AT+UWSROE?", "UWSROE", 0, -1, false },
    { "AT+UWSROS0?", "UWSROS0", 0, -1, false },
    { "AT+UWSROS1?", "UWSROS1", 0, -1, false },
    { "AT+UWSROS2?", "UWSROS2", 0, -1, false },
    { "AT+UWSROS3?", "UWSROS3", 0, -1, false },
    { "AT+UWSROS4?", "UWSROS4", 0, -1, false },
    { "AT+UWSROS5?", "UWSROS5", 0, -1, false },
};

/* Write-only settings (passwords, security mode, static IP) cfg.load accepts when written into a file by hand. */
static const char *const kWriteOnly[] = {
    "UWSSW", "UWSSO", "UWSSE", "UWSSP", "UWSIPS", "UWSIPD", "UWAPIPS", "UWAPSW", "UWAPSO",
};

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} cfg_text_t;

static bool textAdd(cfg_text_t *t, const char *s)
{
    size_t n = strlen(s);

    if (t->len + n + 2 > t->cap) {
        size_t cap = t->cap == 0 ? 1024 : t->cap * 2;
        while (cap < t->len + n + 2) {
            cap *= 2;
        }
        char *p = realloc(t->buf, cap);
        if (p == NULL) {
            return false;
        }
        t->buf = p;
        t->cap = cap;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len++] = '\n';
    t->buf[t->len] = '\0';
    return true;
}

/* End of parameter number n (0-based) in s, honouring "quoted, strings". */
static const char *paramEnd(const char *s, int n)
{
    bool quote = false;
    bool esc = false;

    for (; *s != '\0'; s++) {
        if (quote) {
            if (esc) {
                esc = false;
            } else if (*s == '\\') {
                esc = true;
            } else if (*s == '"') {
                quote = false;
            }
        } else if (*s == '"') {
            quote = true;
        } else if (*s == ',') {
            if (n-- == 0) {
                return s;
            }
        }
    }
    return s;
}

static bool paramEmpty(const char *params, int n)
{
    const char *start = params;

    if (n > 0) {
        start = paramEnd(params, n - 1);
        if (*start == '\0') {
            return true;
        }
        start++;
    }
    size_t len = (size_t)(paramEnd(start, 0) - start);
    return len == 0 || (len == 2 && ((start[0] == '"' && start[1] == '"') || (start[0] == '[' && start[1] == ']')));
}

static const cfg_item_t *findItem(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); i++) {
        if (strlen(kItems[i].name) == len && strncmp(kItems[i].name, name, len) == 0) {
            return &kItems[i];
        }
    }
    return NULL;
}

static bool isWriteOnly(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(kWriteOnly) / sizeof(kWriteOnly[0]); i++) {
        if (strlen(kWriteOnly[i]) == len && strncmp(kWriteOnly[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Reads one setting; every response line becomes an "AT+NAME=..." line in out. */
static void readItem(ucxapp_t *app, const cfg_item_t *item, cfg_text_t *out)
{
    uCxAtClient_t *client = app->ucx->pAtClient;
    char prefix[24];
    char set[CFG_LINE_MAX];
    size_t before = out->len;
    char *line;

    snprintf(prefix, sizeof(prefix), "+%s:", item->name);
    uCxAtClientCmdBeginF(client, item->query, "", NULL);
    while ((line = uCxAtClientCmdGetRspParamLine(client, prefix, NULL, NULL)) != NULL) {
        /* Device Information firmware and software revision (2, 3) belong to the module, not the setup. */
        bool own = strcmp(item->name, "UBTDIS") == 0 && (line[0] == '2' || line[0] == '3') && line[1] == ',';
        if (!own && (item->skipEmpty < 0 || !paramEmpty(line, item->skipEmpty))) {
            size_t len = item->keep > 0 ? (size_t)(paramEnd(line, item->keep - 1) - line) : strlen(line);
            snprintf(set, sizeof(set), "AT+%s=%.*s", item->name, (int)len, line);
            textAdd(out, set);
        }
    }
    if (uCxAtClientCmdEnd(client) != 0 && out->buf != NULL) {
        out->len = before;   /* the module rejected the read (other model or firmware) */
        out->buf[before] = '\0';
    }
}

/* First response line of a plain command (AT+GMM, AT+GMR, AT+USYUS?) into out; false on ERROR. */
static bool readText(ucxapp_t *app, const char *cmd, const char *prefix, char *out, size_t size)
{
    uCxAtClient_t *client = app->ucx->pAtClient;
    char *line;

    out[0] = '\0';
    uCxAtClientCmdBeginF(client, cmd, "", NULL);
    while ((line = uCxAtClientCmdGetRspParamLine(client, prefix, NULL, NULL)) != NULL) {
        if (out[0] == '\0') {
            size_t len = strlen(line);
            if (len >= 2 && line[0] == '"' && line[len - 1] == '"') {
                snprintf(out, size, "%.*s", (int)(len - 2), line + 1);
            } else {
                snprintf(out, size, "%s", line);
            }
        }
    }
    return uCxAtClientCmdEnd(client) == 0 && out[0] != '\0';
}

static bool readModule(ucxapp_t *app, cfg_text_t *out)
{
    char model[64];
    char fw[64];
    char baud[64];
    char line[CFG_LINE_MAX];

    if (!readText(app, "AT+GMM", NULL, model, sizeof(model))) {
        snprintf(model, sizeof(model), "unknown");
    }
    if (!readText(app, "AT+GMR", NULL, fw, sizeof(fw))) {
        snprintf(fw, sizeof(fw), "unknown");
    }
    snprintf(line, sizeof(line), "# u-connectXpress settings from %s %s, saved by u-connectXpressApp %s",
             model, fw, CFG_APP_VERSION);
    textAdd(out, line);
    textAdd(out, "# Load with cfg.load <file>. Passwords cannot be read from the module and are not saved; add");
    textAdd(out, "# AT+UWSSW=0,\"<passphrase>\" (or UWSSO, UWSSE, UWAPSW ...) by hand if the file should set them.");
    if (readText(app, "AT+USYUS?", "+USYUS:", baud, sizeof(baud))) {
        snprintf(line, sizeof(line), "# UART settings (not loaded, would change the link): AT+USYUS=%s", baud);
        textAdd(out, line);
    }
    size_t before = out->len;
    for (size_t i = 0; i < sizeof(kItems) / sizeof(kItems[0]); i++) {
        readItem(app, &kItems[i], out);
    }
    return out->len > before;
}

/* Diff key of a set line: "UWSCP,0" for indexed settings, else the command name. False if not a set line. */
static bool lineKey(const char *line, char *key, size_t size, const cfg_item_t **item)
{
    if (strncmp(line, "AT+", 3) != 0) {
        return false;
    }
    const char *name = line + 3;
    size_t len = strcspn(name, "=");
    if (name[len] != '=') {
        return false;
    }
    *item = findItem(name, len);
    if (*item != NULL && (*item)->indexed) {
        const char *params = name + len + 1;
        snprintf(key, size, "%.*s,%.*s", (int)len, name, (int)(paramEnd(params, 0) - params), params);
    } else {
        snprintf(key, size, "%.*s", (int)len, name);
    }
    return true;
}

/* Next line of text (CR/LF stripped, blanks and '#' comments skipped) into out; NULL at the end. */
static const char *nextLine(const char *p, char *out, size_t size)
{
    while (p != NULL && *p != '\0') {
        size_t len = strcspn(p, "\r\n");
        const char *next = p + len;
        while (*next == '\r' || *next == '\n') {
            next++;
        }
        while (len > 0 && isspace((unsigned char)*p)) {
            p++;
            len--;
        }
        while (len > 0 && isspace((unsigned char)p[len - 1])) {
            len--;
        }
        if (len > 0 && *p != '#') {
            snprintf(out, size, "%.*s", (int)len, p);
            return next;
        }
        p = next;
    }
    return NULL;
}

/* The set line for key in text into line; false if text has none. */
static bool findKey(const char *text, const char *key, char *line, size_t size)
{
    const char *p = text;
    char k[64];
    const cfg_item_t *item;

    while ((p = nextLine(p, line, size)) != NULL) {
        if (lineKey(line, k, sizeof(k), &item) && strcmp(k, key) == 0) {
            return true;
        }
    }
    return false;
}

/* Prints the lines that differ; returns how many. */
static int diffTexts(const char *a, const char *b, const char *nameA, const char *nameB)
{
    char line[CFG_LINE_MAX];
    char other[CFG_LINE_MAX];
    char key[64];
    const cfg_item_t *item;
    const char *p = a;
    int diffs = 0;

    while ((p = nextLine(p, line, sizeof(line))) != NULL) {
        if (!lineKey(line, key, sizeof(key), &item)) {
            continue;
        }
        if (!findKey(b, key, other, sizeof(other))) {
            ucxapp_print("- %s   (only in %s)\n", line, nameA);
            diffs++;
        } else if (strcmp(line, other) != 0) {
            ucxapp_print("~ %s: %s\n    %s: %s\n", nameA, line, nameB, other);
            diffs++;
        }
    }
    p = b;
    while ((p = nextLine(p, line, sizeof(line))) != NULL) {
        if (lineKey(line, key, sizeof(key), &item) && !findKey(a, key, other, sizeof(other))) {
            ucxapp_print("+ %s   (only in %s)\n", line, nameB);
            diffs++;
        }
    }
    return diffs;
}

static char *loadFile(const char *name)
{
    size_t len;
    char *text = (char *)ucxapp_store_load(name, &len);

    if (text == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", name);
    }
    return text;
}

static bool needModule(ucxapp_t *app)
{
    return app->ucx != NULL;
}

static int cmdShow(ucxapp_t *app, int argc, char *argv[])
{
    cfg_text_t t = { 0 };

    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needModule(app)) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    readModule(app, &t);
    if (t.buf != NULL) {
        ucxapp_print("%s", t.buf);
    }
    free(t.buf);
    return UCXAPP_OK;
}

static int cmdSave(ucxapp_t *app, int argc, char *argv[])
{
    cfg_text_t t = { 0 };

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (!needModule(app)) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    if (!readModule(app, &t)) {
        free(t.buf);
        ucxapp_print("ERROR: the module returned no settings\n");
        return UCXAPP_ERR_FAILED;
    }
    int settings = 0;
    for (const char *p = t.buf; (p = strstr(p, "\nAT+")) != NULL; p++) {
        settings++;
    }
    bool ok = ucxapp_store_save(argv[1], t.buf, t.len);
    free(t.buf);
    if (!ok) {
        ucxapp_print("ERROR: cannot write %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("%d settings saved to %s (passwords are not readable and not saved)\n", settings, argv[1]);
    return UCXAPP_OK;
}

static int cmdLoad(ucxapp_t *app, int argc, char *argv[])
{
    char line[CFG_LINE_MAX];
    char live[CFG_LINE_MAX];
    int applied = 0;
    int unchanged = 0;
    int failed = 0;
    int skipped = 0;
    cfg_text_t now = { 0 };

    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "nostore") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    bool store = argc == 2;
    if (!needModule(app)) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    char *text = loadFile(argv[1]);
    if (text == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    /* Only changed settings are sent: some (the Wi-Fi station ones) are refused while the link is up. */
    readModule(app, &now);
    uCxAtClient_t *client = app->ucx->pAtClient;
    const char *p = text;
    while ((p = nextLine(p, line, sizeof(line))) != NULL) {
        char key[64];
        const cfg_item_t *item;

        if (strcmp(line, "AT&W") == 0) {
            continue;
        }
        if (!lineKey(line, key, sizeof(key), &item) || (item == NULL && !isWriteOnly(line + 3, strcspn(line + 3, "=")))) {
            ucxapp_print("skipped (not a module setting cfg.load writes): %s\n", line);
            skipped++;
            continue;
        }
        if (now.buf != NULL && findKey(now.buf, key, live, sizeof(live)) && strcmp(live, line) == 0) {
            unchanged++;
            continue;
        }
        uCxAtClientCmdBeginF(client, line, "", NULL);
        int32_t rc = uCxAtClientCmdEnd(client);
        if (rc != 0) {
            ucxapp_print("FAILED %s (%d%s)%s\n", line, (int)rc, ucxapp_errname(rc),
                         rc == -15 ? ": busy, Wi-Fi station settings need wifi.disconnect first" : "");
            failed++;
        } else {
            ucxapp_print("set %s\n", line);
            applied++;
        }
    }
    free(text);
    free(now.buf);
    if (store && applied > 0) {
        uCxAtClientCmdBeginF(client, "AT&W", "", NULL);
        if (uCxAtClientCmdEnd(client) != 0) {
            ucxapp_print("ERROR: AT&W failed, the settings are active but not stored\n");
            return UCXAPP_ERR_FAILED;
        }
    }
    ucxapp_print("%d applied, %d unchanged, %d failed, %d skipped%s\n", applied, unchanged, failed, skipped,
                 applied == 0 ? "" : store ? "; stored (AT&W), some take effect after dev.reboot"
                                           : "; not stored (nostore), lost at reboot");
    return failed > 0 || applied + unchanged == 0 ? UCXAPP_ERR_FAILED : UCXAPP_OK;
}
static int cmdDiff(ucxapp_t *app, int argc, char *argv[])
{
    char *b;
    const char *nameB;

    if (argc != 2 && argc != 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2 && !needModule(app)) {
        return UCXAPP_ERR_NOT_CONNECTED;
    }
    char *a = loadFile(argv[1]);
    if (a == NULL) {
        return UCXAPP_ERR_FAILED;
    }
    if (argc == 3) {
        b = loadFile(argv[2]);
        nameB = argv[2];
        if (b == NULL) {
            free(a);
            return UCXAPP_ERR_FAILED;
        }
    } else {
        cfg_text_t t = { 0 };
        readModule(app, &t);
        b = t.buf != NULL ? t.buf : calloc(1, 1);
        nameB = "module";
    }
    int diffs = b != NULL ? diffTexts(a, b, argv[1], nameB) : -1;
    free(a);
    free(b);
    if (diffs != 0) {
        ucxapp_print("%d difference%s\n", diffs, diffs == 1 ? "" : "s");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("no differences\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gCfgCmds[] = {
    { "cfg.show", "", "Print the module's stored settings (Bluetooth, Wi-Fi, MQTT, NTP, SPS ...) as AT set commands",
      UCXAPP_CMD_NEEDS_DEVICE, cmdShow },
    { "cfg.save", "<file>", "Save the module's settings to a file of AT set commands; passwords cannot be read and "
      "are not saved", UCXAPP_CMD_NEEDS_DEVICE, cmdSave },
    { "cfg.load", "<file> [nostore]", "Write the settings of a cfg.save file that differ from the module and store "
      "them (AT&W) unless 'nostore'; reports applied, unchanged, failed and skipped lines (only known setting "
      "commands are sent)",
      UCXAPP_CMD_NEEDS_DEVICE, cmdLoad },
    { "cfg.diff", "<file> [file2]", "Compare a cfg.save file with the module (or with file2); fails when they differ",
      0, cmdDiff },
};

void ucxapp_register_cfg_cmds(void)
{
    ucxapp_cmd_register(gCfgCmds, sizeof(gCfgCmds) / sizeof(gCfgCmds[0]));
}

#else /* UCXAPP_LITE */

void ucxapp_register_cfg_cmds(void)
{
}

#endif
