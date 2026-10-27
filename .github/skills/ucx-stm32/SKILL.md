---
name: ucx-stm32
description: Build, flash and use u-connectXpressApp as firmware on NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, NUCLEO-F401RE, NUCLEO-F446RE, STM32F4DISCOVERY (STM32F407VG) or NUCLEO-F103RB / NUCLEO-G071RB (lite builds) boards driving a u-connectXpress module. Use for STM32 setup, HAL paths, flashing over ST-Link, the console and UART bridge mode.
---

# u-connectXpressApp on STM32 Nucleo

## Prerequisites
- Arm GNU toolchain (`arm-none-eabi-gcc`), CMake, Ninja, STM32CubeProgrammer (ST-Link flashing).
- The matching STM32Cube HAL: set `STM32CUBEH7_PATH`, `STM32CUBEF7_PATH`, `STM32CUBEF4_PATH` or `STM32CUBEF1_PATH` or `STM32CUBEG0_PATH`, or init the u-connectClient submodules. `STM32Cube*` checkouts next to this repo are found automatically.

## Build, flash, log
```powershell
.\ucxapp --stm32-freertos --board h753 --build --flash --log   # h753 (default), f767, f439, f407, f401, f446, f103, g071 or all
.\ucxapp --stm32-zephyr --board h753 --build --flash --log       # Zephyr build, H753 only so far
```
Only official ST NUCLEO and Discovery boards are supported. `--stm32` still works as the old name of `--stm32-freertos`.
On Linux: `./ucxapp.py --stm32-freertos ...`. Other options: `--rebuild`, `--clean`, `--erase`, `--recover`, `--log COMx`, `--help`. Prebuilt `.bin/.hex/.elf` images for each board are attached to every GitHub release; flash a `.bin` at `0x08000000` with STM32CubeProgrammer.

Files (Wi-Fi profiles, certificates) live in a littlefs file system in the top two flash sectors on H753, F767, F439 and F407, so they survive reset and reflash (`--erase` wipes them; `file.format yes` erases them from the shell). The other boards keep files in RAM only. Fetch littlefs once with `git submodule update --init third-party/littlefs`.

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
- The F103 and G071 (64 MHz Cortex-M0+) get a lite build: device, wifi, sock, sys, app, bt and gatt commands (no net, mqtt, cert, TLS or `examples`/`<group>.ex`). Scan and GATT discovery tables are small (8 devices, 8 services, 24 characteristics) and Bluetooth company, appearance and characteristic names are shown as numbers only.

## Use
- The shell connects to the module at startup; `dev.connect [uart] [baud]` reconnects.
- `bridge [baud]` makes the board a transparent console-to-module bridge (AT debugging, XMODEM updates). Press Ctrl-] with 1 s of silence before and after to return to the shell.
- Command list: `COMMANDS.md`.

## Module documentation
Look up AT commands and behaviour with the `u-blox-docs` MCP server (`.vscode/mcp.json`).
