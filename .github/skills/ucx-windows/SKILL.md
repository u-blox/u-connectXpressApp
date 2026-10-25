---
name: ucx-windows
description: Build and run u-connectXpressApp on Windows to test and configure a u-connectXpress module (NORA-W36, NORA-B26). Use for Windows setup, COM port connection, scripted --exec runs and firmware updates.
---

# u-connectXpressApp on Windows

## Build and run
1. Install Visual Studio 2022 Build Tools (Desktop development with C++), CMake 3.15+, Git and Python 3.10+.
2. `git submodule update --init u-connectClient`
3. `.\ucxapp` builds on first run and opens the shell. It is a thin caller of `ucxapp.py`; `--help` lists the options (`--debug`, `--clean`, `--rebuild`, `--build --run`, `--selftest`, `--linux` for WSL, `--stm32`).

## Find the hardware
`.\ucxapp --detect` lists the serial ports with their role (EVK, GNSS, USB-UART, ST-Link), the NORA modules, the GNSS receiver (port and baud) and the IMU, and prints the `UCX_*` variables for the tests. `--detect --test tests/hardware` runs the hardware tests on what it found.

Always use `--detect` (or `gnss.detect`, `imu.detect`, `--list-modules`, `--list-ports` in the app). Do not write one-off probe scripts; if detection misses a device, improve `detect()` in `ucxapp.py` or the detector in `app/src/` and add a test.

## Use
- Connect an EVK (FTDI USB). The app auto-detects the COM port, or pass it: `ucxapp COM13`.
- Script: `ucxapp COM13 --exec "device.info" --exec "wifi.scan"` (exit code 1 on first failure).
- Firmware update: `--exec "fw.flash <file>"`.
- Type `help` in the shell for all commands; see `COMMANDS.md`. `examples` lists worked examples, `<group>.ex` (e.g. `gnss.ex`, `wifi.ex`) shows one group.
- `.\ucxapp --coverage` runs the offline app tests on a gcov build in WSL and writes line coverage to `build/coverage.txt`.

## Module documentation
Look up AT commands and behaviour with the `u-blox-docs` MCP server (`.vscode/mcp.json`).
