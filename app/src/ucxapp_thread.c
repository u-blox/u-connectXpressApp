#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <stdlib.h>

#include "ucxapp_thread.h"

struct ucxapp_thread {
#ifdef _WIN32
    HANDLE handle;
    DWORD id;
#else
    pthread_t handle;
#endif
    void (*fn)(void *);
    void *arg;
};

#ifdef _WIN32
static DWORD WINAPI entry(LPVOID p)
{
    struct ucxapp_thread *t = (struct ucxapp_thread *)p;
    t->fn(t->arg);
    return 0;
}
#else
static void *entry(void *p)
{
    struct ucxapp_thread *t = (struct ucxapp_thread *)p;
    t->fn(t->arg);
    return NULL;
}
#endif

ucxapp_thread_t *ucxapp_thread_start(void (*fn)(void *), void *arg)
{
    struct ucxapp_thread *t = (struct ucxapp_thread *)calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;
#ifdef _WIN32
    t->handle = CreateThread(NULL, 0, entry, t, 0, &t->id);
    if (t->handle == NULL) {
#else
    if (pthread_create(&t->handle, NULL, entry, t) != 0) {
#endif
        free(t);
        return NULL;
    }
    return t;
}

bool ucxapp_thread_is_self(const ucxapp_thread_t *t)
{
    if (t == NULL) {
        return false;
    }
#ifdef _WIN32
    return GetCurrentThreadId() == t->id;
#else
    return pthread_equal(pthread_self(), t->handle) != 0;
#endif
}

void ucxapp_thread_join(ucxapp_thread_t *t)
{
    if (t == NULL) {
        return;
    }
    /* Until the thread has ended: the caller frees what the thread works on next. A thread asked to join
       itself cannot wait for its own end; it is left to finish. */
#ifdef _WIN32
    if (!ucxapp_thread_is_self(t)) {
        WaitForSingleObject(t->handle, INFINITE);
    }
    CloseHandle(t->handle);
#else
    if (ucxapp_thread_is_self(t)) {
        pthread_detach(t->handle);
    } else {
        pthread_join(t->handle, NULL);
    }
#endif
    free(t);
}
