# u-connectXpressApp

**Your u-connectXpress module, live in seconds. Plug in, type a command, see it work, then automate exactly that.**

**[Try it in your browser now](https://u-blox.github.io/u-connectXpressApp/)**: no install, Chrome or Edge talks to the EVK over Web Serial. No EVK? Click **Virtual module**.

```powershell
ucxapp wifi.connect MySsid MyPassword
ucxapp mqtt.publish test/ucx hello
ucxapp gatt.server.hrs 60             # your module is now a heart rate sensor on any phone
```

- **One command set, everywhere**: interactive shell, `--exec` scripts, CI, the browser, the module's own web UI and STM32 firmware (FreeRTOS and Zephyr).
- **Batteries included**: Wi-Fi, Bluetooth LE, GATT, SPS, HID, sockets, multicast, mDNS, TLS, HTTP, MQTT, certificates, firmware update.
- **Built for testing**: `--json`, `--script` with `expect`, JUnit reports, AT traces with Wireshark export, `dev.diagnose` with PASS/FAIL verdicts.
- **Ready to run**: signed Windows `.exe`, single Linux binary and STM32 images on the [Releases page](https://github.com/u-blox/u-connectXpressApp/releases).

```
 ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
 │  Shell, TUI  │ │   Browser,   │ │ --exec/json, │ │    STM32     │
 │              │ │  module web  │ │ --script, CI │ │   firmware   │
 └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
        └────────────────┴───────┬────────┴────────────────┘
                                 ▼
               ╔═══════════════════════════════════╗
               ║        one command engine         ║
               ║   same commands on every target   ║
               ╚═════════════════╤═════════════════╝
                                 ▼
               ┌───────────────────────────────────┐
               │  u-connectClient (AT client lib)  │
               └─────────────────┬─────────────────┘
                                 │  UART / SPI
                                 ▼
               ┌───────────────────────────────────┐
               │       NORA-W36  /  NORA-B26       │
               │   Wi-Fi  Bluetooth LE  Thread     │
               └───────────────────────────────────┘
```

### Supported modules

| Capability | NORA-W36 | NORA-B26 | NORA-W56, W57 |
|---|:-:|:-:|:-:|
| Device, firmware update | yes | yes | not yet |
| Bluetooth LE, GATT, SPS, HID | yes | yes | not yet |
| Wi-Fi, sockets, TLS, HTTP, MQTT | yes | - | not yet |

The app asks the module what it can do (`dev.caps`) and shows a short summary on connect.

## Try it in 30 seconds

```powershell
ucxapp
```

No port needed: the app finds your EVK, shows the module, firmware and radios, and opens the shell. Type `help`, or `wifi.example` for ready-to-copy recipes. The first Wi-Fi command asks once where you are (regulatory domain); CI sets `UCX_REGDOMAIN`.

## What do you want to do?

Pick a goal, run the recipe, read the verdict. No radio background needed.

| Your goal | Run | What you get |
|---|---|---|
| Will my product work on the Wi-Fi where it is installed? | `ucxapp --script examples/use-cases/wifi-site-check.ucx` | Signal and stability over 30 s, internet check, a verdict (EXCELLENT, GOOD, FAIR, POOR) with advice |
| Send sensor readings to the cloud and get commands back | `ucxapp --script examples/use-cases/sensor-to-cloud.ucx` | A proven MQTT round trip through a public broker |
| Show live data in a phone app, with no app of my own | `ucxapp --script examples/use-cases/phone-heart-rate.ucx` | The module acts as a standard heart rate sensor that nRF Connect and fitness apps read |
| Replace a serial cable with Bluetooth LE | `ucxapp --script examples/use-cases/phone-serial-link.ucx` | Text from a phone terminal app printed by the module |
| Something does not work | `ucxapp dev.diagnose` | Each check PASS, FAIL, NOT RUN or NOT SUPPORTED, with the next step to try |

## Wow in one line

```powershell
ucxapp --exec "wifi.connect MySsid ${UCX_WIFI_PASS}" --exec "http.get https://example.com"   # online in seconds
ucxapp --exec "fw.download latest module.zip" --exec "fw.flash module.zip"                   # newest firmware, SHA256 checked
ucxapp COM13 --web                                                                           # the module serves its own web UI
ucxapp --script examples/automation/production-test.ucx --junit production.xml               # production test with a CI report
ucxapp --json dev.info                                                                       # machine-readable for your tools
```

Something not working? `ucxapp dev.diagnose` checks module, Wi-Fi, DNS, internet and certificates and tells you what to try next.

Every command, with arguments: [COMMANDS.md](COMMANDS.md), or `help` in the shell.

## Quick start from source

```powershell
git clone https://github.com/u-blox/u-connectXpressApp.git
cd u-connectXpressApp
git submodule update --init u-connectClient
git submodule update --init --recursive third-party/mbedtls
.\ucxapp             # builds on first run, then starts the shell
```

Linux: `./ucxapp.py`. STM32: `.\ucxapp --stm32-freertos --build --flash --log` (Zephyr: `--stm32-zephyr`). Browser: `.\ucxapp --wasm`. Details: [BUILDING.md](BUILDING.md).

## Small enough for a microcontroller

The full app fits a NUCLEO-F401RE; a lite build runs in 20 KB RAM on an F103. Updated by `.\ucxapp --everything --build` ([details](docs/STM32_MEMORY.md)).

<!-- memory -->
| Target | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |
|---|---|---|---|---|---|---|---|
| `win` | Windows x64 | 2.1 MB | - | - | 10.0 MB | - | - |
| `linux` | Linux x64 | 2.1 MB | - | - | 2.4 MB | - | - |
| `h753` | NUCLEO-H753ZI | 502 KB | 2048 KB | 25% | 309 KB | 512 KB | 60% |
| `f767` | NUCLEO-F767ZI | 491 KB | 2048 KB | 24% | 323 KB | 368 KB | 88% |
| `f439` | NUCLEO-F439ZI | 494 KB | 2048 KB | 24% | 127 KB | 192 KB | 66% |
| `f407` | STM32F4DISCOVERY | 491 KB | 1024 KB | 48% | 104 KB | 128 KB | 81% |
| `f401` | NUCLEO-F401RE | 464 KB | 512 KB | 91% | 83 KB | 96 KB | 87% |
| `f446` | NUCLEO-F446RE | 476 KB | 512 KB | 93% | 103 KB | 128 KB | 81% |
| `f103` | NUCLEO-F103RB | 127 KB | 128 KB | 100% | 15 KB | 20 KB | 79% |
| `g071` | NUCLEO-G071RB | 127 KB | 128 KB | 100% | 25 KB | 36 KB | 72% |
<!-- /memory -->

## Secure by default

Saved Wi-Fi passwords are sealed with AES-256-GCM (per-install key), the web UI needs a random 128-bit token (`web.start https` for untrusted networks), passwords never appear in traces or the web page's command history, and `app.sbom` lists every component. The CRA / EN 18031 points the app can show are pinned to [tests/test_cra.py](tests/test_cra.py).

mbedTLS is only built into the host targets (Windows, Linux, browser), for `tls.*`, `utils.cert.*`, `utils.encrypt` and the sealed passwords. The STM32 builds (FreeRTOS and Zephyr) leave it out: the module does TLS itself and `cert.*` only uploads files to it. On STM32, saved Wi-Fi passwords are only obfuscated (XOR), not encrypted, so turn on read-out protection (RDP level 1) on boards that keep real credentials.

## Part of u-connectXpress

- **[u-connectXpress](https://github.com/u-blox/u-connectXpress)**: the AT command firmware in NORA-W36 and NORA-B26 that this app drives, with its documentation and examples.
- **[u-connectXplorer](https://github.com/u-blox/u-connectXplorer)**: the companion phone and desktop app. It scans, browses GATT, talks SPS and NUS and provisions Wi-Fi over BLE. Use it as the phone on the other side of ucxapp: `gatt.server.sps` for a serial link, `gatt.server.hrs` for live data, `gatt.server.wps 120` to let it send the module the Wi-Fi settings. Get it on [Google Play](https://play.google.com/store/apps/details?id=com.ublox.uconnectxplorer) or the [App Store](https://apps.apple.com/app/u-connectxplorer/id6766648417).
- **[u-connectClient](https://github.com/u-blox/u-connectClient)**: the small-footprint AT client library ucxapp is built on.

Wi-Fi provisioning (`gatt.server.wps`, `gatt.client.wps`, `gatt.wps`) is the **Nordic Wi-Fi Provisioning Service** from the nRF Connect SDK (UUID `14387800-130c-49e7-b877-2881c89cb258`, protobuf messages), so u-connectXplorer and Nordic's provisioning apps work with it; the Bluetooth SIG has no standard service for this. "wps" here is not Wi-Fi Protected Setup (the router push button). The server also accepts ucxapp's own text commands (`SET_CONFIG:<ssid>:<password>:<WPA2|WPA3|OPEN>`) on the same service.

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
