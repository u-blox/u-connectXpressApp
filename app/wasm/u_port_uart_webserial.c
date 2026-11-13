/* UART port for the browser build: the page owns the Web Serial port (ucxapp-wasm.js), feeds received bytes
 * in with ucxapp_webserial_rx() and sends what the AT client writes. One port, named "webserial". */

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "u_port_uart.h"

#define RX_SIZE 65536

static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gCond = PTHREAD_COND_INITIALIZER;
static uint8_t gRx[RX_SIZE];
static size_t gHead, gCount;
static bool gReady;     /* the page has an open serial port */
static bool gOpen;
static bool gWake;
static int gHandle;     /* any non-NULL address will do as the handle */

/* Open and the page still has its port. A lost port (unplugged, closed by the page) fails every read at once, so
 * a command in flight ends with an I/O error instead of waiting out its timeout. Called with gMutex held. */
static bool alive(void)
{
    return gOpen && gReady;
}

/* Called by the page (main thread) for every chunk the serial port delivers; bytes beyond the ring are lost. */
EMSCRIPTEN_KEEPALIVE void ucxapp_webserial_rx(const uint8_t *data, int len)
{
    pthread_mutex_lock(&gMutex);
    for (int i = 0; i < len && gOpen && gCount < RX_SIZE; i++) {
        gRx[(gHead + gCount++) % RX_SIZE] = data[i];
    }
    pthread_cond_broadcast(&gCond);
    pthread_mutex_unlock(&gMutex);
}

/* The page opened (1) or lost (0) its serial port. */
EMSCRIPTEN_KEEPALIVE void ucxapp_webserial_ready(int ready)
{
    pthread_mutex_lock(&gMutex);
    gReady = ready != 0;
    pthread_cond_broadcast(&gCond);
    pthread_mutex_unlock(&gMutex);
}

/* Waits until data, a wake or the timeout (-1 = forever); called with gMutex held. */
static void waitData(int32_t timeoutMs)
{
    struct timespec until;

    if (timeoutMs < 0) {
        while (gCount == 0 && !gWake && alive()) {
            pthread_cond_wait(&gCond, &gMutex);
        }
        return;
    }
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeoutMs / 1000;
    until.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec++;
    }
    while (gCount == 0 && !gWake && alive()) {
        if (pthread_cond_timedwait(&gCond, &gMutex, &until) != 0) {
            break;
        }
    }
}

uPortUartHandle_t uPortUartOpen(const char *pDevName, int32_t baudRate, bool useFlowControl)
{
    uPortUartHandle_t handle = NULL;

    (void)baudRate;   /* the page opens the port at 115200 */
    (void)useFlowControl;
    if (pDevName == NULL || strcmp(pDevName, "webserial") != 0) {
        return NULL;
    }
    pthread_mutex_lock(&gMutex);
    if (gReady && !gOpen) {
        gOpen = true;
        gWake = false;
        gHead = gCount = 0;
        handle = &gHandle;
    }
    pthread_mutex_unlock(&gMutex);
    return handle;
}

void uPortUartClose(uPortUartHandle_t handle)
{
    if (handle == NULL) {
        return;
    }
    pthread_mutex_lock(&gMutex);
    gOpen = false;
    pthread_cond_broadcast(&gCond);
    pthread_mutex_unlock(&gMutex);
}

int32_t uPortUartWrite(uPortUartHandle_t handle, const void *pData, size_t length)
{
    if (handle == NULL || pData == NULL || length == 0 || !gReady) {
        return -1;
    }
    uint8_t *copy = (uint8_t *)malloc(length);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, pData, length);
    /* The page writes it and frees the copy. */
    MAIN_THREAD_ASYNC_EM_ASM({ Module.ucxSerialWrite($0, $1); }, copy, (int)length);
    return (int32_t)length;
}

int32_t uPortUartRead(uPortUartHandle_t handle, void *pData, size_t length, int32_t timeoutMs)
{
    size_t n = 0;

    if (handle == NULL || length == 0) {
        return -1;
    }
    if (pData == NULL) {
        return 0;
    }
    pthread_mutex_lock(&gMutex);
    waitData(timeoutMs);
    while (n < length && gCount > 0) {
        ((uint8_t *)pData)[n++] = gRx[gHead];
        gHead = (gHead + 1) % RX_SIZE;
        gCount--;
    }
    gWake = false;
    bool ok = alive();
    pthread_mutex_unlock(&gMutex);
    return ok ? (int32_t)n : -1;
}

int32_t uPortUartWaitForData(uPortUartHandle_t handle, int32_t timeoutMs)
{
    int32_t result;

    if (handle == NULL) {
        return -1;
    }
    pthread_mutex_lock(&gMutex);
    waitData(timeoutMs);
    result = !alive() ? -1 : gCount > 0 ? 1 : 0;
    gWake = false;
    pthread_mutex_unlock(&gMutex);
    return result;
}

void uPortUartWake(uPortUartHandle_t handle)
{
    (void)handle;
    pthread_mutex_lock(&gMutex);
    gWake = true;
    pthread_cond_broadcast(&gCond);
    pthread_mutex_unlock(&gMutex);
}

void uPortUartRxSignalFromIsr(void)
{
}
