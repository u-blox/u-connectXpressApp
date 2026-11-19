/* Child processes for the PC builds (ucxapp_proc.h). Windows: cmd.exe in a new process group inside a job object, so
 * a stop reaches the whole tree and nothing outlives the app; POSIX: /bin/sh in its own process group. */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_platform.h"
#include "ucxapp_proc.h"

struct ucxapp_proc {
#if defined(_WIN32)
    HANDLE process, job, out, in;
    DWORD pid;
#else
    int pid, out, in;
    struct ucxapp_proc *next;
#endif
    int flags;
    bool done;
    int code;
};

#if defined(__EMSCRIPTEN__)

ucxapp_proc_t *ucxapp_proc_start(const char *cmdline, const char *outFile, int flags)
{
    (void)cmdline;
    (void)outFile;
    (void)flags;
    return NULL;
}

int32_t ucxapp_proc_read(ucxapp_proc_t *p, char *buf, size_t size)
{
    (void)p;
    (void)buf;
    (void)size;
    return -1;
}

bool ucxapp_proc_write(ucxapp_proc_t *p, const void *data, size_t len)
{
    (void)p;
    (void)data;
    (void)len;
    return false;
}

bool ucxapp_proc_running(ucxapp_proc_t *p, int *code)
{
    (void)p;
    if (code != NULL) {
        *code = -1;
    }
    return false;
}

int ucxapp_proc_wait(ucxapp_proc_t *p, uint32_t ms)
{
    (void)p;
    (void)ms;
    return -1;
}

int ucxapp_proc_stop(ucxapp_proc_t *p, uint32_t graceMs)
{
    (void)p;
    (void)graceMs;
    return -1;
}

void ucxapp_proc_release(ucxapp_proc_t *p)
{
    (void)p;
}

bool ucxapp_proc_which(const char *name, char *out, size_t size)
{
    (void)name;
    (void)out;
    (void)size;
    return false;
}

#else

static void interrupt(ucxapp_proc_t *p);
static void kill_tree(ucxapp_proc_t *p);
static void close_all(ucxapp_proc_t *p);

#if defined(_WIN32)

ucxapp_proc_t *ucxapp_proc_start(const char *cmdline, const char *outFile, int flags)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE outRd = NULL, outWr = NULL, inRd = NULL, inWr = NULL, file = INVALID_HANDLE_VALUE, nul;
    HANDLE inherit[4];
    DWORD nInherit = 0;
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi = { 0 };
    SIZE_T attrSize = 0;
    bool detached = (flags & UCXAPP_PROC_DETACHED) != 0;
    bool ok = false;
    size_t n = strlen(cmdline) + 32;
    char *line = (char *)malloc(n);
    ucxapp_proc_t *p = (ucxapp_proc_t *)calloc(1, sizeof(*p));

    if (line == NULL || p == NULL) {
        free(line);
        free(p);
        return NULL;
    }
    snprintf(line, n, "cmd.exe /d /s /c \"%s\"", cmdline);
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                      NULL);
    if (outFile != NULL) {
        file = CreateFileA(outFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (nul == INVALID_HANDLE_VALUE || (outFile != NULL && file == INVALID_HANDLE_VALUE) ||
        (!detached && (!CreatePipe(&outRd, &outWr, &sa, 0) || !SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0))) ||
        ((flags & UCXAPP_PROC_STDIN) != 0 &&
         (!CreatePipe(&inRd, &inWr, &sa, 0) || !SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0)))) {
        goto done;
    }
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRd != NULL ? inRd : nul;
    si.StartupInfo.hStdOutput = file != INVALID_HANDLE_VALUE ? file : (outWr != NULL ? outWr : nul);
    si.StartupInfo.hStdError = outWr != NULL ? outWr : nul;
    /* only these handles go to the child: not the app's sockets, ports or other children's pipes */
    {
        HANDLE all[3] = { si.StartupInfo.hStdInput, si.StartupInfo.hStdOutput, si.StartupInfo.hStdError };
        for (int i = 0; i < 3; i++) {
            bool dup = false;
            for (DWORD k = 0; k < nInherit; k++) {
                dup = dup || inherit[k] == all[i];
            }
            if (!dup) {
                inherit[nInherit++] = all[i];
            }
        }
    }
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    if (si.lpAttributeList == NULL || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) {
        free(si.lpAttributeList);
        goto done;
    }
    if (UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                  nInherit * sizeof(HANDLE), NULL, NULL)) {
        DWORD cf = CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                   (detached ? CREATE_NO_WINDOW : 0);
        /* Wireshark must not die with a job the app itself may be in (some terminals put it in one) */
        ok = detached && CreateProcessA(NULL, line, NULL, NULL, TRUE, cf | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL,
                                        &si.StartupInfo, &pi);
        ok = ok || CreateProcessA(NULL, line, NULL, NULL, TRUE, cf, NULL, NULL, &si.StartupInfo, &pi);
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (ok) {
        if (!detached) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
            ZeroMemory(&li, sizeof(li));
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            p->job = CreateJobObjectA(NULL, NULL);
            if (p->job != NULL && (!SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof(li)) ||
                                   !AssignProcessToJobObject(p->job, pi.hProcess))) {
                CloseHandle(p->job);
                p->job = NULL;
            }
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        p->process = pi.hProcess;
        p->pid = pi.dwProcessId;
        p->out = outRd;
        p->in = inWr;
        p->flags = flags;
        outRd = NULL;
        inWr = NULL;
    }
