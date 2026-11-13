#include "ucxapp_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#else
#define MKDIR(p) ((void)(p), 0)   /* bare-metal: no directories */
#endif

#define NAME_MAX_LEN 256
#define TMP_SUFFIX   ".part"

static char gDir[NAME_MAX_LEN];

static bool isAbsolute(const char *name)
{
    return name[0] == '/' || name[0] == '\\' || (name[0] != '\0' && name[1] == ':');
}

static bool fullPath(const char *name, char *out, size_t outSize)
{
    int n = snprintf(out, outSize, "%s%s", isAbsolute(name) ? "" : gDir, name);
    return n > 0 && (size_t)n < outSize;
}

static void *stdioOpen(const char *name, bool forWrite)
{
    char path[NAME_MAX_LEN * 2];
    if (!fullPath(name, path, sizeof(path))) {
        return NULL;
    }
    FILE *f;
#if defined(__unix__) || defined(__APPLE__)
    if (forWrite) {
        /* owner-only from the first byte: keys, passwords and fixes never sit world-readable, even for a moment */
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        f = fd >= 0 ? fdopen(fd, "wb") : NULL;
        if (fd >= 0 && f == NULL) {
            close(fd);
        }
        return f;
    }
#endif
    f = fopen(path, forWrite ? "wb" : "rb");
    /* Files given by the user (firmware images, certificates) may still live in the working directory. */
    if (f == NULL && !forWrite && gDir[0] != '\0' && !isAbsolute(name)) {
        f = fopen(name, "rb");
    }
    return f;
}

static int32_t stdioRead(void *h, uint8_t *buf, size_t len)
{
    size_t n = fread(buf, 1, len, (FILE *)h);
    return (n == 0 && ferror((FILE *)h)) ? -1 : (int32_t)n;
}

static int32_t stdioWrite(void *h, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)h) == len ? (int32_t)len : -1;
}

static void stdioClose(void *h)
{
    fclose((FILE *)h);
}

static bool stdioRemove(const char *name)
{
    char path[NAME_MAX_LEN * 2];
    return fullPath(name, path, sizeof(path)) && remove(path) == 0;
}

static bool stdioRename(const char *from, const char *to)
{
    char a[NAME_MAX_LEN * 2];
    char b[NAME_MAX_LEN * 2];
    if (!fullPath(from, a, sizeof(a)) || !fullPath(to, b, sizeof(b))) {
        return false;
    }
#ifdef _WIN32
    /* replaces in one step: a crash leaves the old file or the new one, never none */
    return MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__unix__) || defined(__APPLE__)
    return rename(a, b) == 0;
#else
    remove(b);
    return rename(a, b) == 0;
#endif
}

static int stdioList(ucxapp_store_list_cb_t cb, void *ctx)
{
    return ucxapp_platform_list_files(gDir[0] != '\0' ? gDir : ".", cb, ctx);
}

static const ucxapp_store_ops_t gStdioOps = {
    stdioOpen, stdioRead, stdioWrite, stdioClose, stdioRemove, stdioRename, stdioList
};
static const ucxapp_store_ops_t *gOps = &gStdioOps;
static const ucxapp_store_secure_t *gSecure;

struct ucxapp_store_writer {
    void *handle;          /* NULL for a secret: it is kept in memory and only ever written sealed */
    uint8_t *mem;
    size_t memLen;
    size_t memCap;
    char name[NAME_MAX_LEN];
    char tmp[NAME_MAX_LEN + sizeof(TMP_SUFFIX)];
    bool failed;
};

#define SECRET_MAX (256u * 1024u)   /* certificates and keys; more is not a key file */

void ucxapp_store_set_backend(const ucxapp_store_ops_t *ops)
{
    gOps = ops != NULL ? ops : &gStdioOps;
}

void ucxapp_store_set_secure(const ucxapp_store_secure_t *secure)
{
    gSecure = secure;
}

