---
name: ucx-capture
description: "Capture BLE and Wi-Fi air traffic around a u-connectXpress module with ucxapp: the Nordic nRF Sniffer for Bluetooth LE (sniff.*) and a WLAN Pi doing Wi-Fi monitor-mode capture over ssh (wlanpi.*), with an optional live view in Wireshark. Use when: asked about sniff.start/stop/status/view, wlanpi.host/setup/start, following the module's BLE address, capturing on the module's Wi-Fi channel, pcap files, Wireshark, nrfutil ble-sniffer, tcpdump on the WLAN Pi, or the capture tests (tests/test_capture.py)."
---

# BLE and Wi-Fi capture

Two host-only helpers (Windows and Linux; not on STM32 or in the browser build). Both write a pcap file that
grows while capturing, so `live` opens Wireshark on it right away. Docs: `docs/COMMANDS.md` sections
"BLE sniffer (PC only)" and "WLAN Pi capture (PC only)".

## BLE: Nordic nRF Sniffer (`sniff.*`)

Hardware: an nRF52840 dongle or DK with the nRF Sniffer firmware (USB 1915:522A or "nRF Sniffer" in its name).
Software: `nrfutil` with `ble-sniffer` (`nrfutil install ble-sniffer`), and Wireshark to view.

A board without the sniffer firmware (it shows up under another USB ID) is flashed with
`py -3 third-party/nrf-sniffer/flash_sniffer.py [--board dongle|52840dk|52833dk|52dk]`, which holds Nordic's
4.1.1 images (MIT). Put the board in its bootloader first: a UF2 drive (Makerdiary MDK dongle, Seeed XIAO: the
image is converted and copied), the Nordic dongle's side RESET button (USB DFU through `nrfutil`), or a DK over
J-Link.

```
.\ucxapp --detect                     # lists the sniffer port (role "nRF Sniffer")
sniff.detect
sniff.start [file.pcap] [follow=module|<addr>|name:<name>] [port=<port>] [adv] [coded] [rssi=<-dBm>] [live]
sniff.status                          # file, size, packet count
sniff.stop
sniff.show [file] [count=50] [skip=0] [filter=<text>] [summary]   # text view, no Wireshark needed
sniff.view <file.pcap>                # opens Wireshark
```

- `follow=module` reads the connected module's BLE address and follows it, so connections to it are captured.
- `adv` keeps only advertising, `rssi=-60` drops far devices, `coded` listens on LE Coded PHY.
- Sniffer ports are never probed by the module, GNSS or IMU detectors (`ucxapp_port_is_sniffer`,
  `ucxapp_port_skip`) and are left out of `--detect-force`.

Example (`sniff.example`):

```
sniff.start follow=module live
# connect to the module from a phone, then:
sniff.stop
```

## Wi-Fi: WLAN Pi (`wlanpi.*`)

ucxapp runs tcpdump on the WLAN Pi over ssh and streams the capture back. Login is by password only (no SSH keys):
`wlanpi.setup <password>` seals it in the store.

```
wlanpi.host 192.168.1.202 user=wlanpi iface=wlan0   # saved in the store (ucxapp.wlanpi)
wlanpi.setup <password>   # once: saves it sealed in the secure store, checks the login, adds the sudo rule
wlanpi.start [file.pcap] [<n>|channel=<n>|module] [width=20|40+|40-|80] [filter=<bpf>] [live]   # wlanpi.start 11 = channel 11
wlanpi.status
wlanpi.stop
wlanpi.show filter=eapol                 # text view (summary: one line per transmitter)
```

- `channel=module` asks the module for its current Wi-Fi channel; it fails if the module is not connected.
- The remote side puts the interface in monitor mode, sets the channel with `iw`, then runs
  `exec sudo -n tcpdump -i IF -U -w -`; stop sends `sudo -n kill -INT <pid>`.
- Login is by password, no SSH key: `wlanpi.setup <password>` seals it in the store (`ucxapp.wlanpi.key`), ssh
  gets it from ucxapp itself as `SSH_ASKPASS` (`UCXAPP_ASKPASS`, `ucxapp_main.c`), and sudo once on stdin to write
  `/etc/sudoers.d/ucxapp` (checked by `visudo`) allowing `tcpdump`, `iw`, `ip` and `kill` without a password.

## Code

| File | What |
|---|---|
| `app/src/ucxapp_proc.c/.h` | child processes (start, read, stop) on Windows and POSIX |
| `app/src/ucxapp_capture.c/.h` | Wireshark lookup, live feeder thread, pcap/pcapng packet count, default file names |
| `app/src/ucxapp_cmds_sniff.c` | `sniff.*` |
| `app/src/ucxapp_cmds_wlanpi.c` | `wlanpi.*` |
| `ucxapp.py` | sniffer port role in `--detect` |

Examples live in `kExamples` inside `#ifndef __EMSCRIPTEN__`. The number of example groups must stay below
`GROUP_MAX` in `ucxapp_cmds_examples.c`, or later groups lose their `.example` command.

## Tests

`tests/test_capture.py` runs offline with the stand-in `tests/capture_fake.py`, selected through overrides that
are raw command prefixes: `UCXAPP_NRFUTIL`, `UCXAPP_SSH`, `UCXAPP_WIRESHARK`. `UCX_FAKE_DIR` and `UCX_FAKE_FAIL`
steer the fake. Python stand-ins need about 1.5 s to start, so tests use `app.wait 1500`. An `--exec` chain stops
at the first error, so test each usage error in its own run.

```
py -3 -m pytest tests/test_capture.py
```
