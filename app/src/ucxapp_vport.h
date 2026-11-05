#ifndef UCXAPP_VPORT_H
#define UCXAPP_VPORT_H

/* Serial devices simulated inside the app: "virt:imu" (a WitMotion IMU) and "virt:cell" (a u-blox SARA-R5 modem
 * with an NTRIP caster on the far side of its data link) and "virt:pc" (a PC tool on a null-modem pair). They let every imu.* and cell.* command, bridge and
 * example run with a virtual module (vucx or the browser demo). Any other port name goes to uPortUart. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u_port_uart.h"

#define UCXAPP_VPORT_IMU  "virt:imu"
#define UCXAPP_VPORT_CELL "virt:cell"
#define UCXAPP_VPORT_PC   "virt:pc"

/* True for a simulated serial device name (not for virtual streams such as "virt:gnss"). */
bool ucxapp_vport_is(const char *port);
/* One line description of a simulated device, for detect output. */
const char *ucxapp_vport_desc(const char *port);

uPortUartHandle_t ucxapp_uart_open(const char *port, int32_t baud);
void ucxapp_uart_close(uPortUartHandle_t h);
int32_t ucxapp_uart_read(uPortUartHandle_t h, void *buf, size_t size, int32_t timeoutMs);
int32_t ucxapp_uart_write(uPortUartHandle_t h, const void *data, size_t len);

#endif
