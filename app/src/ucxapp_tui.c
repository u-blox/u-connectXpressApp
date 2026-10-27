/* Text UI: Commands, Log and Events panes in 80 columns, fed by the same core as the web page. */

#include "ucxapp_tui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ucxapp_platform.h"
#include "ucxapp_logcfg.h"
#include "ucxapp_ui.h"
#include "ucxapp_stream.h"
#ifdef APP_VERSION_MAJOR
#include "version.h"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_port.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define W_CMD     gWCmd
#define W_LOG     gWLog
#define W_EVT     gWEvt
#define MIN_ROWS  16
#define MAX_ROWS  45
#define MAX_COLS  180
#define LB_LINES  400
#define LINE_LEN  160
#define IN_LEN    200
#define HIST_LEN  24

/* UTF-8 box drawing, written as octal escapes because the build does not force a source charset */
#define G_H  "\342\224\200"
#define G_V  "\342\224\202"
#define G_TL "\342\224\214"
#define G_TR "\342\224\220"
#define G_BL "\342\224\224"
#define G_BR "\342\224\230"
#define G_TJ "\342\224\254"
#define G_BJ "\342\224\264"
#define G_OPEN  "\342\226\270"
#define G_CLOSE "\342\226\276"

/* ---- wrapped, colored line buffers -------------------------------------------- */

typedef struct {
    char text[LB_LINES][LINE_LEN];
    uint8_t col[LB_LINES];
    unsigned total;
    int width;
    int (*classify)(const char *line);
    char cur[LINE_LEN];
    int curLen, curCols, esc;
    int spaceAt;             /* index+1 of the last space in cur, 0 if none */
    bool cont;               /* the current line continues a wrapped one */
    uint8_t logicalCol;
} lineBuf_t;

static int classifyLog(const char *s)
{
    if (strncmp(s, "> ", 2) == 0) {
        return 36;
    }
    if (strncmp(s, "[exit 0]", 8) == 0) {
        return 32;
    }
    if (strncmp(s, "[exit", 5) == 0 || strncmp(s, "ERROR", 5) == 0 || strncmp(s, "busy", 4) == 0) {
        return 31;
    }
    if (strncmp(s, "WARNING", 7) == 0 || strncmp(s, "warning", 7) == 0) {
        return 33;
    }
    return 0;
}

static int classifyEvent(const char *s)
{
    s += strlen(s) >= 9 ? 9 : 0;   /* skip "HH:MM:SS " */
    if (strncmp(s, "wifi", 4) == 0 || strncmp(s, "net", 3) == 0) {
        return 32;
    }
    if (strncmp(s, "mqtt", 4) == 0) {
        return 35;
    }
    if (strncmp(s, "bt", 2) == 0 || strncmp(s, "gatt", 4) == 0 || strncmp(s, "sps", 3) == 0) {
        return 33;
    }
    if (strncmp(s, "http", 4) == 0 || strncmp(s, "ping", 4) == 0) {
        return 36;
    }
    return 0;
}

static void lbCommit(lineBuf_t *lb, bool wrapped)
{
    unsigned slot = lb->total % LB_LINES;

    lb->cur[lb->curLen] = '\0';
    if (!lb->cont) {
        lb->logicalCol = (uint8_t)lb->classify(lb->cur);
    }
    memcpy(lb->text[slot], lb->cur, (size_t)lb->curLen + 1);
    lb->col[slot] = lb->logicalCol;
    lb->total++;
    lb->curLen = lb->curCols = 0;
    lb->spaceAt = 0;
    lb->cont = wrapped;
}

static void lbPutByte(lineBuf_t *lb, unsigned char c)
{
    if (lb->esc == 1) {
        lb->esc = (c == '[') ? 2 : 0;
        return;
    }
    if (lb->esc == 2) {
        if (c >= 0x40 && c <= 0x7E) {
            lb->esc = 0;
        }
        return;
    }
    if (c == 27) {
        lb->esc = 1;
    } else if (c == '\r') {
        lb->curLen = lb->curCols = 0;
        lb->spaceAt = 0;
    } else if (c == '\n') {
        lbCommit(lb, false);
    } else if (c < 0x20 && c != '\t') {
        /* ignored */
    } else if ((c & 0xC0) == 0x80) {
        if (lb->curLen < LINE_LEN - 1) {
            lb->cur[lb->curLen++] = (char)c;
        }
    } else {
        if (c == '\t') {
            c = ' ';
        }
        if (lb->curCols >= lb->width) {
            if (c == ' ') {
                lbCommit(lb, true);
                return;
            }
            if (lb->spaceAt > 0) {
                char rest[LINE_LEN];
                int restLen = lb->curLen - lb->spaceAt, restCols = 0;

                memcpy(rest, lb->cur + lb->spaceAt, (size_t)restLen);
                lb->curLen = lb->spaceAt - 1;
                lbCommit(lb, true);
                for (int i = 0; i < restLen; i++) {
                    if (((unsigned char)rest[i] & 0xC0) != 0x80) {
                        restCols++;
                    }
                }
                memcpy(lb->cur, rest, (size_t)restLen);
                lb->curLen = restLen;
                lb->curCols = restCols;
            } else {
                lbCommit(lb, true);
            }
        }
        if (lb->curLen < LINE_LEN - 1) {
            if (c == ' ' && lb->curLen > 0 && lb->cur[lb->curLen - 1] != ' ') {
                lb->spaceAt = lb->curLen + 1;
            }
            lb->cur[lb->curLen++] = (char)c;
            lb->curCols++;
        }
    }
}

static void lbPutText(lineBuf_t *lb, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        lbPutByte(lb, (unsigned char)s[i]);
    }
}

