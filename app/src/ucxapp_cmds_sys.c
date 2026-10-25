#include "ucxapp_cmd.h"
#include "ucxapp_hash.h"
#include "ucxapp_stats.h"
#include "ucxapp_store.h"
#include "u_port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT_MAX_LEN 256

static bool gLogAt;
static bool gLogWarn = true;
static bool gLogEvents;
static bool gWarnLocked, gWarnSaved;
static ucxapp_store_writer_t *gSaveWriter;

bool ucxappLogWarnEnabled(void)
{
    return gLogWarn;
}

void ucxappLogWarnSet(bool on)
{
    gLogWarn = on;
}

/* The text UI owns the screen: library warnings stay off until it releases the lock. */
void ucxappLogWarnLock(bool locked)
{
    if (locked && !gWarnLocked) {
        gWarnSaved = gLogWarn;
        gLogWarn = false;
    } else if (!locked && gWarnLocked) {
        gLogWarn = gWarnSaved;
    }
    gWarnLocked = locked;
}

bool ucxappLogEventsEnabled(void)
{
    return gLogEvents;
}

bool ucxappLogAtEnabled(void)
{
    return gLogAt;
}

static int cmdLogAtOn(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gLogAt = true;
    ucxapp_print("AT log on\n");
    return UCXAPP_OK;
}

static int cmdLogAtOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gLogAt = false;
    ucxapp_print("AT log off\n");
    return UCXAPP_OK;
}

static int setWarn(bool on)
{
    if (on && gWarnLocked) {
        ucxapp_print("warnings are muted while the text UI runs (they would garble the screen)\n");
        return UCXAPP_ERR_FAILED;
    }
    gLogWarn = on;
    ucxapp_print("warning log %s\n", on ? "on" : "off");
    return UCXAPP_OK;
}

static int cmdLogWarnOn(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    return argc != 1 ? UCXAPP_ERR_USAGE : setWarn(true);
}

static int cmdLogWarnOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    return argc != 1 ? UCXAPP_ERR_USAGE : setWarn(false);
}

static int cmdLogEventsOn(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gLogEvents = true;
    ucxapp_print("event log on\n");
    return UCXAPP_OK;
}

static int cmdLogEventsOff(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gLogEvents = false;
    ucxapp_print("event log off\n");
    return UCXAPP_OK;
}

static int cmdLogLevel(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    bool all = strcmp(argv[1], "all") == 0;
    bool warn = all || strcmp(argv[1], "warn") == 0;

    if (!all && !warn && strcmp(argv[1], "off") != 0) {
        return UCXAPP_ERR_USAGE;
    }
    gLogAt = all;
    gLogEvents = all;
    gLogWarn = warn && !gWarnLocked;
    ucxapp_print("log level %s (warnings %s, AT trace %s, events %s)\n", argv[1], gLogWarn ? "on" : "off",
                 gLogAt ? "on" : "off", gLogEvents ? "on" : "off");
    return UCXAPP_OK;
}

static void saveSink(const char *text, size_t len)
{
    if (gSaveWriter != NULL) {
        ucxapp_store_append(gSaveWriter, text, len);
    }
}

