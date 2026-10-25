# u-connectXpressApp

**Talk to your u-connectXpress module in one line.** Scan Wi-Fi, join a network, open sockets, publish over MQTT, fetch HTTPS pages, pair Bluetooth devices and update firmware, from an interactive shell, a script or a web page. The same commands run on Windows, Linux and on an STM32 board.

One command set from development to production: interactive shell, web console, CI scripts with JSON output, diagnostics, firmware update and an embedded host.

```
   Shell / TUI      Web UI       --exec / --json / --script (CI, production test)
        \             |             /
         +------ command engine ---+      one table of commands (app/src/ucxapp_cmds_*.c)
                      |
               u-connectClient            AT client library
                      |
                UART (SPI coming)
                      |
           NORA-W36 / NORA-B26 module
```

Every interface runs the same commands, so what works in the shell works in a script, in the browser and on an STM32 board.

Supported modules: **NORA-W36** (Wi-Fi + Bluetooth LE) and **NORA-B26** (Bluetooth LE). NORA-W40 is not supported yet.

| Windows | Linux | STM32 firmware |
|---|---|---|
| Signed `.exe`, FTDI driver built in | Single binary | NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI |

Download ready-made builds from the [Releases page](https://github.com/u-blox/u-connectXpressApp/releases). No installation needed.

## Try it in 30 seconds

```powershell
ucxapp device.info
```

Leave out the port and the app finds your EVK by itself. Type `help` in the shell to list every command.

## Powerful one-liners

**Get online and fetch a web page**
```powershell
ucxapp COM13 --exec "wifi.connect MySsid MyPassword" --exec "net.ping u-blox.com" --exec "http.get https://example.com"
```

**Scan the air for networks**
```powershell
ucxapp wifi.scan
```

**Update the module firmware over XMODEM, with SHA256 verification**
```powershell
ucxapp fw.flash module.bin
```

**Always the newest firmware**: look up the latest GitHub release for the connected module and flash it
```powershell
ucxapp --exec "fw.download latest module.zip" --exec "fw.flash module.zip"
```

**Download firmware straight from GitHub and flash it**: the module fetches the release asset over Wi-Fi (redirects followed), the app verifies the SHA256 and flashes it
```powershell
ucxapp --exec "wifi.connect MySsid MyPassword" --exec "fw.download https://github.com/<owner>/<repo>/releases/download/<tag>/module.bin module.bin <sha256>" --exec "fw.flash module.bin"
```
`fw.download` deletes the file if the SHA256 does not match, so a bad download is never flashed. Without Wi-Fi, download the file in a browser and use `fw.flash` directly.

**Remote control from any browser**: the module serves its own web UI over Wi-Fi
```powershell
ucxapp COM13 --exec "wifi.connect MySsid MyPassword" --exec "web.start" --shell
```
It prints `http://<module-ip>:<port>/#t=<token>`. Open it on your phone or laptop for a command box with live output.

**Secure GNSS data bridge**: the app runs its own small CA and serves the receiver stream over TLS (add `clientauth` to require client certificates); `web.start https` does the same for the web UI
```powershell
ucxapp COM13 --exec "utils.tls.ca.create" --exec "utils.tls.server.create server 192.168.1.42" --exec "wifi.connect MySsid MyPassword" --exec "gnss.open" --exec "gnss.bridge tls 2947" --shell
```
Clients trust `tls-ca.crt`; another module connects with `cert.upload ca tls-ca.crt` and `sock.tls`.

**Use it in CI and test scripts**: every `--exec` runs in order and the app exits with code 1 on the first failure.

## Automation and diagnostics

**Machine-readable output**: `--json` prints one JSON object per command with `success`, a stable `code` (`OK`, `USAGE`, `NOT_CONNECTED`, `UNKNOWN_CMD`, `FAILED`), the `error` text, the `output` lines and the `Key: value` lines as `data`.
```powershell
ucxapp --json device.info
{"command":"device.info","success":true,"code":"OK","output":[...],"data":{"manufacturer":"u-blox","model":"NORA-W36","firmware_version":"3.1.0"}}
```
`help --json` lists every command with its arguments and whether it needs a module or is destructive (`danger`).

**Keep secrets off the command line**: `${NAME}` in `--exec` and shell commands takes an environment variable, and `wifi.connect <ssid> -` prompts for the password.
```powershell
$env:UCX_WIFI_PASS = "..."; ucxapp --exec 'wifi.connect MySsid ${UCX_WIFI_PASS}'
```

**Test scripts with a report**: one command per line, `expect <regex>` checks the output of the command above, `expect-fail` expects it to fail. Steps print PASS/FAIL; `--junit` writes a report for CI. See [examples/](examples/).
```powershell
ucxapp --script examples/automation/production-test.ucx --junit production.xml
```

**Support report**: `device.caps` shows what the module and this build can do; `device.diagnose` checks the module, Wi-Fi, DNS, internet and certificates and ends with PASS or FAIL. `ucxapp --doctor` checks the PC side: tools, Python, serial access and the attached modules.

## Highlights

- **Bluetooth**: scan, connect, bond, SPS, GATT client and server, saved profiles
- **Wi-Fi** (NORA-W36): station and AP modes, scanning, auto-reconnect, saved profiles
- **Network services**: sockets, HTTP/HTTPS, MQTT, TLS and certificates, ping, DNS
- **Firmware updates**: XMODEM with SHA256 verification
- **Scriptable**: `--exec` for one-shots, `--shell` for interactive use
- **Portable C11 command layer**: add a command by adding one table entry
- **On a microcontroller too**: run the same shell on an STM32 Nucleo board with the module wired to a UART, with a transparent `bridge` mode for AT debugging

## Commands at a glance

Type `help` in the shell, or `help wifi` to filter by prefix. `examples` lists ready-to-copy command sequences; `gnss.ex` (or `examples gnss`) shows them for one group. Full list with arguments: [command reference](COMMANDS.md).

| Area | Commands |
|---|---|
| Device | `device.info`, `device.reboot`, `device.factory confirm`, `at <raw AT command>` |
| Wi-Fi | `wifi.scan`, `wifi.connect <ssid> [pw]`, `wifi.status`, `wifi.disconnect`, `wifi.ip`, `wifi.rssi`, `wifi.profile.*`, `wifi.ap.*` |
| Network | `net.ping <host>`, `net.dns <host>`, `http.get <url>`, `http.download <url> <file>` |
| Sockets | `sock.open`, `sock.tls`, `sock.connect`, `sock.send`, `sock.read`, `sock.listen`, `sock.accept`, `sock.sendto`, `sock.recvfrom`, `sock.close` |
| MQTT | `mqtt.connect`, `mqtt.subscribe`, `mqtt.publish`, `mqtt.read`, `mqtt.disconnect` |
| App TLS | `utils.tls.ca.create`, `utils.tls.server.create`, `utils.tls.client.create`, `utils.tls.list`, `utils.tls.pem`, `utils.tls.remove` (HTTPS and TLS bridges) |
| Bluetooth | `bt.scan`, `bt.connect`, `bt.pairing`, `bt.bonds`, `bt.profile.*`, `bt.list` |
| GATT | `gatt.services`, `gatt.read`, `gatt.write`, `gatt.subscribe`, `gatt.client.*`, `gatt.server.*` (heart rate, battery, NUS, SPS, Wi-Fi provisioning and more) |
| HID | `hid.setup`, `hid.text`, `hid.key`, `hid.mouse`, `hid.click`, `hid.media` (Bluetooth keyboard, mouse and media keys); `hid.client` (read a BLE keyboard or mouse) |
| Utilities | `utils.qr.text`, `utils.qr.wifi`, `utils.qr.geo`, `utils.zip`, `utils.unzip` |
| Sensors | `gnss.detect`, `gnss.open`, `gnss.status`, `gnss.sats`, `gnss.bridge`, `imu.detect`, `imu.open`, `imu.status`, `imu.bridge` (PC serial sensors, bridged over TCP, UDP or BLE) |
| Cellular | `cell.detect`, `cell.open`, `cell.status`, `cell.apn`, `cell.attach`, `cell.ping`, `cell.connect`, `cell.bridge`, `cell.at` (a cellular modem on a PC serial port) |
| Certificates | `cert.list`, `cert.upload`, `cert.info`, `cert.remove` |
| Firmware | `fw.download [latest [file]]` or `fw.download <url> <file> [sha256]`, `fw.flash <file> [baud]` |
| Web UI | `web.start`, `web.stop`, `web.status` |

## Quick start from source

```powershell
git clone https://github.com/u-blox/u-connectXpressApp.git
cd u-connectXpressApp
git submodule update --init u-connectClient
git submodule update --init --recursive third-party/mbedtls
.\ucxapp             # builds on first run, then starts the shell
```

Linux: `./ucxapp.py`. STM32: `.\ucxapp --stm32 --build --flash --log`.
Find attached modules, GNSS receivers and IMUs: `.\ucxapp --detect`.

## Memory footprint

Release builds, updated by `.\ucxapp --everything --build`. On Windows and Linux: executable size and peak RAM
after start; on STM32: flash and RAM used (RAM includes the FreeRTOS heap, see [docs/STM32_MEMORY.md](docs/STM32_MEMORY.md)).

<!-- memory -->
| Target | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `win` | Windows x64 | 1.6 MB | - | - | 8.6 MB | - | - |
| `linux` | Linux x64 | 1.4 MB | - | - | 2.2 MB | - | - |
| `h753` | NUCLEO-H753ZI | 416 KB | 2048 KB | 20% | 284 KB | 512 KB | 56% |
| `f767` | NUCLEO-F767ZI | 403 KB | 2048 KB | 20% | 299 KB | 368 KB | 81% |
| `f439` | NUCLEO-F439ZI | 406 KB | 2048 KB | 20% | 102 KB | 192 KB | 53% |
| `f407` | STM32F4DISCOVERY | 403 KB | 1024 KB | 39% | 102 KB | 128 KB | 80% |
| `f401` | NUCLEO-F401RE | 384 KB | 512 KB | 75% | 85 KB | 96 KB | 89% |
| `f446` | NUCLEO-F446RE | 384 KB | 512 KB | 75% | 101 KB | 128 KB | 79% |
| `f103` | NUCLEO-F103RB | 113 KB | 128 KB | 89% | 15 KB | 20 KB | 78% |
| `g071` | NUCLEO-G071RB | 117 KB | 128 KB | 92% | 25 KB | 36 KB | 71% |
<!-- /memory -->

## Documentation

- [Command reference](COMMANDS.md): every command with usage
- [Building, flashing and releasing](BUILDING.md): prerequisites, launchers, STM32 boards, signing, file structure
- [u-connectClient](https://github.com/u-blox/u-connectClient): the small-footprint AT client library underneath

## Security notes

- Saved Wi-Fi passwords (`ucxapp.wifi`, `wifi.profile.*`) are XOR-obfuscated so they are not readable at a glance. This is obfuscation, not protection: anyone with the file can recover them.
- The web UI is protected by a random 128-bit token in the printed URL. Use `web.start https` on networks you do not trust.

---

# Disclaimer
Copyright &#x00a9; u-blox

u-blox reserves all rights in this deliverable (documentation, software, etc.,
hereafter “Deliverable”).

u-blox grants you the right to use, copy, modify and distribute the
Deliverable provided hereunder for any purpose without fee.

THIS DELIVERABLE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
WARRANTY. IN PARTICULAR, NEITHER THE AUTHOR NOR U-BLOX MAKES ANY
REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY OF THIS
DELIVERABLE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.

In case you provide us a feedback or make a contribution in the form of a
further development of the Deliverable (“Contribution”), u-blox will have the
same rights as granted to you, namely to use, copy, modify and distribute the
Contribution provided to us for any purpose without fee.
