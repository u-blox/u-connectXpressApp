---
name: ucx-matter-plugin
description: "Build, run, test and extend ucxmatter, the Matter plug-in of u-connectXpressApp: a Matter node on a u-connectXpress module (NORA-W36 Wi-Fi, NORA-B26 and ALMA-W1 Thread, commissioning over Bluetooth LE). Use when: asked about ucxapp --matter, matter.start, matter.status, matter.set, matter.qr, the QR or manual pairing code, adding or removing Matter endpoints, app/src/ucxapp_matter.h, the matter/ folder, gn or ninja, the Matter SDK submodule, matter/patches, porting code from u-connectMatter, or the Matter tests."
---

# ucxmatter: the Matter plug-in of u-connectXpressApp

ucxapp stays the main app and owns the UART. The Matter node is an optional plug-in, `ucxmatter.dll`
(`libucxmatter.so` on Linux), next to the app. `matter.start` loads it, lends it the app's AT client and passes it a
copy of every URC, so every other command, `at.trace`, the TUI and the web page keep working while the node runs.

**Today:** the Matter server runs in the plug-in on the device layer `matter/platform/ucxclient`, with the data
model `matter/data_model/ucxmatter.zap` (root node, dimmable light, temperature sensor, contact sensor). It defines
the Matter GATT service (FFF6, C1 and C2) on the module, opens a commissioning window, advertises and shows the QR
and manual codes. Discriminator, passcode, SPAKE2+ salt and the UniqueID are the node's own: drawn at random the
first time it starts and kept sealed as `matter.factory.key` (as the i.MX 93 kit does for its end nodes), so
nothing can be worked out from the module's address. `matter.reset` keeps them, like the label on a product;
`matter.reset all` draws new ones. `matter.qr` only shows the codes; `matter.open` opens the window and advertises.
A commissioner connects and
pairs over Bluetooth LE (BTP, PASE), installs its fabric and gives the node a Wi-Fi network: the Wi-Fi driver
(`NetworkCommissioningDriver.cpp`) scans, joins it with the module and reports back when the module has an
address. From then on the node talks UDP through a socket on the module and is announced by the module's mDNS.
A module that is on Wi-Fi already (`wifi.connect`) is used as it is. To a controller the node is the product
`u-connectXpressApp` of vendor `u-blox` (Basic Information, `DeviceInstanceInfoProviderImpl.h` from
`support/DeviceConfig.h`): the module's type code is its hardware version and part number, the module's serial
number its own and the module's firmware version its software version (`matter.status` shows them). Checked so
far: a whole commissioning with a phone over Bluetooth LE and Wi-Fi (the light then switches from the home app),
the Bluetooth LE transport over the air between two modules, and a pairing request over UDP on the virtual
module. **Not yet:** the endpoints of `matter.set`
are not tied to the data model's attributes (a controller's change shows as an event); the module's mDNS has no
subtypes, so a commissioner cannot browse for the discriminator over the network. Thread waits for a module with
Thread firmware.

The default log (`matter.log info`) shows commissioning as six steps, one line each
(`commissioning [##----] 2/6 fabric installed`, `matter/src/progress.cpp`): paired with the passcode, fabric
installed, network credentials received, joining the network, on the network, complete. A step that fails says
which one and why. The Matter SDK's own log is `matter.log detail`.

## Build

```powershell
.\ucxapp --matter --build      # also --clean, --rebuild; the result is app/bin/ucxmatter.dll
```

- Needs GCC: MSYS2 with `pacman -S mingw-w64-ucrt-x86_64-gcc` on Windows (the Matter SDK does not build with MSVC),
  `g++` on Linux. Git and this Python; the launcher installs the SDK's build modules with pip the first time.
- `gn.exe` and `ninja.exe` are in `matter/tools`: the binaries u-connectMatter uses (gn is signed by u-blox and
  accepted by IT). Use these, do not download others. Linux takes `gn` and `ninja` from `../u-connectMatter/tools` or PATH.
- The first build fetches the Matter SDK into `matter/connectedhomeip` (about 1.5 GB). It is an opt-in submodule
  (`update = none`): nothing else fetches it, and the app builds and tests without it.
