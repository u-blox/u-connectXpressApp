#ifndef UCXAPP_ZEPHYR_H
#define UCXAPP_ZEPHYR_H

#include <stdint.h>

/** One byte from the console UART: 1 if read, 0 when nothing arrived within timeoutMs (0 = poll). */
int32_t ucxapp_zephyr_console_read(uint8_t *byte, int32_t timeoutMs);

#endif /* UCXAPP_ZEPHYR_H */
