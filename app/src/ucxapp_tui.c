/* Text UI: Commands, Log and Events panes in 80 columns, fed by the same core as the web page. */

#include "ucxapp_tui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ucxapp_platform.h"
#include "ucxapp_logcfg.h"
#include "ucxapp_ui.h"
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

typedef struct {
    bool isGroup;
    bool isQuit;
    int gi;
    int ci;
} entry_t;

static lineBuf_t gLog, gEvents;
static char gGroups[UCXAPP_UI_MAX_GROUPS][UCXAPP_UI_GROUP_LEN];
static int gGroupCount;
static int gExpanded = -1;
static const ucxapp_cmd_t *gGrpCmds[UCXAPP_UI_MAX_GROUP_CMDS];
static int gGrpCmdCount;
static int gSel, gTop;
static bool gListFocus;
static int gWCmd = 18, gWLog = 34, gWEvt = 24;
static unsigned gLogScroll;
static bool gBurstHold;
#define BURST_LINES 30u

static char gIn[IN_LEN];
static int gInLen, gInPos;
static char gHist[HIST_LEN][IN_LEN];
static int gHistCount, gHistPos;

static int entryCount(void)
{
    return gGroupCount + (gExpanded >= 0 ? gGrpCmdCount : 0) + 1;
}

static entry_t entryAt(int i)
{
    entry_t e = { true, false, i, -1 };

    if (i == entryCount() - 1) {
        e.isGroup = false;
        e.isQuit = true;
        e.gi = -1;
        return e;
    }
    if (gExpanded >= 0) {
        if (i > gExpanded && i <= gExpanded + gGrpCmdCount) {
            e.isGroup = false;
            e.gi = gExpanded;
            e.ci = i - gExpanded - 1;
        } else if (i > gExpanded + gGrpCmdCount) {
            e.gi = i - gGrpCmdCount;
        }
    }
    return e;
}

static int gHostStart;   /* index of the first host group; groups are sorted module first */

static int hostHeadingRow(void)
{
    if (gHostStart >= gGroupCount) {
        return -1;
    }
    return 1 + gHostStart + (gExpanded >= 0 && gExpanded < gHostStart ? gGrpCmdCount : 0);
}

