/* hw.*: the board's pins behind one backend. The host build emulates them (a board profile from hw.profile.load
 * names the pins); firmware for a board defines UCXAPP_HW_BOARD and gives its real GPIO as ucxapp_hw_board. */
#ifndef UCXAPP_HW_H
#define UCXAPP_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { UCXAPP_GPIO_IN, UCXAPP_GPIO_OUT, UCXAPP_GPIO_PULLUP, UCXAPP_GPIO_PULLDOWN };

typedef struct {
    const char *name;
    bool (*gpioMode)(int pin, int mode);
    bool (*gpioWrite)(int pin, int level);
    int (*gpioRead)(int pin);
    /* write wl bytes, then read rl bytes (repeated start); returns 0 on ACK, negative on NACK */
    int (*i2cXfer)(int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl);
    bool (*spiXfer)(const uint8_t *tx, uint8_t *rx, size_t n);
    int (*adcRead)(int ch);   /* 12-bit raw, negative on error */
    bool (*pwmSet)(int ch, uint32_t hz, int permille);
} ucxapp_hw_backend_t;

/* A board's own pins (firmware with UCXAPP_HW_BOARD): the backend, its GPIO names and the user button and LED
   (indexes into the names, -1 for none). */
typedef struct {
    const ucxapp_hw_backend_t *backend;
    const char *board;
    const char *const *gpio;
    int nGpio;
    int button;
    int buttonPressed;   /* level the button reads when pressed */
    int led;
} ucxapp_hw_board_t;

#ifdef UCXAPP_HW_BOARD
extern const ucxapp_hw_board_t ucxapp_hw_board;
#endif

/* The user button: 1 pressed, 0 released, -1 when the board (or the loaded profile) has none. */
int ucxapp_hw_button(void);

/* Drives the user LED; false when there is none. */
bool ucxapp_hw_led(bool on);

#ifdef __cplusplus
}
#endif

#endif
