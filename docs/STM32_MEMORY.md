# STM32 flash and RAM usage

Updated by `ucxapp --stm32 --build` (Release builds). RAM includes the static FreeRTOS heap
(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32/CMakeLists.txt`), so free RAM is the headroom left
after that heap. Flash is code, read-only data and the initial values of `.data`.

| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `h753` | NUCLEO-H753ZI | 433 KB | 2048 KB | 21% | 287 KB | 512 KB | 56% |
| `f767` | NUCLEO-F767ZI | 419 KB | 2048 KB | 21% | 299 KB | 368 KB | 81% |
| `f439` | NUCLEO-F439ZI | 423 KB | 2048 KB | 21% | 105 KB | 192 KB | 55% |
| `f407` | STM32F4DISCOVERY | 420 KB | 1024 KB | 41% | 105 KB | 128 KB | 82% |
| `f401` | NUCLEO-F401RE | 400 KB | 512 KB | 78% | 88 KB | 96 KB | 92% |
| `f446` | NUCLEO-F446RE | 401 KB | 512 KB | 78% | 104 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 115 KB | 128 KB | 90% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 119 KB | 128 KB | 94% | 25 KB | 36 KB | 72% |
