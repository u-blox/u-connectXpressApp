#ifndef UCXAPP_STORE_H
#define UCXAPP_STORE_H

/* Named-blob storage shared by all commands (downloads, saved settings, firmware images).
 * Hosts use the stdio backend; an embedded target installs its own flash/filesystem backend
 * with ucxapp_store_set_backend(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ucxapp_store_list_cb_t)(const char *name, uint64_t size, void *ctx);

typedef struct {
    /* Returns a handle or NULL. Names are plain relative file names. */
    void *(*open)(const char *name, bool forWrite);
    /* Return bytes transferred, or < 0 on error; read returns 0 at end of data. */
    int32_t (*read)(void *handle, uint8_t *buf, size_t len);
    int32_t (*write)(void *handle, const uint8_t *data, size_t len);
    void (*close)(void *handle);
    bool (*remove)(const char *name);
    /* Replaces `to` if it exists. */
    bool (*rename)(const char *from, const char *to);
    /* Calls cb for every stored file; returns the count, -1 if listing failed. */
    int (*list)(ucxapp_store_list_cb_t cb, void *ctx);
} ucxapp_store_ops_t;

typedef struct ucxapp_store_writer ucxapp_store_writer_t;

/** Install a backend; NULL restores the stdio default. */
void ucxapp_store_set_backend(const ucxapp_store_ops_t *ops);

/** Directory prepended to every name by the stdio backend (include the trailing separator); created if missing. Reads fall back to the current directory. */
void ucxapp_store_set_dir(const char *dir);

/** Stores a whole blob atomically (written to a temporary name, then renamed). */
bool ucxapp_store_save(const char *name, const void *data, size_t len);

/** Reads a whole blob into a malloc'ed buffer (caller frees; one extra NUL byte follows the data). NULL on failure. */
uint8_t *ucxapp_store_load(const char *name, size_t *outLen);

bool ucxapp_store_exists(const char *name);
/** Streaming read for files too big to load whole; read returns 0 at the end. */
void *ucxapp_store_open_read(const char *name);
int32_t ucxapp_store_read(void *handle, uint8_t *buf, size_t len);
void ucxapp_store_close(void *handle);
bool ucxapp_store_remove(const char *name);
/** Calls cb for every stored file (no folders); returns the count, -1 if the backend cannot list. */
int ucxapp_store_list(ucxapp_store_list_cb_t cb, void *ctx);

/** Streaming write for large data such as downloads. Nothing appears under `name` until commit. */
ucxapp_store_writer_t *ucxapp_store_begin(const char *name);
bool ucxapp_store_append(ucxapp_store_writer_t *w, const void *data, size_t len);
/** commit=true publishes the file, false discards it. Returns false if publishing failed. Frees the writer. */
bool ucxapp_store_end(ucxapp_store_writer_t *w, bool commit);

#ifdef __cplusplus
}
#endif

#endif /* UCXAPP_STORE_H */
