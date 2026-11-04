# u-connectMatter on u-connectXpressApp: analysis

Snapshot of both trees on 2026-11-04 (u-connectMatter `f22d61f`, ucxapp with u-connectClient `0e59f65`).
Question: what would it take to run u-connectMatter on top of ucxapp instead of directly on u-connectClient,
what does it add, and does it still fit an STM32 H7?

## 1. What u-connectMatter is today

| Part | Content |
|---|---|
| Matter SDK | `connectedhomeip` submodule (v1.0.0.2-12278), C++17, GN + ninja, mbedTLS 3.6.7, miniz, FTXUI |
| Device layer | `platform/ucxclient/` (~800 KB C++): `UcxClientManager.cpp` (208 KB), `UDPEndPointImplUcx.cpp` (50 KB), `BLEManagerImpl.cpp` (74 KB, CHIPoBLE on the module's GATT server), `DnssdImpl.cpp` (36 KB, module mDNS), `ComPortDetection.cpp` (36 KB), `OTAImageProcessorImpl.cpp` (37 KB, XMODEM), `ThreadStackManagerImpl.cpp` (15 KB, NORA-B26), `NetworkCommissioningDriver.cpp` (26 KB, Wi-Fi), `WebServerManager.cpp` (21 KB), `KeyValueStoreManagerImpl.cpp` |
| OS layers | `platform/Windows`, `platform/ucxclient-freertos`, `platform/ucxclient-zephyr`, `pico/`, `stm32/` (H723, H735, H743, H750, H753, F767, F407, F429, ODIN-W26) |
| App | `app/main.cpp` (117 KB), `AppTask.cpp`, `VirtualHardware.cpp` (109 KB), ZAP device types light / switch / lock / temperature / combo |
| u-connectClient | branch `cmag_matter_support` (3fd6f7e), 27 commits behind `master` |
| Launcher | one `.cmd`/`.sh` per target (`u-connect-matter-windows.cmd` 68 KB, `u-connect-matter-stm32.cmd` 54 KB, ...) |

u-connectMatter calls 165 distinct `uCx*` functions. The groups that matter for the comparison:

| Area | Calls | In ucxapp? |
|---|---|---|
| Sockets (`uCxSocketCreate2/Bind/SendTo/ReceiveFrom/SetReadMode/MulticastGroup/RegisterDataAvailable`, `UcxSocket_*` BSD wrapper) | 25 | partly: `sock.*` incl. `sock.mcast`, `ucxapp_stream.c`; reads are polled after `+UESODA`, Matter takes the payload inline from `+UESODBF`; no `sock.option` command yet (`uCxSocketSetOption` is used internally for NO_DELAY) |
| mDNS (`uCxMdnsServiceAdd4/TxtRecordAdd/SetHostname/...`; browse/resolve/subtype are TODOs in Matter, the firmware has none) | 14 | yes: `mdns.*` (status, on/off, hostname, add, remove, txt) |
| GATT server for CHIPoBLE (`uCxGattServerServiceDefine/HostCharDefine/SendIndication/ReadReqResponse/WriteReqResponse`, legacy advertising with service data) | 17 | yes as commands: `gatt.server.hostchar/indicate/readresp/writeresp`, `bt.adv <hex> [type]`; no C API |
| Wi-Fi station + AP (`uCxWifiStation*`, `uCxWifiAp*`, static IP, regulatory domain, channel list) | 28 | yes: `wifi.*` |
| Thread (`uCxThreadSetOperationalDataset/Join/SetConfig`, up/down/role callbacks) | 7 | yes: `thread.*` (16 commands, 67 calls) |
| System / firmware (`uCxSystemStartSerialFirmwareUpdateDefault`, `uCxXmodem*`, UART settings, factory reset) | 15 | yes: `fw.flash`, `fw.update`, `dev.factory`, `dev.connect` |
| HTTP (OTA image download) | 7 | yes: `http.get`, `http.download`, `fw.download` |
| Session lock (`uCxSessionLock/Unlock`) | 53 | **no**, not in `master` either |
| Ping, power save, logging, identification | 12 | yes |

## 2. What ucxapp is today

- C11, CMake, `app/src` ~1.6 MB C, one `add_executable(ucxapp)` per host and one per board; **no library target**.
- Portable command layer (`ucxapp_cmds_*.c`) over an IO layer (`ucxapp_io_t`, sinks) and a platform layer
  (`ucxapp_platform.h`: Windows, POSIX, FreeRTOS, Zephyr, WebAssembly).
- URC model: `ucxapp_urc.c` registers minimal handlers that raise `gUcxappEventFlags`; commands wait with
  `ucxapp_events_wait()`. Socket data is read with `uCxSocketReceiveFrom` when `URC_FLAG_SOCK_DATA` is set.
- u-connectClient `master`, generated API `MERGED` (NORA-W36X + NORA-B26X). `master` already carries the
  binary-URC signature (`uUESODBF_t` with `pBinaryData, binaryDataLen`) for `NORA-B26X-THREAD`, but not for
  `NORA-W36X` or `MERGED`.
- Own copy of the web server (`app/src/u_webserver.c`, 51 KB); Matter's branch has the same lineage in
  `u-connectClient/webserver/u_webserver.c` (1211 lines) plus `WebServerManager.cpp`.

## 3. Three ways to "move"

### A. Matter as a command pack inside ucxapp (`matter.*`)
ucxapp becomes the process and links the CHIP SDK. This means a C++17 toolchain in a C11 project, a GN build
of `connectedhomeip` feeding a CMake link, Matter's event loop and stack lock owning the main thread, and
ucxapp's URC thread replaced by Matter's single-dispatcher rule. The tagged-port, bridge and peripheral
features of ucxapp have nothing to do with Matter and would be dragged in. Not recommended.

### B. ucxapp as a library inside u-connectMatter (`libucxapp`) - recommended
Matter keeps its DeviceLayer and owns the `uCxHandle_t`, URC dispatch and the stack lock; ucxapp's command
layer is linked in as a static library and gets the handle through `ucxapp_t.ucx`. The user gets the ucxapp
prompt from the Matter console (a key drops into it, `--exec "cmd"` runs one). Needs:

1. In ucxapp: split `app/CMakeLists.txt` into `add_library(ucxapp_core STATIC ...)` (everything in `app/src`
   except `ucxapp_main.c`) plus the thin executable; export `ucxapp_cmd.h`, `ucxapp.h`, `ucxapp_platform.h`
   with `extern "C"` (`ucxapp_platform.h` already has it). Allow the host to supply the handle and skip
   `ucxapp_urc_register()`; add a hook so command execution runs under a host lock (Matter's
   `PlatformMgr().LockChipStack()` + `uCxSessionLock`).
2. In u-connectMatter: GN `static_library` wrapping the CMake output or a `ucxapp_core` GN target listing the
   same sources; feed `ucxapp_io_t` from the Matter console; route `ucxapp_print` into `ChipLog` or the
   FTXUI pane.
3. Shared u-connectClient: see section 5. Both apps must build from one branch.

### C. Tools only, no code link
Keep the runtimes separate and use ucxapp before/after Matter runs: `.\ucxapp --detect` for ports and module
detection (replaces 36 KB of `ComPortDetection.cpp` logic by reuse, not by deletion), `fw.flash` / `fw.update`
instead of the 'x' XMODEM menu, `thread.dataset` / `thread.join` to provision NORA-B26, `at.trace` to
record AT traffic, `tests/hardware` style pytest for regression. Zero risk, available today, and the natural
first stage of B.

## 4. The additions: what Matter needs that ucxapp lacks

| Need | Why Matter needs it | ucxapp today | Work |
|---|---|---|---|
| UDP payload inline in `+UESODBF` | Matter's data path; polling after `+UESODA` costs 200-330 ms round trips and misses edge-triggered notifications (`UcxClientManager.h`) | polled `uCxSocketReceiveFrom` | u-connectClient `master`: binary URC params for W36X/MERGED; ucxapp `ucxapp_urc.c` can keep its flags |
| `uCxSessionLock` | holds `cmdMutex` across multi-command sequences so URC handling and other threads do not interleave | none | upstream from `cmag_matter_support` |
| mDNS (`+UMDS*`, `+UMDB`, `+UMDR`) | commissionable / operational advertising, resolving peers | none | new `mdns.*` command group (publish, txt, subtype, browse, resolve); a genuine u-connectXpress capability, so it belongs in ucxapp anyway |
| `uCxSocketMulticastGroup` | mDNS and Matter group casting (`ff02::fb`, `ff35:40:fd00::`) | none | `sock.mcast join <sock> <group>` |
| CHIPoBLE GATT server (0xFFF6, C1 write, C2 indicate, C3 read) as C API, advertising with 0xFFF6 service data (discriminator, VID/PID) | commissioning over BLE | commands only; no `bt.adv` command | expose `ucxapp_gatt_server_*` helpers out of `ucxapp_cmds_gatt.c`, add `bt.adv <hex>`; or keep `BLEManagerImpl.cpp` as is |
| Callback delivery into Matter's `SystemLayer` | every URC becomes a `PlatformMgr().PostEvent` | flags | Matter keeps its own URC registrations; ucxapp commands only read the flags they raise themselves |
| Persistent KVS (fabrics, keys) | `KeyValueStoreManagerImpl` | `ucxapp_store.c` (files on hosts, littlefs/RAM on boards) | could back the KVS, optional |
| OTA image processor, Wi-Fi AP fallback, factory data / attestation | Matter clusters | partly (`fw.*`, `wifi.ap.*`) | stays in Matter |
| C++17, GN/ninja, `connectedhomeip` checkout (GBs) | the SDK | C11, CMake | B keeps this on the Matter side |

## 5. u-connectClient convergence

Matter's branch differs from `master` in 109 files; the parts that affect API compatibility:

- `ucx_api/generated/NORA-W36X/u_cx_types.h`: `uUESODB_t` / `uUESODBF_t` gain `uint8_t *pBinaryData, size_t binaryDataLen`.
- `ucx_api/generated/NORA-W36X/u_cx_urc.c/.h`: `uCxUrcParse(..., pBinaryData, binaryDataLen)`.
- `ucx_api/u_cx.c/.h`: `uCxSessionLock/Unlock` (locks `pAtClient->cmdMutex`), `sessionMutex` in the handle.
- `src/u_cx_xmodem.c`: `U_CX_XMODEM_FILE_SUPPORT` / `U_CX_XMODEM_VERBOSE_DEBUG` defaults, MinGW format pragmas.
- `webserver/`: `u_webserver.c/.h`, `matter_sse_example.c`, README (ucxapp has its own newer copy in `app/src`).
- Port work (STM32 F7/F4/H7 UART DMA fixes, SPI transport, MQTT/BLE example) that `master` partly has.

Order: regenerate `NORA-W36X` and `MERGED` with binary URC parameters on `master` (the generator already does
it for `NORA-B26X-THREAD`), add `uCxSessionLock`, rebase u-connectMatter onto `master`, then retire
`cmag_matter_support`. Until then ucxapp and Matter cannot share one checkout.

## 6. Footprint on STM32

Measured / documented:

| Image | Flash | Static RAM (BSS) | Source |
|---|---|---|---|
| u-connectMatter, NUCLEO-H753ZI, standard mode | 678 KB | ~440 KB (SDK 400-500 KB BSS) | `README_STM32.md` |
| u-connectMatter, NUCLEO-F767ZI | 619 KB | 234 KB | `README_STM32.md` |
| u-connectMatter, F407 / F429 / ODIN-W26 (minimal mode) | 673-679 KB | 174-224 KB | `README_STM32.md` |
| ucxapp Zephyr, NUCLEO-H753ZI | 400 KB | 291 KB | `arm-none-eabi-size build/zephyr_h753/zephyr/zephyr.elf` |
| ucxapp Zephyr, NUCLEO-F767ZI | 397 KB | 275 KB | `build/zephyr_f767/zephyr/zephyr.elf` |
| ucxapp FreeRTOS heap per board | - | 6 KB (G071) to 160 KB (H753) | `app/stm32_freertos/CMakeLists.txt` `UCXAPP_HEAP_SIZE` |

Sum on H753: ~1.08 MB flash of 2 MB (fine), ~730 KB static RAM plus Matter's heap (session, crypto and
message buffers) and ucxapp's heap. The H753 has 1 MB RAM but no region larger than 512 KB (D1 AXI SRAM);
D2 288 KB, DTCM 128 KB, D3 64 KB. Matter's ~440 KB BSS alone needs D1 (the H723 with 320 KB D1 already fails,
see `README_STM32.md`). ucxapp's 291 KB would have to move to D2 + DTCM, and most of it is static tables
(GNSS, IMU, CAN, perf, web page, scan and GATT tables) that Matter never uses.

