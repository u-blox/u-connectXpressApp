# u-connectXpressApp Build Instructions

## Project Overview

u-connectXpressApp is a command shell for testing u-connectXpress devices (NORA-W36, NORA-B26). It runs on Windows, Linux and as FreeRTOS firmware on STM32 boards (NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, NUCLEO-F401RE, NUCLEO-F446RE, STM32F4DISCOVERY, NUCLEO-F103RB and NUCLEO-G071RB as lite builds). It uses the u-connectClient library as a git submodule (branch `master`).

## Documentation

Always use the `u-blox-docs` MCP server (`.vscode/mcp.json`) to look up u-connectXpress AT commands, module behaviour and firmware details before answering or changing module-related code.

## Targets and Launcher

`ucxapp.py` is the one launcher for every target; `ucxapp.cmd` is only a thin Windows caller of it (type `.\ucxapp` in PowerShell, `./ucxapp.py` on Linux). `ucxapp-stm32.cmd` and `ucxapp.sh` just forward to it. The target is this OS unless a flag selects another one:

| Target | Command | Notes |
|---|---|---|
| Windows | `.\ucxapp` | Visual Studio 2022 + CMake |
| Linux | `./ucxapp.py` (or `.\ucxapp --linux` for WSL) | CMake + gcc, binary `app/bin/ucxapp` (Windows exe: `app/bin/ucxapp.exe`) |
| STM32 | `.\ucxapp --stm32-freertos [--board h753\|f767\|f439\|f407\|f401\|f446\|f103\|g071\|all] --build --flash --log` | H753 is the default; only official ST NUCLEO/Discovery boards are supported; f407 is the STM32F4DISCOVERY; f401/f446/f103/g071 are NUCLEO-64 boards (module on USART1 D8/D2); f103 and g071 are lite builds (`UCXAPP_LITE`: Wi-Fi + BT/GATT, no net/cert, no BT company/appearance/characteristic names, small buffers) for 20 KB RAM / 128 KB flash; needs the matching STM32Cube HAL (`STM32CUBEH7_PATH`, `STM32CUBEF7_PATH`, `STM32CUBEF4_PATH`, `STM32CUBEF1_PATH`, `STM32CUBEG0_PATH`); `--stm32` is the old name |
| STM32 Zephyr | `.\ucxapp --stm32-zephyr --board h753 --build --flash --log` | `app/zephyr`, west + upstream Zephyr workspace (`ZEPHYR_BASE`, default `C:/zephyrproject/zephyr`); H753 only so far |

The command layer in `app/src/ucxapp_*.c` is portable; platform code lives in `ucxapp_platform_*.c`, `app/stm32_freertos/` and `app/zephyr/`.

Customer-facing skills for each target are in `.github/skills/`. Releases are built by `.github/workflows/release.yml` on `v*` tags (Windows, Linux and the three STM32 images).

## Scope and priorities

- A feature belongs in ucxapp only if it demonstrates, tests, automates or integrates a u-connectXpress capability. GNSS, IMU and cellular stay supporting actors (bridges, demos); no generic serial terminal, file manager or IoT platform features.
- Prefer making existing commands solid (tests, docs, error messages) over adding new ones.
- Keep the web UI a thin view of the command layer; logic goes into C commands so the TUI, `--exec` and the web share it.

## Use the same scripts every time

- Find hardware with `.\ucxapp --detect`: ports with their role, NORA modules, GNSS receiver (port and baud), IMU, and the `UCX_*` test variables. `--detect --test tests/hardware` runs the hardware tests (split per area: device, wifi, net, ble, bridges) on what it found.
- Do not write one-off probe, port-scan or test scripts (no temporary `.ps1`/`.py` files, no ad-hoc loops over COM ports). If the launcher, the app detectors (`gnss.detect`, `imu.detect`, `cell.detect`, `--list-modules`, `--list-ports`) or the tests miss something, improve them and add a test in `tests/`.
- New launcher features go into `ucxapp.py`, never into the `.cmd` callers.
- Do not create or run VS Code tasks (`tasks.json`) to run commands; run them in the terminal.

## Building

### Quick Build (Recommended)
```powershell
.\ucxapp
```
This auto-builds if needed and launches the application.

### Build Commands
```powershell
.\ucxapp                  # Release build if needed, then run
.\ucxapp --debug          # Debug build
.\ucxapp --clean          # clean build artifacts
.\ucxapp --rebuild        # reconfigure and rebuild from scratch
.\ucxapp --build --run    # build, then start
.\ucxapp --help           # all options
```

### Manual CMake Build
```powershell
cmake -S app -B build/windows -A x64
cmake --build build/windows --config Release
```

**DO NOT USE:**
- Direct Visual Studio builds without CMake configuration
- Building from wrong directory (must be from repo root)

## Project Structure

