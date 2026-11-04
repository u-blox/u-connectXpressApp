---
name: ucx-matter
description: "How u-connectMatter (Matter on a host MCU/PC over NORA-W36 Wi-Fi, NORA-B26 Thread, later ALMA-W1) relates to u-connectXpressApp and u-connectClient. Use when: asked about Matter, CHIPoBLE, commissioning, u-connectMatter, moving Matter onto ucxapp, linking ucxapp into another app (libucxapp), the u-connectClient cmag_matter_support branch, mDNS or binary-URC sockets, or whether Matter + ucxapp fit on an STM32 H7."
---

# u-connectMatter and u-connectXpressApp

u-connectMatter (`E:\u-blox\u-connectMatter`, https://github.com/u-blox/u-connectMatter) runs the Matter SDK
(connectedhomeip, C++17, GN/ninja) on a host and uses a u-connectXpress module as its radio: NORA-W36 for
Wi-Fi + CHIPoBLE, NORA-B26 for Thread, ALMA-W1 next. It talks to the module through u-connectClient directly,
via its own Matter `DeviceLayer` in `platform/ucxclient/` (about 800 KB of C++, `UcxClientManager.cpp` alone
is 208 KB, 165 distinct `uCx*` calls). ucxapp is a C11 command shell over the same library. The two share the
library and the hardware, nothing else, and they cannot share one UART at the same time.

The full comparison, the API gap list and the footprint budget are in
[references/analysis.md](./references/analysis.md). Read it before proposing any merge or shared code.

## Short answers

- **Can Matter move onto ucxapp?** Not as a process swap. ucxapp has no library target, a flag-based URC model
  (`gUcxappEventFlags`) and polls sockets with `uCxSocketReceiveFrom`; Matter needs callbacks into its
  `SystemLayer`, UDP payload pushed inline in `+UESODBF`, `uCxSessionLock`, mDNS and multicast, none of which
  ucxapp has today. The realistic direction is the other way: ucxapp as a **library inside u-connectMatter**
  (`libucxapp`: `app/src` minus `ucxapp_main.c`), Matter keeps its DeviceLayer and owns the `uCxHandle_t`.
- **What Matter gains:** `--detect`/`--list-modules`/`--exec` launcher, `fw.flash`/`fw.update`, `thread.*`,
  `wifi.*`, `gatt.server.*`, `at.trace`, `bridge`, the web UI and one `u_webserver.c` instead of two copies,
  the pytest hardware test framework, the store (KVS) and the STM32/Zephyr ports.
- **What ucxapp must add first** (useful to ucxapp on its own): `mdns.*` (done: responder, host name,
  services, TXT records, `+UEMDU`/`+UEMDD` flags), `sock.mcast` (done: `uCxSocketMulticastGroup`, IPv4 and
  IPv6 with `::`), `bt.adv` (done: custom AD payload, type, interval, start/stop). Still open: u-connectClient
  `master` must take the binary-URC socket callbacks and `uCxSessionLock` from the `cmag_matter_support`
  branch so both apps build from one library; the module firmware has no mDNS browse/resolve/subtype yet.
- **Does it fit an H7?** Flash yes, RAM no. Matter on NUCLEO-H753ZI is 678 KB flash and ~440 KB BSS
  (`README_STM32.md`); ucxapp on the same board is 400 KB flash and 291 KB static RAM (Zephyr `zephyr.elf`).
  Together ~1.1 MB flash (2 MB available) but ~730 KB static RAM plus heaps, and the H753's 1 MB RAM is split
  (512 KB D1 AXI, 288 KB D2, 128 KB DTCM, 64 KB D3). Only a lite ucxapp (module packs only, no GNSS/IMU/CAN/
  perf/web, like `UCXAPP_LITE` on F103) could ride along on H753/F767. **Windows and Linux first**; F4 never.

## Working with both repos

- Hardware: one module per app at a time. Stop u-connectMatter before `.\ucxapp COMx`; use ucxapp for
  `fw.flash`, `wifi.scan`, `thread.dataset`, `at.trace`, then start Matter.
- u-connectClient: ucxapp tracks `master` (MERGED generated API, NORA-W36X + NORA-B26X); Matter pins the
  `cmag_matter_support` branch (binary URC payload in `uUESODB_t`/`uUESODBF_t`, `uCxSessionLock`, webserver).
  `master` already has the binary-URC signature for `NORA-B26X-THREAD`; the W36X/MERGED variants do not.
- Module behaviour and AT commands: the `u-blox-docs` MCP server (`.mcp.json`).
- Matter console keys ('x' XMODEM, 'u' AT log, 'c' commissioning window, 'w' web, '!' factory reset) map to
  ucxapp `fw.flash`, `app.log.at.on`/`at.trace`, a future `matter.commission`, `web.start`, `dev.factory`.
