# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 872 KB | 2048 KB | 43% | 407 KB | 512 KB | 80% |
| `f767` | NUCLEO-F767ZI | 861 KB | 2048 KB | 42% | 342 KB | 368 KB | 93% |
| `f439` | NUCLEO-F439ZI | 548 KB | 2048 KB | 27% | 130 KB | 192 KB | 68% |
| `f407` | STM32F4DISCOVERY | 546 KB | 1024 KB | 53% | 107 KB | 128 KB | 84% |
| `f401` | NUCLEO-F401RE | 497 KB | 512 KB | 97% | 84 KB | 96 KB | 88% |
| `f446` | NUCLEO-F446RE | 509 KB | 512 KB | 100% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 127 KB | 128 KB | 100% | 15 KB | 20 KB | 80% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
