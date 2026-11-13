#ifndef UCXAPP_THREAD_H
#define UCXAPP_THREAD_H

#include <stdbool.h>

typedef struct ucxapp_thread ucxapp_thread_t;

/* Starts fn(arg) on a new thread; returns NULL on failure. */
ucxapp_thread_t *ucxapp_thread_start(void (*fn)(void *), void *arg);

/* Waits for the thread to end (the caller must have told it to stop) and frees the handle. */
void ucxapp_thread_join(ucxapp_thread_t *t);

/* True when called on the thread itself. */
bool ucxapp_thread_is_self(const ucxapp_thread_t *t);

#endif /* UCXAPP_THREAD_H */
