---
name: ucx-matter-plugin
description: "Build, run, test and extend ucxmatter, the Matter plug-in of u-connectXpressApp: a Matter node on a u-connectXpress module (NORA-W36 Wi-Fi, NORA-B26 and ALMA-W1 Thread, commissioning over Bluetooth LE). Use when: asked about ucxapp --matter, matter.start, matter.status, matter.set, matter.qr, the QR or manual pairing code, adding or removing Matter endpoints, app/src/ucxapp_matter.h, the matter/ folder, gn or ninja, the Matter SDK submodule, matter/patches, porting code from u-connectMatter, or the Matter tests."
---

# ucxmatter: the Matter plug-in of u-connectXpressApp

ucxapp stays the main app and owns the UART. The Matter node is an optional plug-in, `ucxmatter.dll`
(`libucxmatter.so` on Linux), next to the app. `matter.start` loads it, lends it the app's AT client and passes it a
copy of every URC, so every other command, `at.trace`, the TUI and the web page keep working while the node runs.

**Today (step 0):** the plug-in advertises a commissionable node over Bluetooth LE and shows the QR and manual codes
from the Matter SDK's own generators; endpoints are chosen at `matter.start`. **Not yet:** the Matter stack itself
(commissioning, Wi-Fi and Thread drivers, the data model), so a phone finds the node but cannot pair with it.

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

## Run

```powershell
.\ucxapp COM75                 # then, in the shell:
matter.start                   # default endpoints: light, temp, contact; or: matter.start light light contact
matter.status                  # plug-in, radios, node state; matter.qr shows the codes again
matter.endpoints               # endpoints of the node and the kinds the plug-in can build
matter.set light 40            # act as the device: light on|off|0..100, temp -40..125, contact open|closed
matter.log detail              # how much of the SDK's log is printed: error, info, detail
matter.stop                    # also done when the app leaves; matter.reset forgets what the node stored
```

The module needs Bluetooth LE with a peripheral role, and Wi-Fi or Thread (`dev.caps`). `matter.example` shows worked examples.

## Layout

| Path | What |
|---|---|
| `app/src/ucxapp_matter.h` | The whole interface between app and plug-in, plain C: AT client calls, URC copy, print, log, event, QR, sealed store, tick |
| `app/src/ucxapp_cmds_matter.c` | Loads the plug-in, `matter.*` commands, the URC tap |
| `matter/src/` | The plug-in: `plugin.cpp` (entry, commands, advertising), `endpoints.cpp` (endpoint kinds), `onboarding.cpp` (SDK setup codes), `logging.cpp` (SDK log to the app), `ucxm_host.c` and `ucxm_at.h` (the generated u-connectXpress API on the app's AT client) |
| `matter/BUILD.gn`, `.gn`, `args.gni`, `build_overrides/` | The gn build; output in `build/matter` |
| `matter/patches/` | Whole Matter SDK files from u-connectMatter, copied over the SDK before every build |
| `matter/compat/windows/` | POSIX headers the SDK includes and MinGW lacks |
| `matter/tools/` | `gn.exe`, `ninja.exe` |
| `tests/matter_stub/` | Stand-in plug-in built with the app, so the offline tests need no SDK |

## Rules

- Nothing in the plug-in bypasses `ucxapp_matter.h`: no UART, no files, no sockets of the PC, no console. Storage goes
  through `storeSave`/`storeLoad` (sealed by the secure store as `matter.<name>.key`), never a plain file.
- The interface is versioned (`UCXAPP_MATTER_ABI`): raise it when a struct changes; add fields at the end.
- Windows is the reference platform. Keep Linux (`.so`), STM32 (the same sources linked into the firmware, ucxapp
  calling the entry directly) and WebAssembly possible: Windows-only code stays in `matter/compat/windows`.
- Take code from `E:\u-blox\u-connectMatter` (the working reference) rather than rewriting it. Its module bring-up
  (`UcxClientManager`), web server, console and main loop are ucxapp's job and stay behind; `uCxSessionLock` and
  direct binary sockets are not needed with ucxapp's u-connectClient.
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
.\ucxapp --detect --test tests/hardware/test_hw_matter.py            # two modules: the second finds the node's advertisement
```

`tests/test_matter_plugin.py` and the hardware test skip when `ucxmatter` is not built. `UCX_MATTER_PLUGIN` points
the app at another plug-in file (the tests use it); it is never set from a command.

How u-connectMatter itself relates to ucxapp, and the footprint on STM32: the `ucx-matter` skill.
