/* FAT SD card over bit-banged SPI (mode 0) on the board's "spi" pins, used as the file store when a card is present. */

#include "ucxapp_sdcard.h"

#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#if defined(STM32H753xx)
#include "stm32h7xx_hal.h"
#elif defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#elif defined(STM32F439xx) || defined(STM32F407xx) || defined(STM32F401xE) || defined(STM32F446xx)
#include "stm32f4xx_hal.h"
#endif

#if defined(STM32H753xx) || defined(STM32F767xx) || defined(STM32F439xx)
/* Zio CN9: PE2 SCK, PE5 MISO, PE6 MOSI, PE4 CS (app/boards/<board>.json "spi"). */
#define SD_PORT      GPIOE
#define SD_CLK_ON()  __HAL_RCC_GPIOE_CLK_ENABLE()
#define SD_SCK       GPIO_PIN_2
#define SD_MISO      GPIO_PIN_5
#define SD_MOSI      GPIO_PIN_6
#define SD_CS        GPIO_PIN_4
#elif defined(STM32F407xx) || defined(STM32F401xE) || defined(STM32F446xx)
/* PB13 SCK, PB14 MISO, PB15 MOSI, PB12 CS (the F407 Discovery PA5-PA7 bus belongs to the accelerometer). */
#define SD_PORT      GPIOB
#define SD_CLK_ON()  __HAL_RCC_GPIOB_CLK_ENABLE()
#define SD_SCK       GPIO_PIN_13
#define SD_MISO      GPIO_PIN_14
#define SD_MOSI      GPIO_PIN_15
#define SD_CS        GPIO_PIN_12
#endif

#ifdef SD_PORT

#include "ff.h"
#include "diskio.h"

#define CMD0   0U
#define CMD1   1U
#define CMD8   8U
#define CMD9   9U
#define CMD12  12U
#define CMD16  16U
#define CMD17  17U
#define CMD18  18U
#define CMD24  24U
#define CMD25  25U
#define CMD55  55U
#define CMD58  58U
#define ACMD41 (0x80U | 41U)

#define CT_MMC   0x01U
#define CT_SD1   0x02U
#define CT_SD2   0x04U
#define CT_BLOCK 0x08U

static uint8_t gType;
static DSTATUS gStat = STA_NOINIT;
static bool gSlow;
static FATFS gFs;
static SemaphoreHandle_t gLock;

/* ---- SPI ---- */

static void halfBit(void)
{
    if (gSlow) {
        for (volatile int i = 0; i < 200; i++) {
        }
    }
}

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;

    for (int i = 0; i < 8; i++) {
        SD_PORT->BSRR = (out & 0x80U) ? SD_MOSI : ((uint32_t)SD_MOSI << 16);
        out = (uint8_t)(out << 1);
        halfBit();
        SD_PORT->BSRR = SD_SCK;
        in = (uint8_t)((in << 1) | ((SD_PORT->IDR & SD_MISO) ? 1U : 0U));
        halfBit();
        SD_PORT->BSRR = (uint32_t)SD_SCK << 16;
    }
    return in;
}

static bool elapsed(TickType_t start, uint32_t ms)
{
    return (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(ms);
}

static bool waitReady(uint32_t ms)
{
    TickType_t start = xTaskGetTickCount();

    while (xfer(0xFF) != 0xFF) {
        if (elapsed(start, ms)) {
            return false;
        }
    }
    return true;
}

static void deselect(void)
{
    SD_PORT->BSRR = SD_CS;
    (void)xfer(0xFF);
}

static bool selectCard(void)
{
    SD_PORT->BSRR = (uint32_t)SD_CS << 16;
    (void)xfer(0xFF);
    if (waitReady(500)) {
        return true;
    }
    deselect();
    return false;
}

static uint8_t sendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t r;
    int n = 10;

    if (cmd & 0x80U) {
        cmd &= 0x7FU;
        r = sendCmd(CMD55, 0);
        if (r > 1U) {
            return r;
        }
    }
    deselect();
    if (!selectCard()) {
        return 0xFF;
    }
    (void)xfer((uint8_t)(0x40U | cmd));
    (void)xfer((uint8_t)(arg >> 24));
    (void)xfer((uint8_t)(arg >> 16));
    (void)xfer((uint8_t)(arg >> 8));
    (void)xfer((uint8_t)arg);
    (void)xfer(cmd == CMD0 ? 0x95U : cmd == CMD8 ? 0x87U : 0x01U);
    if (cmd == CMD12) {
        (void)xfer(0xFF);
    }
    do {
        r = xfer(0xFF);
    } while ((r & 0x80U) && --n > 0);
    return r;
}

static bool rcvBlock(uint8_t *buf, uint32_t len)
{
    TickType_t start = xTaskGetTickCount();
    uint8_t token;

    do {
        token = xfer(0xFF);
    } while (token == 0xFF && !elapsed(start, 200));
    if (token != 0xFE) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = xfer(0xFF);
    }
    (void)xfer(0xFF);
    (void)xfer(0xFF);
    return true;
}