- On Windows the SDK's build scripts run in a Python environment the launcher makes in `build/matter-python`
  (pip, once): the SDK's build fails when Python and the build folder are on different drives. The build's own
  actions run in `build/matter/python-venv` (`build_venv` in `matter/BUILD.gn`), which holds only what the SDK's
  code generator package asks for; the SDK's default environment installs dozens of modules, some of which do
  not build on a new Python. A fresh build folder needs the network for both.
- The SDK's weak callbacks have strong definitions in `matter/src/callbacks.cpp` (MinGW's linker does not take a
  weak definition from another object file): a server cluster added to the data model needs its
  `emberAf...ClusterInitCallback` there.
- Data model: edit `matter/data_model/ucxmatter.zap` with the ZAP tool, then `.\ucxapp --matter --zap` regenerates
  `ucxmatter.matter` and the code under `matter/data_model/ucxmatter` (ZAP from `ZAP_INSTALL_PATH` or
  `../u-connectMatter/tools/zap-*`). All of it is checked in, so a build needs no ZAP tool.

### In the browser and on boards (linked in)

```powershell
.\ucxapp --wasm --matter --build   # build/matter-wasm/libucxmatter.a, linked into build/wasm/site
```

A browser (and a board) loads no library at run time: the node is built as a static library and linked into the
app, which defines `UCXAPP_MATTER_STATIC` and calls `ucxmatter_plugin()` directly (`matter.status` shows
`plug-in: built in`). Everything else (`matter.*`, the AT client, the URC copy, the store) is the same interface.
The node builds u-connectClient into itself as the app does, so every C name it defines gets the prefix `ucxm_`
(`matter/compat/wasm/ucxm_symbols.h`, force-included; `ucxm_at.h` includes it first). The launcher brings the
list up to date from `emnm` and builds once more when it changed: commit it. Names the SDK tests with `#ifndef`
(`chipAbort`, `chipDie`: `MATTER_WASM_HOOKS` in `ucxapp.py`) keep their own. The node uses the app's mbedTLS,
linked once. Test: `tests/test_wasm.py::test_matter_node_is_built_in`.

## Run

```powershell
.\ucxapp COM75                 # then, in the shell:
matter.start                   # default endpoints: light, temp, contact; or: matter.start light light contact
matter.status                  # plug-in, radios, node state; matter.qr shows the codes again
matter.card                    # the node at a glance; the web page's Matter tab under What's happening is this card
matter.endpoints               # endpoints of the node and the kinds the plug-in can build
matter.set light 40            # act as the device: light on|off|0..100, temp -40..125, contact open|closed
matter.open                    # commissioning window for 15 minutes again, with the codes
matter.bridge tcp              # the node as lines on port 7540: EP light = on, STATE ..., EVT ...; send set light on, get
matter.bridge mqtt             # after mqtt.connect: ucxapp/matter/<endpoint> (retained), ucxapp/matter/<endpoint>/set
matter.log detail              # how much of the SDK's log is printed: error, info, detail
matter.stop                    # also done when the app leaves
matter.reset                   # forget what the node stored, leave Wi-Fi, reboot the module, start the node again
matter.reset all               # as matter.reset, new setup codes, and the module factory-reset instead of rebooted
matter.selftest                # plug-in, module, sealed store, random generator, crypto known answers (SHA-256, HMAC,
                               # HKDF, PBKDF2, AES-CCM, AES-CTR, ECDSA, ECDH, CSR, SPAKE2+, session keys, fabric ID;
                               # matter/src/crypto_check.cpp, the SDK's vectors), setup codes, Bluetooth LE, network
```

After a reset the node (when it ran) starts again with the same endpoints and, having no fabric, opens the
commissioning window for 15 minutes and advertises (FFF6 with its discriminator): scan the QR or type the manual
code in the home app; when the window has closed, `matter.open`. A factory reset also puts the module's UART back
to 115200 baud.

The module needs Bluetooth LE with a peripheral role, and Wi-Fi or Thread (`dev.caps`). `matter.example` shows worked examples.

The web page has a Matter tab in What's happening whenever the plug-in is installed: the node's state, the
commissioning steps as segments, a button or a number field for each value an endpoint takes, and buttons for
start, stop, QR code, open window, SDK log and factory default. It is a view of `matter.card`: the plug-in's
`card()` gives `TAG value` lines (`EP    1 light = on 40%  [on | off | 0..100 (percent)]`: the page builds the
controls from the bracket, so a new endpoint kind needs no change in the page), the app adds `LOG`, and every
button runs a `matter.*` command. `card()` is called from the web thread four times a second: it sends no AT
command and only tries the Matter stack lock (`NodeSnapshot`), keeping the last state when the thread is busy.

