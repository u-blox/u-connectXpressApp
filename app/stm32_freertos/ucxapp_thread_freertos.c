/* ucxapp_thread on FreeRTOS: a task per thread, joined through a binary semaphore. The web server starts two:
 * its pump (the TLS handshake, ECDSA and ECDH on P-256, runs on its stack) and the UI worker that runs commands. */

#include <stdlib.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ucxapp_thread.h"

#ifndef UCXAPP_THREAD_STACK_WORDS
#define UCXAPP_THREAD_STACK_WORDS 3072
#endif

struct ucxapp_thread {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
};

static void entry(void *p)
{
    struct ucxapp_thread *t = (struct ucxapp_thread *)p;

    t->fn(t->arg);
    xSemaphoreGive(t->done);
    vTaskDelete(NULL);
}

ucxapp_thread_t *ucxapp_thread_start(void (*fn)(void *), void *arg)
{
    struct ucxapp_thread *t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
    t->done = xSemaphoreCreateBinary();
    if (t->done == NULL ||
        xTaskCreate(entry, "ucxthr", UCXAPP_THREAD_STACK_WORDS, t, tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        if (t->done != NULL) {
            vSemaphoreDelete(t->done);
        }
        free(t);
        return NULL;
    }
    return t;
}

/* newlib's malloc is not thread-safe on its own; with a second task (web pump, TLS) it needs these hooks. */
struct _reent;

void __malloc_lock(struct _reent *r)
{
    (void)r;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskSuspendAll();
    }
}

void __malloc_unlock(struct _reent *r)
{
    (void)r;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        (void)xTaskResumeAll();
    }
}

void ucxapp_thread_join(ucxapp_thread_t *t)
{
    if (t == NULL) {
        return;
    }
    xSemaphoreTake(t->done, portMAX_DELAY);
    vSemaphoreDelete(t->done);
    free(t);
}