static bool xmitBlock(const uint8_t *buf, uint8_t token)
{
    if (!waitReady(500)) {
        return false;
    }
    (void)xfer(token);
    if (token == 0xFD) {
        return true;
    }
    for (uint32_t i = 0; i < 512U; i++) {
        (void)xfer(buf[i]);
    }
    (void)xfer(0xFF);
    (void)xfer(0xFF);
    return (xfer(0xFF) & 0x1FU) == 0x05U;
}

static void pinsInit(void)
{
    GPIO_InitTypeDef io = { 0 };

    SD_CLK_ON();
    SD_PORT->BSRR = SD_CS;
    io.Pin = SD_SCK | SD_MOSI | SD_CS;
    io.Mode = GPIO_MODE_OUTPUT_PP;
    io.Pull = GPIO_NOPULL;
    io.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SD_PORT, &io);
    io.Pin = SD_MISO;
    io.Mode = GPIO_MODE_INPUT;
    io.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SD_PORT, &io);
}

/* ---- FatFs disk interface ---- */

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == 0 ? gStat : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    uint8_t ocr[4];
    uint8_t cmd;
    TickType_t start;

    if (pdrv != 0) {
        return STA_NOINIT;
    }
    pinsInit();
    gSlow = true;
    gType = 0;
    for (int i = 0; i < 10; i++) {
        (void)xfer(0xFF);
    }
    if (sendCmd(CMD0, 0) == 1U) {
        start = xTaskGetTickCount();
        if (sendCmd(CMD8, 0x1AA) == 1U) {
            for (int i = 0; i < 4; i++) {
                ocr[i] = xfer(0xFF);
            }
            if (ocr[2] == 0x01U && ocr[3] == 0xAAU) {
                while (!elapsed(start, 1000) && sendCmd(ACMD41, 1UL << 30) != 0U) {
                }
                if (!elapsed(start, 1000) && sendCmd(CMD58, 0) == 0U) {
                    for (int i = 0; i < 4; i++) {
                        ocr[i] = xfer(0xFF);
                    }
                    gType = (ocr[0] & 0x40U) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
            }
        } else {
            if (sendCmd(ACMD41, 0) <= 1U) {
                gType = CT_SD1;
                cmd = ACMD41;
            } else {
                gType = CT_MMC;
                cmd = CMD1;
            }
            while (!elapsed(start, 1000) && sendCmd(cmd, 0) != 0U) {
            }
            if (elapsed(start, 1000) || sendCmd(CMD16, 512) != 0U) {
                gType = 0;
            }
        }
    }
    deselect();
    gSlow = false;
    gStat = gType != 0U ? 0 : STA_NOINIT;
    return gStat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || count == 0U) {
        return RES_PARERR;
    }
    if (gStat & STA_NOINIT) {
        return RES_NOTRDY;
    }
    if (!(gType & CT_BLOCK)) {
        sector *= 512U;
    }
    if (count == 1U) {
        if (sendCmd(CMD17, sector) == 0U && rcvBlock(buff, 512)) {
            count = 0;
        }
    } else if (sendCmd(CMD18, sector) == 0U) {
        while (count > 0U && rcvBlock(buff, 512)) {
            buff += 512;
            count--;
        }
        (void)sendCmd(CMD12, 0);
    }
    deselect();
    return count == 0U ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || count == 0U) {
        return RES_PARERR;
    }
    if (gStat & STA_NOINIT) {
        return RES_NOTRDY;
    }
    if (!(gType & CT_BLOCK)) {
        sector *= 512U;
    }
    if (count == 1U) {
        if (sendCmd(CMD24, sector) == 0U && xmitBlock(buff, 0xFE)) {
            count = 0;
        }
    } else if (sendCmd(CMD25, sector) == 0U) {
        while (count > 0U && xmitBlock(buff, 0xFC)) {
            buff += 512;
            count--;
        }
        if (!xmitBlock(NULL, 0xFD)) {
            count = 1;
        }
    }
    deselect();
    return count == 0U ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    uint8_t csd[16];
    DRESULT res = RES_ERROR;

    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (gStat & STA_NOINIT) {
        return RES_NOTRDY;
    }
    switch (cmd) {
        case CTRL_SYNC:
            if (selectCard()) {
                res = RES_OK;
            }
            break;
        case GET_SECTOR_COUNT:
            if (sendCmd(CMD9, 0) == 0U && rcvBlock(csd, 16)) {
                if ((csd[0] >> 6) == 1U) {
                    DWORD size = (DWORD)csd[9] + ((DWORD)csd[8] << 8) + ((DWORD)(csd[7] & 0x3FU) << 16) + 1U;
                    *(DWORD *)buff = size << 10;
                } else {
                    uint32_t n = (csd[5] & 15U) + ((csd[10] & 128U) >> 7) + ((csd[9] & 3U) << 1) + 2U;
                    DWORD size = (DWORD)(csd[8] >> 6) + ((DWORD)csd[7] << 2) + ((DWORD)(csd[6] & 3U) << 10) + 1U;
                    *(DWORD *)buff = size << (n - 9U);
                }
                res = RES_OK;
            }
            break;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            res = RES_OK;
            break;
        default:
            res = RES_PARERR;
            break;
    }
    deselect();
    return res;
}