## Layout

| Path | What |
|---|---|
| `app/src/ucxapp_matter.h` | The whole interface between app and plug-in, plain C: AT client calls, URC copy, print, log, event, QR, sealed store, tick; from the plug-in: start, stop, urc, command, card, sbom (its components, for `app.sbom`) |
| `app/src/ucxapp_cmds_matter.c` | Loads the plug-in, `matter.*` commands, the URC tap, the live card (`ucxapp_matter_card`, sent to the page by `ucxapp_cmds_web.c` as the `matter` event) |
| `matter/src/` | The plug-in: `plugin.cpp` (entry, commands), `node.cpp` (the Matter server: start, stop, commissioning window), `progress.cpp` (the commissioning steps of the default log), `callbacks.cpp` (the data model's callbacks), `endpoints.cpp` (endpoint kinds), `onboarding.cpp` (SDK setup codes), `UbloxCommissionableDataProvider.cpp` (the node's random setup codes, kept in the store), `logging.cpp` (SDK log to the app), `ucxm_host.c` and `ucxm_at.h` (the generated u-connectXpress API on the app's AT client) |
| `matter/data_model/` | `ucxmatter.zap` (the data model), `ucxmatter.matter` and the generated code in `ucxmatter/` |
| `matter/platform/ucxclient/` | The Matter device layer from u-connectMatter: `BLEManagerImpl` (GATT service, advertising), `NetworkCommissioningDriver` (Wi-Fi scan and join for the commissioner), `UcxNetwork.cpp` (Wi-Fi state from the module's URCs, the UDP sockets and their data), `DnssdImpl` and `UcxMdns.cpp` (mDNS on the module), configuration and key-value store (through the app's sealed store, `UcxStore.h`), `PlatformManagerImpl` (the Matter thread). `UcxClientManager` here is a small facade: the handle, the URC queue and which URCs are the node's alone |
| `matter/BUILD.gn`, `.gn`, `args.gni`, `build_overrides/` | The gn build; output in `build/matter` (Windows), `build/matter-linux` (Linux, WSL included, so the two never clobber each other) and `build/matter-wasm` (`--wasm --matter`) |
| `matter/mbedtls/` | The app's mbedTLS 4 (`third-party/mbedtls`, app config and `app/third-party/mbedtls-gen`) as gn target `//mbedtls`; the SDK's crypto uses it through its PSA backend (`chip_crypto = "psa"` with the PSA session keystore: the PSA AES-CCM takes PSA key ids, with the raw keystore every message after PASE fails to decrypt). The SDK's own mbedTLS 2.28 (end of life) is never fetched |
| `matter/patches/` | Whole Matter SDK files from u-connectMatter, copied over the SDK before every build |
| `matter/compat/windows/` | POSIX headers the SDK includes and MinGW lacks |
| `matter/tools/` | `gn.exe`, `ninja.exe` |
| `tests/matter_stub/` | Stand-in plug-in built with the app, so the offline tests need no SDK |

## Rules

- Nothing in the plug-in bypasses `ucxapp_matter.h`: no UART, no files, no sockets of the PC, no console. Storage goes
  through `storeSave`/`storeLoad` (sealed by the secure store as `matter.<name>.key`), never a plain file. The
  blob `factory` is what the node is made with (its setup codes): `matter.reset` (done by the app, not the
  plug-in) removes every blob but that one.
  Random numbers come from the SDK's `Crypto::DRBG_get_bytes` (`psa_generate_random` of the app's mbedTLS: the system's entropy on a PC; a board needs its
  hardware generator as the entropy source before the node may draw codes there).
- The interface is versioned (`UCXAPP_MATTER_ABI`, now 4): raise it when a struct changes; add fields at the end.
- Windows is the reference platform. Keep Linux (`.so`), STM32 (the same sources linked into the firmware, ucxapp
  calling the entry directly) and WebAssembly possible: Windows-only code stays in `matter/compat/windows`.
