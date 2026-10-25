#ifndef UCXAPP_H
#define UCXAPP_H

/* Portable application core: runs on Windows, Linux and embedded targets.
 * Feature code must only use this API for output and input (no printf/fgets/getchar). */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4201)
#endif
#include "u_cx.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct {
    uCxHandle_t *ucx;   /* NULL while no module is connected */
} ucxapp_t;

/* ---- IO layer ---------------------------------------------------------- */

typedef struct {
    void (*write)(const char *text, size_t len);
    int (*readline)(char *buf, size_t size);   /* length without newline, -1 on EOF */
} ucxapp_io_t;

extern const ucxapp_io_t ucxapp_io_stdio;      /* ucxapp_io_stdio.c (PC targets) */

void ucxapp_io_set(const ucxapp_io_t *io);

/* Extra consumers of everything ucxapp_print() writes (e.g. web servers). Called on any thread; must not block. */
typedef void (*ucxapp_sink_fn_t)(const char *text, size_t len);
bool ucxapp_io_add_sink(ucxapp_sink_fn_t sink);
void ucxapp_io_remove_sink(ucxapp_sink_fn_t sink);
void ucxapp_print(const char *fmt, ...);
int ucxapp_readline(const char *prompt, char *buf, size_t size);

/* ---- URC events -------------------------------------------------------- */

#define URC_FLAG_NETWORK_UP         (1 << 0)  /* Wi-Fi station network up (IP assigned) */
#define URC_FLAG_NETWORK_DOWN       (1 << 1)
#define URC_FLAG_SOCK_CONNECTED     (1 << 2)
#define URC_FLAG_SOCK_DATA          (1 << 3)
#define URC_FLAG_SPS_CONNECTED      (1 << 4)
#define URC_FLAG_SPS_DISCONNECTED   (1 << 5)
#define URC_FLAG_SPS_DATA           (1 << 6)
#define URC_FLAG_STARTUP            (1 << 7)
#define URC_FLAG_PING_COMPLETE      (1 << 8)
#define URC_FLAG_WIFI_LINK_UP       (1 << 9)
#define URC_FLAG_WIFI_LINK_DOWN     (1 << 10)
#define URC_FLAG_MQTT_CONNECTED     (1 << 11)
#define URC_FLAG_MQTT_DATA          (1 << 12)
#define URC_FLAG_BT_CONNECTED       (1 << 13)
#define URC_FLAG_BT_DISCONNECTED    (1 << 14)
#define URC_FLAG_HTTP_RESPONSE_READY (1 << 15)
#define URC_FLAG_HTTP_DISCONNECTED  (1 << 16)
#define URC_FLAG_BT_PASSKEY_REQUEST (1 << 17)
#define URC_FLAG_SOCK_CLOSED        (1 << 18)
#define URC_FLAG_MQTT_DISCONNECTED  (1 << 19)
#define URC_FLAG_SOCK_INCOMING      (1 << 20)
#define URC_FLAG_MQTT_SUBSCRIBED    (1 << 21)  /* +UEMQSC: subscribe or unsubscribe completed */

/* Registers minimal URC handlers (ucxapp_urc.c) that raise the flags below; for hosts without their own. */
void ucxapp_urc_register(uCxHandle_t *ucx);
/* Track BLE connections the module already has (left open by an earlier session) so "[conn]" can default to them. */
void ucxapp_adopt_connections(uCxHandle_t *ucx);
void ucxapp_udp_reset_limit(void);   /* forget the UDP read limit learned from the previous module */
/* Tick of the last +STARTUP; *exact is false when none was seen and the time is the connect time instead. */
uint32_t ucxapp_module_boot_ms(bool *exact);

/* Event flags set by URC handlers. Direct access to the flags requires the lock. */
extern volatile uint32_t gUcxappEventFlags;

/* Totals from the last +UDIAGPINGCOMPLETE URC. */
extern volatile int32_t gUcxappPingSent;
extern volatile int32_t gUcxappPingReceived;
extern volatile int32_t gUcxappPingAvgMs;

void ucxapp_events_init(void);
void ucxapp_events_deinit(void);
void ucxapp_events_lock(void);
void ucxapp_events_unlock(void);
void ucxapp_events_signal(uint32_t flags);
void ucxapp_events_clear(uint32_t flags);
bool ucxapp_events_wait(uint32_t flag, uint32_t timeoutS);   /* consumes the flag */

#endif /* UCXAPP_H */
