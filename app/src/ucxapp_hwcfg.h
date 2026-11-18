/* Board hardware profile loaded from JSON (see app/boards and scripts/hw_config.py). */
#ifndef UCXAPP_HWCFG_H
#define UCXAPP_HWCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HWCFG_MAX_GPIO 32
#define HWCFG_MAX_ADC  16
#define HWCFG_MAX_PWM  8
#define HWCFG_MAX_RES  16
#define HWCFG_MAX_EMU  8
#define HWCFG_PIN_LEN  8

typedef struct {
    char pin[HWCFG_PIN_LEN];
    char timer[16];
} hwcfgPwm_t;

typedef struct {
    int addr;
    uint8_t regs[256];
} hwcfgI2cDev_t;

typedef struct {
    char board[40];
    char gpio[HWCFG_MAX_GPIO][HWCFG_PIN_LEN];
    int nGpio;
    char adc[HWCFG_MAX_ADC][HWCFG_PIN_LEN];
    int nAdc;
    hwcfgPwm_t pwm[HWCFG_MAX_PWM];
    int nPwm;
    char reserved[HWCFG_MAX_RES][HWCFG_PIN_LEN];
    int nReserved;
    char i2cScl[HWCFG_PIN_LEN], i2cSda[HWCFG_PIN_LEN];
    char spiSck[HWCFG_PIN_LEN], spiMiso[HWCFG_PIN_LEN], spiMosi[HWCFG_PIN_LEN], spiCs[HWCFG_PIN_LEN];
    char canRx[HWCFG_PIN_LEN], canTx[HWCFG_PIN_LEN];
    char button[HWCFG_PIN_LEN];   /* the user button, one of gpio; "" when the board has none */
    int buttonPressed;            /* level the button reads when pressed */
    char led[HWCFG_PIN_LEN];      /* the user LED, one of gpio */
    hwcfgI2cDev_t emuI2c[HWCFG_MAX_EMU];
    int nEmuI2c;
} hwcfg_t;

/* Parses and validates JSON text; on failure returns false and fills err. */
bool ucxapp_hwcfg_parse(const char *json, hwcfg_t *cfg, char *err, size_t errSize);

/* Reads a file and parses it. */
bool ucxapp_hwcfg_load(const char *path, hwcfg_t *cfg, char *err, size_t errSize);

#endif