static void lbPutLine(lineBuf_t *lb, const char *s)
{
    lbPutText(lb, s, strlen(s));
    lbPutByte(lb, '\n');
}

/* Lines currently available, including the one being written. */
static unsigned lbCount(const lineBuf_t *lb)
{
    unsigned avail = lb->total < LB_LINES ? lb->total : LB_LINES;

    return avail + (lb->curLen > 0 ? 1u : 0u);
}

static const char *lbLine(lineBuf_t *lb, unsigned k, int *col)
{
    unsigned avail = lb->total < LB_LINES ? lb->total : LB_LINES;

    if (k < avail) {
        unsigned slot = (lb->total - avail + k) % LB_LINES;

        *col = lb->col[slot];
        return lb->text[slot];
    }
    lb->cur[lb->curLen] = '\0';
    *col = lb->cont ? lb->logicalCol : lb->classify(lb->cur);
    return lb->cur;
}

/* ---- frame output ------------------------------------------------------------- */

static char gFrame[128 * 1024];
static size_t gFrameLen;

static void out(const char *fmt, ...)
{
    va_list ap;

    if (gFrameLen + 512 >= sizeof(gFrame)) {
        return;
    }
    va_start(ap, fmt);
    int n = vsnprintf(gFrame + gFrameLen, sizeof(gFrame) - gFrameLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        gFrameLen += (size_t)n;
    }
}

static void outRepeat(const char *glyph, int n)
{
    for (int i = 0; i < n; i++) {
        out("%s", glyph);
    }
}

/* Writes text clipped or padded to exactly width columns. */
static void outCell(const char *text, int width, int col, bool reverse)
{
    int cols = 0;

    if (reverse) {
        out("\033[7m");
    }
    if (col != 0) {
        out("\033[%dm", col);
    }
    for (const char *p = text; *p != '\0' && cols < width;) {
        int len = 1;

        while ((p[len] & 0xC0) == 0x80) {
            len++;
        }
        out("%.*s", len, p);
        p += len;
        cols++;
    }
    for (; cols < width; cols++) {
        out(" ");
    }
    if (col != 0 || reverse) {
        out("\033[0m");
    }
}

static void outTitleBorder(const char *left, const char *title, int width)
{
    int used = 1 + (int)strlen(title);

    out("%s" G_H "\033[1m%s\033[0m", left, title);
    outRepeat(G_H, width - used);
}

/* ---- state -------------------------------------------------------------------- */

enum { E_HEAD, E_GROUP, E_SUB, E_CMD, E_QUIT };

#define SUB_LEN      32
#define SUBMENU_MIN  15   /* groups with more commands than this get sub-menus */
#define MAX_ENTRIES  (UCXAPP_UI_MAX_GROUPS + UCXAPP_UI_MAX_GROUP_CMDS + 4)

typedef struct {
    uint8_t kind;
    int8_t gi;
    const ucxapp_cmd_t *cmd;
    char sub[SUB_LEN];        /* E_SUB, and E_CMD inside a sub-menu: "gatt.client" */
    const char *title;        /* E_HEAD */
} entry_t;

static lineBuf_t gLog, gEvents;
static char gGroups[UCXAPP_UI_MAX_GROUPS][UCXAPP_UI_GROUP_LEN];
static int gGroupCount;
static int gExpanded = -1;
static char gOpenSub[SUB_LEN];
static entry_t gEntries[MAX_ENTRIES];
static int gEntryCount;
static int gSel, gTop;   /* indexes into gEntries; gTop is the first row shown */
static bool gListFocus;
static int gWCmd = 18, gWLog = 34, gWEvt = 24;
static unsigned gLogScroll;
static unsigned gEvScroll;

/* A module request waiting for an answer: highlighted hint, input pre-filled, Esc sends gPromptCancel. */
static char gPrompt[96];
static char gPromptCancel[48];
static uint32_t gPromptSeen;

/* Pinned status card at the top of the Events pane; cached state only, never AT calls from this thread. */
#define DASH_ROWS 12
static char gDash[DASH_ROWS][LINE_LEN];
static bool gDashOn[DASH_ROWS];
static int gDashCount;

static void dashRow(int *n, bool on, const char *name, const char *text, bool *changed)
{
    char line[LINE_LEN];

    if (*n >= DASH_ROWS) {
        return;
    }
    snprintf(line, sizeof(line), "%s %-4s %s", name[0] ? (on ? "\xE2\x97\x8F" : "\xE2\x97\x8B") : " ", name, text);
    if (strcmp(line, gDash[*n]) != 0 || gDashOn[*n] != on) {
        snprintf(gDash[*n], LINE_LEN, "%s", line);
        gDashOn[*n] = on;
        *changed = true;
    }
    (*n)++;
}

/* One area: rows split at '\n', and at double spaces when a part is wider than the pane. */
static void dashArea(int *n, bool on, const char *name, const char *text, bool *changed)
{
    int avail = W_EVT - 7 > 10 ? W_EVT - 7 : 10;
    const char *p = text;
    bool first = true;

    do {
        const char *end = strchr(p, '\n');
        int len = end != NULL ? (int)(end - p) : (int)strlen(p);
        char part[LINE_LEN];

        if (len > avail) {
            for (int i = avail; i > 0; i--) {
                if (p[i] == ' ' && p[i - 1] == ' ') {
                    len = i - 1;
                    end = p + i;
                    break;
                }
            }
        }
        snprintf(part, sizeof(part), "%.*s", len, p);
        dashRow(n, on, first ? name : "", part, changed);
        first = false;
        p = end != NULL ? end + 1 : p + len;
        while (*p == ' ') {
            p++;
        }
    } while (*p != '\0');
}