/* Display row of an entry; row 0 and hostHeadingRow() are the section headings. */
static int rowOfEntry(int idx)
{
    int host = hostHeadingRow();
    int row = idx + 1;

    return host >= 0 && row >= host ? row + 1 : row;
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

static void expandGroup(int gi)
{
    if (gExpanded == gi) {
        gExpanded = -1;
        gGrpCmdCount = 0;
        gSel = gi;
        return;
    }
    gExpanded = gi;
    gGrpCmdCount = ucxapp_ui_group_cmds(gGroups[gi], gGrpCmds, UCXAPP_UI_MAX_GROUP_CMDS);
    gSel = gi;
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
    setInput(common);
    if (count == 1) {
        inInsert(' ');
    }
}

/* ---- drawing ------------------------------------------------------------------ */

static void drawCommandCell(int row, int bodyRows, int width)
{
    int disp = gTop + row;
    int host = hostHeadingRow();
    int idx = disp - 1 - (host >= 0 && disp > host ? 1 : 0);

    if (row < bodyRows && (disp == 0 || disp == host)) {
        const char *title = disp == 0 ? " Module " : " Host ";

        out("\033[90m" G_H G_H "%s", title);
        outRepeat(G_H, width - 2 - (int)strlen(title));
        out("\033[0m");
        return;
    }
    if (idx >= entryCount() || row >= bodyRows) {
        outCell("", width, 0, false);
        return;
    }
    entry_t e = entryAt(idx);
    char text[64];

    if (e.isQuit) {
        outCell("  exit", width, 97, idx == gSel && gListFocus);
        return;
    }
    if (e.isGroup) {
        const ucxapp_cmd_t *only[2];
        bool single = ucxapp_ui_group_cmds(gGroups[e.gi], only, 2) == 1;

        snprintf(text, sizeof(text), "%s %s", single ? " " : gGroupCount > 0 && e.gi == gExpanded ? G_CLOSE : G_OPEN,
                 gGroups[e.gi]);
        out(idx == gSel && gListFocus ? "\033[7;1m" : "\033[1m");
        outCell(text, width, 0, false);
        out("\033[0m");
    } else {
        snprintf(text, sizeof(text), "  %s", gGrpCmds[e.ci]->name);
        outCell(text, width, 0, idx == gSel && gListFocus);
    }
}

static const ucxapp_cmd_t *hintCommand(void)
{
    if (gListFocus) {
        if (gSel >= 0 && gSel < entryCount()) {
            entry_t e = entryAt(gSel);

            return e.isGroup ? NULL : gGrpCmds[e.ci];
        }
        return NULL;
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
    unsigned logLines = lbCount(&gLog), evLines = lbCount(&gEvents);
    unsigned logFirst = logLines > (unsigned)body ? logLines - (unsigned)body : 0u;
    unsigned evFirst = evLines > (unsigned)body ? evLines - (unsigned)body : 0u;

    if (gLogScroll > logFirst) {
        gLogScroll = logFirst;
    }
    logFirst -= gLogScroll;

    gFrameLen = 0;
    out("\033[?25l\033[1;1H");
    outTitleBorder(G_TL, "Commands", W_CMD);
    outTitleBorder(G_TJ, gLogScroll > 0 ? "Log (scrolled)" : "Log", W_LOG);
    outTitleBorder(G_TJ, "Events", W_EVT);
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
        {
            int col = 0;
            const char *t = r + evFirst < evLines ? lbLine(&gEvents, evFirst + (unsigned)r, &col) : "";

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

        if (ucxapp_at_mode_active() && !busy) {
            outCell("AT terminal: type AT commands (AT prefix optional), Up/Down history, Esc or 'back' returns to commands",
                    W_CMD + W_LOG + W_EVT + 4, 96, false);
        } else if (cmd != NULL) {
            snprintf(text, sizeof(text), "[%s] %s%s%s  -  %s", ucxapp_ui_cmd_runs_on(cmd), cmd->name,
                     cmd->usage[0] != '\0' ? " " : "", cmd->usage, cmd->help);
            outHint2(text, W_CMD + W_LOG + W_EVT + 4, body);
        } else if (gListFocus && gSel >= 0 && gSel < entryCount() && entryAt(gSel).isGroup) {
            const char *g = gGroups[entryAt(gSel).gi];

            snprintf(text, sizeof(text), "[%s] %s  -  %s", ucxapp_ui_runs_on(g), g, ucxapp_ui_group_help(g));
            outCell(text, W_CMD + W_LOG + W_EVT + 4, 0, false);
        } else if (busy) {
            outCell("running...", W_CMD + W_LOG + W_EVT + 4, 33, false);
        } else {
            char state[200];

            ucxapp_ui_state(state, sizeof(state));
            snprintf(text, sizeof(text), "%s   (Tab complete, Esc list/input, Ctrl-C or F10 quit)", state);
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
    entry_t e;

    if (gSel < 0 || gSel >= entryCount()) {
        return false;
    }
    e = entryAt(gSel);
    if (e.isQuit) {
        return true;
    }
    if (e.isGroup) {
        const ucxapp_cmd_t *only[2];

        if (ucxapp_ui_group_cmds(gGroups[e.gi], only, 2) == 1) {
            char text[64];

            snprintf(text, sizeof(text), "%s ", only[0]->name);
            setInput(text);
            if (only[0]->usage[0] == '\0' && !(only[0]->flags & UCXAPP_CMD_DANGER)) {
                submitLine();
            } else {
                gListFocus = false;
            }
            return false;
        }
        expandGroup(e.gi);
        return false;
    }
    char text[64];

    snprintf(text, sizeof(text), "%s ", gGrpCmds[e.ci]->name);
    setInput(text);
    if (gGrpCmds[e.ci]->usage[0] == '\0' && !(gGrpCmds[e.ci]->flags & UCXAPP_CMD_DANGER)) {
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
    if (gListFocus) {
        int count = entryCount();

        switch (key) {
        case UCXAPP_KEY_UP:   if (gSel > 0) { gSel--; } return false;
        case UCXAPP_KEY_DOWN: if (gSel + 1 < count) { gSel++; } return false;
        case UCXAPP_KEY_HOME: gSel = 0; return false;
        case UCXAPP_KEY_END:  gSel = count - 1; return false;
        case UCXAPP_KEY_RIGHT:
        case 13:
        case 10:              return activateEntry();
        case UCXAPP_KEY_LEFT: {
            entry_t e = entryAt(gSel);

            if (!e.isGroup) {
                gSel = e.gi;
            } else if (gExpanded == e.gi) {
                expandGroup(e.gi);
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

        gListFocus = !ucxapp_at_mode_active();
        return quitNow;
    }
    case 27:
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

static const ucxapp_io_t kNullIo = { nullWrite, NULL };

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
    gGrpCmdCount = 0;
    gSel = gTop = 0;
    gListFocus = true;
    gLogScroll = 0;
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

            int r = rowOfEntry(gSel);
            int top = (r - 1 == 0 || r - 1 == hostHeadingRow()) ? r - 1 : r;   /* keep a heading above its first group */

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
