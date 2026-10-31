/* session.* records the commands of a session (with their output as comments) to a .ucx script and replays it;
 * prod.test runs a check script on the connected module and writes a report named after its serial number.
 * Host builds only. */

#include "ucxapp_cmd.h"
#include "ucxapp_logcfg.h"

#if !defined(UCXAPP_LITE)

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx_general.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define LINE_MAX_LEN 512

static FILE *gRec;
static bool gRecLineStart = true;
static uint32_t gRecLast;

/* ---- secrets ---------------------------------------------------------------------------------------------------- */

/* True when the usage token for argument idx (1-based) names a password, key password or token. */
static bool isSecretArg(const char *usage, int idx)
{
    const char *p = usage;
    int i = 0;

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *s = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (++i == idx) {
            size_t n = (size_t)(p - s);
            char tok[48];
            snprintf(tok, sizeof(tok), "%.*s", (int)(n < sizeof(tok) - 1 ? n : sizeof(tok) - 1), s);
            return strstr(tok, "password") != NULL || strcmp(tok, "[pw]") == 0 || strstr(tok, "token") != NULL;
        }
    }
    return false;
}

/* One command line with secrets replaced by ${UCX_WIFI_PASS} (Wi-Fi) or ${UCX_SECRET}. */
static void formatLine(const ucxapp_cmd_t *cmd, int argc, char *argv[], char *out, size_t size)
{
    size_t n = 0;

    out[0] = '\0';
    for (int i = 0; i < argc && n + 1 < size; i++) {
        const char *a = argv[i];
        if (i > 0 && isSecretArg(cmd->usage, i) && strcmp(a, "-") != 0) {
            a = strncmp(cmd->name, "wifi.", 5) == 0 ? "${UCX_WIFI_PASS}" : "${UCX_SECRET}";
        }
        bool quote = a[0] == '\0' || strpbrk(a, " \t") != NULL;
        n += (size_t)snprintf(out + n, size - n, "%s%s%s%s", i > 0 ? " " : "", quote ? "\"" : "", a, quote ? "\"" : "");
    }
}

/* ---- session.record --------------------------------------------------------------------------------------------- */

static void recSink(const char *text, size_t len)
{
    FILE *f = gRec;

    if (f == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (gRecLineStart) {
            fputs("# ", f);
            gRecLineStart = false;
        }
        if (text[i] != '\r') {
            fputc(text[i], f);
        }
        if (text[i] == '\n') {
            gRecLineStart = true;
        }
    }
}

static void recCommand(const ucxapp_cmd_t *cmd, int argc, char *argv[], bool done, int rc)
{
    char line[LINE_MAX_LEN];
    FILE *f = gRec;

    if (f == NULL || strncmp(cmd->name, "session.", 8) == 0) {
        return;
    }
    if (!gRecLineStart) {
        fputc('\n', f);
        gRecLineStart = true;
    }
    if (done) {
        if (rc != UCXAPP_OK) {
            fprintf(f, "#! %s\n", ucxapp_err_code(rc));
        }
        fflush(f);
        return;
    }
    uint32_t now = ucxapp_platform_tick_ms();
    if (now - gRecLast >= 1000) {
        fprintf(f, "#+ %.1f s\n", (now - gRecLast) / 1000.0);
    }
    gRecLast = now;
    formatLine(cmd, argc, argv, line, sizeof(line));
    fprintf(f, "%s\n", line);
}

static void recStop(void)
{
    FILE *f = gRec;

    ucxapp_cmd_set_recorder(NULL);
    ucxapp_io_remove_sink(recSink);
    gRec = NULL;
    if (f != NULL) {
        fclose(f);
    }
}