static void dashHandles(char *out, size_t size, ucxapp_handle_kind_t kind)
{
    int32_t ids[8];
    int count = ucxapp_handle_list(kind, ids, 8);
    int32_t cur = ucxapp_handle_current(kind);
    size_t n = 0;

    out[0] = '\0';
    for (int i = 0; i < count && n < size; i++) {
        n += (size_t)snprintf(out + n, size - n, "%s%d%s", i > 0 ? " " : "", (int)ids[i], ids[i] == cur ? "*" : "");
    }
}

static bool refreshDash(void)
{
    char text[LINE_LEN];
    char wifiText[96];
    char cellText[96];
    char ids[48];
    char extra[64];
    bool changed = false;
    int n = 0;
    const char *port = ucxapp_device_port();
    bool mod = port != NULL && port[0] != '\0';
    int wifi = ucxapp_ui_wifi();

    ucxapp_ui_dash_text(wifiText, sizeof(wifiText), cellText, sizeof(cellText));
    dashArea(&n, mod, "MOD", mod ? port : "not connected", &changed);

    dashHandles(ids, sizeof(ids), UCXAPP_HANDLE_SOCK);
    snprintf(text, sizeof(text), "%s%s%s", wifiText[0] ? wifiText : wifi < 0 ? "?" : wifi ? "up" : "not connected",
             ids[0] ? "\nsockets " : "", ids);
    dashArea(&n, wifi > 0, "WIFI", text, &changed);

    dashHandles(ids, sizeof(ids), UCXAPP_HANDLE_CONN);
    (void)ucxapp_gatt_summary(extra, sizeof(extra));
    snprintf(text, sizeof(text), "%s%s%s", ids[0] ? "conn " : "no links", ids, extra[0] ? "\n" : "");
    snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s", extra);
    dashArea(&n, ids[0] != '\0', "BLE", text, &changed);

    port = ucxapp_stream_open_port("gnss");
    bool on = ucxapp_gnss_dash(extra, sizeof(extra));
    snprintf(text, sizeof(text), "%s%s%s", on ? extra : "not open", port != NULL ? "\n" : "", port != NULL ? port : "");
    dashArea(&n, on, "GNSS", text, &changed);

    port = ucxapp_stream_open_port("imu");
    on = ucxapp_imu_dash(extra, sizeof(extra));
    snprintf(text, sizeof(text), "%s%s%s", on ? extra : "not open", port != NULL ? "\n" : "", port != NULL ? port : "");
    dashArea(&n, on, "IMU", text, &changed);

    port = ucxapp_stream_open_port("cell");
    snprintf(text, sizeof(text), "%s%s%s", cellText[0] ? cellText : "not open", port != NULL ? "\n" : "",
             port != NULL ? port : "");
    dashArea(&n, cellText[0] != '\0', "CELL", text, &changed);

    if (n != gDashCount) {
        gDashCount = n;
        changed = true;
    }
    return changed;
}
static bool gBurstHold;
#define BURST_LINES 30u

static char gIn[IN_LEN];
static int gInLen, gInPos;
static char gHist[HIST_LEN][IN_LEN];
static int gHistCount, gHistPos;

static void addEntry(uint8_t kind, int gi, const ucxapp_cmd_t *cmd, const char *sub, const char *title)
{
    if (gEntryCount < MAX_ENTRIES) {
        entry_t *e = &gEntries[gEntryCount++];

        e->kind = kind;
        e->gi = (int8_t)gi;
        e->cmd = cmd;
        snprintf(e->sub, sizeof(e->sub), "%s", sub != NULL ? sub : "");
        e->title = title;
    }
}

/* "gatt.client" for "gatt.client.hrs"; empty when the name has only one dot. */
static void subOf(const char *name, char out[SUB_LEN])
{
    const char *d1 = strchr(name, '.');
    const char *d2 = d1 != NULL ? strchr(d1 + 1, '.') : NULL;

    out[0] = '\0';
    if (d2 != NULL && d2 - name < SUB_LEN) {
        snprintf(out, SUB_LEN, "%.*s", (int)(d2 - name), name);
    }
}

static int gHostStart;   /* index of the first host group; groups are sorted module first */

static void addGroupCommands(int gi)
{
    static const ucxapp_cmd_t *cmds[UCXAPP_UI_MAX_GROUP_CMDS];
    int n = ucxapp_ui_group_cmds(gGroups[gi], cmds, UCXAPP_UI_MAX_GROUP_CMDS);
    char sub[SUB_LEN], other[SUB_LEN];

    /* Sub-menus first (in command order, e.g. client then server), then the loose commands. */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n; i++) {
        int shared = 0, firstAt = i;

        sub[0] = '\0';
        if (n > SUBMENU_MIN) {
            subOf(cmds[i]->name, sub);
        }
        for (int j = 0; j < n && sub[0] != '\0'; j++) {
            subOf(cmds[j]->name, other);
            if (strcmp(sub, other) == 0) {
                firstAt = shared++ == 0 ? j : firstAt;
            }
        }
        if (shared < 3) {
            if (pass == 1) {
                addEntry(E_CMD, gi, cmds[i], NULL, NULL);
            }
            continue;
        }
        if (pass == 1 || firstAt != i) {
            continue;   /* the sub-menu was added with its first command */
        }
        addEntry(E_SUB, gi, NULL, sub, NULL);
        if (strcmp(gOpenSub, sub) != 0) {
            continue;
        }
        for (int j = i; j < n; j++) {
            subOf(cmds[j]->name, other);
            if (strcmp(sub, other) == 0) {
                addEntry(E_CMD, gi, cmds[j], sub, NULL);
            }
        }
    }
}

