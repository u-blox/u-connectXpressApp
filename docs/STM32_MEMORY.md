# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 910 KB | 2048 KB | 44% | 418 KB | 512 KB | 82% |
| `f767` | NUCLEO-F767ZI | 899 KB | 2048 KB | 44% | 349 KB | 368 KB | 95% |
| `f439` | NUCLEO-F439ZI | 561 KB | 2048 KB | 27% | 134 KB | 192 KB | 70% |
| `f407` | STM32F4DISCOVERY | 550 KB | 1024 KB | 54% | 108 KB | 128 KB | 85% |
| `f401` | NUCLEO-F401RE | 501 KB | 512 KB | 98% | 85 KB | 96 KB | 89% |
| `f446` | NUCLEO-F446RE | 509 KB | 512 KB | 100% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 127 KB | 128 KB | 100% | 15 KB | 20 KB | 80% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