static bool isSecret(const char *name, bool *mayBeSealed)
{
#ifdef UCXAPP_LITE   /* no secure store on the 128 KB boards: let the compiler drop the sealing paths */
    (void)name;
    if (mayBeSealed != NULL) {
        *mayBeSealed = false;
    }
    return false;
#else
    bool sealed = false;
    bool secret = gSecure != NULL && gSecure->secret(name, &sealed);
    if (mayBeSealed != NULL) {
        *mayBeSealed = gSecure != NULL && sealed;
    }
    return secret;
#endif
}

void ucxapp_store_set_dir(const char *dir)
{
    snprintf(gDir, sizeof(gDir), "%s", dir != NULL ? dir : "");
    if (gDir[0] != '\0') {
        (void)MKDIR(gDir);
    }
}

void ucxapp_store_make_private(const char *name)
{
#if defined(__unix__) || defined(__APPLE__)
    char path[NAME_MAX_LEN * 2];
    if (gOps == &gStdioOps && fullPath(name, path, sizeof(path))) {
        (void)chmod(path, 0600);
    }
#else
    (void)name;
#endif
}

int ucxapp_store_list(ucxapp_store_list_cb_t cb, void *ctx)
{
    return gOps->list != NULL ? gOps->list(cb, ctx) : -1;
}

/* Closes a file written through the store; false when what was written did not all reach it. */
static bool closeWritten(void *h)
{
    if (gOps == &gStdioOps) {
        bool ok = fflush((FILE *)h) == 0 && !ferror((FILE *)h);
        return fclose((FILE *)h) == 0 && ok;
    }
    gOps->close(h);
    return true;
}

ucxapp_store_writer_t *ucxapp_store_begin(const char *name)
{
    if (name == NULL || strlen(name) >= NAME_MAX_LEN) {
        return NULL;
    }
    ucxapp_store_writer_t *w = (ucxapp_store_writer_t *)calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    snprintf(w->name, sizeof(w->name), "%s", name);
    snprintf(w->tmp, sizeof(w->tmp), "%s%s", name, TMP_SUFFIX);
    if (isSecret(name, NULL)) {
        return w;   /* streamed in plain (web upload, BLE transfer): sealed at the end, never on disk before */
    }
    w->handle = gOps->open(w->tmp, true);
    if (w->handle == NULL) {
        free(w);
        return NULL;
    }
    return w;
}

static bool memAppend(ucxapp_store_writer_t *w, const void *data, size_t len)
{
    if (len > SECRET_MAX - w->memLen) {
        return false;
    }
    if (w->memLen + len > w->memCap) {
        size_t cap = w->memCap != 0 ? w->memCap : 4096;
        while (cap < w->memLen + len) {
            cap *= 2;
        }
        uint8_t *bigger = (uint8_t *)malloc(cap);
        if (bigger == NULL) {
            return false;
        }
        if (w->mem != NULL) {
            memcpy(bigger, w->mem, w->memLen);
            memset(w->mem, 0, w->memCap);
            free(w->mem);
        }
        w->mem = bigger;
        w->memCap = cap;
    }
    memcpy(w->mem + w->memLen, data, len);
    w->memLen += len;
    return true;
}

bool ucxapp_store_append(ucxapp_store_writer_t *w, const void *data, size_t len)
{
    if (w == NULL || w->failed) {
        return false;
    }
    if (len > 0 && (w->handle == NULL ? !memAppend(w, data, len)
                                      : gOps->write(w->handle, (const uint8_t *)data, len) < 0)) {
        w->failed = true;
        return false;
    }
    return true;
}

bool ucxapp_store_end(ucxapp_store_writer_t *w, bool commit)
{
    bool ok;

    if (w == NULL) {
        return false;
    }
    if (w->handle == NULL) {
        ok = commit && !w->failed && ucxapp_store_save(w->name, w->mem != NULL ? w->mem : (const uint8_t *)"", w->memLen);
        if (w->mem != NULL) {
            memset(w->mem, 0, w->memCap);
            free(w->mem);
        }
        free(w);
        return ok;
    }
    ok = closeWritten(w->handle) && commit && !w->failed && gOps->rename(w->tmp, w->name);
    if (!ok) {
        gOps->remove(w->tmp);
    }
    free(w);
    return ok;
}