/* Rebuilds the visible list and keeps the selection on the same item. */
static void rebuildEntries(void)
{
    entry_t keep = gSel >= 0 && gSel < gEntryCount ? gEntries[gSel] : (entry_t){ E_QUIT, -1, NULL, "", NULL };

    gEntryCount = 0;
    addEntry(E_HEAD, -1, NULL, NULL, " Module ");
    for (int gi = 0; gi < gGroupCount; gi++) {
        if (gi == gHostStart) {
            addEntry(E_HEAD, -1, NULL, NULL, " Host ");
        }
        addEntry(E_GROUP, gi, NULL, NULL, NULL);
        if (gi == gExpanded) {
            addGroupCommands(gi);
        }
    }
    addEntry(E_QUIT, -1, NULL, NULL, NULL);
    gSel = 1;
    for (int i = 0; i < gEntryCount; i++) {
        const entry_t *e = &gEntries[i];

        if (e->kind == keep.kind && e->gi == keep.gi && e->cmd == keep.cmd && strcmp(e->sub, keep.sub) == 0) {
            gSel = i;
            break;
        }
    }
}

static void sortGroupsBySide(void)
{
    char tmp[UCXAPP_UI_MAX_GROUPS][UCXAPP_UI_GROUP_LEN];
    int n = 0;

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            gHostStart = n;
        }
        for (int i = 0; i < gGroupCount; i++) {
            if ((strcmp(ucxapp_ui_runs_on(gGroups[i]), "host") == 0) == (pass == 1)) {
                memcpy(tmp[n++], gGroups[i], UCXAPP_UI_GROUP_LEN);
            }
        }
    }
    memcpy(gGroups, tmp, sizeof(tmp[0]) * (size_t)gGroupCount);
}

static void toggleGroup(int gi)
{
    gExpanded = gExpanded == gi ? -1 : gi;
    gOpenSub[0] = '\0';
    rebuildEntries();
}

static void toggleSub(const char *sub)
{
    if (strcmp(gOpenSub, sub) == 0) {
        gOpenSub[0] = '\0';
    } else {
        snprintf(gOpenSub, sizeof(gOpenSub), "%s", sub);
    }
    rebuildEntries();
}

/* Index of the entry that opens e (its sub-menu or group), or -1. */
static int parentOf(int idx)
{
    const entry_t *e = &gEntries[idx];

    for (int i = idx - 1; i >= 0; i--) {
        const entry_t *p = &gEntries[i];

        if (e->kind == E_CMD && e->sub[0] != '\0' ? (p->kind == E_SUB && strcmp(p->sub, e->sub) == 0)
                                                    : (p->kind == E_GROUP && p->gi == e->gi)) {
            return i;
        }
    }
    return -1;
}

static void moveSel(int dir)
{
    int i = gSel + dir;

    while (i >= 0 && i < gEntryCount && gEntries[i].kind == E_HEAD) {
        i += dir;
    }
    if (i >= 0 && i < gEntryCount) {
        gSel = i;
    }
}

static void setInput(const char *text)
{
    snprintf(gIn, sizeof(gIn), "%s", text);
    gInLen = gInPos = (int)strlen(gIn);
}

static void inInsert(int c)
{
    if (gInLen < IN_LEN - 1) {
        memmove(gIn + gInPos + 1, gIn + gInPos, (size_t)(gInLen - gInPos) + 1);
        gIn[gInPos++] = (char)c;
        gInLen++;
    }
}

static void inDelete(int at)
{
    if (at >= 0 && at < gInLen) {
        memmove(gIn + at, gIn + at + 1, (size_t)(gInLen - at));
        gInLen--;
    }
}

/* ---- tab completion ------------------------------------------------------------ */

static void logLocal(const char *text);

