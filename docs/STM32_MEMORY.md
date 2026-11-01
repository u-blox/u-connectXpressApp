# STM32 flash and RAM usage

Updated by `ucxapp --stm32-freertos --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32_freertos/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 456 KB | 2048 KB | 22% | 314 KB | 512 KB | 61% |
| `f767` | NUCLEO-F767ZI | 443 KB | 2048 KB | 22% | 323 KB | 368 KB | 88% |
| `f439` | NUCLEO-F439ZI | 446 KB | 2048 KB | 22% | 131 KB | 192 KB | 69% |
| `f407` | STM32F4DISCOVERY | 443 KB | 1024 KB | 43% | 104 KB | 128 KB | 81% |
| `f401` | NUCLEO-F401RE | 424 KB | 512 KB | 83% | 87 KB | 96 KB | 91% |
| `f446` | NUCLEO-F446RE | 424 KB | 512 KB | 83% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 118 KB | 128 KB | 93% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 122 KB | 128 KB | 96% | 25 KB | 36 KB | 72% |
