## What's in this release

u-connectXpressApp is the command shell for u-connectXpress modules (NORA-W36, NORA-B26). It runs on Windows and Linux, in the browser, and as FreeRTOS or Zephyr firmware on the NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, NUCLEO-F401RE, NUCLEO-F446RE, STM32F4DISCOVERY and NUCLEO-F103RB (lite) boards.

- **Matter:** `matter.start` makes the module a Matter node (light, temperature and contact sensor). Scan the QR code in Apple Home, Google Home or Alexa; `matter.status` and the web page show how long the commissioning took, step by step. The plug-in ships with the app (`ucxmatter.dll`, `libucxmatter.so`) and is built into the browser app
- **In the browser:** the whole app as WebAssembly with the module over Web Serial, installable and usable offline
- **Secure by default:** private keys and passwords sealed with AES-256-GCM, the web UI over HTTPS with a random token
- **Phone as a test peer:** `xplorer.*` and `perf.phone` drive u-connectXplorer on a phone (echo server, BLE central, throughput)
- One launcher, `ucxapp.py`, for Windows, Linux (also WSL), STM32 and the browser; `--detect` finds modules, GNSS receivers, IMUs, modems and the PPK2
- Bridges for GNSS, IMU, CAN, cellular and UART over TCP, TLS, UDP and BLE

## Downloads

`ucxapp-<version>.zip` holds every target, with SHA-256 checksums next to each file:

```
windows/ucxapp.exe            signed; run it with the module's COM port
windows/ucxmatter.dll         signed; the Matter plug-in, keep it next to ucxapp.exe
linux/ucxapp                  run it with the serial port, e.g. /dev/ttyUSB0
linux/libucxmatter.so         the Matter plug-in, keep it next to ucxapp
stm32/stm32_h753/             .bin/.hex/.elf; flash with STM32CubeProgrammer at 0x08000000
stm32/stm32_f767/
stm32/stm32_f439/
stm32/stm32_f407/             STM32F4DISCOVERY
stm32/stm32_f401/
stm32/stm32_f446/
stm32/stm32_f103/             lite build
zephyr/zephyr_<board>/        the same boards on Zephyr (ucxapp_zephyr.bin/.hex/.elf), flashed the same way
wasm/                         the app in the browser (WebAssembly + Web Serial, Matter node built in), see below
```

`ucxapp.exe` and `ucxmatter.dll` (Windows, signed), `ucxapp` and `libucxmatter.so` (Linux x64) and `ucxapp-wasm-<version>.zip` (the browser app) are also attached on their own.

## The app in the browser

The same app runs as WebAssembly in Chrome or Edge, online at https://u-blox.github.io/u-connectXpressApp/ or from
`wasm/` on your own PC: `python -m http.server 8098 --bind 127.0.0.1` in that folder, then open
http://127.0.0.1:8098/. **Connect module** opens the EVK's serial port (Web Serial); **Virtual module** runs a
simulated NORA-W36 without hardware.

## Connecting a NORA-W36 to a NUCLEO board

All boards use the Arduino header CN10. The shell runs on the ST-Link virtual COM port (115200 baud).

| NORA-W36 pin | Arduino pin | NUCLEO-H753ZI | NUCLEO-F767ZI / F439ZI | Function |
|---|---|---|---|---|
| TX | D0 (CN10) | PB7 (USART1_RX) | PG9 (USART6_RX) | Board receives |
| RX | D1 (CN10) | PB6 (USART1_TX) | PG14 (USART6_TX) | Board transmits |
| GND | GND (CN10) | GND | GND | Ground |
| VCC | 3V3 (CN10) | 3.3V | 3.3V | Power |

The module UART runs at 115200 baud. Use `bridge` in the shell to talk to the module directly from a PC terminal.
