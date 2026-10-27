#ifndef UCXAPP_FLASHFS_H
#define UCXAPP_FLASHFS_H

#include <stdbool.h>
#include <stdint.h>

#include "ucxapp_store.h"

/** Mounts (formatting on first use) the flash file area; NULL when the board has none. */
const ucxapp_store_ops_t *ucxapp_flashfs_mount(uint32_t *sizeKb);

/** Erases every file. */
bool ucxapp_flashfs_format(void);

#endif
