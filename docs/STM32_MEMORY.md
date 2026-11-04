# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 491 KB | 2048 KB | 24% | 309 KB | 512 KB | 60% |
| `f767` | NUCLEO-F767ZI | 480 KB | 2048 KB | 23% | 323 KB | 368 KB | 88% |
| `f439` | NUCLEO-F439ZI | 483 KB | 2048 KB | 24% | 126 KB | 192 KB | 66% |
| `f407` | STM32F4DISCOVERY | 480 KB | 1024 KB | 47% | 103 KB | 128 KB | 81% |
| `f401` | NUCLEO-F401RE | 453 KB | 512 KB | 89% | 83 KB | 96 KB | 87% |
| `f446` | NUCLEO-F446RE | 465 KB | 512 KB | 91% | 102 KB | 128 KB | 80% |
| `f103` | NUCLEO-F103RB | 122 KB | 128 KB | 96% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
