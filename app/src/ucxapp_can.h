/* CAN frame type, controller access and the 17-byte record used by can.bridge. */
#ifndef UCXAPP_CAN_H
#define UCXAPP_CAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Record: ts_ms u32 LE, id u32 LE (above 0x7FF = extended), dlc u8, data[8]. */
#define UCXAPP_CAN_REC_LEN 17
/* Records per datagram: 32 * 17 = 544 bytes, well below the module UDP payload limit. */
#define UCXAPP_CAN_BATCH   32

typedef struct {
    uint32_t id;
    uint8_t len;
    uint8_t data[8];
} ucxapp_can_frame_t;

/* Packs one record into out (UCXAPP_CAN_REC_LEN bytes). len above 8 is clamped to 8. */
size_t ucxapp_can_pack(uint32_t ts_ms, const ucxapp_can_frame_t *f, uint8_t *out);
/* Returns false if the record has a DLC above 8. */
bool ucxapp_can_unpack(const uint8_t *in, uint32_t *ts_ms, ucxapp_can_frame_t *f);

/* CAN controller: an emulated loopback on Windows and Linux (or a SocketCAN interface on Linux, can.open), the
   board's bxCAN/FDCAN on STM32 (UCXAPP_CAN_HW). send/recv are thread safe; recv takes frames in arrival order. */
bool ucxapp_can_send(const ucxapp_can_frame_t *f);
bool ucxapp_can_recv(ucxapp_can_frame_t *f);

typedef enum {
    UCXAPP_CAN_NORMAL,
    UCXAPP_CAN_LOOPBACK,   /* receives its own frames without a bus or transceiver */
    UCXAPP_CAN_LISTEN      /* receives only: no acknowledge, no error frames, nothing sent */
} ucxapp_can_mode_t;

#ifdef UCXAPP_CAN_HW
/* Backend per platform (app/stm32_freertos/ucxapp_can_stm32.c, app/zephyr/ucxapp_can_zephyr.c). */
typedef struct {
    uint8_t tec;          /* transmit error counter */
    uint8_t rec;          /* receive error counter */
    const char *state;    /* "error-active", "error-warning", "error-passive", "bus-off" */
} ucxapp_can_hw_state_t;

/* (Re)starts the controller in the mode. NULL on success, otherwise why not (e.g. the bit rate cannot be made from
   the CAN clock). */
const char *ucxapp_can_hw_start(int32_t bitrate, ucxapp_can_mode_t mode);
/* Queues one frame for transmission; false when every transmit buffer is full. */
bool ucxapp_can_hw_send(const ucxapp_can_frame_t *f);
void ucxapp_can_hw_state(ucxapp_can_hw_state_t *st);
/* Controller and pins, e.g. "bxCAN CAN1, RX PD0, TX PD1". */
const char *ucxapp_can_hw_name(void);
/* Called by the backend from its receive interrupt. */
void ucxapp_can_rx_isr(const ucxapp_can_frame_t *f);
/* Starts fn on its own task (can.bridge); called once. False if the task could not be created. */
bool ucxapp_can_hw_task(void (*fn)(void));
#endif

#endif
