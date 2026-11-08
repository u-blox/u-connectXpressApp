#include "ucxapp_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ucxapp_manual_text.h"

#define MANUAL_LINE   1024
#define MANUAL_COLS   12
#define MANUAL_WIDTH  44

#define gText kManual

const char *ucxapp_manual_text(void)
{
    return kManual;
}

/* Copies the line starting at p into out (no newline); returns the start of the next line or NULL at the end. */
static const char *nextLine(const char *p, char *out, size_t size)
{
    const char *end;
    size_t len;

    if (p == NULL || *p == '\0') {
        return NULL;
    }
    end = strchr(p, '\n');
    len = end != NULL ? (size_t)(end - p) : strlen(p);
    if (len > 0 && p[len - 1] == '\r') {
        len--;
    }
    if (len >= size) {
        len = size - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return end != NULL ? end + 1 : p + strlen(p);
}

static int headingLevel(const char *line)
{
    int level = 0;

    while (line[level] == '#') {
        level++;
    }
    return level > 0 && level <= 6 && line[level] == ' ' ? level : 0;
}

static bool containsNoCase(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    if (n == 0) {
        return true;
    }
    for (; *hay != '\0'; hay++) {
        size_t i = 0;
        while (i < n && hay[i] != '\0' && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

/* Like containsNoCase, but only letters and digits count, so "wifi" finds "Wi-Fi". */
static bool containsLoose(const char *hay, const char *needle)
{
    char h[MANUAL_LINE], n[96];
    size_t w = 0;

    for (; *hay != '\0' && w + 1 < sizeof(h); hay++) {
        if (isalnum((unsigned char)*hay)) {
            h[w++] = *hay;
        }
    }
    h[w] = '\0';
    w = 0;
    for (; *needle != '\0' && w + 1 < sizeof(n); needle++) {
        if (isalnum((unsigned char)*needle)) {
            n[w++] = *needle;
        }
    }
    n[w] = '\0';
    return w > 0 && containsNoCase(h, n);
}

/* Markdown inline markup to plain text: drops ** and `, turns [text](url) into "text". */
static void plainInline(const char *in, char *out, size_t size)
{
    size_t w = 0;

    for (const char *r = in; *r != '\0' && w + 1 < size; r++) {
        if (r[0] == '*' && r[1] == '*') {
            r++;
            continue;
        }
        if (*r == '`') {
            continue;
        }
        if (r[0] == '\\' && r[1] == '|') {
            out[w++] = '|';
            r++;
            continue;
        }
        if (*r == '[') {
            const char *close = strstr(r, "](");
            const char *paren = close != NULL ? strchr(close, ')') : NULL;
            if (close != NULL && paren != NULL && memchr(r, '\n', (size_t)(close - r)) == NULL) {
                for (const char *t = r + 1; t < close && w + 1 < size; t++) {
                    out[w++] = *t;
                }
                r = paren;
                continue;
            }
        }
        out[w++] = *r;
    }
    out[w] = '\0';
}

static bool isTableRow(const char *line)
{
    return line[0] == '|';
}

static bool isTableRule(const char *line)
{
    if (line[0] != '|') {
        return false;
    }
    for (const char *p = line; *p != '\0'; p++) {
        if (strchr("|-: ", *p) == NULL) {
            return false;
        }
    }
    return true;
}

/* The next '|' that is not escaped as "\|", or NULL. */
static char *cellBar(char *p)
{
    for (; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        } else if (*p == '|') {
            return p;
        }
    }
    return NULL;
}

/* Splits "| a | b |" into trimmed, plain cells; returns the count. */
static int tableCells(const char *line, char cells[MANUAL_COLS][MANUAL_LINE / 2])
{
    char raw[MANUAL_LINE];
    int count = 0;
    char *p;

    snprintf(raw, sizeof(raw), "%s", line);
    p = raw[0] == '|' ? raw + 1 : raw;
    while (*p != '\0' && count < MANUAL_COLS) {
        char *bar = cellBar(p);
        char *end;
        if (bar != NULL) {
            *bar = '\0';
        }
        while (*p == ' ') {
            p++;
        }
        end = p + strlen(p);
        while (end > p && end[-1] == ' ') {
            *--end = '\0';
        }
        plainInline(p, cells[count++], MANUAL_LINE / 2);
        if (bar == NULL) {
            break;
        }
        p = bar + 1;
        if (*p == '\0') {
            break;
        }
    }
    return count;
}

/* Prints the table starting at p, columns padded to the widest cell; returns where it ends. */
static const char *printTable(const char *p)
{
    static char cells[MANUAL_COLS][MANUAL_LINE / 2];
    char line[MANUAL_LINE];
    size_t width[MANUAL_COLS] = { 0 };
    const char *q = p;
    const char *next;
    int cols = 0;

    while ((next = nextLine(q, line, sizeof(line))) != NULL && isTableRow(line)) {
        if (!isTableRule(line)) {
            int n = tableCells(line, cells);
            for (int i = 0; i < n; i++) {
                size_t len = strlen(cells[i]);
                if (len > MANUAL_WIDTH) {
                    len = MANUAL_WIDTH;  /* a long cell overflows its column instead of widening every row */
                }
                if (len > width[i]) {
                    width[i] = len;
                }
            }
            if (n > cols) {
                cols = n;
            }
        }
        q = next;
    }
    q = p;
    while ((next = nextLine(q, line, sizeof(line))) != NULL && isTableRow(line)) {
        if (isTableRule(line)) {
            ucxapp_print("  ");
            for (int i = 0; i < cols; i++) {
                for (size_t k = 0; k < width[i]; k++) {
                    ucxapp_print("-");
                }
                ucxapp_print(i + 1 < cols ? "  " : "\n");
            }
        } else {
            int n = tableCells(line, cells);
            ucxapp_print("  ");
            for (int i = 0; i < n; i++) {
                if (i + 1 < n) {
                    ucxapp_print("%-*s  ", (int)width[i], cells[i]);
                } else {
                    ucxapp_print("%s\n", cells[i]);
                }
            }
        }
        q = next;
    }
    return q;
}

/* Prints the markdown from p as plain text until a heading at or above stopLevel (0: to the end). */
static void printSection(const char *p, int stopLevel)
{
    char line[MANUAL_LINE], plain[MANUAL_LINE];
    bool fence = false, first = true;
    const char *next;

    while ((next = nextLine(p, line, sizeof(line))) != NULL) {
        int level = fence ? 0 : headingLevel(line);
        if (strncmp(line, "```", 3) == 0) {
            fence = !fence;
            p = next;
            continue;
        }
        if (!first && level > 0 && stopLevel > 0 && level <= stopLevel) {
            break;
        }
        first = false;
        if (fence) {
            ucxapp_print("    %s\n", line);
        } else if (level > 0) {
            size_t len;
            plainInline(line + level + 1, plain, sizeof(plain));
            len = strlen(plain);
            ucxapp_print("\n%s\n", plain);
            for (size_t i = 0; i < len; i++) {
                ucxapp_print(level <= 2 ? "=" : "-");
            }
            ucxapp_print("\n");
        } else if (isTableRow(line)) {
            p = printTable(p);
            continue;
        } else {
            plainInline(line, plain, sizeof(plain));
            ucxapp_print("%s\n", plain);
        }
        p = next;
    }
}

/* Finds the heading whose text contains topic; returns its line and level, or NULL. */
static const char *findHeading(const char *topic, int *levelOut)
{
    char line[MANUAL_LINE];
    const char *p = gText, *next;
    bool fence = false;

    while ((next = nextLine(p, line, sizeof(line))) != NULL) {
        if (strncmp(line, "```", 3) == 0) {
            fence = !fence;
        } else if (!fence) {
            int level = headingLevel(line);
            if (level > 0 && (containsNoCase(line + level + 1, topic) || containsLoose(line + level + 1, topic))) {
                *levelOut = level;
                return p;
            }
        }
        p = next;
    }
    return NULL;
}

/* The heading of the section whose table has a row for `topic` (first cell), e.g. "wifi.ap" under "Wi-Fi". */
static const char *findCommand(const char *topic, int *levelOut)
{
    char line[MANUAL_LINE], pattern[96];
    const char *p = gText, *next, *heading = NULL;
    int headingLvl = 0;
    bool fence = false;

    snprintf(pattern, sizeof(pattern), "| `%s", topic);
    while ((next = nextLine(p, line, sizeof(line))) != NULL) {
        if (strncmp(line, "```", 3) == 0) {
            fence = !fence;
        } else if (!fence) {
            int level = headingLevel(line);
            if (level > 0) {
                heading = p;
                headingLvl = level;
            } else if (heading != NULL && strncmp(line, pattern, strlen(pattern)) == 0) {
                *levelOut = headingLvl;
                return heading;
            }
        }
        p = next;
    }
    return NULL;
}

static int cmdManual(ucxapp_t *app, int argc, char *argv[])
{
    char line[MANUAL_LINE], plain[MANUAL_LINE];
    const char *p, *next;
    bool fence = false;
    int level = 0;
    (void)app;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 1) {
        ucxapp_print("Sections (manual <topic>, manual all, manual.search <text>):\n");
        for (p = gText; (next = nextLine(p, line, sizeof(line))) != NULL; p = next) {
            if (strncmp(line, "```", 3) == 0) {
                fence = !fence;
            } else if (!fence && (level = headingLevel(line)) >= 2 && level <= 3) {
                plainInline(line + level + 1, plain, sizeof(plain));
                ucxapp_print("%s%s\n", level == 2 ? "  " : "    ", plain);
            }
        }
        return UCXAPP_OK;
    }
    if (strcmp(argv[1], "all") == 0) {
        printSection(gText, 0);
        return UCXAPP_OK;
    }
    p = findHeading(argv[1], &level);
    if (p == NULL) {
        p = findCommand(argv[1], &level);
    }
    if (p == NULL) {
        ucxapp_print("ERROR: no section about '%s'; 'manual' lists them, 'manual.search %s' searches the text\n",
                     argv[1], argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    printSection(p, level);
    return UCXAPP_OK;
}

static int cmdManualSearch(ucxapp_t *app, int argc, char *argv[])
{
    char line[MANUAL_LINE], plain[MANUAL_LINE], heading[MANUAL_LINE] = "";
    char shown[MANUAL_LINE] = "";
    const char *p, *next;
    bool fence = false;
    int hits = 0;
    (void)app;

    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    for (p = gText; (next = nextLine(p, line, sizeof(line))) != NULL; p = next) {
        int level;
        if (strncmp(line, "```", 3) == 0) {
            fence = !fence;
            continue;
        }
        level = fence ? 0 : headingLevel(line);
        if (level > 0) {
            plainInline(line + level + 1, heading, sizeof(heading));
            continue;
        }
        if (!containsNoCase(line, argv[1])) {
            continue;
        }
        if (strcmp(shown, heading) != 0) {
            ucxapp_print("%s:\n", heading[0] != '\0' ? heading : "(top)");
            snprintf(shown, sizeof(shown), "%s", heading);
        }
        plainInline(line, plain, sizeof(plain));
        ucxapp_print("  %s\n", plain);
        hits++;
    }
    if (hits == 0) {
        ucxapp_print("No match for '%s'\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gManualCmds[] = {
    { "manual",        "[<topic>|<command>|all]", "Read the command manual: sections, one section (e.g. manual wifi.ap) or all of it", 0, cmdManual },
    { "manual.search", "<text>",                  "Lines of the command manual that mention a word, grouped by section",              0, cmdManualSearch },
};

void ucxapp_register_manual_cmds(void)
{
    ucxapp_cmd_register(gManualCmds, sizeof(gManualCmds) / sizeof(gManualCmds[0]));
}