done:
    if (outRd != NULL) {
        CloseHandle(outRd);
    }
    if (inWr != NULL) {
        CloseHandle(inWr);
    }
    if (outWr != NULL) {
        CloseHandle(outWr);
    }
    if (inRd != NULL) {
        CloseHandle(inRd);
    }
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    free(line);
    if (!ok) {
        free(p);
        return NULL;
    }
    return p;
}

int32_t ucxapp_proc_read(ucxapp_proc_t *p, char *buf, size_t size)
{
    DWORD avail = 0, got = 0;

    if (p == NULL || p->out == NULL || !PeekNamedPipe(p->out, NULL, 0, NULL, &avail, NULL)) {
        return -1;
    }
    if (avail == 0) {
        return 0;
    }
    if (!ReadFile(p->out, buf, (DWORD)(avail < size ? avail : size), &got, NULL)) {
        return -1;
    }
    return (int32_t)got;
}

bool ucxapp_proc_write(ucxapp_proc_t *p, const void *data, size_t len)
{
    const char *d = (const char *)data;

    while (p != NULL && p->in != NULL && len > 0) {
        DWORD put = 0;
        if (!WriteFile(p->in, d, (DWORD)(len > 65536 ? 65536 : len), &put, NULL)) {
            return false;
        }
        d += put;
        len -= put;
    }
    return p != NULL && p->in != NULL;
}

bool ucxapp_proc_running(ucxapp_proc_t *p, int *code)
{
    DWORD c = 0;

    if (p != NULL && !p->done && p->process != NULL && WaitForSingleObject(p->process, 0) == WAIT_OBJECT_0) {
        p->done = true;
        p->code = GetExitCodeProcess(p->process, &c) ? (int)c : -1;
    }
    if (p == NULL || p->done) {
        if (code != NULL) {
            *code = p != NULL ? p->code : -1;
        }
        return false;
    }
    return true;
}

static void interrupt(ucxapp_proc_t *p)
{
    (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, p->pid);
}

static void kill_tree(ucxapp_proc_t *p)
{
    if (p->job != NULL) {
        (void)TerminateJobObject(p->job, 1);
    } else {
        (void)TerminateProcess(p->process, 1);
    }
}

static void close_all(ucxapp_proc_t *p)
{
    HANDLE h[] = { p->in, p->out, p->process, p->job };   /* the job last: closing it ends what is left */

    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
        if (h[i] != NULL) {
            CloseHandle(h[i]);
        }
    }
    free(p);
}

