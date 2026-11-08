/* Entropy for mbedTLS (MBEDTLS_PSA_DRIVER_GET_ENTROPY) on Zephyr: the zephyr,entropy device, which is the STM32
 * RNG peripheral (hardware true random number generator) on every web board (boards/<board>.overlay). Zephyr's
 * STM32 entropy driver checks the seed and clock error flags and fails instead of returning weak data; this
 * function then fails too, and so do psa_crypto_init() and every key or token. There is no software fallback.
 * See docs/SECURITY.md (Random numbers). */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size)
{
    const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));

    (void)flags;
    *estimate_bits = 0;
    if (!device_is_ready(dev) || output_size > UINT16_MAX ||
        entropy_get_entropy(dev, output, (uint16_t)output_size) != 0) {
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    }
    *estimate_bits = output_size * 8;
    return 0;
}

/* Millisecond clock for mbedTLS (MBEDTLS_PLATFORM_MS_TIME_ALT). */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)k_uptime_get();
}
