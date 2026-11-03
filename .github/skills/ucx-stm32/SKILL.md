---
name: ucx-stm32
description: Build, flash and use u-connectXpressApp as FreeRTOS or Zephyr firmware on NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, NUCLEO-F401RE, NUCLEO-F446RE, STM32F4DISCOVERY (STM32F407VG) or NUCLEO-F103RB / NUCLEO-G071RB (lite builds) boards driving a u-connectXpress module. Use for STM32 setup, HAL paths, Zephyr/NCS setup, flashing over ST-Link, the console and UART bridge mode.
---

# u-connectXpressApp on STM32 Nucleo

## Prerequisites
- Arm GNU toolchain (`arm-none-eabi-gcc`), CMake, Ninja, STM32CubeProgrammer (ST-Link flashing).
- The matching STM32Cube HAL: set `STM32CUBEH7_PATH`, `STM32CUBEF7_PATH`, `STM32CUBEF4_PATH` or `STM32CUBEF1_PATH` or `STM32CUBEG0_PATH`, or init the u-connectClient submodules. `STM32Cube*` checkouts next to this repo are found automatically.

## Build, flash, log
```powershell
.\ucxapp --stm32-freertos --board h753 --build --flash --log   # h753 (default), f767, f439, f407, f401, f446, f103, g071 or all
.\ucxapp --stm32-zephyr --board h753 --build --flash --log       # Zephyr build, same boards (or all)
```
Only official ST NUCLEO and Discovery boards are supported. `--stm32` still works as the old name of `--stm32-freertos`.
On Linux: `./ucxapp.py --stm32-freertos ...`. Other options: `--rebuild`, `--clean`, `--erase`, `--recover`, `--log COMx`, `--help`. Prebuilt `.bin/.hex/.elf` images for each board are attached to every GitHub release; flash a `.bin` at `0x08000000` with STM32CubeProgrammer.

## Zephyr (`--stm32-zephyr`)
- Needs `west` and an upstream Zephyr workspace with the Zephyr SDK (hal_stm32 comes with it):
  ```powershell
  pip install west
  west init C:\zephyrproject; cd C:\zephyrproject; west update
  west sdk install
  ```
- `ZEPHYR_BASE` defaults to `C:/zephyrproject/zephyr` (or `~/zephyrproject/zephyr`). An nRF Connect SDK under `C:/ncs` also works with [hal_stm32](https://github.com/zephyrproject-rtos/hal_stm32) in `C:\ncs\extra\hal_stm32`, but its `nrf` module warns that the SoC is not supported. `UCXAPP_ZEPHYR_MODULES` sets extra modules.
- Output: `build/zephyr_<board>/zephyr/zephyr.bin` (single image, no sysbuild). Same boards, wiring and console as the FreeRTOS build (`app/zephyr/boards/<zephyr board>.overlay`; the F4 Discovery is `stm32f4_disco` in Zephyr). `<board>.conf` sets the heap per board; F103 and G071 build lite like FreeRTOS (no MQTT, net, time, cert, YMODEM).
- Run `west` from the repo root with relative paths: it fails when the source and the current directory are on different drives. In PowerShell quote `'-DEXTRA_ZEPHYR_MODULES=C:/...'`, or the colon splits the argument.
- Files are kept in RAM only for now.

## Files (FreeRTOS builds)
Files (Wi-Fi profiles, certificates) live in a littlefs file system in the flash above the firmware on H753 (1 MB, bank 2), F767 (1.25 MB), F439 (896 KB) and F407 (384 KB), so they survive reset and reflash (`--erase` wipes them; `file.format yes` erases them from the shell). Each file larger than about 256 bytes takes a whole erase sector (128 or 256 KB), so the F407 holds only one such file. The other boards keep files in RAM only. Fetch littlefs once with `git submodule update --init third-party/littlefs`.

## Wiring
NORA-W36 on the Arduino header CN10, 115200 baud:

| NORA-W36 | Arduino | H753 | F767 / F439 |
|---|---|---|---|
| TX | D0 | PB7 (USART1_RX) | PG9 (USART6_RX) |
| RX | D1 | PB6 (USART1_TX) | PG14 (USART6_TX) |
| GND | GND | GND | GND |
| VCC | 3V3 | 3.3V | 3.3V |
- The shell runs on the ST-Link virtual COM port (USART3, 115200).

### STM32F4DISCOVERY (STM32F407VG, `--board f407`)
No Arduino header and the ST-Link VCP is not routed to the MCU, so wire it by hand:

| Signal | Pin |
|---|---|
| NORA-W36 TX -> USART3_RX | PB11 |
| NORA-W36 RX <- USART3_TX | PB10 |
| Shell console USART2 TX / RX (USB-UART adapter, 115200) | PA2 / PA3 |
| GND, 3V3 | GND, 3V |

### NUCLEO-F401RE / F446RE / F103RB / G071RB (`--board f401` / `f446` / `f103` / `g071`)
Arduino D0/D1 is USART2, which is the ST-Link VCP, so the module goes on USART1:

| Signal | Pin |
|---|---|
| NORA-W36 TX -> USART1_RX | D2 (PA10) |
| NORA-W36 RX <- USART1_TX | D8 (PA9) |
| NORA-W36 RTS / CTS (only with flow control) | PA11 (CTS) / PA12 (RTS) |
| GND, 3V3 | GND, 3V3 |
- The shell runs on the ST-Link virtual COM port (USART2, 115200). The F401 runs at 84 MHz, the F446 at 168 MHz, the F103 at 64 MHz.
- The G071 is paused: it is full, so `--board all`, CI and releases skip it; `--board g071` still builds it.
- The F103 and G071 (64 MHz Cortex-M0+) get a lite build: device, wifi, sock, sys, app, bt and gatt commands (no net, mqtt, cert, TLS or `examples`/`<group>.example`). Scan and GATT discovery tables are small (8 devices, 8 services, 24 characteristics) and Bluetooth company, appearance and characteristic names are shown as numbers only.

## Use
- The shell connects to the module at startup; `dev.connect [uart] [baud]` reconnects.
- `bridge [baud]` makes the board a transparent console-to-module bridge (AT debugging, XMODEM updates). Press Ctrl-] with 1 s of silence before and after to return to the shell.
- Command list: `COMMANDS.md`.

## Low power (host MCU)
- FreeRTOS: a `sleep` task at idle priority (listed by `sys.tasks`) stops the core with WFI until the next interrupt, and the shell polls the console every 10 ms once it has been quiet for 200 ms. Interrupts wake the core at once, so AT and bridge latency are unchanged.
- Zephyr: the idle thread does the same with WFI on a tickless kernel. STOP modes (`CONFIG_PM`) stay off because the UARTs would drop bytes at 115200 while the clocks restart.
- `bridge` keeps the core busy for lowest latency until you leave it.

## Module documentation
Look up AT commands and behaviour with the `u-blox-docs` MCP server (`.vscode/mcp.json`).
