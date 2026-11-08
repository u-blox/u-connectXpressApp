/* ucxapp_thread on Zephyr: the web server's two threads (pump with the TLS handshake, UI worker) from a static
 * pool, so no dynamic thread stacks are needed. */

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "ucxapp_thread.h"

#define POOL       2
#define STACK_SIZE 12288

struct ucxapp_thread {
    struct k_thread thread;
    void (*fn)(void *);
    void *arg;
    bool used;
};

K_THREAD_STACK_ARRAY_DEFINE(gStacks, POOL, STACK_SIZE);
static struct ucxapp_thread gPool[POOL];
K_MUTEX_DEFINE(gPoolLock);

static void entry(void *p1, void *p2, void *p3)
{
    struct ucxapp_thread *t = p1;

    (void)p2;
    (void)p3;
    t->fn(t->arg);
}

ucxapp_thread_t *ucxapp_thread_start(void (*fn)(void *), void *arg)
{
    struct ucxapp_thread *t = NULL;
    int i;

    k_mutex_lock(&gPoolLock, K_FOREVER);
    for (i = 0; i < POOL; i++) {
        if (!gPool[i].used) {
            t = &gPool[i];
            t->used = true;
            break;
        }
    }
    k_mutex_unlock(&gPoolLock);
    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
    k_thread_create(&t->thread, gStacks[i], K_THREAD_STACK_SIZEOF(gStacks[i]), entry, t, NULL, NULL,
                    K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(&t->thread, "ucxthr");
    return t;
}

void ucxapp_thread_join(ucxapp_thread_t *t)
{
    if (t == NULL) {
        return;
    }
    k_thread_join(&t->thread, K_FOREVER);
    k_mutex_lock(&gPoolLock, K_FOREVER);
    t->used = false;
    k_mutex_unlock(&gPoolLock);
}
