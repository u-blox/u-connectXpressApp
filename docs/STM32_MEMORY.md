# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 538 KB | 2048 KB | 26% | 320 KB | 512 KB | 63% |
| `f767` | NUCLEO-F767ZI | 525 KB | 2048 KB | 26% | 331 KB | 368 KB | 90% |
| `f439` | NUCLEO-F439ZI | 528 KB | 2048 KB | 26% | 137 KB | 192 KB | 72% |
| `f407` | STM32F4DISCOVERY | 525 KB | 1024 KB | 51% | 110 KB | 128 KB | 86% |
| `f401` | NUCLEO-F401RE | 493 KB | 512 KB | 96% | 89 KB | 96 KB | 94% |
| `f446` | NUCLEO-F446RE | 506 KB | 512 KB | 99% | 109 KB | 128 KB | 86% |
| `f103` | NUCLEO-F103RB | 123 KB | 128 KB | 96% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
