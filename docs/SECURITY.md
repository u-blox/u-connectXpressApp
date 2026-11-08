# Security notes

## Random numbers

Every key, certificate serial, TLS nonce and web access token in u-connectXpressApp comes from the PSA Crypto
random generator of mbedTLS (`psa_generate_random`, see `ucxapp_tls_random` in `app/src/ucxapp_tls.c`). That
generator is a CTR_DRBG (NIST SP 800-90A, AES-256), seeded and reseeded from the platform's entropy source below.
If the entropy source fails, `psa_crypto_init()` fails, and so do key generation, `web.start` and every other
command that needs randomness. **No platform falls back to a weak source** (time, counters or `rand()`).

| Platform | Entropy source | Kind |
|---|---|---|
| Windows | `BCryptGenRandom` (mbedTLS built-in) | OS CSPRNG, fed by the CPU and system TRNGs |
| Linux | `getrandom()` (mbedTLS built-in) | OS CSPRNG (kernel), fed by hardware sources |
| Browser (WebAssembly) | Emscripten `getrandom()` → `crypto.getRandomValues()` | browser CSPRNG |
| STM32 FreeRTOS: NUCLEO-H753ZI, NUCLEO-F767ZI | RNG peripheral (`app/stm32_freertos/ucxapp_rng_stm32.c`) | **hardware TRNG** |
| STM32 Zephyr: NUCLEO-H753ZI, NUCLEO-F767ZI | RNG peripheral through `zephyr,entropy` (`app/zephyr/ucxapp_rng_zephyr.c`) | **hardware TRNG** |

On the STM32 boards the mbedTLS configuration (`app/mbedtls_embedded_crypto.h`) turns off the built-in entropy
source and takes it only from the TRNG (`MBEDTLS_PSA_DRIVER_GET_ENTROPY`). The FreeRTOS driver:

- fails on the RNG's own seed error (SECS) and clock error (CECS) flags,
- discards the first word after start-up,
- runs a continuous test that rejects two equal 32-bit words in a row,
- returns `PSA_ERROR_INSUFFICIENT_ENTROPY` on any failure.

The Zephyr driver relies on Zephyr's STM32 entropy driver, which checks the same flags.

### Boards without TLS

The other boards have no TLS at all, so no keys are generated on them:

- **NUCLEO-F439ZI and STM32F4DISCOVERY.** They have a TRNG, but their 128–192 KB of RAM cannot hold the page
  buffers and a TLS session next to the app.
- **NUCLEO-F401RE.** No RNG peripheral.
- **NUCLEO-F446RE.** Not enough RAM.
- **Lite builds (NUCLEO-F103RB, NUCLEO-G071RB).** Not enough RAM or flash.

None of these boards serves the web UI. The module (NORA-W36) handles its own TLS for `mqtt`, `net` and
`sock` with its own random source.

## Web UI on the STM32 boards

The H753 and F767 serve the web UI over HTTPS only; `web.start ... http` is refused. On the first `web.start`
the board makes an ECDSA P-256 key and a certificate for the module's address, signed by a local CA (`tls-ca`).
Both keys come from the TRNG. The access token in the URL (`#t=`) comes from the same generator.

Limitations:

- Certificates are valid from the build date, because the boards have no set clock.
- On Zephyr the certificates are kept in RAM, so a new CA and certificate are made after every reset; accept
  the browser's warning again.
- One TLS session needs about 35 KB of heap. The boards serve one browser at a time comfortably.

## Saved Wi-Fi passwords on boards without crypto

Boards built without `UCXAPP_CRYPTO` (no mbedTLS) only obfuscate saved Wi-Fi passwords; anyone with the board can read
them. `wifi.connect` and `wifi.profile.add` print a note about it, except on the lite builds (NUCLEO-F103RB,
NUCLEO-G071RB), whose 128 KB flash has no room for the message: this section is their notice.

