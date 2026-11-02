#include "ucxapp.h"
#include "u_port.h"

static U_CX_MUTEX_HANDLE gMutex;
volatile uint32_t gUcxappEventFlags = 0;
volatile int32_t gUcxappPingSent = 0;
volatile int32_t gUcxappPingReceived = 0;
volatile int32_t gUcxappPingAvgMs = 0;

void ucxapp_events_init(void)
{
    U_CX_MUTEX_CREATE(gMutex);
}

void ucxapp_events_deinit(void)
{
    U_CX_MUTEX_DELETE(gMutex);
}

void ucxapp_events_lock(void)
{
    U_CX_MUTEX_LOCK(gMutex);
}

void ucxapp_events_unlock(void)
{
    U_CX_MUTEX_UNLOCK(gMutex);
}

void ucxapp_events_signal(uint32_t flags)
{
    ucxapp_events_lock();
    gUcxappEventFlags |= flags;
    ucxapp_events_unlock();
}

void ucxapp_events_clear(uint32_t flags)
{
    ucxapp_events_lock();
    gUcxappEventFlags &= ~flags;
    ucxapp_events_unlock();
}

bool ucxapp_events_take(uint32_t flag)
{
    bool set;

    ucxapp_events_lock();
    set = (gUcxappEventFlags & flag) != 0;
    gUcxappEventFlags &= ~flag;
    ucxapp_events_unlock();
    return set;
}

bool ucxapp_events_wait(uint32_t flag, uint32_t timeoutS)
{
    int32_t timeoutMs = (int32_t)(timeoutS * 1000);
    int32_t startTime = U_CX_PORT_GET_TIME_MS();

    do {
        ucxapp_events_lock();
        if (gUcxappEventFlags & flag) {
            gUcxappEventFlags &= ~flag;
            ucxapp_events_unlock();
            return true;
        }
        ucxapp_events_unlock();
        U_CX_PORT_SLEEP_MS(50);   /* let the RX task process URCs */
    } while (U_CX_PORT_GET_TIME_MS() - startTime < timeoutMs);

    return false;
}
