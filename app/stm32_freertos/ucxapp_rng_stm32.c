/* Entropy for mbedTLS (MBEDTLS_PSA_DRIVER_GET_ENTROPY) from the STM32 hardware true random number generator.
 *
 * The RNG peripheral samples free-running ring oscillators. Its own checks flag a seed error (SECS: too few or
 * too many equal bits in a row) and a clock error (CECS); either makes this function fail, so psa_crypto_init()
 * and every key or token request fail too. There is no fallback to a software source. A continuous test also
 * rejects two equal 32-bit words in a row, and the first word after start-up is discarded.
 * The 48 MHz RNG clock is PLLQ on the F4/F7 (main_stm32.c) and HSI48 on the H7 (switched on here). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(STM32H753xx)
#include "stm32h7xx_hal.h"
#elif defined(STM32F767xx)
#include "stm32f7xx_hal.h"
#else
#include "stm32f4xx_hal.h"
#endif

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

static bool gStarted;
static uint32_t gLast;

static void rngStart(void)
{
#if defined(STM32H753xx)
    RCC->CR |= RCC_CR_HSI48ON;
    for (uint32_t i = 0; i < 1000000U && !(RCC->CR & RCC_CR_HSI48RDY); i++) {
    }
#endif
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    (void)RCC->AHB2ENR;
    RNG->CR &= ~RNG_CR_RNGEN;
    RNG->SR = 0;
    RNG->CR |= RNG_CR_RNGEN;
}

/* One 32-bit word, or false when the generator reports an error or stalls. */
static bool rngWord(uint32_t *out)
{
    for (uint32_t i = 0; i < 100000U; i++) {
        uint32_t sr = RNG->SR;
        if (sr & (RNG_SR_SECS | RNG_SR_CECS)) {
            return false;
        }
        if (sr & RNG_SR_DRDY) {
            *out = RNG->DR;
            return true;
        }
    }
    return false;
}

static bool rngNext(uint32_t *out)
{
    uint32_t w;

    if (!gStarted) {
        rngStart();
        if (!rngWord(&gLast)) {   /* discarded: the first word after enabling */
            return false;
        }
        gStarted = true;
    }
    if (!rngWord(&w) || w == gLast) {
        gStarted = false;   /* restart on the next request; this one fails */
        return false;
    }
    gLast = w;
    *out = w;
    return true;
}

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size)
{
    if (flags != 0) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    for (size_t done = 0; done < output_size; done += sizeof(uint32_t)) {
        uint32_t w;
        size_t n = output_size - done < sizeof(w) ? output_size - done : sizeof(w);
        if (!rngNext(&w)) {
            memset(output, 0, output_size);
            *estimate_bits = 0;
            return PSA_ERROR_INSUFFICIENT_ENTROPY;
        }
        memcpy(output + done, &w, n);
    }
    *estimate_bits = 8 * output_size;
    return 0;
}

/* Millisecond clock for mbedTLS (MBEDTLS_PLATFORM_MS_TIME_ALT): the FreeRTOS tick. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)xTaskGetTickCount() * 1000 / configTICK_RATE_HZ;
}