- Take code from `E:\u-blox\u-connectMatter` (the working reference) rather than rewriting it. Its module bring-up
  (`UcxClientManager`), web server, console and main loop are ucxapp's job and stay behind. No `uCxSessionLock`
  anywhere (ucxapp's u-connectClient needs none) and no direct binary sockets.
- Targets are ucxapp's: Windows, Linux, WebAssembly and the STM32 boards (FreeRTOS or Zephyr) with room for the
  Matter SDK (H753, F767, F439, F407; `UcxRamTier.h`). Code for other boards (Pico, LCD console, macOS) stays in
  u-connectMatter.
- mDNS runs on the module (`AT+UMD*`, `chip_mdns = "platform"`; the build refuses anything else), like the sockets:
  never the SDK's responder on the host, which would need multicast through the module and keep the host awake.
- While it runs, the node does not unbond the module, change its Bluetooth mode or local name, or store (`AT&W`).
  Only `matter.reset` touches the module: it disconnects Wi-Fi and reboots it, and `matter.reset all`
  factory-resets it (`AT+USYFR`, `AT&W`, reboot). What the node sets (pairing and security mode, connection and advertising intervals, its GATT
  service) lasts until the module reboots. Wi-Fi is shared with the app: the node joins the network a
  commissioner gives it (as `wifi.connect` would) and uses a network that is up already. When the node stops, its mDNS service is removed and the
  responder and its host name are put back as they were. Its UDP socket is made in buffered read mode, and the
  read mode the app had is restored right after.
- URCs reach the plug-in on the AT client's thread: it queues them for the Matter thread and returns true for the
  ones that are the node's alone (its GATT handles, data on its sockets), which the app's own handlers then do
  not see. Wi-Fi URCs go to both. The URC handlers run on the Matter thread without the stack lock: what they
  give the SDK (a datagram, the answer to the commissioner) takes the lock or goes through `ScheduleWork`.
- The SDK's log lines are `detail`; the node's own milestones and the commissioning steps are `info` (`matter.log`).
- Warnings: our files build with none. For SDK sources the launcher turns off `-Wundef` and
  `-Wignored-attributes` (they test macros of other platforms); fix anything else at its cause (`EnforceFormat.h`
  patch, `compat/windows`).
- `UCX_KEYLOG_FILE` (patched `PSASessionKeystore.cpp`) writes the session keys of the node to that file for
  Wireshark, only in a plug-in built with `ucxapp --matter --keylog --build` (`matter.status` says `KEY LOG BUILD`):
  a debug aid, never for a node that is in use. A normal build has no such code.
- New endpoint kind: one entry in `kKinds[]` in `matter/src/endpoints.cpp` (name, device type, values, init/set/show)
  and the same kind in `tests/matter_stub` when the offline tests should know it.

## Matter SDK and patches

- The SDK version is the submodule commit (v1.6.0.0). `matter/patches` mirrors the SDK tree with whole files made
  for that commit; the launcher copies them when they differ and refuses another SDK commit, because whole files of
  one version would undo the changes of another. To move the SDK: update the submodule, redo each patch on the new
  file, commit both together.
- The patches are stored byte for byte (`.gitattributes`: `matter/patches/** -text`).

## Tests

```powershell
py -3 -m pytest tests/test_matter.py tests/test_matter_plugin.py     # stand-in plug-in; real plug-in on the virtual module
.\ucxapp --detect --test tests/hardware/test_hw_matter.py            # two modules: the second finds the node's advertisement and its GATT service
```

The virtual module plays a commissioner that opens the transport (BTP handshake on C1) when the Matter service is
activated, is on Wi-Fi from the start, and with `sockets.requests` on port 5540 plays a client on the network
(the tests send a pairing request over UDP and check the Matter server's answer). `at_burst` in
`tests/harness/matter.py` measures the node's runs of AT commands from an `at.trace`:
the tests (offline and on a real UART) fail if a command waits for the receive thread, which is what
u-connectMatter needed its session lock for. A whole commissioning needs a commissioner (a phone, chip-tool):
no test here does that.

`tests/test_matter_plugin.py` and the hardware test skip when `ucxmatter` is not built. `UCX_MATTER_PLUGIN` points
the app at another plug-in file (the tests use it); it is never set from a command.

How u-connectMatter itself relates to ucxapp, and the footprint on STM32: the `ucx-matter` skill.
