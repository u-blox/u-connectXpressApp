/* hw.* on the board's own pins under Zephyr: GPIO from the board profile (app/boards/<board>.json, turned into
 * hw_config.h by scripts/hw_config.py at configure time) through the gpio API, with the user button and LED as the
 * profile names them. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "hw_config.h"
#include "ucxapp_hw.h"

typedef struct {
    uint8_t port;   /* 0 = A */
    uint8_t pin;
} hwPin_t;

#define PIN_ENTRY(p, n) { (uint8_t)(#p[0] - 'A'), n },
#define PIN_NAME(p, n) #p #n,
static const hwPin_t kPins[] = { HW_GPIO_LIST(PIN_ENTRY) };
static const char *const kNames[] = { HW_GPIO_LIST(PIN_NAME) };
#define N_PINS ((int)(sizeof(kPins) / sizeof(kPins[0])))

#define PORT(l) DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio##l))
static const struct device *const kPorts[] = { PORT(a), PORT(b), PORT(c), PORT(d), PORT(e), PORT(f),
                                               PORT(g), PORT(h), PORT(i), PORT(j), PORT(k) };

static const struct device *portOf(int p)
{
    if (p < 0 || p >= N_PINS || kPins[p].port >= sizeof(kPorts) / sizeof(kPorts[0])) {
        return NULL;
    }
    return kPorts[kPins[p].port] != NULL && device_is_ready(kPorts[kPins[p].port]) ? kPorts[kPins[p].port] : NULL;
}

static bool gpioMode(int p, int mode)
{
    const struct device *dev = portOf(p);
    gpio_flags_t flags = GPIO_INPUT;

    if (dev == NULL) {
        return false;
    }
    if (mode == UCXAPP_GPIO_OUT) {
        flags = GPIO_OUTPUT;
    } else if (mode == UCXAPP_GPIO_PULLUP) {
        flags = GPIO_INPUT | GPIO_PULL_UP;
    } else if (mode == UCXAPP_GPIO_PULLDOWN) {
        flags = GPIO_INPUT | GPIO_PULL_DOWN;
    }
    return gpio_pin_configure(dev, kPins[p].pin, flags) == 0;
}

static bool gpioWrite(int p, int level)
{
    const struct device *dev = portOf(p);

    return dev != NULL && gpio_pin_set_raw(dev, kPins[p].pin, level ? 1 : 0) == 0;
}

static int gpioRead(int p)
{
    const struct device *dev = portOf(p);

    /* on the STM32 the input register shows the level an output drives as well */
    return dev == NULL ? -1 : gpio_pin_get_raw(dev, kPins[p].pin);
}

static int i2cXfer(int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    (void)addr;
    (void)w;
    (void)wl;
    (void)r;
    (void)rl;
    return -1;
}

static bool spiXfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    (void)tx;
    (void)rx;
    (void)n;
    return false;
}

static int adcRead(int ch)
{
    (void)ch;
    return -1;
}

static bool pwmSet(int ch, uint32_t hz, int permille)
{
    (void)ch;
    (void)hz;
    (void)permille;
    return false;
}

static const ucxapp_hw_backend_t kBoard = { "board gpio", gpioMode, gpioWrite, gpioRead, i2cXfer, spiXfer, adcRead,
                                            pwmSet };

const ucxapp_hw_board_t ucxapp_hw_board = {
    &kBoard, HW_BOARD_NAME, kNames, N_PINS,
#ifdef HW_BUTTON_INDEX
    HW_BUTTON_INDEX, HW_BUTTON_PRESSED,
#else
    -1, 1,
#endif
#ifdef HW_LED_INDEX
    HW_LED_INDEX,
#else
    -1,
#endif
};
