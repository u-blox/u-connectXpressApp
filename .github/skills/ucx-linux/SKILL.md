---
name: ucx-linux
description: Build and run u-connectXpressApp on Linux to test and configure a u-connectXpress module (NORA-W36, NORA-B26) over a serial port. Use for Linux setup, serial permissions and scripted runs.
---

# u-connectXpressApp on Linux

## Build and run
1. Fresh Ubuntu/Debian: `sudo apt install -y git cmake build-essential ninja-build python3-pytest`, then `sudo usermod -aG dialout $USER` and log in again (serial ports).
2. `git submodule update --init u-connectClient`
3. `./ucxapp.py` builds and starts the shell (the binary is `app/bin/ucxapp`). `--help` lists the options.
4. From Windows, `.\ucxapp --linux` runs the same build inside WSL. Attach USB serial devices to WSL with `usbipd` first.
5. Or use a real Linux machine over SSH: set up key login once (append `~/.ssh/id_ed25519.pub` to the machine's `~/.ssh/authorized_keys`), then `.\ucxapp --linux-host user@host --build` (or set `UCX_LINUX_HOST`). The working tree, uncommitted edits too, is copied to `~/ucxapp` (changed files only) and the same launcher command runs there, e.g. `--linux-host user@host --test` or `--detect --test tests/hardware`.

## Find the hardware
`./ucxapp.py --detect` lists the ports, NORA modules, GNSS receiver and IMU and prints the `UCX_*` test variables; `--detect --test tests/hardware` runs the hardware tests on them, one line per test with the `[ NN%]` progress (do not add `-q`). Do not write one-off probe scripts; improve `detect()` in `ucxapp.py` or the app detectors instead.

## Use
- Serial access: add your user to the `dialout` group (`sudo usermod -aG dialout $USER`, then log in again).
- Connect to a module: `dev.connect /dev/ttyUSB0` in the shell, or pass the port on the command line.
- Script commands non-interactively with `--exec "<command>"`; type `help` for the full list (see `COMMANDS.md`) and `<group>.example` (e.g. `gnss.example`) for worked examples.
- `./ucxapp.py --coverage` builds an instrumented binary in `~/ucxapp-coverage`, runs the offline app tests and writes per-file line coverage to `build/coverage.txt`.

## Module documentation
Look up AT commands and behaviour with the `u-blox-docs` MCP server (`.vscode/mcp.json`).
