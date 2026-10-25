#include "ucxapp_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_platform.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
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
    FILE *f = fopen(path, forWrite ? "wb" : "rb");
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
    remove(b);   /* rename() does not replace an existing file on Windows */
    return rename(a, b) == 0;
}

static int stdioList(ucxapp_store_list_cb_t cb, void *ctx)
{
    return ucxapp_platform_list_files(gDir[0] != '\0' ? gDir : ".", cb, ctx);
}

static const ucxapp_store_ops_t gStdioOps = {
    stdioOpen, stdioRead, stdioWrite, stdioClose, stdioRemove, stdioRename, stdioList
};
static const ucxapp_store_ops_t *gOps = &gStdioOps;

struct ucxapp_store_writer {
    void *handle;
    char name[NAME_MAX_LEN];
    char tmp[NAME_MAX_LEN + sizeof(TMP_SUFFIX)];
    bool failed;
};

void ucxapp_store_set_backend(const ucxapp_store_ops_t *ops)
{
    gOps = ops != NULL ? ops : &gStdioOps;
}

void ucxapp_store_set_dir(const char *dir)
{
    snprintf(gDir, sizeof(gDir), "%s", dir != NULL ? dir : "");
    if (gDir[0] != '\0') {
        (void)MKDIR(gDir);
    }
}

int ucxapp_store_list(ucxapp_store_list_cb_t cb, void *ctx)
{
    return gOps->list != NULL ? gOps->list(cb, ctx) : -1;
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
    w->handle = gOps->open(w->tmp, true);
    if (w->handle == NULL) {
        free(w);
        return NULL;
    }
    return w;
}

bool ucxapp_store_append(ucxapp_store_writer_t *w, const void *data, size_t len)
{
    if (w == NULL || w->failed) {
        return false;
    }
    if (len > 0 && gOps->write(w->handle, (const uint8_t *)data, len) < 0) {
        w->failed = true;
        return false;
    }
    return true;
}

bool ucxapp_store_end(ucxapp_store_writer_t *w, bool commit)
{
    if (w == NULL) {
        return false;
    }
    gOps->close(w->handle);
    bool ok = commit && !w->failed && gOps->rename(w->tmp, w->name);
    if (!ok) {
        gOps->remove(w->tmp);
    }
    free(w);
    return ok;
}

bool ucxapp_store_save(const char *name, const void *data, size_t len)
{
    ucxapp_store_writer_t *w = ucxapp_store_begin(name);
    if (w == NULL) {
        return false;
    }
    bool ok = ucxapp_store_append(w, data, len);
    return ucxapp_store_end(w, ok);
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
