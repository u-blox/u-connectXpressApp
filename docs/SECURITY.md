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
Both keys come from the TRNG. HTTPS runs without an access token by default, so anyone who can reach the board on
the network can use the page; `web.start 443 token` adds a random token in the URL (`#t=`) from the same generator.

Limitations:

- Certificates start on the build date unless the clock was set (`app.time sync`); the web certificate is valid
  825 days, so firmware older than that serves an expired certificate until the clock is set.
- On Zephyr the certificates are kept in RAM, so a new CA and certificate are made after every reset; accept
  the browser's warning again.
- One TLS session needs about 35 KB of heap. The boards serve one browser at a time comfortably.

## Secure store

Private keys in the store (`*.key`) and saved Wi-Fi passwords and tokens are sealed with AES-256-GCM (mbedTLS,
the same code and file format on every target). Each sealed key file is `UCXS`, a version byte, a random 12-byte IV,
the cipher text and the 16-byte tag; the header and the file name are authenticated. The 256-bit install key
`ucxapp.key` is made from the random generator on first use. Only where that key is kept differs per target:

| Target | Install key | Protects against |
|---|---|---|
| Windows | `storage/ucxapp.key` wrapped with DPAPI (`CryptProtectData`, this user) | a copied `storage/` folder, another user, a disk read on another PC |
| Linux, macOS | `storage/ucxapp.key`, readable only by the owner | other users; not root or a copied disk |
| STM32 H753, F767 (FreeRTOS) | internal flash (littlefs), also when files are on the SD card | a pulled SD card; with readout protection level 1, reading the flash over SWD |
| Zephyr web boards | RAM (lost at reset, like the files) | nothing beyond RAM |

These STM32 parts (Cortex-M7/M4/M3/M0+) have no TrustZone or secure element, so readout protection is the hardware
barrier: set RDP level 1 with STM32CubeProgrammer (`-ob RDP=0xBB`) once the board is flashed; `utils.secure` shows
the level. Going back to level 0 erases the flash, keys included. Level 2 is permanent; do not use it on
development boards. `file.format` on the internal flash erases the install key, so sealed files on the SD card
can no longer be opened.

A key from an older version (a raw 32-byte `ucxapp.key`) is used as it is and wrapped on Windows the first time it
is read. Plain `*.key` files are sealed at start. `utils.secure` lists the key protection and every key file.

## Saved Wi-Fi passwords on boards without crypto

Boards built without mbedTLS (`UCXAPP_SECURE`) only obfuscate saved Wi-Fi passwords; anyone with the board can read
them. `wifi.connect` and `wifi.profile.add` print a note about it, except on the lite builds (NUCLEO-F103RB,
NUCLEO-G071RB), whose 128 KB flash has no room for the message: this section is their notice.

## Matter node

The Matter node (`matter.*`) is the optional plug-in `ucxmatter.dll` / `libucxmatter.so`, built from `matter/`
with the Matter SDK. It is not part of the app's releases; whoever builds and hands it on ships the components
`app.sbom` lists for it (the plug-in reports them itself: Matter SDK, its mbedTLS, Pigweed, nlassert, nlio and on
Windows the GCC and MinGW-w64 runtime, with versions and commits). The points below are pinned to tests in
`tests/test_cra.py`.

| Point | How the node meets it |
|---|---|
| Setup codes | Discriminator, passcode, SPAKE2+ salt and UniqueID are drawn at random the first time a node starts, never the SDK's test codes (3840, 20202021) and never from the module's address. A failed draw is an error: the node does not start, and there are no stand-in codes. `matter.reset all` draws new ones |
| Random numbers | The Matter SDK's generator: CTR_DRBG of the SDK's mbedTLS, seeded by the operating system (`CryptGenRandom` on Windows, `getrandom()` on Linux). It also makes the session keys and the operational key pair |
| Stored secrets | Fabric keys and certificates, the Wi-Fi password a commissioner gave, the setup codes: all in the app's secure store (`matter.kvs.key`, `matter.config.key`, `matter.factory.key`), sealed as described under Secure store. The plug-in has no file access of its own |
| Log and trace | The passcode is shown by `matter.status`, `matter.qr` and the card to the user who asks, and is in neither the log (`matter.log detail` included) nor an AT trace. The Wi-Fi password is hidden in the AT trace like the app's own |
| Communication | Commissioning runs over PASE (SPAKE2+ with the passcode), everything after over CASE sessions with the fabric's certificates; AES-CCM on every message. The Bluetooth LE link itself is unpaired, as Matter defines it: the Matter session inside protects the data |
| Access | Without a fabric the commissioning window is open for 15 minutes, then the node stops advertising until `matter.open`. A commissioned node advertises only after `matter.open`. A Wi-Fi network with a password is joined with WPA2 or better |
| Malformed input | Datagrams and Bluetooth LE writes go to the SDK's parsers; junk is dropped and the node goes on. A file that is no plug-in, or one for another version of the app, is refused |
| Stop | `matter.stop` and leaving the app take the advertisement, the node's mDNS services and its socket off the module; the module's bonds, Bluetooth mode, name and stored configuration are never changed by a running node |
| Removing data | `matter.reset` forgets every fabric, removes what the node stored, disconnects the module from the Wi-Fi network the node used and reboots it; the setup codes stay, as the label on a product does. `matter.reset all` removes them too and factory-resets the module (`AT+USYFR`), so the network and its password are gone from it as well |
| Self-test | `matter.selftest` checks the plug-in, the module's roles, that the store seals what the node saves, the random generator, that the setup codes are the node's own and not the test codes, Bluetooth LE and the network |

Limitations:

- **Attestation certificates are the Matter SDK's test ones** (vendor 0xFFF1). Their private key is public, so
  attestation proves nothing and home apps show the node as a test device. A product needs its own vendor ID,
  DAC and PAI, with the DAC key in the secure store.
- **The SDK's mbedTLS is 2.28**, the version the Matter SDK pins; `app.sbom` shows the exact one. It is a
  long-term-support branch of its own, older than the app's mbedTLS: check it against the Mbed TLS security
  advisories before handing a plug-in on.
- **Key log builds.** `ucxapp --matter --keylog --build` makes a debug plug-in that writes the node's session
  keys to the file `UCX_KEYLOG_FILE` names, for Wireshark. `matter.status` and the log say `KEY LOG BUILD`.
  A plug-in built without `--keylog` contains no such code. Never hand a key log build on.
- **Boards.** The plug-in runs on Windows and Linux today. On an STM32 board the SDK's generator needs the
  hardware TRNG as its entropy source, as the app's own has, before a node may draw setup codes there.
- The module's mDNS answers queries but has no subtypes, and Thread waits for module firmware.