bool ucxapp_proc_which(const char *name, char *out, size_t size)
{
    DWORD n = SearchPathA(NULL, name, ".exe", (DWORD)size, out, NULL);

    return n > 0 && n < size;
}

#else   /* POSIX */

static ucxapp_proc_t *gLive;   /* killed at exit: a capture tool must not outlive the app */

static void unlink_live(ucxapp_proc_t *p)
{
    for (ucxapp_proc_t **q = &gLive; *q != NULL; q = &(*q)->next) {
        if (*q == p) {
            *q = p->next;
            return;
        }
    }
}

static void kill_live(void)
{
    for (ucxapp_proc_t *p = gLive; p != NULL; p = p->next) {
        (void)kill(-p->pid, SIGKILL);
    }
}

ucxapp_proc_t *ucxapp_proc_start(const char *cmdline, const char *outFile, int flags)
{
    static bool atExit;
    bool detached = (flags & UCXAPP_PROC_DETACHED) != 0;
    int outp[2] = { -1, -1 }, inp[2] = { -1, -1 }, fd = -1;
    ucxapp_proc_t *p = (ucxapp_proc_t *)calloc(1, sizeof(*p));
    pid_t pid;

    if (p == NULL) {
        return NULL;
    }
    if ((outFile != NULL && (fd = open(outFile, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) ||
        (!detached && pipe(outp) != 0) || ((flags & UCXAPP_PROC_STDIN) != 0 && pipe(inp) != 0)) {
        goto fail;
    }
    signal(SIGPIPE, SIG_IGN);   /* a closed Wireshark must not end the app */
    pid = fork();
    if (pid < 0) {
        goto fail;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        long maxFd = sysconf(_SC_OPEN_MAX);
        if (detached) {
            (void)setsid();
            if (fork() != 0) {
                _exit(0);
            }
        } else {
            (void)setpgid(0, 0);
        }
        (void)dup2(inp[0] >= 0 ? inp[0] : nul, 0);
        (void)dup2(fd >= 0 ? fd : (outp[1] >= 0 ? outp[1] : nul), 1);
        (void)dup2(outp[1] >= 0 ? outp[1] : nul, 2);
        for (long i = 3; i < (maxFd > 0 && maxFd < 4096 ? maxFd : 4096); i++) {
            (void)close((int)i);
        }
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    if (detached) {
        int st;
        (void)waitpid(pid, &st, 0);   /* the middle process ends at once; the program runs on alone */
        p->done = true;
    } else {
        (void)setpgid(pid, pid);
        p->pid = pid;
        p->next = gLive;
        gLive = p;
        if (!atExit) {
            atExit = true;
            (void)atexit(kill_live);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    if (outp[1] >= 0) {
        close(outp[1]);
        (void)fcntl(outp[0], F_SETFL, fcntl(outp[0], F_GETFL) | O_NONBLOCK);
        (void)fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    }
    if (inp[0] >= 0) {
        close(inp[0]);
        (void)fcntl(inp[1], F_SETFD, FD_CLOEXEC);
    }
    p->out = outp[0];
    p->in = inp[1];
    p->flags = flags;
    return p;
fail:
    for (int i = 0; i < 2; i++) {
        if (outp[i] >= 0) {
            close(outp[i]);
        }
        if (inp[i] >= 0) {
            close(inp[i]);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    free(p);
    return NULL;
}

int32_t ucxapp_proc_read(ucxapp_proc_t *p, char *buf, size_t size)
{
    ssize_t n;

    if (p == NULL || p->out < 0) {
        return -1;
    }
    n = read(p->out, buf, size);
    if (n < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    }
    return n == 0 ? -1 : (int32_t)n;
}

bool ucxapp_proc_write(ucxapp_proc_t *p, const void *data, size_t len)
{
    const char *d = (const char *)data;

    while (p != NULL && p->in >= 0 && len > 0) {
        ssize_t n = write(p->in, d, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        d += n;
        len -= (size_t)n;
    }
    return p != NULL && p->in >= 0;
}

bool ucxapp_proc_running(ucxapp_proc_t *p, int *code)
{
    int st;

    if (p != NULL && !p->done && waitpid(p->pid, &st, WNOHANG) == p->pid) {
        p->done = true;
        p->code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        unlink_live(p);
    }
    if (p == NULL || p->done) {
        if (code != NULL) {
            *code = p != NULL ? p->code : -1;
        }
        return false;
    }
    return true;
}

static void interrupt(ucxapp_proc_t *p)
{
    (void)kill(-p->pid, SIGINT);
}

static void kill_tree(ucxapp_proc_t *p)
{
    (void)kill(-p->pid, SIGKILL);
}

static void close_all(ucxapp_proc_t *p)
{
    if (p->in >= 0) {
        close(p->in);
    }
    if (p->out >= 0) {
        close(p->out);
    }
    if (!p->done && p->pid > 0) {
        (void)kill(-p->pid, SIGKILL);
        (void)waitpid(p->pid, NULL, 0);
    }
    unlink_live(p);
    free(p);
}

bool ucxapp_proc_which(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");

    while (path != NULL && *path != '\0') {
        const char *end = strchr(path, ':');
        size_t len = end != NULL ? (size_t)(end - path) : strlen(path);
        int n = snprintf(out, size, "%.*s/%s", (int)len, len > 0 ? path : ".", name);
        if (n > 0 && (size_t)n < size && access(out, X_OK) == 0) {
            return true;
        }
        path = end != NULL ? end + 1 : NULL;
    }
    return false;
}

#endif

static int finish(ucxapp_proc_t *p, uint32_t ms)
{
    uint32_t t0 = ucxapp_platform_tick_ms();
    int code = -1;

    while (ucxapp_proc_running(p, &code) && ucxapp_platform_tick_ms() - t0 < ms) {
        ucxapp_platform_sleep_ms(10);
    }
    if (ucxapp_proc_running(p, NULL)) {
        kill_tree(p);
        t0 = ucxapp_platform_tick_ms();
        while (ucxapp_proc_running(p, NULL) && ucxapp_platform_tick_ms() - t0 < 2000u) {
            ucxapp_platform_sleep_ms(10);
        }
        code = -1;
    }
    close_all(p);
    return code;
}

int ucxapp_proc_wait(ucxapp_proc_t *p, uint32_t ms)
{
    return p != NULL ? finish(p, ms) : -1;
}

int ucxapp_proc_stop(ucxapp_proc_t *p, uint32_t graceMs)
{
    if (p == NULL) {
        return -1;
    }
    if (ucxapp_proc_running(p, NULL)) {
        interrupt(p);
    }
    return finish(p, graceMs);
}

void ucxapp_proc_release(ucxapp_proc_t *p)
{
    if (p == NULL) {
        return;
    }
    p->done = true;   /* closing our handles (no job for a detached child) leaves it running */
    close_all(p);
}

#endif

bool ucxapp_proc_quote(const char *arg, char *out, size_t size)
{
    size_t j = 0;

    if (size < 3) {
        return false;
    }
#if defined(_WIN32)
    size_t bs = 0;
    out[j++] = '"';
    for (const char *s = arg; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == '"' || c == '%' || c == '!' || j + 2 >= size) {
            return false;
        }
        out[j++] = (char)c;
        bs = c == '\\' ? bs + 1 : 0;
    }
    for (; bs > 0; bs--) {   /* a\ before the closing quote would escape it */
        if (j + 2 >= size) {
            return false;
        }
        out[j++] = '\\';
    }
#else
    out[j++] = '\'';
    for (const char *s = arg; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 && c != '\t') {
            return false;
        }
        if (c == '\'') {
            if (j + 6 >= size) {
                return false;
            }
            memcpy(out + j, "'\\''", 4);
            j += 4;
        } else {
            if (j + 2 >= size) {
                return false;
            }
            out[j++] = (char)c;
        }
    }
#endif
#if defined(_WIN32)
    out[j++] = '"';
#else
    out[j++] = '\'';
#endif
    out[j] = '\0';
    return true;
}