A ucxapp lite build (module packs only, as `UCXAPP_LITE` on F103/G071 which fits 20 KB RAM / 128 KB flash
with Wi-Fi + BT/GATT) costs on the order of 100-150 KB flash and 20-40 KB RAM. That is the only shape that
coexists with Matter on H753 / F767: `libucxapp` built with `UCXAPP_HOST=0` and a Matter-specific pack list
(device, wifi, thread, sock, gatt, fw, at). Nothing fits next to Matter on F4 boards (Matter already runs
minimal mode there with 174-224 KB BSS of 192-256 KB).

Conclusion: do B on Windows and Linux first (no footprint question, Matter is already there), take H753 /
F767 with the lite pack as a second step if the demo wants a shell on the board, forget F4 and the lite MCUs.

## 7. What Matter gains, concretely

| Matter today | With ucxapp |
|---|---|
| `ComPortDetection.cpp` (36 KB C++), per-target `.cmd` launchers | `ucxapp.py --detect` / `--list-modules` / `--list-ports` (`ucxapp_platform_list_serial_ports`), `UCX_*` variables for tests |
| 'x' key XMODEM menu, `OTAImageProcessorImpl` firmware check, `IsFirmwareTooOld` | `fw.flash <file.zip>`, `fw.update [force]`, `fw.github.list`, `fw.download latest` |
| 'u' key AT logging | `app.log.at.on`, `at.trace <file>` + `at.trace.show` / `at.trace.pcap` (Wireshark) |
| `ThreadStackManagerImpl` + `tests/thread` scripts | `thread.status/dataset/join/scan/neighbors/routers/srp/dns/ping` |
| `NetworkCommissioningDriver` Wi-Fi scan / connect | `wifi.scan`, `wifi.connect`, `wifi.profile.*`, `wifi.ap.*`, `wifi.loc` |
| `BLEManagerImpl` hand-built GATT service | `gatt.server.hostchar`, `gatt.server.indicate`, `gatt.server.readresp`; `gatt.server.wps` for Wi-Fi provisioning from a phone |
| `WebServerManager` + branch `u_webserver.c` | one `u_webserver.c`, `web.start`, the ucxapp web UI with a Matter pane |
| `KeyValueStoreManagerImpl` per OS | `ucxapp_store.c` (host files, littlefs on H753/F767/F439/F407, RAM elsewhere) |
| Hand-written test `.cmd` / `.py` | `tests/` pytest with `--detect --test tests/hardware`, stress and soak markers |
| Windows, Linux, FreeRTOS, Zephyr, Pico ports | the same four (no Pico) plus WebAssembly; CAN, GNSS, IMU and cellular bridges for demos |

## 8. Risks

- Two owners of one `uCxAtClient_t`: ucxapp commands issue AT commands from the console thread while Matter's
  URC dispatcher runs; every ucxapp command must run under `uCxSessionLock` and must not call
  `uCxAtClientHandleRx`/`ProcessUrcs` (ucxapp's `ucxapp_thread.c` URC task has to be disabled in library mode).
- Socket handles: ucxapp's `sock.*` and `ucxapp_stream.c` keep their own tables; a `sock.close` on a Matter
  socket would break a session. Hide host sockets from the shell or mark them foreign.
- `ucxapp_urc_register()` installs handlers for every URC (`+UEWSNU`, `+UESODA`, `+UUBTC`, ...); u-connectClient
  keeps one callback per URC, so whoever registers last wins. In library mode Matter registers and ucxapp reads
  Matter's state (`IsNetworkReady`, `GetUdpEndpoint`) instead.
- Binary size on PCs is irrelevant (ucxapp.exe is 2.2 MB); build time grows by the CMake step inside GN.
- ALMA-W1 (Wi-Fi 6 + BLE + Thread): neither tree has module-specific code yet; the MERGED generated API and
  `ucxapp_module_has_thread()` are where it lands in ucxapp, `ThreadStackManagerImpl` in Matter.