static int cmdRecord(ucxapp_t *app, int argc, char *argv[])
{
    char stamp[32];
    time_t t = time(NULL);

    (void)app;
    if (argc != 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (gRec != NULL) {
        ucxapp_print("already recording; session.stop first\n");
        return UCXAPP_ERR_FAILED;
    }
    FILE *f = fopen(argv[1], "w");
    if (f == NULL) {
        ucxapp_print("ERROR: cannot create %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "# u-connectXpressApp session recorded %s\n# replay: session.replay %s (or ucxapp --script)\n"
               "# secrets are written as ${UCX_WIFI_PASS} / ${UCX_SECRET}; '#!' marks a failed command\n",
            stamp, argv[1]);
    gRecLineStart = true;
    gRecLast = ucxapp_platform_tick_ms();
    gRec = f;
    if (!ucxapp_io_add_sink(recSink)) {
        recStop();
        ucxapp_print("ERROR: no free output sink\n");
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_cmd_set_recorder(recCommand);
    ucxapp_print("recording to %s (session.stop to finish; app.log.at.on adds the AT trace)\n", argv[1]);
    return UCXAPP_OK;
}

static int cmdStop(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    if (gRec == NULL) {
        ucxapp_print("not recording\n");
        return UCXAPP_OK;
    }
    recStop();
    ucxapp_print("recording stopped\n");
    return UCXAPP_OK;
}

/* ---- replay ----------------------------------------------------------------------------------------------------- */

/* Runs every command line of a script; danger commands only with all. Returns the number of failures, -1 if unreadable. */
static int runScript(ucxapp_t *app, const char *path, bool all, bool stopOnFail, int *count)
{
    char line[LINE_MAX_LEN];
    char expanded[LINE_MAX_LEN];
    char name[64];
    int failed = 0;
    FILE *f = fopen(path, "r");

    *count = 0;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *p = line + strspn(line, " \t");
        if (*p == '\0' || *p == '#') {
            continue;
        }
        snprintf(name, sizeof(name), "%.*s", (int)strcspn(p, " \t"), p);
        const ucxapp_cmd_t *cmd = ucxapp_cmd_find(name);
        if (cmd != NULL && strncmp(cmd->name, "session.", 8) == 0) {
            continue;
        }
        if (cmd != NULL && (cmd->flags & UCXAPP_CMD_DANGER) && !all) {
            ucxapp_print("> %s\nskipped: changes the module for good (add 'all' to run it)\n", p);
            continue;
        }
        ucxapp_print("> %s\n", p);
        (*count)++;
        if (!ucxapp_expand_env(p, expanded, sizeof(expanded)) || ucxapp_cmd_exec(app, expanded) != UCXAPP_OK) {
            failed++;
            if (stopOnFail) {
                break;
            }
        }
    }
    fclose(f);
    return failed;
}

static int cmdReplay(ucxapp_t *app, int argc, char *argv[])
{
    bool all = false;
    bool stop = false;
    int count;

    if (argc < 2 || argc > 4) {
        return UCXAPP_ERR_USAGE;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0) {
            all = true;
        } else if (strcmp(argv[i], "stop") == 0) {
            stop = true;
        } else {
            return UCXAPP_ERR_USAGE;
        }
    }
    int failed = runScript(app, argv[1], all, stop, &count);
    if (failed < 0) {
        ucxapp_print("ERROR: cannot read %s\n", argv[1]);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("replayed %d command(s), %d failed\n", count, failed);
    return failed == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* ---- prod.test -------------------------------------------------------------------------------------------------- */

static void query(ucxapp_t *app, bool (*begin)(uCxHandle_t *, const char **), char *out, size_t size)
{
    const char *text = NULL;

    out[0] = '\0';
    if (begin(app->ucx, &text) && text != NULL) {
        snprintf(out, size, "%s", text);
    }
    uCxEnd(app->ucx);
}

/* Keeps letters, digits, '-' and '_' so a serial number is a safe file name. */
static void safeName(const char *in, char *out, size_t size)
{
    size_t n = 0;

    for (; *in != '\0' && n + 1 < size; in++) {
        char c = *in;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_') {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static FILE *gReport;

static void reportSink(const char *text, size_t len)
{
    FILE *f = gReport;
    if (f != NULL) {
        fwrite(text, 1, len, f);
    }
}

static int runBuiltIn(ucxapp_t *app, int *count)
{
    static const char *const kChecks[] = { "dev.info", "dev.caps", "dev.diagnose" };
    char line[64];
    int failed = 0;

    *count = 0;
    for (size_t i = 0; i < sizeof(kChecks) / sizeof(kChecks[0]); i++) {
        snprintf(line, sizeof(line), "%s", kChecks[i]);
        ucxapp_print("> %s\n", line);
        (*count)++;
        if (ucxapp_cmd_exec(app, line) != UCXAPP_OK) {
            failed++;
        }
    }
    return failed;
}

/* prod.test [script|-] [dir] */
static int cmdProdTest(ucxapp_t *app, int argc, char *argv[])
{
    const char *script = argc > 1 && strcmp(argv[1], "-") != 0 ? argv[1] : NULL;
    const char *dir = argc > 2 ? argv[2] : "reports";
    char serial[64], model[48], fw[48], safe[64], path[300], stamp[32], fileStamp[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int count = 0;
    int failed;

    if (argc > 3) {
        return UCXAPP_ERR_USAGE;
    }
    query(app, uCxGeneralGetSerialNumberBegin, serial, sizeof(serial));
    query(app, uCxGeneralGetDeviceModelIdentificationBegin, model, sizeof(model));
    query(app, uCxGeneralGetSoftwareVersionBegin, fw, sizeof(fw));
    safeName(serial, safe, sizeof(safe));
    if (safe[0] == '\0') {
        ucxapp_print("ERROR: the module did not report a serial number\n");
        return UCXAPP_ERR_FAILED;
    }
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
    strftime(fileStamp, sizeof(fileStamp), "%Y%m%d-%H%M%S", tm);
    (void)MKDIR(dir);
    snprintf(path, sizeof(path), "%s/%s_%s.txt", dir, safe, fileStamp);
    gReport = fopen(path, "w");
    if (gReport == NULL) {
        ucxapp_print("ERROR: cannot create %s\n", path);
        return UCXAPP_ERR_FAILED;
    }
    fprintf(gReport, "Serial:   %s\nModel:    %s\nFirmware: %s\nDate:     %s\nScript:   %s\n\n", serial, model, fw,
            stamp, script != NULL ? script : "(built-in checks)");
    ucxapp_io_add_sink(reportSink);
    failed = script != NULL ? runScript(app, script, false, false, &count) : runBuiltIn(app, &count);
    ucxapp_io_remove_sink(reportSink);
    if (failed < 0) {
        fclose(gReport);
        gReport = NULL;
        remove(path);
        ucxapp_print("ERROR: cannot read %s\n", script);
        return UCXAPP_ERR_FAILED;
    }
    fprintf(gReport, "\nResult:   %s (%d of %d step(s) failed)\n", failed == 0 ? "PASS" : "FAIL", failed, count);
    fclose(gReport);
    gReport = NULL;

    snprintf(path, sizeof(path), "%s/report.csv", dir);
    FILE *csv = fopen(path, "r");
    bool header = csv == NULL;
    if (csv != NULL) {
        fclose(csv);
    }
    csv = fopen(path, "a");
    if (csv != NULL) {
        if (header) {
            fprintf(csv, "date,serial,model,firmware,script,steps,failed,result\n");
        }
        fprintf(csv, "%s,%s,%s,%s,%s,%d,%d,%s\n", stamp, safe, model, fw, script != NULL ? script : "built-in", count,
                failed, failed == 0 ? "PASS" : "FAIL");
        fclose(csv);
    }
    ucxapp_print("%s: serial %s, %d of %d step(s) failed; report in %s/%s_%s.txt\n", failed == 0 ? "PASS" : "FAIL",
                 safe, failed, count, dir, safe, fileStamp);
    return failed == 0 ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

static int cmdSelftest(ucxapp_t *app, int argc, char *argv[])
{
    static const ucxapp_cmd_t kWifi = { "wifi.connect", "[ssid|n] [password|-]", "", 0, NULL };
    static const ucxapp_cmd_t kCert = { "cert.upload", "<ca|client|key> <file> [name] [pw]", "", 0, NULL };
    char *wifi[] = { "wifi.connect", "My Net", "hunter2" };
    char *dash[] = { "wifi.connect", "home", "-" };
    char *cert[] = { "cert.upload", "key", "k.pem", "k", "pw1" };
    char line[128];
    char safe[16];

    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    formatLine(&kWifi, 3, wifi, line, sizeof(line));
    if (strcmp(line, "wifi.connect \"My Net\" ${UCX_WIFI_PASS}") != 0) {
        ucxapp_print("ERROR: session selftest: Wi-Fi password not masked (%s)\n", line);
        return UCXAPP_ERR_FAILED;
    }
    formatLine(&kWifi, 3, dash, line, sizeof(line));
    if (strcmp(line, "wifi.connect home -") != 0) {
        ucxapp_print("ERROR: session selftest: '-' changed (%s)\n", line);
        return UCXAPP_ERR_FAILED;
    }
    formatLine(&kCert, 5, cert, line, sizeof(line));
    if (strcmp(line, "cert.upload key k.pem k ${UCX_SECRET}") != 0) {
        ucxapp_print("ERROR: session selftest: key password not masked (%s)\n", line);
        return UCXAPP_ERR_FAILED;
    }
    static const char *const kTrace[] = {
        "\33[0;36m[AT TX][0] ", "AT+UWSSW=", "0", ",", "\"hun,ter2\"", ",", "0", "\33[0m\n",
        "AT+UMQCP=0,\"h\",1883,\"c\",\"u\",\"p\\\"w\"\n", "AT+UWAPSW=\"ap1\",2\n", "AT+UWSC=0\n",
    };
    static const char *const kMasked[] = {
        "\33[0;36m[AT TX][0] ", "AT+UWSSW=", "0", ",\"****\"", "", ",", "0", "\33[0m\n",
        "AT+UMQCP=0,\"h\",1883,\"c\",\"u\",\"****\"\n", "AT+UWAPSW=\"****\",2\n", "AT+UWSC=0\n",
    };
    for (size_t i = 0; i < sizeof(kTrace) / sizeof(kTrace[0]); i++) {
        ucxappLogMask(kTrace[i], line, sizeof(line));
        if (strcmp(line, kMasked[i]) != 0) {
            ucxapp_print("ERROR: session selftest: AT trace not masked (%s)\n", line);
            return UCXAPP_ERR_FAILED;
        }
    }
    safeName("35/0A:1b..x", safe, sizeof(safe));
    if (strcmp(safe, "350A1bx") != 0) {
        ucxapp_print("ERROR: session selftest: serial file name (%s)\n", safe);
        return UCXAPP_ERR_FAILED;
    }
    ucxapp_print("session selftest: secret masking (scripts and AT trace) and report names ok\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gSessionCmds[] = {
    { "session.record",   "<file.ucx>",            "Record the commands typed from now on, with their output as comments, to a script for support; passwords are written as ${UCX_WIFI_PASS} / ${UCX_SECRET}", 0, cmdRecord },
    { "session.stop",     "",                      "Stop recording",                                                   0, cmdStop },
    { "session.replay",   "<file.ucx> [all] [stop]", "Run the commands of a recorded session or script; firmware, factory reset and removals only with 'all'; 'stop' ends at the first failure", 0, cmdReplay },
    { "session.selftest", "",                      "Offline check of secret masking (scripts, AT trace) and report file names",           0, cmdSelftest },
    { "prod.test",        "[script.ucx|-] [dir]",  "Production test: run a script (or '-' for dev.info, dev.caps and dev.diagnose) and write <dir>/<serial>_<date>.txt plus a line in <dir>/report.csv (default dir: reports)", UCXAPP_CMD_NEEDS_DEVICE, cmdProdTest },
};

void ucxapp_register_session_cmds(void)
{
    ucxapp_cmd_register(gSessionCmds, sizeof(gSessionCmds) / sizeof(gSessionCmds[0]));
}

void ucxapp_session_end(void)
{
    if (gRec != NULL) {
        recStop();
    }
}

#else /* UCXAPP_LITE */

void ucxapp_register_session_cmds(void)
{
}

void ucxapp_session_end(void)
{
}

#endif
