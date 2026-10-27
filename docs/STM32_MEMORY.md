# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 450 KB | 2048 KB | 22% | 314 KB | 512 KB | 61% |
| `f767` | NUCLEO-F767ZI | 422 KB | 2048 KB | 21% | 299 KB | 368 KB | 81% |
| `f439` | NUCLEO-F439ZI | 425 KB | 2048 KB | 21% | 105 KB | 192 KB | 55% |
| `f407` | STM32F4DISCOVERY | 423 KB | 1024 KB | 41% | 105 KB | 128 KB | 82% |
| `f401` | NUCLEO-F401RE | 403 KB | 512 KB | 79% | 88 KB | 96 KB | 92% |
| `f446` | NUCLEO-F446RE | 403 KB | 512 KB | 79% | 104 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 116 KB | 128 KB | 91% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 121 KB | 128 KB | 95% | 25 KB | 36 KB | 72% |
