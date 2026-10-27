# Building and deploying u-connectXpressApp

Technical reference for building, flashing, signing and releasing. For an overview and examples see [README.md](README.md).

## Prerequisites

- Windows 10/11 (64-bit) for the Windows build; Linux for the Linux build; an STM32 Nucleo board for the firmware build
- FTDI USB device or a UART connection to a NORA module (NORA-W36 or NORA-B26)

Windows build tools:
1. Visual Studio 2022 Build Tools with the "Desktop development with C++" workload: https://aka.ms/vs/17/release/vs_BuildTools.exe
2. CMake 3.15 or later (on PATH): https://cmake.org/download/
3. Git: https://git-scm.com/download/win

STM32 builds additionally need the Arm GNU toolchain, Ninja and STM32CubeProgrammer.

## Get the sources

```powershell
git clone https://github.com/u-blox/u-connectXpressApp.git
cd u-connectXpressApp

# Only the u-connectClient submodule (without recursing into its submodules)
git submodule update --init u-connectClient
```

## Launcher

`ucxapp.py` builds, runs, tests and flashes every target. On Windows `ucxapp.cmd` calls it;
`ucxapp-stm32.cmd` and `ucxapp.sh` forward to it.

| Target | Command |
|---|---|
| Windows | `.\ucxapp` |
| Linux | `./ucxapp.py` (from Windows: `.\ucxapp --linux` runs it in WSL) |
| STM32 (FreeRTOS) | `.\ucxapp --stm32-freertos` |
| STM32 (Zephyr) | `.\ucxapp --stm32-zephyr` |

### Windows

```powershell
.\ucxapp                 # build if needed and run (auto-detects a NORA EVK)
.\ucxapp COM13 --exec "dev.info"   # arguments the launcher does not know go to the app
.\ucxapp --debug         # debug build
.\ucxapp --update        # update the u-connectClient submodule
.\ucxapp --clean         # clean build artifacts
.\ucxapp --rebuild       # rebuild from scratch
.\ucxapp --clean --build --run   # clean, rebuild, then start the app
.\ucxapp --sign YOUR_CERT_THUMBPRINT
.\ucxapp --selftest      # workspace validation
.\ucxapp --detect        # ports, NORA modules, GNSS receiver and IMU, with the UCX_* test variables
.\ucxapp --detect --test tests/hardware   # hardware tests on the detected devices
.\ucxapp --coverage      # line coverage of app/src from the offline tests (gcov build in WSL), report in build/coverage.txt
```

Manual build:

```powershell
cmake -S app -B build/windows
cmake --build build/windows --config Release
```

### STM32 (NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI, NUCLEO-F401RE, NUCLEO-F446RE, STM32F4DISCOVERY, NUCLEO-F103RB, NUCLEO-G071RB)

```powershell
# Build, flash over the ST-Link and open the console on its virtual COM port (default board: H753)
.\ucxapp --stm32-freertos --build --flash --log

# Select another board: h753 (default), f767, f439, f407, f401, f446, f103, g071, or all (parallel build, logs in build/all_*.log)
.\ucxapp --stm32-freertos --board f767 --build --flash --log

# Other options: --rebuild, --clean, --erase, --recover, --log COMx, --help (--stm32 is the old name of --stm32-freertos)

# Zephyr build (NUCLEO-H753ZI): west + ZEPHYR_BASE (default: newest C:/ncs/v*/zephyr),
# extra modules in UCXAPP_ZEPHYR_MODULES (default: C:/ncs/extra/hal_stm32)
.\ucxapp --stm32-zephyr --board h753 --build --flash --log
```

The module is wired to USART1 (H753) or USART6 (F767, F439); the shell runs on the ST-Link VCP. On the STM32F4DISCOVERY (f407) the module is on
USART3 (PB10 TX, PB11 RX) and the shell on USART2 (PA2 TX, PA3 RX) through a USB-UART adapter. On the NUCLEO-F401RE, NUCLEO-F446RE, NUCLEO-F103RB and NUCLEO-G071RB (f401, f446, f103, g071)
the module is on USART1 (D8 = PA9 TX, D2 = PA10 RX) and the shell on the ST-Link VCP. `bridge [baud]` turns the board into a
transparent console-to-module bridge (for AT debugging or XMODEM firmware update); press Ctrl-] with 1 s of silence around it to return to the shell.

Every STM32 build prints its flash and RAM usage and updates the table in [docs/STM32_MEMORY.md](docs/STM32_MEMORY.md); commit it with the change that moved the numbers.

The NUCLEO-F103RB (`f103`) and NUCLEO-G071RB (`g071`), both 128 KB flash, get a lite build with Wi-Fi, sockets, Bluetooth and GATT but no net, MQTT or cert commands, smaller scan/discovery tables and no Bluetooth company, appearance or characteristic names.
Only official ST NUCLEO and Discovery boards are supported.

The build needs the matching STM32Cube HAL (`STM32CUBEH7_PATH`, `STM32CUBEF7_PATH`, `STM32CUBEF4_PATH` or `STM32CUBEF1_PATH` or `STM32CUBEG0_PATH`, or the u-connectClient submodule).
Release builds of all five boards are published with each tag.

## File structure

```
u-connectXpressApp/
├── ucxapp.py                       # Launcher for every target (build, run, test, detect, STM32 flash)
├── ucxapp.cmd                      # Thin Windows caller of ucxapp.py
├── app/
│   ├── src/                        # Command layer and entry point (ucxapp_main.c)
│   ├── stm32/                      # STM32 firmware entry, platform and CMake
│   ├── CMakeLists.txt
│   ├── bin/                        # Build outputs (gitignored)
│   ├── release/                    # Signed releases
│   └── third-party/                # FTDI DLL, Bluetooth SIG, etc.
├── u-connectClient/                # Git submodule: AT client library
└── build/                          # CMake artifacts per target (gitignored)
```

## Architecture notes

- The command layer (`app/src/ucxapp_*.c`) has no OS dependencies and is shared by Windows, Linux and STM32.
- Host services (COM-port discovery, HTTPS download, key input) sit behind `app/src/ucxapp_platform.h`; the Windows backend is `ucxapp_platform_win.c`.
- A command is a table entry (`name`, `usage`, `help`, `run()`); see `app/src/ucxapp_cmds_wifi.c`.
- The web UI engine is `app/src/u_webserver.c`; routes are in `app/src/ucxapp_cmds_web.c`. Routes: `GET /`, `GET /events` (SSE), `POST /api/exec?cmd=...`.
- Full command reference: [COMMANDS.md](COMMANDS.md).

## Production deployment

```powershell
.\ucxapp --rebuild --sign YOUR_CERT_THUMBPRINT   # creates ucxapp-signed.exe in release/
```

`app/release/ucxapp-signed.exe` is self-contained (~2.5 MB) with the FTDI DLL embedded. It is not
committed; `scripts/release.py` uploads it to the GitHub Releases page.
See [docs/UCX_WIN_APP_RELEASE.md](docs/UCX_WIN_APP_RELEASE.md) and `scripts/release.py` for the full release flow.
