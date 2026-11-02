# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 520 KB | 2048 KB | 25% | 320 KB | 512 KB | 63% |
| `f767` | NUCLEO-F767ZI | 502 KB | 2048 KB | 25% | 331 KB | 368 KB | 90% |
| `f439` | NUCLEO-F439ZI | 506 KB | 2048 KB | 25% | 137 KB | 192 KB | 72% |
| `f407` | STM32F4DISCOVERY | 503 KB | 1024 KB | 49% | 110 KB | 128 KB | 86% |
| `f401` | NUCLEO-F401RE | 470 KB | 512 KB | 92% | 89 KB | 96 KB | 93% |
| `f446` | NUCLEO-F446RE | 483 KB | 512 KB | 95% | 109 KB | 128 KB | 85% |
| `f103` | NUCLEO-F103RB | 119 KB | 128 KB | 93% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 124 KB | 128 KB | 97% | 25 KB | 36 KB | 72% |
