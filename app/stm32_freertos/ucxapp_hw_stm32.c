/* hw.* on the board's own pins: GPIO from the board profile (app/boards/<board>.json, turned into hw_config.h by
 * scripts/hw_config.py at configure time), with the user button and LED as the profile names them. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw_config.h"
#include "ucxapp_hw.h"

#if defined(STM32H753xx)
#include "stm32h7xx_hal.h"
#define GPIO_CLK_EN(n) (RCC->AHB4ENR |= (1u << (n)))
#elif defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#define GPIO_CLK_EN(n) (RCC->AHB1ENR |= (1u << (n)))
#else
#include "stm32f4xx_hal.h"
#define GPIO_CLK_EN(n) (RCC->AHB1ENR |= (1u << (n)))
#endif

typedef struct {
    uint8_t port;   /* 0 = A */
    uint8_t pin;
} hwPin_t;

#define PIN_ENTRY(p, n) { (uint8_t)(#p[0] - 'A'), n },
#define PIN_NAME(p, n) #p #n,
static const hwPin_t kPins[] = { HW_GPIO_LIST(PIN_ENTRY) };
static const char *const kNames[] = { HW_GPIO_LIST(PIN_NAME) };
#define N_PINS ((int)(sizeof(kPins) / sizeof(kPins[0])))

static GPIO_TypeDef *portOf(int p)
{
    /* GPIOA..GPIOK are 0x400 apart on the F4, F7 and H7 */
    return (GPIO_TypeDef *)(GPIOA_BASE + 0x400u * kPins[p].port);
}

static bool gpioMode(int p, int mode)
{
    GPIO_InitTypeDef init = { 0 };

    if (p < 0 || p >= N_PINS) {
        return false;
    }
    GPIO_CLK_EN(kPins[p].port);
    (void)RCC->AHB1ENR;
    init.Pin = 1u << kPins[p].pin;
    init.Mode = mode == UCXAPP_GPIO_OUT ? GPIO_MODE_OUTPUT_PP : GPIO_MODE_INPUT;
    init.Pull = mode == UCXAPP_GPIO_PULLUP ? GPIO_PULLUP : mode == UCXAPP_GPIO_PULLDOWN ? GPIO_PULLDOWN : GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(portOf(p), &init);
    return true;
}

static bool gpioWrite(int p, int level)
{
    if (p < 0 || p >= N_PINS) {
        return false;
    }
    HAL_GPIO_WritePin(portOf(p), (uint16_t)(1u << kPins[p].pin), level ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

static int gpioRead(int p)
{
    GPIO_TypeDef *g;
    uint32_t bit;

    if (p < 0 || p >= N_PINS) {
        return -1;
    }
    g = portOf(p);
    bit = 1u << kPins[p].pin;
    /* an output reads back what it drives */
    if (((g->MODER >> (2u * kPins[p].pin)) & 3u) == 1u) {
        return (g->ODR & bit) ? 1 : 0;
    }
    return (g->IDR & bit) ? 1 : 0;
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
