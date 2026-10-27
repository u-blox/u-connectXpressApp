#ifndef UCXAPP_SDCARD_H
#define UCXAPP_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

#include "ucxapp_store.h"

/** Mounts a FAT SD card on the board's SPI pins; NULL if there is no card or no SD support on this board. */
const ucxapp_store_ops_t *ucxapp_sdcard_mount(uint32_t *sizeMb);

/** Creates a fresh FAT file system on the mounted card. */
bool ucxapp_sdcard_format(void);

#endif /* UCXAPP_SDCARD_H */
