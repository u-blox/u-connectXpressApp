/* littlefs in the top two erase sectors of the internal flash, used as the file store
 * (profiles, certificates, downloads) so files survive a reset. */

#include "ucxapp_flashfs.h"

#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "lfs.h"

#if defined(STM32H753xx)
#include "stm32h7xx_hal.h"
/* Bank 2 sectors 6-7: no CPU stall, the code runs from bank 1. */
#define FS_BASE      0x081C0000UL
#define FS_BLOCK     (128U * 1024U)
#define FS_SECTOR0   6U
#define FS_PROG      32U
#elif defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#define FS_BASE      0x08180000UL
#define FS_BLOCK     (256U * 1024U)
#define FS_SECTOR0   10U
#define FS_PROG      4U
#elif defined(STM32F439xx)
#include "stm32f4xx_hal.h"
/* Bank 2 sectors 22-23. */
#define FS_BASE      0x081C0000UL
#define FS_BLOCK     (128U * 1024U)
#define FS_SECTOR0   22U
#define FS_PROG      4U
#elif defined(STM32F407xx)
#include "stm32f4xx_hal.h"
#define FS_BASE      0x080C0000UL
#define FS_BLOCK     (128U * 1024U)
#define FS_SECTOR0   10U
#define FS_PROG      4U
#endif

#ifdef FS_BASE

#define FS_BLOCKS    2U
#define FS_CACHE     256U
#define FS_LOOKAHEAD 16U

extern uint32_t _sidata, _sdata, _edata;

static lfs_t gLfs;
static SemaphoreHandle_t gLock;
static bool gMounted;
static uint8_t gReadBuf[FS_CACHE];
static uint8_t gProgBuf[FS_CACHE];
static uint8_t gLookahead[FS_LOOKAHEAD] __attribute__((aligned(4)));

typedef struct {
    lfs_file_t file;
    struct lfs_file_config cfg;
    uint8_t cache[FS_CACHE];
} fsHandle_t;

static void invalidate(uint32_t addr, uint32_t size)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((void *)addr, (int32_t)size);
#elif defined(FLASH_ACR_DCEN)
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_ENABLE();
    (void)addr;
    (void)size;
#else
    (void)addr;
    (void)size;
#endif
}

static int fsRead(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size)
{
    (void)c;
    memcpy(buf, (const void *)(FS_BASE + block * FS_BLOCK + off), size);
    return LFS_ERR_OK;
}