- `app/` - Main application source
  - `src/` - Command layer; `src/ucxapp_main.c` is the entry point
  - `CMakeLists.txt` - Build configuration
  - `bin/` - Build outputs (gitignored)
  - `release/` - Signed releases
  - `third-party/` - FTDI DLL, QR code generator
- `u-connectClient/` - AT command client library (git submodule)
- `build/windows`, `build/linux`, `build/stm32_<board>`, `build/zephyr_<board>` - CMake artifacts per target (gitignored)

## Submodule Management

```powershell
# Initialize submodule (first time)
git submodule update --init u-connectClient

# Update submodule to latest
.\ucxapp --update
```

## Code Style

- **Zero warnings**: every target (Windows, Linux, all STM32 boards) must build with no compiler warnings. Fix the code; only vendor HAL/FreeRTOS/CubeMX sources may be silenced (in `cmake/stm32.cmake`).
- C11 standard
- MSVC compiler with /W4 warning level
- Functions prefixed with module name (e.g., `uCxAtClient_`)
- Use `_CRT_SECURE_NO_WARNINGS` for Windows APIs

## Testing

```powershell
# Static checks (ruff, shellcheck) and repo sanity checks
python -m pip install pytest ruff
.\ucxapp --test

# Hardware tests on the detected modules, GNSS receiver and IMU
.\ucxapp --detect --test tests/hardware

# Line coverage of app/src from the offline tests (gcov build in WSL), report in build/coverage.txt
.\ucxapp --coverage
```

CI (`.github/workflows/build.yml`) runs pytest and a Windows Release build on every push.

**Coverage goal: 100% of `app/src`** (currently 49.3%, see `build/coverage.txt`). Stability comes before new features: every change should raise coverage or keep it level, and new code ships with tests that cover it. Lowest files first: `ucxapp_urc.c`, `ucxapp_events.c`, `ucxapp_adv.c`, `ucxapp_ui.c` and `u_webserver.c` (0%), then gatt, hid, bt, net, sock, fw, device, cert, mqtt, web and tui. Reach module-only code offline through recorded AT and URC traffic, malformed input and the `tests/test_stress.py` style. Code that only real hardware can reach is covered by `tests/hardware`. Stress tests (`-m stress`) check that nothing leaks (sockets, links, memory) after long runs.

**Tests must pass 100%**, offline and hardware alike. A failing test is a bug to fix in the app or the test (state left over from an earlier test, a race, a wrong timeout) before anything is committed or released. Never hide a failure with `skip`, `xfail`, a looser assert or a retry loop; a test may only skip when its hardware or setting is missing. A test that passes alone but fails in the full run is a cleanup bug: every test must start from a known state (reboot the modules it uses).

Visual feedback matters, but keep it short: `pyproject.toml` makes pytest print one line per test with its `[ NN%]` progress, short tracebacks and a failures-only summary. Do not add `-q` (it turns this back into bare dots), and do not add `-vv`, `-s` or `-rs` (they flood the output with detail and skip reasons) unless you are debugging one test. Long-running commands and scripts should likewise show what they are doing and how far along they are, in a line or two per step rather than a wall of text.

New code goes into `app/src/` as separate command modules. Add worked examples for new commands to `kExamples` in `app/src/ucxapp_cmds_examples.c` (shown by `<group>.ex`). Long usage strings with top-level `|` alternatives are printed one alternative per line by `help`.

```powershell
# Workspace and executable checks
.\ucxapp --selftest

# Firmware update
.\ucxapp COM13 --exec "fw.flash <file>"
```

## Release Process

`python scripts/release.py` (or `.\ucxapp --release`) rebuilds, signs, tags and uploads; `--dry-run` only builds and signs. Signing alone: `.\ucxapp --rebuild --sign CERT_THUMBPRINT`.

## Performance Priorities

- Latency and throughput are the main requirements for every bridge (UART, UDP, TCP, BLE, CAN, GNSS, IMU). Batch per datagram or write, avoid per-byte calls, and never block a data path with a long sleep (idle polls use 1 ms).
- SPI for AT and DATA (instead of UART) is coming in u-connectClient. Keep the transport behind the port layer so the command code in `app/src/` does not change when SPI lands.
- CAN frames use the fixed 17-byte record in `app/src/ucxapp_can.h` (ts_ms, id, dlc, data[8]), batched per UDP datagram. CAN code stays out of the AT, UART and `hw.*` paths; `ucxapp_cmds_can.c` owns the controller and `can.bridge`.

## Version Numbering

- Format: `MAJOR.MINOR.PATCH.BUILD` (e.g., 3.2.0.260219)
- BUILD = YYMMDD (CalVer, auto-generated from build date)
- Major.Minor.Patch follows UCX API version

## Platform Requirements

- Windows 10/11 (64-bit)
- Visual Studio 2022 Build Tools
- CMake 3.15+
- Python 3.10+ (launcher and tests)
- FTDI USB device or UART connection to a NORA module
- STM32 builds: Arm GNU toolchain, Ninja, STM32CubeProgrammer
