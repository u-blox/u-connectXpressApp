/* CAN backend for Zephyr: the zephyr,canbus controller through the generic CAN API (bxCAN or FDCAN as the board
 * has it; pins in boards/<board>.overlay, same as the FreeRTOS build). */

#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

#include "ucxapp_can.h"

static const struct device *const gCan = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static int gStdFilter = -ENOSPC;
static int gExtFilter = -ENOSPC;
static char gErr[64];

static void rxFrame(const struct device *dev, struct can_frame *frame, void *user)
{
    ucxapp_can_frame_t f;
    uint8_t len = can_dlc_to_bytes(frame->dlc);

    (void)dev;
    (void)user;
    f.id = frame->id;
    f.len = len > 8 ? 8 : len;
    memcpy(f.data, frame->data, sizeof(f.data));
    ucxapp_can_rx_isr(&f);
}

/* Completion of a queued frame; errors show in the controller state and counters. */
static void txDone(const struct device *dev, int error, void *user)
{
    (void)dev;
    (void)error;
    (void)user;
}

const char *ucxapp_can_hw_name(void)
{
    return gCan->name;
}

static const char *failed(const char *what, int err)
{
    snprintf(gErr, sizeof(gErr), "%s failed (%d)", what, err);
    return gErr;
}

const char *ucxapp_can_hw_start(int32_t bitrate, ucxapp_can_mode_t mode)
{
    const struct can_filter stdAll = { .id = 0, .mask = 0, .flags = 0 };
    const struct can_filter extAll = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };
    int err;

    if (!device_is_ready(gCan)) {
        return "the CAN controller is not ready";
    }
    (void)can_stop(gCan);
    err = can_set_bitrate(gCan, (uint32_t)bitrate);
    if (err != 0) {
        return failed("bit rate", err);
    }
    err = can_set_mode(gCan, mode == UCXAPP_CAN_LOOPBACK ? CAN_MODE_LOOPBACK
                           : mode == UCXAPP_CAN_LISTEN ? CAN_MODE_LISTENONLY : CAN_MODE_NORMAL);
    if (err != 0) {
        return failed("mode", err);
    }
    if (gStdFilter < 0) {
        gStdFilter = can_add_rx_filter(gCan, rxFrame, NULL, &stdAll);
    }
    if (gExtFilter < 0) {
        gExtFilter = can_add_rx_filter(gCan, rxFrame, NULL, &extAll);
    }
    if (gStdFilter < 0 || gExtFilter < 0) {
        return failed("receive filter", gStdFilter < 0 ? gStdFilter : gExtFilter);
    }
    err = can_start(gCan);
    return err != 0 ? failed("start", err) : NULL;
}

bool ucxapp_can_hw_send(const ucxapp_can_frame_t *f)
{
    struct can_frame frame = { 0 };
    uint8_t len = f->len > 8 ? 8 : f->len;

    frame.id = f->id & CAN_EXT_ID_MASK;
    frame.flags = f->id > CAN_STD_ID_MASK ? CAN_FRAME_IDE : 0;
    frame.dlc = can_bytes_to_dlc(len);
    memcpy(frame.data, f->data, len);
    return can_send(gCan, &frame, K_NO_WAIT, txDone, NULL) == 0;
}

void ucxapp_can_hw_state(ucxapp_can_hw_state_t *st)
{
    enum can_state state = CAN_STATE_STOPPED;
    struct can_bus_err_cnt cnt = { 0 };

    (void)can_get_state(gCan, &state, &cnt);
    st->tec = cnt.tx_err_cnt;
    st->rec = cnt.rx_err_cnt;
    st->state = state == CAN_STATE_ERROR_ACTIVE ? "error-active" : state == CAN_STATE_ERROR_WARNING ? "error-warning"
              : state == CAN_STATE_ERROR_PASSIVE ? "error-passive" : state == CAN_STATE_BUS_OFF ? "bus-off" : "stopped";
}

/* can.bridge thread: below the shell (main) thread, so commands stay responsive while it runs. */
#define CAN_THREAD_STACK    2048
#define CAN_THREAD_PRIORITY 8

K_THREAD_STACK_DEFINE(gTaskStack, CAN_THREAD_STACK);
static struct k_thread gTask;
static void (*gTaskFn)(void);

static void taskEntry(void *a, void *b, void *c)
{
    (void)a;
    (void)b;
    (void)c;
    gTaskFn();
}

bool ucxapp_can_hw_task(void (*fn)(void))
{
    gTaskFn = fn;
    return k_thread_create(&gTask, gTaskStack, K_THREAD_STACK_SIZEOF(gTaskStack), taskEntry, NULL, NULL, NULL,
                           CAN_THREAD_PRIORITY, 0, K_NO_WAIT) != NULL;
}
