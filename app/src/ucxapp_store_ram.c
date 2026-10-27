/* RAM-backed store for embedded targets without a file system (files are lost at reset). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ucxapp_store.h"

#define RAM_FILES     8
#define RAM_NAME_LEN  40

typedef struct {
    char name[RAM_NAME_LEN];
    uint8_t *data;
    size_t len;
    bool used;
} ramFile_t;

typedef struct {
    ramFile_t *file;
    size_t pos;
} ramHandle_t;

static ramFile_t gRamFiles[RAM_FILES];

static ramFile_t *ramFind(const char *name)
{
    for (int i = 0; i < RAM_FILES; i++) {
        if (gRamFiles[i].used && strcmp(gRamFiles[i].name, name) == 0) {
            return &gRamFiles[i];
        }
    }
    return NULL;
}

static void ramFree(ramFile_t *f)
{
    free(f->data);
    memset(f, 0, sizeof(*f));
}

static void *ramOpen(const char *name, bool forWrite)
{
    ramFile_t *f = ramFind(name);
    ramHandle_t *h;

    if (strlen(name) >= RAM_NAME_LEN) {
        return NULL;
    }
    if (forWrite) {
        if (f != NULL) {
            free(f->data);
            f->data = NULL;
            f->len = 0;
        } else {
            for (int i = 0; i < RAM_FILES && f == NULL; i++) {
                f = gRamFiles[i].used ? NULL : &gRamFiles[i];
            }
            if (f == NULL) {
                return NULL;
            }
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->used = true;
        }
    } else if (f == NULL) {
        return NULL;
    }
    h = (ramHandle_t *)calloc(1, sizeof(*h));
    if (h != NULL) {
        h->file = f;
    }
    return h;
}

static int32_t ramRead(void *handle, uint8_t *buf, size_t len)
{
    ramHandle_t *h = (ramHandle_t *)handle;
    size_t n = h->file->len - h->pos;

    if (n > len) {
        n = len;
    }
    memcpy(buf, h->file->data + h->pos, n);
    h->pos += n;
    return (int32_t)n;
}

static int32_t ramWrite(void *handle, const uint8_t *data, size_t len)
{
    ramHandle_t *h = (ramHandle_t *)handle;
    uint8_t *grown = (uint8_t *)realloc(h->file->data, h->file->len + len + 1);

    if (grown == NULL) {
        return -1;
    }
    memcpy(grown + h->file->len, data, len);
    h->file->len += len;
    grown[h->file->len] = '\0';
    h->file->data = grown;
    return (int32_t)len;
}

static void ramClose(void *handle)
{
    free(handle);
}

static bool ramRemove(const char *name)
{
    ramFile_t *f = ramFind(name);

    if (f == NULL) {
        return false;
    }
    ramFree(f);
    return true;
}

static bool ramRename(const char *from, const char *to)
{
    ramFile_t *src = ramFind(from);
    ramFile_t *dst = ramFind(to);

    if (src == NULL || strlen(to) >= RAM_NAME_LEN) {
        return false;
    }
    if (dst != NULL && dst != src) {
        ramFree(dst);
    }
    snprintf(src->name, sizeof(src->name), "%s", to);
    return true;
}

static int ramList(ucxapp_store_list_cb_t cb, void *ctx)
{
    int count = 0;

    for (int i = 0; i < RAM_FILES; i++) {
        if (gRamFiles[i].used) {
            cb(gRamFiles[i].name, gRamFiles[i].len, ctx);
            count++;
        }
    }
    return count;
}

const ucxapp_store_ops_t ucxapp_store_ram = { ramOpen, ramRead, ramWrite, ramClose, ramRemove, ramRename, ramList };
