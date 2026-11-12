# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 836 KB | 2048 KB | 41% | 404 KB | 512 KB | 79% |
| `f767` | NUCLEO-F767ZI | 820 KB | 2048 KB | 40% | 334 KB | 368 KB | 91% |
| `f439` | NUCLEO-F439ZI | 524 KB | 2048 KB | 26% | 127 KB | 192 KB | 66% |
| `f407` | STM32F4DISCOVERY | 521 KB | 1024 KB | 51% | 104 KB | 128 KB | 82% |
| `f401` | NUCLEO-F401RE | 494 KB | 512 KB | 97% | 84 KB | 96 KB | 88% |
| `f446` | NUCLEO-F446RE | 506 KB | 512 KB | 99% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 126 KB | 128 KB | 99% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