static void tabComplete(void)
{
    char common[64];
    int count;

    if (gInLen == 0 || strchr(gIn, ' ') != NULL) {
        return;
    }
    count = ucxapp_cmd_complete(gIn, common, sizeof(common));
    if (count == 0) {
        return;
    }
    if (count > 1 && (int)strlen(common) == gInLen) {
        char list[LINE_LEN];
        size_t n = (size_t)snprintf(list, sizeof(list), "%d matches:", count);

        for (int gi = 0; gi < gGroupCount && n < sizeof(list); gi++) {
            static const ucxapp_cmd_t *cmds[UCXAPP_UI_MAX_GROUP_CMDS];
            int c = ucxapp_ui_group_cmds(gGroups[gi], cmds, UCXAPP_UI_MAX_GROUP_CMDS);

            for (int i = 0; i < c && n < sizeof(list); i++) {
                if (strncmp(cmds[i]->name, gIn, (size_t)gInLen) == 0) {
                    n += (size_t)snprintf(list + n, sizeof(list) - n, " %s", cmds[i]->name);
                }
            }
        }
        logLocal(list);
        return;
    }
    setInput(common);
    if (count == 1) {
        inInsert(' ');
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static void drawCommandCell(int row, int bodyRows, int width)
{
    int idx = gTop + row;

    if (idx >= gEntryCount || row >= bodyRows) {
        outCell("", width, 0, false);
        return;
    }
    const entry_t *e = &gEntries[idx];
    bool sel = idx == gSel && gListFocus;
    char text[64];

    switch (e->kind) {
    case E_HEAD:
        out("\033[90m" G_H G_H "%s", e->title);
        outRepeat(G_H, width - 2 - (int)strlen(e->title));
        out("\033[0m");
        return;
    case E_QUIT:
        outCell("  exit", width, 97, sel);
        return;
    case E_GROUP: {
        const ucxapp_cmd_t *only[2];
        bool single = ucxapp_ui_group_cmds(gGroups[e->gi], only, 2) == 1;

        snprintf(text, sizeof(text), "%s %s", single ? " " : e->gi == gExpanded ? G_CLOSE : G_OPEN, gGroups[e->gi]);
        out(sel ? "\033[7;1m" : "\033[1m");
        outCell(text, width, 0, false);
        out("\033[0m");
        return;
    }
    case E_SUB:
        snprintf(text, sizeof(text), "  %s %s", strcmp(gOpenSub, e->sub) == 0 ? G_CLOSE : G_OPEN,
                 strchr(e->sub, '.') + 1);
        out(sel ? "\033[7;1m" : "\033[1m");
        outCell(text, width, 0, false);
        out("\033[0m");
        return;
    default:
        snprintf(text, sizeof(text), "%s%s", e->sub[0] != '\0' ? "      " : "  ",
                 e->sub[0] != '\0' ? e->cmd->name + strlen(e->sub) + 1 : e->cmd->name);
        outCell(text, width, 0, sel);
        return;
    }
}

static const ucxapp_cmd_t *hintCommand(void)
{
    if (gListFocus) {
        return gSel >= 0 && gSel < gEntryCount && gEntries[gSel].kind == E_CMD ? gEntries[gSel].cmd : NULL;
    }
    char name[48];
    int n = 0;

    while (n < (int)sizeof(name) - 1 && gIn[n] != '\0' && gIn[n] != ' ') {
        name[n] = gIn[n];
        n++;
    }
    name[n] = '\0';
    return n > 0 ? ucxapp_cmd_find(name) : NULL;
}

/* Rows below the panels: bottom border, two hint lines, input. */
#define TUI_FOOTER 5

/* Split text at the last ' ' or '|' that fits width; the second line keeps the rest. */
static void outHint2(const char *text, int width, int body)
{
    int len = (int)strlen(text), cut = len;

    if (len > width) {
        cut = width;
        for (int i = width; i > width / 2; i--) {
            if (text[i] == ' ' || text[i] == '|') {
                cut = i;
                break;
            }
        }
    }
    {
        char first[256];

        snprintf(first, sizeof(first), "%.*s", cut, text);
        outCell(first, width, 0, false);
    }
    out("\033[%d;1H", body + 4);
    outCell(text[cut] == ' ' ? text + cut + 1 : text + cut, width, 0, false);
}

static void drawFrame(int rows, bool busy)
{
    int body = rows - TUI_FOOTER;
    int dashRows = body >= 2 * (gDashCount + 1) ? gDashCount + 1 : 0;
    int evBody = body - dashRows;
    unsigned logLines = lbCount(&gLog), evLines = lbCount(&gEvents);
    unsigned logFirst = logLines > (unsigned)body ? logLines - (unsigned)body : 0u;
    unsigned evFirst = evLines > (unsigned)evBody ? evLines - (unsigned)evBody : 0u;

    if (gLogScroll > logFirst) {
        gLogScroll = logFirst;
    }
    logFirst -= gLogScroll;
    if (gEvScroll > evFirst) {
        gEvScroll = evFirst;
    }
    evFirst -= gEvScroll;

    gFrameLen = 0;
    out("\033[?25l\033[1;1H");
    outTitleBorder(G_TL, "Commands", W_CMD);
    outTitleBorder(G_TJ, gLogScroll > 0 ? "Log (scrolled, PgDn)" : "Log", W_LOG);
    outTitleBorder(G_TJ, gEvScroll > 0 ? "Status / Events (scrolled)" : "Status / Events", W_EVT);
    out(G_TR);

    for (int r = 0; r < body; r++) {
        out("\033[%d;1H" G_V, r + 2);
        drawCommandCell(r, body, W_CMD);
        out(G_V);
        {
            int col = 0;
            const char *t = r + logFirst < logLines ? lbLine(&gLog, logFirst + (unsigned)r, &col) : "";

            outCell(t, W_LOG, col, false);
        }
        out(G_V);
        if (r < dashRows - 1) {
            outCell(gDash[r], W_EVT, gDashOn[r] ? 92 : 90, false);
        } else if (r == dashRows - 1) {
            out("\033[90m");
            outRepeat(G_H, W_EVT);
            out("\033[0m");
        } else {
            int col = 0;
            int er = r - dashRows;
            const char *t = er + evFirst < evLines ? lbLine(&gEvents, evFirst + (unsigned)er, &col) : "";

            outCell(t, W_EVT, col, false);
        }
        out(G_V);
    }

    out("\033[%d;1H" G_BL, body + 2);
    outRepeat(G_H, W_CMD);
    out(G_BJ);
    outRepeat(G_H, W_LOG);
    out(G_BJ);
    outRepeat(G_H, W_EVT);
    out(G_BR);

    /* hint lines */
    out("\033[%d;1H", body + 4);
    outCell("", W_CMD + W_LOG + W_EVT + 4, 0, false);
    out("\033[%d;1H", body + 3);
    {
        char text[384];
        const ucxapp_cmd_t *cmd = hintCommand();

        if (gPrompt[0] != '\0') {
            outCell(gPrompt, W_CMD + W_LOG + W_EVT + 4, 93, true);
        } else if (ucxapp_at_mode_active() && !busy) {
            outCell("AT terminal: type AT commands (AT prefix optional), Up/Down history, Esc or 'back' returns to commands",
                    W_CMD + W_LOG + W_EVT + 4, 96, false);
        } else if (cmd != NULL) {
            snprintf(text, sizeof(text), "[%s] %s%s%s  -  %s", ucxapp_ui_cmd_runs_on(cmd), cmd->name,
                     cmd->usage[0] != '\0' ? " " : "", cmd->usage, cmd->help);
            outHint2(text, W_CMD + W_LOG + W_EVT + 4, body);
        } else if (gListFocus && gSel >= 0 && gSel < gEntryCount && gEntries[gSel].kind == E_GROUP) {
            const char *g = gGroups[gEntries[gSel].gi];

            snprintf(text, sizeof(text), "[%s] %s  -  %s", ucxapp_ui_runs_on(g), g, ucxapp_ui_group_help(g));
            outCell(text, W_CMD + W_LOG + W_EVT + 4, 0, false);
        } else if (gListFocus && gSel >= 0 && gSel < gEntryCount && gEntries[gSel].kind == E_SUB) {
            snprintf(text, sizeof(text), "%s.*  (Enter opens, Left closes)", gEntries[gSel].sub);
            outCell(text, W_CMD + W_LOG + W_EVT + 4, 0, false);
        } else if (busy) {
            outCell("running...", W_CMD + W_LOG + W_EVT + 4, 33, false);
        } else {
            char state[200];

            ucxapp_ui_state(state, sizeof(state));
            snprintf(text, sizeof(text), "%s   (Tab complete, Esc list/input, PgUp/PgDn log, Ctrl+PgUp/PgDn events, F10 quit)", state);
            outCell(text, W_CMD + W_LOG + W_EVT + 4, 90, false);
        }
    }

    /* input line */
    {
        bool atMode = ucxapp_at_mode_active();
        int promptLen = atMode ? 4 : 2;
        int avail = 79 - promptLen, start = gInPos >= avail ? gInPos - avail + 1 : 0;
        char view[IN_LEN];

        out("\033[%d;1H\033[1m%s\033[0m", body + 5, atMode ? "\033[96mAT> " : "> ");
        snprintf(view, sizeof(view), "%s", gIn + start);
        outCell(view, avail, busy ? 90 : 0, false);
        if (!gListFocus) {
            out("\033[%d;%dH\033[?25h", body + 5, promptLen + 1 + gInPos - start);
        }
    }
    fwrite(gFrame, 1, gFrameLen, stdout);
    fflush(stdout);
}

/* ---- actions ------------------------------------------------------------------- */

static void logLocal(const char *text)
{
    lbPutLine(&gLog, text);
}

static bool submitLine(void)
{
    char line[IN_LEN], echo[IN_LEN + 4];
    int start = 0, end = gInLen;

    while (start < end && gIn[start] == ' ') {
        start++;
    }
    while (end > start && gIn[end - 1] == ' ') {
        end--;
    }
    if (end == start) {
        return false;
    }
    snprintf(line, sizeof(line), "%.*s", end - start, gIn + start);
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        return true;
    }
    if (gHistCount == 0 || strcmp(gHist[(gHistCount - 1) % HIST_LEN], line) != 0) {
        snprintf(gHist[gHistCount++ % HIST_LEN], IN_LEN, "%s", line);
    }
    gHistPos = gHistCount;
    gLogScroll = 0;
    if (strcmp(line, "clear") == 0 || strcmp(line, "log.clear") == 0) {
        gLog.total = 0;
        gLog.curLen = gLog.curCols = 0;
        setInput("");
        return false;
    }
    snprintf(echo, sizeof(echo), "%s%s", ucxapp_at_mode_active() ? "AT> " : "> ", line);
    if (ucxapp_ui_submit(line)) {
        logLocal(echo);
        setInput("");
    } else {
        logLocal("busy: a command is still running");
    }
    return false;
}

static void historyMove(int dir)
{
    int count = gHistCount < HIST_LEN ? gHistCount : HIST_LEN;
    int pos = gHistPos + dir;

    if (count == 0 || pos < gHistCount - count) {
        return;
    }
    if (pos >= gHistCount) {
        gHistPos = gHistCount;
        setInput("");
        return;
    }
    gHistPos = pos;
    setInput(gHist[pos % HIST_LEN]);
}

/* Returns true to quit. */
static bool activateEntry(void)
{
    const ucxapp_cmd_t *cmd;
    char text[64];

    if (gSel < 0 || gSel >= gEntryCount) {
        return false;
    }
    const entry_t *e = &gEntries[gSel];

    if (e->kind == E_QUIT) {
        return true;
    }
    if (e->kind == E_SUB) {
        toggleSub(e->sub);
        return false;
    }
    if (e->kind == E_GROUP) {
        const ucxapp_cmd_t *only[2];

        if (ucxapp_ui_group_cmds(gGroups[e->gi], only, 2) != 1) {
            toggleGroup(e->gi);
            return false;
        }
        cmd = only[0];
    } else if (e->kind == E_CMD) {
        cmd = e->cmd;
    } else {
        return false;
    }
    snprintf(text, sizeof(text), "%s ", cmd->name);
    setInput(text);
    if (cmd->usage[0] == '\0' && !(cmd->flags & UCXAPP_CMD_DANGER)) {
        submitLine();   /* no arguments at all: run it right away and stay in the list */
        return false;
    }
    gListFocus = false;
    return false;
}

/* Returns true to quit. */
static bool handleKey(int key, int rows)
{
    int page = (rows - TUI_FOOTER) / 2 > 1 ? (rows - TUI_FOOTER) / 2 : 1;

    if (key == 3 || key == UCXAPP_KEY_F10) {
        return true;
    }
    if (key == UCXAPP_KEY_PGUP) {
        gBurstHold = false;
        gLogScroll += (unsigned)page;
        return false;
    }
    if (key == UCXAPP_KEY_PGDN) {
        gLogScroll = gLogScroll > (unsigned)page ? gLogScroll - (unsigned)page : 0u;
        return false;
    }
    if (key == UCXAPP_KEY_CPGUP) {
        gEvScroll += (unsigned)page;
        return false;
    }
    if (key == UCXAPP_KEY_CPGDN) {
        gEvScroll = gEvScroll > (unsigned)page ? gEvScroll - (unsigned)page : 0u;
        return false;
    }
    if (key == UCXAPP_KEY_WHEELUP || key == UCXAPP_KEY_WHEELDN) {
        if (key == UCXAPP_KEY_WHEELUP) {
            gBurstHold = false;
            gLogScroll += 3u;
        } else {
            gLogScroll = gLogScroll > 3u ? gLogScroll - 3u : 0u;
        }
        return false;
    }
    if (gListFocus) {
        switch (key) {
        case UCXAPP_KEY_UP:   moveSel(-1); return false;
        case UCXAPP_KEY_DOWN: moveSel(1); return false;
        case UCXAPP_KEY_HOME: gSel = 0; moveSel(1); return false;
        case UCXAPP_KEY_END:  gSel = gEntryCount - 1; return false;
        case UCXAPP_KEY_RIGHT:
        case 13:
        case 10:              return activateEntry();
        case UCXAPP_KEY_LEFT: {
            const entry_t *e = &gEntries[gSel];

            if (e->kind == E_SUB && strcmp(gOpenSub, e->sub) == 0) {
                toggleSub(e->sub);
            } else if (e->kind == E_GROUP && gExpanded == e->gi) {
                toggleGroup(e->gi);
            } else {
                int p = parentOf(gSel);

                if (p >= 0) {
                    gSel = p;
                }
            }
            return false;
        }
        case 27:              gListFocus = false; return false;
        default:
            if (key >= 32 && key < 127) {
                gListFocus = false;
                inInsert(key);
            }
            return false;
        }
    }
    switch (key) {
    case 13:
    case 10: {
        bool quitNow = submitLine();

        gPrompt[0] = '\0';
        gListFocus = !ucxapp_at_mode_active();
        return quitNow;
    }
    case 27:
        if (gPrompt[0] != '\0') {
            gPrompt[0] = '\0';
            setInput(gPromptCancel);
            (void)submitLine();
            gListFocus = true;
            return false;
        }
        if (ucxapp_at_mode_active()) {
            setInput("back");
            (void)submitLine();
            gListFocus = true;
            return false;
        }
        gListFocus = true;
        return false;
    case 9:               if (!ucxapp_at_mode_active()) { tabComplete(); } return false;
    case 8:               if (gInPos > 0) { inDelete(--gInPos); } return false;
    case UCXAPP_KEY_DEL:  inDelete(gInPos); return false;
    case UCXAPP_KEY_LEFT: if (gInPos > 0) { gInPos--; } return false;
    case UCXAPP_KEY_RIGHT: if (gInPos < gInLen) { gInPos++; } return false;
    case UCXAPP_KEY_HOME: gInPos = 0; return false;
    case UCXAPP_KEY_END:  gInPos = gInLen; return false;
    case UCXAPP_KEY_UP:   historyMove(-1); return false;
    case UCXAPP_KEY_DOWN: historyMove(1); return false;
    default:
        if (key >= 32 && key < 127) {
            inInsert(key);
        }
        return false;
    }
}

static void nullWrite(const char *text, size_t len)
{
    (void)text;
    (void)len;
}

static const ucxapp_io_t kNullIo = { nullWrite, NULL, NULL, NULL };

/* Moves new log bytes, events and the exit status of finished commands into the panes. */
static bool pumpStreams(uint32_t *logCur, uint32_t *evCur, uint32_t *doneSeen)
{
    char buf[256], line[96];
    bool changed = false;
    bool drained = false;
    unsigned added = 0;

    for (int i = 0; i < 64; i++) {
        size_t n = ucxapp_ui_log_peek(logCur, buf, sizeof(buf));

        if (n == 0) {
            drained = true;
            break;
        }
        unsigned before = lbCount(&gLog);

        lbPutText(&gLog, buf, n);
        *logCur += (uint32_t)n;
        changed = true;
        added += lbCount(&gLog) - before;
        if (gLogScroll > 0) {
            gLogScroll += lbCount(&gLog) - before;
        }
    }
    if (added >= BURST_LINES && gLogScroll == 0) {
        gLogScroll = added;   /* flood: keep the view where it was */
        gBurstHold = true;
    }
    if (added == 0 && gBurstHold) {
        gLogScroll = 0;       /* flood over: back to live */
        gBurstHold = false;
        changed = true;
    }
    for (int i = 0; i < 32 && ucxapp_ui_event_peek(evCur, line, sizeof(line)); i++) {
        char stamped[128];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);

        snprintf(stamped, sizeof(stamped), "%02d:%02d:%02d %s", tm != NULL ? tm->tm_hour : 0,
                 tm != NULL ? tm->tm_min : 0, tm != NULL ? tm->tm_sec : 0, line);
        lbPutLine(&gEvents, stamped);
        (*evCur)++;
        changed = true;
    }
    if (drained) {
        int rc;
        uint32_t seq = ucxapp_ui_done_seq(&rc);

        if (seq != *doneSeen) {
            char text[32];

            snprintf(text, sizeof(text), "[exit %d]", rc);
            logLocal(text);
            *doneSeen = seq;
            changed = true;
        }
    }
    return changed;
}