/* ---- store backend ---- */

void *ff_memalloc(UINT msize)
{
    return malloc(msize);
}

void ff_memfree(void *mblock)
{
    free(mblock);
}

static void lock(void)
{
    (void)xSemaphoreTakeRecursive(gLock, portMAX_DELAY);
}

static void unlock(void)
{
    (void)xSemaphoreGiveRecursive(gLock);
}

static void *opsOpen(const char *name, bool forWrite)
{
    FIL *f = (FIL *)calloc(1, sizeof(*f));
    BYTE mode = forWrite ? (FA_WRITE | FA_CREATE_ALWAYS) : FA_READ;
    FRESULT fr;

    if (f == NULL) {
        return NULL;
    }
    lock();
    fr = f_open(f, name, mode);
    unlock();
    if (fr != FR_OK) {
        free(f);
        return NULL;
    }
    return f;
}

static int32_t opsRead(void *handle, uint8_t *buf, size_t len)
{
    UINT n = 0;
    FRESULT fr;

    lock();
    fr = f_read((FIL *)handle, buf, (UINT)len, &n);
    unlock();
    return fr == FR_OK ? (int32_t)n : -1;
}

static int32_t opsWrite(void *handle, const uint8_t *data, size_t len)
{
    UINT n = 0;
    FRESULT fr;

    lock();
    fr = f_write((FIL *)handle, data, (UINT)len, &n);
    unlock();
    return fr == FR_OK ? (int32_t)n : -1;
}

static void opsClose(void *handle)
{
    lock();
    (void)f_close((FIL *)handle);
    unlock();
    free(handle);
}

static bool opsRemove(const char *name)
{
    FRESULT fr;

    lock();
    fr = f_unlink(name);
    unlock();
    return fr == FR_OK;
}

static bool opsRename(const char *from, const char *to)
{
    FRESULT fr;

    lock();
    (void)f_unlink(to);
    fr = f_rename(from, to);
    unlock();
    return fr == FR_OK;
}

static int opsList(ucxapp_store_list_cb_t cb, void *ctx)
{
    DIR dir;
    FILINFO info;
    int count = 0;

    lock();
    if (f_opendir(&dir, "") != FR_OK) {
        unlock();
        return -1;
    }
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
        if (!(info.fattrib & (AM_DIR | AM_HID | AM_SYS))) {
            cb(info.fname, info.fsize, ctx);
            count++;
        }
    }
    (void)f_closedir(&dir);
    unlock();
    return count;
}

static const ucxapp_store_ops_t gSdStore = { opsOpen, opsRead, opsWrite, opsClose, opsRemove, opsRename, opsList };

const ucxapp_store_ops_t *ucxapp_sdcard_mount(uint32_t *sizeMb)
{
    DWORD sectors = 0;
    FRESULT fr;

    if (gLock == NULL) {
        gLock = xSemaphoreCreateRecursiveMutex();
        if (gLock == NULL) {
            return NULL;
        }
    }
    lock();
    (void)f_mount(NULL, "", 0);
    gStat = STA_NOINIT;
    fr = f_mount(&gFs, "", 1);
    if (fr == FR_OK && sizeMb != NULL && disk_ioctl(0, GET_SECTOR_COUNT, &sectors) == RES_OK) {
        *sizeMb = (uint32_t)(sectors / 2048U);
    }
    unlock();
    /* A card without a FAT volume is left alone; file.format creates one. */
    return fr == FR_OK ? &gSdStore : NULL;
}

bool ucxapp_sdcard_format(void)
{
    uint8_t *work = (uint8_t *)malloc(_MAX_SS);
    FRESULT fr = FR_NOT_ENOUGH_CORE;

    if (work == NULL || gLock == NULL) {
        free(work);
        return false;
    }
    lock();
    (void)f_mount(NULL, "", 0);
    fr = f_mkfs("", FM_ANY, 0, work, _MAX_SS);
    if (fr == FR_OK) {
        fr = f_mount(&gFs, "", 1);
    }
    unlock();
    free(work);
    return fr == FR_OK;
}

#else

const ucxapp_store_ops_t *ucxapp_sdcard_mount(uint32_t *sizeMb)
{
    (void)sizeMb;
    return NULL;
}

bool ucxapp_sdcard_format(void)
{
    return false;
}

#endif
