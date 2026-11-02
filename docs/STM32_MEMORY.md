# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 494 KB | 2048 KB | 24% | 316 KB | 512 KB | 62% |
| `f767` | NUCLEO-F767ZI | 467 KB | 2048 KB | 23% | 323 KB | 368 KB | 88% |
| `f439` | NUCLEO-F439ZI | 470 KB | 2048 KB | 23% | 131 KB | 192 KB | 69% |
| `f407` | STM32F4DISCOVERY | 467 KB | 1024 KB | 46% | 104 KB | 128 KB | 82% |
| `f401` | NUCLEO-F401RE | 448 KB | 512 KB | 88% | 87 KB | 96 KB | 91% |
| `f446` | NUCLEO-F446RE | 448 KB | 512 KB | 88% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 123 KB | 128 KB | 96% | 15 KB | 20 KB | 80% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 26 KB | 36 KB | 72% |