const char *ucxapp_version_text(void)
{
#ifdef APP_VERSION_STRING
    return "u-connectXpressApp " APP_VERSION_STRING " (built " __DATE__ " " __TIME__ ")";
#else
    return "u-connectXpressApp (built " __DATE__ " " __TIME__ ")";
#endif
}

int ucxapp_tui_run(ucxapp_t *app)
{
    uint32_t logCur, evCur, doneSeen, stateSeen = 0;
    int rc, cols, rows, lastRows = 0, lastCols = 0;
    bool dirty = true, busy = false, quit = false;

    if (!ucxapp_platform_terminal_enter()) {
        ucxapp_print("ERROR: --tui needs an interactive terminal\n");
        return 1;
    }
    if (!ucxapp_ui_acquire(app)) {
        ucxapp_platform_terminal_leave();
        ucxapp_print("ERROR: cannot start the command runner\n");
        return 1;
    }
    memset(&gLog, 0, sizeof(gLog));
    memset(&gEvents, 0, sizeof(gEvents));
    gLog.width = W_LOG;
    gLog.classify = classifyLog;
    gEvents.width = W_EVT;
    gEvents.classify = classifyEvent;
    gGroupCount = ucxapp_ui_groups(gGroups, UCXAPP_UI_MAX_GROUPS);
    sortGroupsBySide();
    gExpanded = -1;
    gOpenSub[0] = '\0';
    gSel = gTop = 0;
    rebuildEntries();
    moveSel(1);
    gListFocus = true;
    gLogScroll = 0;
    gEvScroll = 0;
    gPrompt[0] = '\0';
    {
        char t[8], f[8], c[8];

        gPromptSeen = ucxapp_ui_prompt_get(t, sizeof(t), f, sizeof(f), c, sizeof(c));
    }
    uint32_t dashMs = ucxapp_platform_tick_ms();
    (void)refreshDash();
    gInLen = gInPos = 0;
    gIn[0] = '\0';
    gHistCount = gHistPos = 0;
    logCur = ucxapp_ui_log_start();
    evCur = ucxapp_ui_event_start();
    doneSeen = ucxapp_ui_done_seq(&rc);

    ucxapp_io_set(&kNullIo);   /* keep stdout clean; the log sink still receives everything */
    ucxappLogWarnLock(true);
    fputs("\033[?1049h\033[2J", stdout);
    logLocal(ucxapp_version_text());
    logLocal("u-connectXpressApp text UI: Up/Down + Enter pick a command, or just type; Esc toggles list/input. 'help' lists commands.");

    while (!quit) {
        int key;

        while (!quit && (key = ucxapp_platform_read_key()) >= 0) {
            quit = handleKey(key, lastRows > 0 ? lastRows : MIN_ROWS);
            dirty = true;
        }
        if (pumpStreams(&logCur, &evCur, &doneSeen)) {
            dirty = true;
        }
        {
            char text[sizeof(gPrompt)], fill[48];
            uint32_t seq = ucxapp_ui_prompt_get(text, sizeof(text), fill, sizeof(fill), gPromptCancel,
                                                sizeof(gPromptCancel));

            if (seq != gPromptSeen) {
                gPromptSeen = seq;
                snprintf(gPrompt, sizeof(gPrompt), "%s", text);
                setInput(fill);
                gListFocus = false;
                fputs("\a", stdout);
                dirty = true;
            }
        }
        if (ucxapp_platform_tick_ms() - dashMs >= 1000u) {
            dashMs = ucxapp_platform_tick_ms();
            if (refreshDash()) {
                dirty = true;
            }
        }
        if (ucxapp_at_mode_active() && gListFocus && !ucxapp_ui_busy()) {
            gListFocus = false;   /* the AT terminal lives on the input line */
            dirty = true;
        }
        if (busy != ucxapp_ui_busy()) {
            busy = !busy;
            dirty = true;
        }
        {
            char state[200];
            uint32_t version = ucxapp_ui_state(state, sizeof(state));

            if (version != stateSeen) {
                stateSeen = version;
                dirty = true;
            }
        }
        ucxapp_platform_terminal_size(&cols, &rows);
        if (rows < MIN_ROWS) {
            rows = MIN_ROWS;
        }
        if (rows > MAX_ROWS) {
            rows = MAX_ROWS;
        }
        if (cols != lastCols) {
            int usable = (cols > MAX_COLS ? MAX_COLS : cols) - 5;

            lastCols = cols;
            if (usable > 76) {
                gWCmd = usable * 26 / 100;
                gWEvt = usable * 26 / 100;
                gWLog = usable - gWCmd - gWEvt;
                if (gWLog > LINE_LEN - 10) {
                    gWLog = LINE_LEN - 10;
                }
            } else {
                gWCmd = 18;
                gWLog = 34;
                gWEvt = 24;
            }
            gLog.width = gWLog;
            gEvents.width = gWEvt;
            lastRows = 0;
        }
        if (rows != lastRows) {
            lastRows = rows;
            fputs("\033[2J", stdout);
            dirty = true;
        }
        {
            int body = rows - TUI_FOOTER;

            int r = gSel;
            int top = (r > 0 && gEntries[r - 1].kind == E_HEAD) ? r - 1 : r;   /* keep a heading above its first group */

            if (top < gTop) {
                gTop = top;
            } else if (r >= gTop + body) {
                gTop = r - body + 1;
            }
        }
        if (dirty && !quit) {
            drawFrame(rows, busy);
            dirty = false;
        }
        U_CX_PORT_SLEEP_MS(20);
    }

    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    ucxapp_io_set(&ucxapp_io_stdio);
    ucxappLogWarnLock(false);
    ucxapp_ui_release();
    ucxapp_platform_terminal_leave();
    return 0;
}