static int fsProg(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buf, lfs_size_t size)
{
    uint32_t addr = FS_BASE + block * FS_BLOCK + off;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t word[FS_PROG / 4U];
    HAL_StatusTypeDef st = HAL_OK;

    (void)c;
    HAL_FLASH_Unlock();
    for (lfs_size_t done = 0; done < size && st == HAL_OK; done += FS_PROG) {
        memcpy(word, src + done, FS_PROG);
#if defined(STM32H753xx)
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + done, (uint32_t)word);
#else
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + done, word[0]);
#endif
    }
    HAL_FLASH_Lock();
    invalidate(addr, size);
    return st == HAL_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fsErase(const struct lfs_config *c, lfs_block_t block)
{
    FLASH_EraseInitTypeDef erase = { 0 };
    uint32_t bad = 0;
    HAL_StatusTypeDef st;

    (void)c;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FS_SECTOR0 + block;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
#if defined(STM32H753xx)
    erase.Banks = FLASH_BANK_2;
#endif
    HAL_FLASH_Unlock();
    st = HAL_FLASHEx_Erase(&erase, &bad);
    HAL_FLASH_Lock();
    invalidate(FS_BASE + block * FS_BLOCK, FS_BLOCK);
    return st == HAL_OK ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fsSync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

static int fsLock(const struct lfs_config *c)
{
    (void)c;
    return xSemaphoreTakeRecursive(gLock, portMAX_DELAY) == pdTRUE ? LFS_ERR_OK : LFS_ERR_IO;
}

static int fsUnlock(const struct lfs_config *c)
{
    (void)c;
    xSemaphoreGiveRecursive(gLock);
    return LFS_ERR_OK;
}

static const struct lfs_config gCfg = {
    .read = fsRead,
    .prog = fsProg,
    .erase = fsErase,
    .sync = fsSync,
    .lock = fsLock,
    .unlock = fsUnlock,
    .read_size = 1,
    .prog_size = FS_PROG,
    .block_size = FS_BLOCK,
    .block_count = FS_BLOCKS,
    .block_cycles = 500,
    .cache_size = FS_CACHE,
    .lookahead_size = FS_LOOKAHEAD,
    .read_buffer = gReadBuf,
    .prog_buffer = gProgBuf,
    .lookahead_buffer = gLookahead,
};

static void *opsOpen(const char *name, bool forWrite)
{
    fsHandle_t *h = (fsHandle_t *)calloc(1, sizeof(*h));
    int flags = forWrite ? (LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) : LFS_O_RDONLY;

    if (h == NULL) {
        return NULL;
    }
    h->cfg.buffer = h->cache;
    if (lfs_file_opencfg(&gLfs, &h->file, name, flags, &h->cfg) < 0) {
        free(h);
        return NULL;
    }
    return h;
}

static int32_t opsRead(void *handle, uint8_t *buf, size_t len)
{
    return (int32_t)lfs_file_read(&gLfs, &((fsHandle_t *)handle)->file, buf, (lfs_size_t)len);
}

static int32_t opsWrite(void *handle, const uint8_t *data, size_t len)
{
    return (int32_t)lfs_file_write(&gLfs, &((fsHandle_t *)handle)->file, data, (lfs_size_t)len);
}

static void opsClose(void *handle)
{
    lfs_file_close(&gLfs, &((fsHandle_t *)handle)->file);
    free(handle);
}

static bool opsRemove(const char *name)
{
    return lfs_remove(&gLfs, name) == LFS_ERR_OK;
}

static bool opsRename(const char *from, const char *to)
{
    return lfs_rename(&gLfs, from, to) == LFS_ERR_OK;
}

static int opsList(ucxapp_store_list_cb_t cb, void *ctx)
{
    lfs_dir_t dir;
    struct lfs_info info;
    int count = 0;

    if (lfs_dir_open(&gLfs, &dir, "/") < 0) {
        return -1;
    }
    while (lfs_dir_read(&gLfs, &dir, &info) > 0) {
        if (info.type == LFS_TYPE_REG) {
            cb(info.name, info.size, ctx);
            count++;
        }
    }
    lfs_dir_close(&gLfs, &dir);
    return count;
}

static const ucxapp_store_ops_t gFlashStore = { opsOpen, opsRead, opsWrite, opsClose, opsRemove, opsRename, opsList };

const ucxapp_store_ops_t *ucxapp_flashfs_mount(uint32_t *sizeKb)
{
    uint32_t imageEnd = (uint32_t)&_sidata + ((uint32_t)&_edata - (uint32_t)&_sdata);

    if (imageEnd > FS_BASE) {
        return NULL;   /* the firmware has grown into the file area */
    }
    if (gMounted) {
        if (sizeKb != NULL) {
            *sizeKb = FS_BLOCKS * FS_BLOCK / 1024U;
        }
        return &gFlashStore;
    }
    if (gLock == NULL) {
        gLock = xSemaphoreCreateRecursiveMutex();
    }
    if (gLock == NULL) {
        return NULL;
    }
    if (lfs_mount(&gLfs, &gCfg) != LFS_ERR_OK) {
        if (lfs_format(&gLfs, &gCfg) != LFS_ERR_OK || lfs_mount(&gLfs, &gCfg) != LFS_ERR_OK) {
            return NULL;
        }
    }
    gMounted = true;
    if (sizeKb != NULL) {
        *sizeKb = FS_BLOCKS * FS_BLOCK / 1024U;
    }
    return &gFlashStore;
}

bool ucxapp_flashfs_format(void)
{
    if (!gMounted) {
        return false;
    }
    lfs_unmount(&gLfs);
    return lfs_format(&gLfs, &gCfg) == LFS_ERR_OK && lfs_mount(&gLfs, &gCfg) == LFS_ERR_OK;
}

#else

const ucxapp_store_ops_t *ucxapp_flashfs_mount(uint32_t *sizeKb)
{
    (void)sizeKb;
    return NULL;
}

bool ucxapp_flashfs_format(void)
{
    return false;
}

#endif