static bool saveRaw(const char *name, const void *data, size_t len)
{
    ucxapp_store_writer_t *w = (ucxapp_store_writer_t *)calloc(1, sizeof(*w));
    bool ok;

    if (w == NULL || strlen(name) >= NAME_MAX_LEN) {
        free(w);
        return false;
    }
    snprintf(w->name, sizeof(w->name), "%s", name);
    snprintf(w->tmp, sizeof(w->tmp), "%s%s", name, TMP_SUFFIX);
    w->handle = gOps->open(w->tmp, true);
    if (w->handle == NULL) {
        free(w);
        return false;
    }
    ok = ucxapp_store_append(w, data, len);
    ok = closeWritten(w->handle) && ok && gOps->rename(w->tmp, w->name);
    if (!ok) {
        gOps->remove(w->tmp);
    }
    free(w);
    return ok;
}

bool ucxapp_store_save(const char *name, const void *data, size_t len)
{
    size_t sealedLen = 0;
    uint8_t *sealed;
    bool ok;

    if (!isSecret(name, NULL)) {
        return saveRaw(name, data, len);
    }
    sealed = gSecure->seal(name, (const uint8_t *)data, len, &sealedLen);
    ok = sealed != NULL && saveRaw(name, sealed, sealedLen);
    free(sealed);
    return ok;
}

/* Opens for reading; a file still under its old name (u-connectClientApp.wifi -> ucxapp.wifi) is moved first. */
static void *openForRead(const char *name)
{
    void *h = gOps->open(name, false);
    char old[NAME_MAX_LEN + 16];
    if (h == NULL && strncmp(name, "ucxapp.", 7) == 0) {
        snprintf(old, sizeof(old), "u-connectClientApp%s", name + 6);
        if (gOps->rename(old, name)) {
            h = gOps->open(name, false);
        }
    }
    return h;
}

uint8_t *ucxapp_store_load(const char *name, size_t *outLen)
{
#ifdef UCXAPP_LITE
    return ucxapp_store_load_raw(name, outLen);
#else
    size_t len = 0;
    bool mayBeSealed = false;
    uint8_t *data = ucxapp_store_load_raw(name, &len);

    (void)isSecret(name, &mayBeSealed);
    if (data != NULL && mayBeSealed && !gSecure->open(name, data, &len)) {
        memset(data, 0, len);
        free(data);
        data = NULL;
    }
    if (data != NULL) {
        data[len] = '\0';
        if (outLen != NULL) {
            *outLen = len;
        }
    }
    return data;
#endif
}

uint8_t *ucxapp_store_load_raw(const char *name, size_t *outLen)
{
    void *h = openForRead(name);
    if (h == NULL) {
        return NULL;
    }
    size_t cap = 4096;
    size_t len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap + 1);
    while (buf != NULL) {
        if (len == cap) {
            uint8_t *bigger = (uint8_t *)realloc(buf, cap * 2 + 1);
            if (bigger == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        int32_t n = gOps->read(h, buf + len, cap - len);
        if (n < 0) {
            free(buf);
            buf = NULL;
            break;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    gOps->close(h);
    if (buf != NULL) {
        buf[len] = '\0';
        if (outLen != NULL) {
            *outLen = len;
        }
    }
    return buf;
}

bool ucxapp_store_exists(const char *name)
{
    void *h = openForRead(name);
    if (h == NULL) {
        return false;
    }
    gOps->close(h);
    return true;
}

bool ucxapp_store_remove(const char *name)
{
    return gOps->remove(name);
}

void *ucxapp_store_open_read(const char *name)
{
    return openForRead(name);
}

int32_t ucxapp_store_read(void *handle, uint8_t *buf, size_t len)
{
    return gOps->read(handle, buf, len);
}

void ucxapp_store_close(void *handle)
{
    gOps->close(handle);
}