static int cmdLogSave(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gSaveWriter != NULL) {
        ucxapp_print("ERROR: already saving the log (log.stop ends it)\n");
        return UCXAPP_ERR_FAILED;
    }
    gSaveWriter = ucxapp_store_begin(argv[1]);
    if (gSaveWriter == NULL || !ucxapp_io_add_sink(saveSink)) {
        if (gSaveWriter != NULL) {
            ucxapp_store_end(gSaveWriter, false);
            gSaveWriter = NULL;
        }
        ucxapp_print("ERROR: cannot save the log to %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("saving the log to %s (the file appears when you run log.stop)\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdLogStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gSaveWriter == NULL) {
        ucxapp_print("not saving the log\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_io_remove_sink(saveSink);
    bool ok = ucxapp_store_end(gSaveWriter, true);
    gSaveWriter = NULL;
    ucxapp_print(ok ? "log saved\n" : "ERROR: could not write the log file\n");
    return ok ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* The text UI clears its own Log pane before this runs. */
static int cmdLogClear(ucxapp_t *app, int argc, char *argv[])
{
    (void)app; (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("\033[2J\033[H");
    return UCXAPP_OK;
}

/* "ati9" -> "ATI9", "+GMI" -> "AT+GMI": the AT prefix is optional. */
static int cmdAt(ucxapp_t *app, int argc, char *argv[])
{
    char cmd[AT_MAX_LEN];
    size_t len = 0;

    if (argc < 2) {
        ucxapp_at_mode(true);
        ucxapp_print("AT mode: each line is sent to the module as typed; 'back' returns to commands\n");
        return UCXAPP_OK;
    }
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (len + n + 3 >= sizeof(cmd)) {
            ucxapp_print("ERROR: command too long\n");
            return UCXAPP_ERR_FAILED;
        }
        if (i > 1) {
            cmd[len++] = ' ';
        }
        memcpy(&cmd[len], argv[i], n);
        len += n;
    }
    cmd[len] = '\0';
    return ucxapp_at_send(app, cmd);
}

int ucxapp_at_send(ucxapp_t *app, const char *cmd)
{
    size_t len = strlen(cmd);
    char full[AT_MAX_LEN + 3];
    bool hasPrefix = len >= 2 && toupper((unsigned char)cmd[0]) == 'A' &&
                     toupper((unsigned char)cmd[1]) == 'T';

    if (len >= AT_MAX_LEN) {
        ucxapp_print("ERROR: command too long\n");
        return UCXAPP_ERR_FAILED;
    }
    snprintf(full, sizeof(full), "%s%s", hasPrefix ? "" : "AT", cmd);

    /* Print every response line (e.g. the model name for AT+GMM), not just the final status.
     * '+NAME:' lines count as URCs unless expected, so expect the command's own prefix. */
    uCxAtClient_t *client = app->ucx->pAtClient;
    const char *rxStart = (const char *)client->pConfig->pRxBuffer;
    char prefix[32] = "";
    char *line;
    if (full[2] == '+') {
        size_t n = strcspn(&full[2], "=?");
        if (n > 1 && n < sizeof(prefix) - 2) {
            snprintf(prefix, sizeof(prefix), "%.*s:", (int)n, &full[2]);
        }
    }
    uCxAtClientCmdBeginF(client, full, "", NULL);
    while ((line = uCxAtClientCmdGetRspParamLine(client, prefix[0] != '\0' ? prefix : NULL, NULL, NULL)) != NULL) {
        /* A matched '+NAME:' line comes back past the prefix; a plain line starts the RX buffer. */
        ucxapp_print("%s%s\n", line != rxStart ? prefix : "", line);
    }
    int32_t result = uCxAtClientCmdEnd(client);
    if (result != 0) {
        ucxapp_print("ERROR: %s failed (%d%s)\n", full, (int)result, ucxapp_errname(result));
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("OK\n");
    return UCXAPP_OK;
}

static int cmdFileHash(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char hex[UCXAPP_SHA256_HEX_LEN];
    size_t len;

    if (argc != 3 || (strcmp(argv[1], "md5") != 0 && strcmp(argv[1], "sha256") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    uint8_t *data = ucxapp_store_load(argv[2], &len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[2]);
        return UCXAPP_ERR_FAILED;
    }
    if (argv[1][0] == 'm') {
        ucxapp_md5_hex(data, len, hex);
    } else {
        ucxapp_sha256_hex(data, len, hex);
    }
    free(data);
    ucxapp_print("%s  %s (%u bytes)\n", hex, argv[2], (unsigned)len);
    return UCXAPP_OK;
}

static int cmdHashText(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    char hex[UCXAPP_SHA256_HEX_LEN];
    char text[512];
    size_t len = 0;

    if (argc < 3 || (strcmp(argv[1], "md5") != 0 && strcmp(argv[1], "sha256") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        int n = snprintf(&text[len], sizeof(text) - len, "%s%s", i > 2 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(text) - len) {
            ucxapp_print("ERROR: text too long (max %d bytes)\n", (int)sizeof(text) - 1);
            return UCXAPP_ERR_FAILED;
        }
        len += (size_t)n;
    }
    if (argv[1][0] == 'm') {
        ucxapp_md5_hex(text, len, hex);
    } else {
        ucxapp_sha256_hex(text, len, hex);
    }
    ucxapp_print("%s\n", hex);
    return UCXAPP_OK;
}

/* Known-answer vectors from RFC 1321 and FIPS 180-2. */
static int cmdHashSelftest(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    static const struct { bool md5; const char *in; const char *out; } kVec[] = {
        { true,  "",    "d41d8cd98f00b204e9800998ecf8427e" },
        { true,  "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { true,  "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                 "57edf4a22be3c955ac49da2e2107b67a" },
        { false, "",    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { false, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { false, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    char hex[UCXAPP_SHA256_HEX_LEN];
    int fails = 0;

    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    for (size_t i = 0; i < sizeof(kVec) / sizeof(kVec[0]); i++) {
        if (kVec[i].md5) {
            ucxapp_md5_hex(kVec[i].in, strlen(kVec[i].in), hex);
        } else {
            ucxapp_sha256_hex(kVec[i].in, strlen(kVec[i].in), hex);
        }
        if (strcmp(hex, kVec[i].out) != 0) {
            ucxapp_print("FAIL %s vector %u\n", kVec[i].md5 ? "md5" : "sha256", (unsigned)i);
            fails++;
        }
    }
    if (fails != 0) {
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("hash selftest: md5 and sha256 ok\n");
    return UCXAPP_OK;
}

#define VIEW_DEFAULT_LINES 40
#define VIEW_LINE_MAX      120

static int cmdFileView(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    size_t len;
    long maxLines = VIEW_DEFAULT_LINES;

    if (argc < 2 || argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 3) {
        char *end = NULL;
        maxLines = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || maxLines <= 0) {
            return UCXAPP_ERR_USAGE;
        }
    }
    uint8_t *data = ucxapp_store_load(argv[1], &len);
    if (data == NULL) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }

    long lineNo = 0;
    size_t pos = 0;
    while (pos < len && lineNo < maxLines) {
        char line[VIEW_LINE_MAX + 1];
        size_t n = 0;
        while (pos < len && data[pos] != '\n') {
            if (n < VIEW_LINE_MAX) {
                uint8_t c = data[pos];
                line[n++] = (c >= 0x20 && c < 0x7f) || c == '\t' || c == '\r' ? (char)c : '.';
            }
            pos++;
        }
        line[n] = '\0';
        pos++;
        ucxapp_print("%4ld  %s\n", ++lineNo, line);
    }
    if (pos < len) {
        ucxapp_print("... (%ld lines shown, file has more; pass a line count to see more)\n", lineNo);
    }
    free(data);
    return UCXAPP_OK;
}

#ifndef LISTED_MAX
#define LISTED_MAX      64
#endif
#ifndef LISTED_NAME_LEN
#define LISTED_NAME_LEN 128
#endif

/* Names shown by the last file.list, so file.remove can take a number. */
static char gListed[LISTED_MAX][LISTED_NAME_LEN];
static int gListedCount;

static void listOne(const char *name, uint64_t size, void *ctx)
{
    (void)ctx;
    if (gListedCount >= LISTED_MAX || strlen(name) >= LISTED_NAME_LEN) {
        return;
    }
    snprintf(gListed[gListedCount], LISTED_NAME_LEN, "%s", name);
    gListedCount++;
    ucxapp_print("%3d  %10lu  %s\n", gListedCount, (unsigned long)size, name);
}

static int cmdFileList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    gListedCount = 0;
    int n = ucxapp_store_list(listOne, NULL);
    if (n < 0) {
        ucxapp_print("ERROR: cannot list files\n");
        return UCXAPP_ERR_FAILED;
    }
    if (n > gListedCount) {
        ucxapp_print("(%d more not shown)\n", n - gListedCount);
    }
    ucxapp_print("%d file(s)\n", n);
    return UCXAPP_OK;
}

static int cmdFileRemove(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    const char *name = argc == 2 ? argv[1] : NULL;
    char *end;

    if (name == NULL) {
        return UCXAPP_ERR_USAGE;
    }
    long index = strtol(name, &end, 10);
    if (*end == '\0') {
        if (index < 1 || index > gListedCount) {
            ucxapp_print("ERROR: no file %ld; run file.list first\n", index);
            return UCXAPP_ERR_FAILED;
        }
        name = gListed[index - 1];
    }
    if (strpbrk(name, "/\\:") != NULL || strcmp(name, "..") == 0 || strcmp(name, ".") == 0) {
        ucxapp_print("ERROR: give a plain file name from file.list\n");
        return UCXAPP_ERR_FAILED;
    }
    if (!ucxapp_store_remove(name)) {
        ucxapp_print("ERROR: cannot remove %s\n", name);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("Removed %s\n", name);
    gListedCount = 0;
    return UCXAPP_OK;
}

static const char *const kStatsNames[UCXAPP_STATS_COUNT] = { "tcp", "udp", "sps", "gatt", "http", "mqtt", "web" };
static uint64_t gStatsTx[UCXAPP_STATS_COUNT];
static uint64_t gStatsRx[UCXAPP_STATS_COUNT];
static U_CX_MUTEX_HANDLE gStatsLock;

void ucxapp_stats_add(ucxapp_stats_ch_t ch, size_t tx, size_t rx)
{
    U_CX_MUTEX_LOCK(gStatsLock);
    gStatsTx[ch] += tx;
    gStatsRx[ch] += rx;
    U_CX_MUTEX_UNLOCK(gStatsLock);
}

/* printf on the STM32 newlib-nano has no %llu. */
static const char *u64Text(uint64_t v, char *buf, size_t size)
{
    char *p = buf + size - 1;
    *p = '\0';
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0 && p > buf);
    return p;
}

static int statsChannel(const char *name)
{
    for (int i = 0; i < UCXAPP_STATS_COUNT; i++) {
        if (strcmp(name, kStatsNames[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int cmdStatsShow(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    int ch = argc > 1 ? statsChannel(argv[1]) : -1;
    char a[24];
    char b[24];

    if (argc > 3 || (argc > 1 && ch < 0) || (argc == 3 && strcmp(argv[2], "rx") != 0 && strcmp(argv[2], "tx") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    U_CX_MUTEX_LOCK(gStatsLock);
    if (argc == 3) {
        ucxapp_print("%s\n", u64Text(argv[2][0] == 'r' ? gStatsRx[ch] : gStatsTx[ch], a, sizeof(a)));
    } else {
        ucxapp_print("%-5s %14s %14s\n", "", "tx", "rx");
        for (int i = 0; i < UCXAPP_STATS_COUNT; i++) {
            if (ch < 0 || ch == i) {
                ucxapp_print("%-5s %14s %14s\n", kStatsNames[i], u64Text(gStatsTx[i], a, sizeof(a)),
                             u64Text(gStatsRx[i], b, sizeof(b)));
            }
        }
    }
    U_CX_MUTEX_UNLOCK(gStatsLock);
    return UCXAPP_OK;
}

static int cmdStatsClear(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    int ch = argc == 2 ? statsChannel(argv[1]) : -1;

    if (argc > 2 || (argc == 2 && ch < 0)) {
        return UCXAPP_ERR_USAGE;
    }
    U_CX_MUTEX_LOCK(gStatsLock);
    for (int i = 0; i < UCXAPP_STATS_COUNT; i++) {
        if (ch < 0 || ch == i) {
            gStatsTx[i] = 0;
            gStatsRx[i] = 0;
        }
    }
    U_CX_MUTEX_UNLOCK(gStatsLock);
    ucxapp_print("OK\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gSysCmds[] = {
    { "at", "[command]", "Send a raw AT command (AT prefix optional); with no command, enter AT mode where every line is sent as typed", UCXAPP_CMD_NEEDS_DEVICE, cmdAt },
    { "log.at.on", "", "Show the AT TX/RX trace", 0, cmdLogAtOn },
    { "log.at.off", "", "Hide the AT TX/RX trace (default)", 0, cmdLogAtOff },
    { "log.warn.on", "", "Show library warnings (not in the text UI)", 0, cmdLogWarnOn },
    { "log.warn.off", "", "Hide library warnings", 0, cmdLogWarnOff },
    { "log.events.on", "", "Also print module events in the log", 0, cmdLogEventsOn },
    { "log.events.off", "", "Keep module events out of the log (default)", 0, cmdLogEventsOff },
    { "log.level", "<off|warn|all>", "off: nothing extra; warn: warnings; all: warnings, AT trace and events", 0, cmdLogLevel },
    { "log.save", "<file>", "Save everything the log shows to a file (written when log.stop runs)", 0, cmdLogSave },
    { "log.stop", "", "Finish and write the log file started by log.save", 0, cmdLogStop },
    { "log.clear", "", "Clear the log", 0, cmdLogClear },
    { "file.hash", "<md5|sha256> <file>", "Print the hash of a file", 0, cmdFileHash },
    { "utils.hash.text", "<md5|sha256> <text...>", "Print the hash of a text", 0, cmdHashText },
    { "utils.hash.selftest", "", "Offline check of md5 and sha256 against RFC 1321 / FIPS 180-2 vectors", 0, cmdHashSelftest },
    { "file.view", "<file> [lines]", "Show the first lines of a text file (read only, default 40 lines)", 0, cmdFileView },
    { "file.list", "", "List the files in the working folder with numbers and sizes", 0, cmdFileList },
    { "file.remove", "<file|number>", "Delete a file, by name or by its number from file.list", UCXAPP_CMD_DANGER, cmdFileRemove },
    { "stats.show", "[tcp|udp|sps|gatt|http|mqtt|web] [rx|tx]", "Payload bytes sent and received per channel; with rx or tx, print just that number", 0, cmdStatsShow },
    { "stats.clear", "[tcp|udp|sps|gatt|http|mqtt|web]", "Reset the counters of one channel, or all", 0, cmdStatsClear },
};

void ucxapp_register_sys_cmds(void)
{
    U_CX_MUTEX_CREATE(gStatsLock);
    ucxapp_cmd_register(gSysCmds, sizeof(gSysCmds) / sizeof(gSysCmds[0]));
}
