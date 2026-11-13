# Command Layer

A small, portable layer for driving a u-connectXpress module with text commands instead of the
menu. It lives in `app/src/ucxapp_*` and has no Windows dependencies, so the same
commands can run in a PC console, a Linux terminal or a UART shell on an embedded target.

## Using it

```powershell
# One-shot: run commands in order, stop at the first failure (exit code 1)
ucxapp COM13 --exec "dev.info" --exec "wifi.scan"

# Interactive shell; with commands piped in, the exit code is 1 when the last one failed (like sh)
ucxapp COM13 --shell

# A single command can be given directly (no spaces, no quotes), port optional
ucxapp COM13 dev.info
ucxapp dev.info
```

- Add `--tui` for a three-pane text UI (see [Text UI](#text-ui)).
- Add `--web` to use only the web UI: Wi-Fi from the saved profiles unless already connected, then `web.start` (unless an `--exec` started it, e.g. `--exec "web.start 443" --web`), until q, Esc or Ctrl+C.

- Without a port argument, `--exec` and `--shell` use the saved port from the ini file.
- `--offline` skips the module auto-detection, for commands that need no device (`tls.*`, `help`).
- If the module cannot be connected, commands that need it fail with `not connected`; `help` still works.
- Arguments are whitespace separated; use double quotes for values with spaces
  (`wifi.connect "My Network" "my password"`). `\"` and `\\` escape inside quotes.
- Lines starting with `#` are comments. Maximum 12 arguments per command.
- `--json` prints one JSON line per command: `command`, `success`, `code` (`OK`, `USAGE`, `NOT_CONNECTED`,
  `UNKNOWN_CMD`, `UNSUPPORTED`, `FAILED`), `error`, `moduleError` (`{"code": -5, "name": "U_ERROR_..."}` when the
  module returned an error code), `output` lines and `data` (the `Key: value` lines).
- Wi-Fi commands (`wifi`, `sock`, `mqtt`, `http`, `net`, `cert`, `tftp`, `web`, `coap`, `dtls`) are refused with
  `UNSUPPORTED` on a Bluetooth LE only module (NORA-B26); the model is read once per connection.
- `${NAME}` in `--exec` and shell commands is replaced by the environment variable (not in the web UI).

## Command reference

Commands are grouped like the u-connectXpress AT manual: general, system, Bluetooth, GATT, Wi-Fi,
network, sockets, MQTT, HTTP, security, then the app's own tools. `help <prefix>` lists a group
(`help wifi`, `help gatt.server`).

### General

| Command | Arguments | Description |
|---|---|---|
| `help` | `[prefix]` | List commands, optionally filtered (`help wifi`); `help --json [prefix]` gives name, usage, help, `needsDevice` and `danger` per command |
| `examples` | `[group]` | List the groups that have examples, or show one group's examples |
| `manual` | `[topic\|command\|all]` | Read this manual in the app: without an argument the section list, with a heading word (`manual wi-fi`) or a command (`manual wifi.ap.enable`) that section as plain text, `all` for everything. In the web UI the **Manual** button (or typing `manual <topic>`) opens it formatted, with contents, search and clickable commands (PC, browser, NUCLEO-H753ZI and NUCLEO-F767ZI builds) |
| `manual.search` | `<text>` | Every line of the manual that mentions the text, grouped by section |
| `<group>.example [n]`, `examples [group] [n]` | | Real scenarios with ready-to-type commands for gnss, imu, wifi, net, http, sock, perf, mqtt, cert, bt, gatt, hid, web, fw, dev, cfg, at, cell, can, hw, coap, tftp, ft, dtls, log, file and utils (`gnss.example`); only groups in the build are listed. With `n`, runs example n line by line and stops at the first error or before a command that changes data (`wifi.example 1`; on the Virtual module the ones that only change the module, such as `dev.factory`, run, but never `fw.*`); the web page's "Run all" uses this |
| `at` | `[command]` | Send a raw AT command; the `AT` prefix is optional (`at +GMI`). With no command it enters AT mode: every line is sent as typed (quotes kept) until you type `back` |
| `at.reset` | | Reboot the module (`AT+CPWROFF`); same as `dev.reboot` |
| `at.factory` | `<confirm>` | Factory reset (`AT+USYFR`) and reboot; same as `dev.factory`, the word `confirm` is required |

### AT trace

Record the AT traffic with timestamps, read it decoded and send it again, like the packet view of u-center.
Recording works on Windows, Linux and in the browser (not in the STM32 firmware).

| Command | Arguments | Description |
|---|---|---|
| `at.trace` | `<file>` | Record every AT command, response and URC with its time (seconds since the start) until `at.trace.stop`; passwords are written as `****`. Lines look like `12.345 TX AT+UWSC=0` / `12.400 RX OK`, after a `# start` line with the wall-clock start time. A name ending in `.pcapng` writes a Wireshark capture instead |
| `at.trace.stop` | | Stop `at.trace` and write the file |
| `at.trace.show` | `<file> [errors]` | Show a trace decoded: each command with its time to OK/ERROR and up to 4 response lines, URCs with their meaning (`+UEWLD:0,3` = Wi-Fi link down), then counts per URC and the 5 slowest commands; `errors` shows only failures |
| `at.trace.pcap` | `<file> [out.pcapng]` | Convert a text trace to a Wireshark capture (default `<file>.pcapng`), see below |
| `at.decode` | `<urc...>` | Explain one URC line with its parameter names (`at.decode +UEWLD:0,3`) |
| `at.replay` | `<file>` | Send the commands of a trace again and compare OK/ERROR with the recording; skips lines with masked passwords, binary data, reboot, factory reset, firmware update and baud rate changes; fails when a result differs |

```powershell
ucxapp COM13 --exec "at.trace join.trace" --exec "wifi.profile.auto" --exec "at.trace.stop"
ucxapp --exec "at.trace.show join.trace"
ucxapp --exec "at.trace.pcap join.trace"
```

**Wireshark.** The text trace stays the main format; the pcapng capture is for Wireshark. Each AT line is one frame
for Wireshark's own AT dissector, with its direction (sent/received) and wall-clock time. That lets you merge the
capture with a Bluetooth sniffer or Wi-Fi capture (*File > Merge*) and see the AT commands next to the packets on air.
The Info column shows each URC's meaning and the time from each command to its OK/ERROR. Filter with
`at.cmd == "+UEWLU"`. Traces recorded before the `# start` line existed get times from 1970-01-01.

For more, load [scripts/wireshark/ucxapp_at.lua](scripts/wireshark/ucxapp_at.lua) (copy it to the personal Lua
plugins folder from *Help > About Wireshark > Folders*, or run `wireshark -X lua_script:ucxapp_at.lua x.pcapng`).
It adds `ucx.*` fields and links each command to its final response: `ucx.cmd == "USPSWB"`, `ucx.time > 100`
(slow commands), `ucx.result == "ERROR"`, `ucx.kind == "urc"`. It also flags failed commands and commands left
without an answer by the next command or a module restart (`ucx.no_reply`); *Analyze > Expert Information* lists
them all.

### Settings backup (cfg)

Save the module's stored settings as a file of AT set commands, compare it with a module or another file, and
load it into another module. Covers the Bluetooth, Wi-Fi station and access point (`wlan 0`), network, MQTT
(`mqtt 0`), NTP, SPS and system settings that can be read back. Passwords and keys cannot be read from the module,
so they are not in the file: add them by hand (`AT+UWSSW=0,"pass",0`) or set them after loading.

| Command | Arguments | Description |
|---|---|---|
| `cfg.show` | | Print the module's stored settings as AT set commands |
| `cfg.save` | `<file>` | Save them to a file with the module model, firmware and app version in `#` comments |
| `cfg.load` | `<file> [nostore]` | Write the lines that differ from the module and store them (`AT&W`) unless `nostore`; reports applied, unchanged, failed and skipped lines. Only known setting commands are sent; anything else is skipped. A setting that cannot change while in use (`-15`, e.g. Wi-Fi while connected) gets a hint to disconnect first |
| `cfg.diff` | `<file> [file2]` | Compare a `cfg.save` file with the module (or with `file2`), one line per difference (`~` changed, `-` only in the first, `+` only in the second); fails when they differ |

```powershell
ucxapp COM13 --exec "cfg.save golden.cfg"
ucxapp COM14 --exec "cfg.load golden.cfg" --exec "cfg.diff golden.cfg"
```

### System

| Command | Arguments | Description |
|---|---|---|
| `dev.info` | | Manufacturer, model, firmware version |
| `dev.summary` | | Short overview shown on connect (text UI, shell, web page once per session, `dev.use`): model, firmware, radios, Wi-Fi and Thread state, open BLE links and sockets, a newer firmware (built-in release or the one `fw.update` last found) and the last Wi-Fi profile and BLE peer when not connected |
| `dev.caps` | | Capabilities grouped by radio, Wi-Fi features and Bluetooth LE features: model, firmware, Wi-Fi, Bluetooth LE, Thread, the saved region, and the sockets/MQTT/TLS/HTTP/GATT features this module and build support, then what to try next. A Bluetooth LE only module (NORA-B26) shows `Wi-Fi: no` and no Wi-Fi features; the TUI and web UI then hide the Wi-Fi, socket and network groups. `Thread: yes` when the module answers `AT+UTHE?` (Thread firmware); otherwise `thread.*` is hidden |
| `dev.uptime` | | Time since the module started, counted from its last `+STARTUP` |
| `dev.stack` | | Developer: stack size, used bytes and usage % of every module task. Not in lite builds |
| `dev.crash` | | Developer: last crash info stored in module flash; reading it clears it. Not in lite builds |
| `dev.diagnose` | `[host]` | Support report: `dev.caps`, uptime, Wi-Fi, DNS and ping of the host (default u-blox.com), certificates. Each check prints `PASS`, `FAIL <code>`, `NOT RUN` (e.g. Wi-Fi not connected) or `NOT SUPPORTED` with a hint for the next step; ends with `Result: PASS`, `PARTIAL` (some checks not run) or `FAIL` |
| `dev.status` | | Overview: module info, Wi-Fi state and IP, open sockets, BLE connections, certificates |
| `dev.reboot` | | Reboot the module (clears GATT services and connections); use as the last command |
| `dev.power` | `[auto [<s>\|off]\|save <0\|1> [idle_ms]\|sleep\|wake\|deep [pin\|reset\|uart]]` | Power. Without arguments: the power save level and the power state (active, power save, light sleep or deep sleep; also in `dev.status` and the status line of the TUI and web page when not active). `auto [<s>]` lets a NORA-W36 save power after `<s>` seconds idle (default 30) and wake on traffic by itself; `auto off` turns it off. `save 1` lets a NORA-W36 save power after the idle time (stored; use 115200 baud or lower, the first command after idle is slower), `save 0` turns it off; the `1` is the level (on), the idle time is in ms. `sleep` is light sleep: a NORA-B26 keeps its Bluetooth links, a NORA-W36 (firmware 3.5.0 or later) keeps Wi-Fi and allows baud rates above 115200 but needs Bluetooth off first (`AT+UBTM=0`, `AT&W`, restart; the command says so otherwise). Events still arrive, and `wake` (or its WAKE_UP pin) wakes it for commands. `deep` is deep sleep without radio; it wakes from the wake-up pin (default), or on a NORA-B26 from `reset` or `uart` RX, and starts as after a reset. `wake` first sends a character (enough for light sleep and `deep uart`), then pulses DTR for hosts that wire DTR to WAKE_UP; on EVK-NORA-B26 and EVK-NORA-W36 the USB DTR does not reach WAKE_UP, so use `deep uart` (NORA-B26), the GPIO_J9 header pin or reset there. Connecting to a module that does not answer pulses DTR too. Not in lite builds |
| `dev.factory` | `<confirm>` | Reset the module to factory settings (erases Wi-Fi, Bluetooth and certificate settings) and reboot; the word `confirm` is required |
| `dev.connect` | `[port]` | Connect to the module (default: saved port). Windows only |
| `dev.disconnect` | | Disconnect. Windows only |
| `dev.use` | `<port>` | Switch the active module to another serial port; the current one is released (and kept if the new one fails). One module is active at a time |
| `fw.download` | `[latest\|eval [file]]` or `<url> <file> [sha256]` | With no arguments (same as `latest`) looks up the newest stable u-connectXpress GitHub release for the connected module family (e.g. NORA-W36) and saves its zip (Windows only); `eval` takes the newest release including evaluation builds. Flash it with `fw.flash`. With a URL: download via the module (follows redirects), print SHA-256, delete the file on mismatch |
| `fw.update` | `[force] [eval] [baud=921600]` | Compares the module firmware with the newest GitHub release for its family (stable only unless `eval`); if that is newer (or `force`), downloads the zip into the app store (reused if already there), flashes it and checks the module reports the new version. Versions compare including the build number (e.g. `3.6.0-012` < `3.6.0-019`). Download needs Windows |
| `fw.list` | | Lists the firmware zips and bin files already downloaded to the app store, with their sizes |
| `fw.github.list` | `[NORA-W36]` | Lists every firmware release on GitHub for the family (newest first), marking evaluation builds. Uses the connected module's family unless one is given (e.g. `NORA-B26`). Needs no device. Windows only
| `fw.bootloader` | `<port> <file.bin\|file.zip> [baud=921600]` | Flash a module that is in bootloader mode (no AT interface): waits for the `>` prompt, sends `x`, then XMODEM; run `dev.disconnect` first |
| `fw.flash` | `<file.bin\|file.zip> [baud=921600]` | XMODEM firmware update (default 921600 baud); a zip is only extracted (built-in unzip), and only the `.bin` inside is flashed |

```powershell
ucxapp COM13 --exec "dev.info"
ucxapp COM13 --exec "fw.download latest" --exec "fw.flash NORA-W36X-3.1.0.zip"
ucxapp COM13 --exec "at +GMR"
```

### Bluetooth

| Command | Arguments | Description |
|---|---|---|
| `bt.scan` | `[full] [filter]` | Scan for BLE devices (up to 30 s). One line per device (duplicates merged, named first, strongest first) with what it advertises: services, manufacturer, iBeacon/Eddystone, appearance. `full` decodes the whole advertising data; `filter` shows only devices matching a `bt.filter` entry. The results are kept for `bt.connect` |
| `bt.connect` | `[addr[,random]\|n\|name]` | With no argument: connect to the last used profile, else scan and take the strongest known device (same as `bt.profile.auto`). Otherwise connect by address, by the number from the last `bt.scan`, or by profile or device name (a unique start of the name is enough). A name that is not in the last scan is looked for with a filtered scan: filter slot 9 is set to the first 10 bytes of the name on AD type 9 (complete local name) and restored afterwards; the module compares bytes, so the case must match. Suggests `bt.profile.add` for devices not yet saved |
| `bt.addr` | `[addr]` | Show the module's Bluetooth address, or set it as `XX:XX:XX:XX:XX:XX`; a new address is stored and takes effect after `dev.reboot` |
| `bt.name` | `[name]` | Show the module's Bluetooth device name, or set it; a new name is active at once and stored |
| `bt.phy` | `[auto\|1m\|2m\|coded]\|<conn> [1m\|2m\|coded]` | Preferred PHY for new links (stored; `coded` is Coded S8, the longest range), or show or change the PHY of a connection |
| `bt.adv` | `[on\|off\|<hex> [connectable\|scannable\|broadcast]\|interval <min_ms> [max_ms]]` | Legacy advertising. Without arguments: on/off, interval, type and the custom AD payload decoded. `on`/`off` starts or stops it (the GATT server examples start it themselves). A hex payload (3-31 bytes of `<len> <type> <value>` AD structures, `0x` and `:` allowed) replaces the module's default data; `connectable` (default; the module prepends the flags, so 28 bytes at most), `scannable` (non-connectable) or `broadcast` (non-connectable, non-scannable) sets the type. `interval` takes 20-10240 ms and applies to the next start. The payload is stored by the module |
| `bt.filter` | `[<0-9> <ad_type> <text\|hex>\|<0-9> clear\|clear]` | Scan data filters. Without arguments: list them. Otherwise set filter 0-9 to an AD type (decimal or `0x..`: 9 complete name, 8 short name, 0xFF manufacturer data, 3 16-bit UUIDs) and up to 10 bytes to match, as text (`NORA`, case-sensitive) or hex (`4E4F5241`, `0x4E4F5241`, `4E:4F:52:41`; a text that is valid hex, like `CAFE`, is read as hex), or clear one or all. Used by `bt.scan filter` and kept in the module. Not in lite builds |
| `bt.cs` | `[on\|off\|initiator]` | Bluetooth Channel Sounding (NORA-B26/B27, firmware 3.4 or later). Without arguments: the mode. `on` makes the module a reflector, so a connected initiator (a phone or tag reader that supports Channel Sounding) measures the distance to it; `initiator` (NORA-B26 cs_initiator evaluation firmware) makes the module measure it; the setting is stored. Progress (capabilities exchanged, security, procedure on or off) is printed as it happens. Read the result on the initiator, or with `gatt.client.ras` when the module is the client. Not in lite builds |
| `bt.cs.range` | `<conn> [seconds=10\|on\|off]` | Initiator: range a reflector and print the distance the module computes (median filter plus Kalman filter, see `bt.cs.filter`), then the count and range of readings; any key stops. Ranging needs a bonded link; when the module refuses, the command bonds and retries for 15 s. `on`/`off` starts or stops ranging in the background, with distances in the events. Not in lite builds |
| `bt.cs.rate` | `[intervals=20]` | Initiator: connection intervals between measurements (1-65535); use a connection interval of 10 ms or more. Not in lite builds |
| `bt.cs.filter` | `[on\|off\|default\|<q> <r>]` | Initiator: the module's Kalman filter. `off` gives raw distances; Q 0-1000 and R 10-10000 in units of 0.001 (default 1 and 120; a larger R is smoother but slower). Not in lite builds |
| `bt.cs.phase` | `[on\|off\|ref off\|500\|1000\|10000]` | Initiator: phase correction term reports (binary, per antenna path) for your own algorithms, and a count of those received; `ref` replaces the measured data with a stored 500 mm, 1 m or 10 m reference (debug). Not in lite builds |
| `bt.disconnect` | `<handle>` | Close a BLE connection |
| `bt.list` |  | List BLE connections |
| `bt.pairing` | `<on\|off>` | Enable or disable pairing mode |
| `bt.iocap` | `<none\|display\|yesno\|keyboard\|keydisplay>` | Set the I/O capabilities (none = Just Works) |
| `bt.security` | `<none\|unauth\|auth\|sc\|sconly>` | Set the required security level |
| `bt.defaults` | `[secure]` | No security, no I/O, pairing on (default, applied automatically before any GATT server setup), or `secure`: LE Secure Connections with fallback to legacy pairing, numeric comparison (auto-confirmed) |
| `bt.autoconfirm` | `<on\|off>` | Accept numeric-comparison pairing automatically (default on) |
| `bt.confirm` | `<yes\|no>` | Answer a waiting pairing request (autoconfirm off) |
| `bt.passkey` | `<digits\|deny>` | Answer a waiting passkey request |
| `bt.bonds` |  | List bonded devices |
| `bt.bond` | `<conn\|addr> [seconds=15]` | Bond (pair and store keys) with a connected device, by connection number from `bt.list` or by address, and wait up to the given seconds for the result. When the peer shows a code, enter it with `bt.passkey`. Ranging with `bt.cs.range` and encrypted GATT need a bond |
| `bt.unbond` | `<addr\|all>` | Remove one or all bonds |
| `bt.profile.add` | `<name> <addr[,random]>` | Save a BLE device as a named profile in `ucxapp.ble` (current directory; max 16) |
| `bt.profile.list` |  | List saved BLE profiles |
| `bt.profile.remove` | `<name>` | Delete a BLE profile |
| `bt.profile.connect` | `<name>` | Connect to a saved BLE profile; it becomes the "last used" profile |
| `bt.profile.auto` |  | Connect to the last used profile, else scan and take the strongest known device |

```powershell
# Find a device, save it once, then reconnect by name
ucxapp COM13 --exec "bt.scan"
ucxapp COM13 --exec "bt.profile.add sensor 11:22:33:44:55:66,random"
ucxapp COM13 --exec "bt.profile.connect sensor"

# Secure pairing
ucxapp COM13 --exec "bt.defaults secure" --exec "bt.connect 11:22:33:44:55:66"
```

### GATT client and server

| Command | Arguments | Description |
|---|---|---|
| `gatt.services` | `[conn]` | Discover and list the remote services and characteristics |
| `gatt.chars` | `[conn] [service-index]` | List the remote characteristics |
| `gatt.read` | `[conn] <handle>` | Read a characteristic; known types are decoded |
| `gatt.write` | `[conn] <handle> <hex\|text:abc> [norsp]` | Write a characteristic |
| `gatt.writelong` | `[conn] <handle> <hex\|text:abc> [offset] [reliable] [more]` | Long write (prepare/execute) |
| `gatt.subscribe` | `[conn] <handle> [notify\|indicate\|off]` | Set the CCCD (assumed at value handle + 1) |
| `gatt.listen` | `[seconds\|off]` | Client examples keep listening after they subscribe: notifications are printed parsed and mirrored to the Events pane until `gatt.listen off` unsubscribes. Without arguments it shows the state; with seconds it waits (needed for `gatt.wps` and server examples) |
| `gatt.client.<svc>` | `[conn]` (`sps`: `[flow]`) | Discover a predefined service, read, decode and subscribe; `<svc>` is `hrs`, `cts`, `ess`, `lns`, `aio`, `bas`, `dis`, `ras` (Ranging Service, Channel Sounding), `nus`, `sps` or `witmotion` (WitMotion BLE IMU; decodes the 0x55 0x61 packets) |
| `gatt.send` | `[conn] <nus\|sps> <text>` | Send text to the remote NUS or SPS service |
| `gatt.sps.credits` | `[conn] <1-127>` | Send SPS credits |
| `gatt.wps` | `[conn] <status\|scan\|connect\|forget\|config <ssid> <pw> [OPEN\|WPA2\|WPA3]\|raw <text>>` | Send a Wi-Fi provisioning command to a remote device and wait for the response |
| `gatt.server.<svc>` | `[seconds]` | Define a predefined local service (once per module restart); with seconds, simulate values for a connected client. `<svc>` is `hrs`, `cts`, `ess`, `lns`, `aio`, `bas`, `ras`, `nus` or `sps` |
| `gatt.server.nus` / `.sps` / `.wps` | `[seconds]` | Nordic UART, u-blox Serial Port and Nordic Wi-Fi Provisioning servers; with seconds, serve a client for that long (client writes are printed, WPS requests are executed: the Nordic protobuf protocol that u-connectXplorer and the nRF apps send, or ucxapp's text commands; `.wps` ends early once a phone has sent the Wi-Fi settings and, for protobuf, been told the result). "wps" is not Wi-Fi Protected Setup |
| `gatt.server.list` |  | List the local characteristics defined by the server examples |
| `gatt.server.set` | `<name\|handle> <hex\|text:abc>` | Set a local characteristic value |
| `gatt.server.notify` | `[conn] <name\|handle> <data>` | Send a notification |
| `gatt.server.indicate` | `[conn] <name\|handle> <data>` | Send an indication |
| `gatt.server.demo` | `[conn] <hrs\|bas\|ess\|aio\|lns\|cts\|ras> [seconds]` | Send simulated values once per second |
| `gatt.server.servicechanged` | `[conn] <start> <end>` | Send a Service Changed indication |
| `gatt.server.error` | `[conn] <read\|write> <att-code>` | Answer a pending read or write with an ATT error |
| `gatt.server.hostchar` | `<svc-uuid> <char-uuid> <props-hex> [rsec] [wsec]` | Define a host-managed characteristic |
| `gatt.server.readresp` / `gatt.server.writeresp` | `<conn> <data>` / `<conn>` | Answer a pending host-characteristic read or write |
| `hid.setup` | `[auth]` | HID over GATT device (HOGP 1.0): composite keyboard, mouse and media-key report map, boot keyboard and mouse, Battery and Device Information (PnP ID); every HID attribute needs an encrypted link (`auth`: MITM pairing); advertises the HID UUID and appearance |
| `hid.status` |  | Show HID handles, notification state and protocol mode |
| `hid.key` | `<key> [ctrl\|shift\|alt\|gui ...]` | Send one key to the connected host (a character, or `enter`, `esc`, `tab`, `backspace`, `space`) |
| `hid.text` | `<text...>` | Type text on the connected host |
| `hid.mouse` | `<dx> <dy> [wheel]` | Move the pointer and scroll; large moves are split into steps of 127 |
| `hid.click` | `[left\|right\|middle] [double]` | Click a mouse button |
| `hid.media` | `<play\|next\|prev\|stop\|mute\|volup\|voldown\|brightup\|brightdown\|home\|back\|search>` | Send a consumer-control (media) key |
| `hid.client` | `<conn> [boot] \| off` | HID host: bond if needed, read the report map, subscribe to every input report and print keys, mouse moves and media keys (also to the Events pane) |

```powershell
# Client: read a heart rate sensor (connection handle 0 from bt.connect)
ucxapp COM13 --exec "bt.connect 11:22:33:44:55:66,random" --exec "gatt.client.hrs 0" --exec "gatt.listen 20"

# Server: act as a battery service and push simulated values for 60 s once a phone connects
ucxapp COM13 --exec "gatt.server.bas 60"

# Send text to a Nordic UART peer
ucxapp COM13 --exec "gatt.client.nus 0" --exec "gatt.send 0 nus hello"

# Use the module as a Bluetooth keyboard
ucxapp COM13 --exec "hid.setup" --exec "hid.text Hello from NORA"

# Read a BLE keyboard or mouse (HID host)
ucxapp COM13 --exec "bt.connect 11:22:33:44:55:66,random" --exec "hid.client 0" --exec "gatt.listen 60"
```

### Wi-Fi

| Command | Arguments | Description |
|---|---|---|
| `wifi.scan` | | List networks strongest first with BSSID, channel and band, signal, security (Open, WPA, WPA2, WPA3) and any saved profile; then channel usage with congestion and the quietest channel for `wifi.ap.enable`. The results are kept for `wifi.connect` |
| `wifi.channels` | `[mixed\|full]` | `mixed`: scans (`wifi.scan`, `wifi.locate`) use only the channels the last scan saw, with a full scan every 10th. `full`: clears that and the module's channel list (AT+UWCLR), back to full channel support. Connect and AP always use the full list. Not in lite builds |
| `wifi.connect` | `[ssid\|n] [password\|-]` | With no argument: connect like `wifi.profile.auto` (the last used profile, else the strongest known network). Otherwise connect by SSID or by the number from the last `wifi.scan`; `-` prompts for the password; waits up to 20 s and shows IP, mask, gateway, channel and signal. A saved profile for the SSID supplies the password; open networks need none |
| `wifi.status` | | Connection state, SSID, RSSI, IP |
| `wifi.eap` | `<ssid> <ca> <cert> <key> <identity> [1.2\|1.3\|any]` | Connect to a WPA2/WPA3-Enterprise network with EAP-TLS. `ca`, `cert` and `key` are names on the module (`cert.upload`, `cert.list`); the module requires an identity; TLS version defaults to `any`. Waits up to 30 s. Not in lite builds |
| `wifi.peap` | `<ssid> <user> <password\|-> <ca> [1.2\|1.3\|any]` | Connect to a WPA2/WPA3-Enterprise network with PEAP (MSCHAPv2); `-` prompts for the password. The module requires the CA that verifies the RADIUS server. Not in lite builds |
| `wifi.disconnect` | | Leave the network |
| `wifi.ip` | `[dhcp \| <ip> <mask> <gw> [dns1 [dns2]]]` | Show, or set, DHCP or static IP; applies on the next `wifi.connect` |
| `wifi.rssi` | `[count=1] [interval_s=1]` | Read the signal strength, repeatedly if requested |
| `wifi.survey` | `[seconds=10] [host]` | Is the Wi-Fi here good enough? Signal average and spread over time (default 10 s), lost readings, internet reachability (ping, default u-blox.com) and `Verdict: EXCELLENT`, `GOOD`, `FAIR` or `POOR` with advice; fails when not connected or the internet is unreachable |
| `wifi.locate` | `[google\|skyhook\|here\|combain] [api-key] \| key [<service> <api-key>] \| body [service]` | Position from a Wi-Fi scan, like ubxlib's `uWifiLoc`. NORA modules have no location command, so the host does the work: the module scans, the strongest access points (up to 20, at least 2; SSIDs ending in `_nomap` are left out) go to the location service as JSON over the module's HTTPS (`http.post`), and the answer is shown as `Position: <lat>, <lon> +/- <m> m`, an OpenStreetMap link and a `gnss.virtual <lat> <lon>` line to use it as a virtual receiver. Needs a Wi-Fi connection and an API key: Google Geolocation API, Skyhook, HERE Network Positioning v2 or Combain (Google, Skyhook and Combain take Google's request format, HERE its own). `key <service> <api-key>` saves the key in `ucxapp.wifiloc` (private) and makes that service the default; `key` alone lists the saved services (keys shortened). `<service> <api-key>` uses a key once without saving it. `body [service]` scans and prints the request without sending it. Not in lite builds |
| `wifi.roaming` | `[on\|off]` | Show or set roaming between access points |
| `wifi.regdomain` | `[0-10\|world\|etsi\|fcc\|ic\|nz\|mkk\|ncc\|acma\|kcc\|sa\|br\|<place>]` | Show, or set and save, where you are (the Wi-Fi regulatory domain). Takes the menu number (0 world ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 10 Brazil, the `AT+UWRD` value), the domain name or a place (`europe`, `usa`, `canada`, `japan`, `new-zealand` ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦). Without an argument at the `ucxapp>` prompt it shows the numbered list and asks; elsewhere it prints the list. Choose it once: `wifi.scan`, `wifi.connect`, `wifi.eap`, `wifi.peap`, `wifi.profile.*` connects, `wifi.ap.enable`, `wifi.locate` and BLE Wi-Fi provisioning ask for it the first time at the prompt; in scripts, `--exec` and the web page they warn and use `world` until one is set. `world` is allowed everywhere, with fewer channels. Scripts and CI can set `UCX_REGDOMAIN` instead; a saved domain wins |
| `wifi.profile.add` | `<name> <ssid> [password]` | Save or update a Wi-Fi profile in `ucxapp.wifi` (passwords encrypted with AES-256-GCM, key in `ucxapp.key`; XOR-obfuscated on STM32; max 16) |
| `wifi.profile.list` |  | List saved profiles (no passwords shown) and the network (IP prefix) each was last used on |
| `wifi.profile.remove` | `<name>` | Delete a saved profile |
| `wifi.profile.connect` | `<name>` | Connect with a saved profile; it becomes the "last used" profile |
| `wifi.profile.auto` | | Try the last used profile (10 s); else scan with the saved regulatory domain and connect to the strongest known profile. Keeps an existing link to the same SSID |
| `wifi.ap.enable` | `[ssid] [password] [channel]` | Start a WPA2 access point and show its join QR code |
| `wifi.ap.disable` |  | Stop the access point |
| `wifi.ap.status` |  | Show AP parameters, addresses and stations |

```powershell
# Connect once
ucxapp COM13 --exec "wifi.connect MySsid MyPassword" --exec "wifi.status"

# Save networks (home, office), then let the app pick the best one anywhere
ucxapp COM13 --exec "wifi.profile.add home MySsid MyPassword"
ucxapp COM13 --exec "wifi.profile.add office CorpNet secret"
ucxapp COM13 --exec "wifi.profile.auto"

# Static IP, applied on the next connect
ucxapp COM13 --exec "wifi.ip 192.168.1.50 255.255.255.0 192.168.1.1 8.8.8.8" --exec "wifi.connect MySsid MyPassword"

# Access point with a join QR code
ucxapp COM13 --exec "wifi.ap.enable NORA-Test secret123 6"
```

Passwords given on the command line are visible in the process list and shell history.

### Network

| Command | Arguments | Description |
|---|---|---|
| `net.ping` | `<host> [count=4]` | ICMP ping, count 1-10 (default 4); fails if no reply |
| `net.iperf` | `<server\|client\|stop> [args...]` | iperf throughput test run by the module: `server [tcp\|udp] [port]` keeps running until `stop`; `client <ip> [tcp\|udp] [seconds] [port]` runs for the given time (default 10 s, port 5001). Needs a Wi-Fi connection. PC side: iperf 2 (`iperf -c <module-ip>` / `iperf -s`) |
| `net.dns` | `<host>` | Resolve a host name |
| `net.ntp` | `<status\|on\|off\|server> [args...]` | NTP client: `status`, `on [manual\|auto]`, `off`, `server <0\|1> <host\|->` (`-` removes) |
| `coap.get` | `<host> <port> [path]` | CoAP GET over UDP via a module socket (confirmable, 3 tries, 2 s each) |
| `coap.selftest` | | Offline check of CoAP request encoding and response parsing |
| `tftp.on` | `[rw] [port=69] [seconds]` | TFTP server (RFC 1350, with `blksize`/`tsize` options) on a module UDP socket, default port 69; serves plain file names from the working folder, read only unless `rw`. Uploads land as `<name>.part` and are renamed when complete. With `seconds` it blocks that long, then stops. No authentication: trusted networks only |
| `tftp.off` | | Stop the TFTP server |
| `tftp.status` | | Server port, mode, transfer count and the last transfer |
| `tftp.get` | `<host> <remote> [local] [port]` | Download a file from a TFTP server |
| `tftp.put` | `<host> <local> [remote] [port]` | Upload a file to a TFTP server |
| `tftp.selftest` | | Offline check of TFTP name rules, options and packet encoding |
| `ft.on` | `[rw] [tcp[:port]] [seconds]` | File transfer server serving plain file names from the working folder; read only unless `rw`. Over BLE: a custom GATT service (`7a3f0001-5c1e-4d2b-9f6a-3e8b2c4d5e6f`, CTRL `...0002`, DATA `...0003`), needs an encrypted link, blocks up to 242 bytes. With `tcp`: a module TCP socket (default port 6970), the same packets framed as `[len u16][channel u8][packet]`, blocks up to 1455 bytes, one client at a time, no authentication (trusted networks only). Go-back-N with a window of 8; every file is checked with CRC-32 and size, uploads land as `<name>.part` and are renamed when verified |
| `ft.off` | | Stop the file transfer server |
| `ft.status` | | Server link, mode, transfer count and the last transfer |
| `ft.get` | `<conn\|host[:port]> <remote> [local] [blk=240] [resume]` | Download a file from a peer's `ft` server: a BLE connection number, or a host for TCP (blk 16-242 over BLE, default 240; up to 1455 over TCP, the default). An interrupted download keeps `<local>.part`; `resume` continues from it |
| `ft.put` | `<conn\|host[:port]> <local> [remote] [blk] [resume]` | Upload a file to a peer's `ft` server (needs `rw` there); `resume` continues from the `.part` the server kept |
| `ft.list` | `<conn\|host[:port]>` | List the files on a peer's `ft` server |
| `ft.remove` | `<conn\|host[:port]> <name>` | Delete a file on a peer's `ft` server (needs `rw` there) |
| `ft.selftest` | | Offline check of the CRC-32, name rules and request encoding |
| `ancs.on` | | Advertise as `ucx ANCS` with an ANCS service solicitation so an iPhone can pair from Settings > Bluetooth |
| `ancs.sub` | `[conn]` | Subscribe to the iPhone's Apple Notification Center Service on a connection (default 0); needs a bonded, encrypted link. New notifications print as `[ANCS] added #uid category` |
| `ancs.off` | | Unsubscribe from the iPhone's notifications |
| `ancs.list` | | Notifications currently on the phone: uid, category, important flag and available actions |
| `ancs.show` | `<uid>` | App id, title, subtitle, message, date and action labels of one notification |
| `ancs.action` | `<uid> pos\|neg` | Positive or negative action, e.g. answer or decline an incoming call |
| `ancs.selftest` | | Offline check of the ANCS event and attribute parsing and request encoding |
| `notify.on` | | Phone notifications for Android (no ANCS there) or any app: defines GATT service `7a3f0010-5c1e-4d2b-9f6a-3e8b2c4d5e6f`; a paired companion app writes `<id>\|<app>\|<title>\|<text>` (UTF-8) to Notification `...0011`, shown as `[notify] #id app: title - text` |
| `notify.off` | | Stop showing phone notifications (the service stays until reboot) |
| `notify.list` | | The last 16 notifications |
| `notify.action` | `<id> <action>` | Notify `<id>\|<action>` on Action `...0012` to the phone app, e.g. `dismiss` |
| `notify.selftest` | | Offline check of the notification format |

```powershell
ucxapp COM13 --exec "wifi.profile.auto" --exec "net.dns example.com" --exec "net.ping example.com 3"

# TFTP server with uploads; from a PC: curl -T fw.bin tftp://<module-ip>/  or  tftp -i <module-ip> GET notes.txt
ucxapp COM13 --exec "wifi.profile.auto" --exec "tftp.on rw"
```

### Sockets

| Command | Arguments | Description |
|---|---|---|
| `sock.open` | `<tcp\|udp>` | Create a socket and print its handle |
| `sock.connect` | `[handle] <host> <port>` | Connect; waits up to 10 s |
| `sock.tls` | `[handle] <ca> [client key]` | Use TLS 1.2/1.3 on a TCP socket before `sock.connect`; names are certificates from `cert.upload` (e.g. `tls-ca.crt` from another app to reach its `bridge tls`) |
| `sock.send` | `[handle] <text...>` | Send text (arguments are joined with single spaces, max 511 bytes; `\r`, `\n` and `\\` are converted) |
| `sock.read` | `[handle] [max] [wait_s=5]` | Wait for data (default 5 s) and print up to `max` bytes (default and maximum 512) |
| `sock.bind` | `[handle] <port>` | Bind a socket to a local port (UDP receive) |
| `sock.listen` | `[handle] [port]` | Listen for TCP connections |
| `sock.accept` | `[wait_s=30]` | Wait for an incoming TCP connection (default 30 s) and print its handle |
| `sock.sendto` | `[handle] <host> <port> <text...>` | Send a UDP datagram |
| `sock.recvfrom` | `[handle] [max] [wait_s=5]` | Receive a UDP datagram and print the sender |
| `sock.mcast` | `[handle] <join\|leave> <group>` | Join or leave a multicast group on a UDP socket that is bound with `sock.bind`: IPv4 `224.0.0.0/4` (mDNS `224.0.0.251`) or IPv6 `ff00::/8` (`ff02::fb`, `::` shorthand accepted). Afterwards `sock.recvfrom` sees the group's datagrams |
| `sock.close` | `[handle] \| all` | Close a socket, or every socket on the module |
| `sock.list` | | List open sockets |
| `sock.echo` | `<tcp\|udp> <port> [seconds=60]` | Echo server for latency/throughput tests (default 60 s, one TCP client) |

```powershell
# Fetch a page over TCP
ucxapp COM13 `
  --exec "wifi.connect MySsid MyPassword" `
  --exec "sock.open tcp" `
  --exec "sock.connect 0 example.com 80" `
  --exec "sock.send 0 GET / HTTP/1.0\r\n\r\n" `
  --exec "sock.read 0 200 5" `
  --exec "sock.close 0"

# Send a UDP datagram
ucxapp COM13 --exec "sock.open udp" --exec "sock.sendto 0 192.168.1.20 5000 hello"
```

### mDNS

The module's mDNS responder (NORA-W36 firmware 3.4.1 or later) answers for `<hostname>.local` and advertises DNS-SD services (RFC 6763) on the Wi-Fi link, so PCs, phones and Matter controllers find it without a DNS server. Wi-Fi (station or access point) must be up before `mdns.on`; the responder follows the link and the `+UEMDU`/`+UEMDD` events show in the event log. The module holds 3 services with 10 TXT records each. There is no browse or resolve on the module yet (`thread.dns` has them over Thread); browse from a PC with `dns-sd -B _http._tcp` or `avahi-browse -t _http._tcp`.

| Command | Arguments | Description |
|---|---|---|
| `mdns.status` | | Responder on/off, host name and every advertised service (`id  instance._type._tcp port N`) with its TXT records. If another device already used the name, the module renames itself (`+UEMDCD`, firmware 3.5.0) and this shows `Name conflict: host name 'nora' was in use, renamed to 'nora-2'` |
| `mdns.on` | `[hostname] [wait_s=5]` | Enable the responder, with a new host name if given, and wait for the up event; without it in time the responder stays enabled and comes up when Wi-Fi has an address |
| `mdns.off` | | Disable the responder |
| `mdns.hostname` | `[name]` | Show or set the host name advertised as `<name>.local` (default: the Wi-Fi host name); another device with the same name makes the module rename itself |
| `mdns.add` | `<_type._tcp\|_type._udp> <port> [instance] [key=value ...]` | Advertise a service and print its id. The type is written as DNS-SD does (`_http._tcp`, `_matterc._udp`), the instance name (up to 63 characters, quoted when it has spaces) defaults to the host name, and every `key=value` becomes a TXT record. Fails when the module already holds 3 services |
| `mdns.remove` | `<id>\|all` | Stop advertising one service or all of them |
| `mdns.txt` | `<id> <key> <value> \| <id> remove <key> \| <id> clear` | Add or replace one TXT record on a service, remove one, or clear them all |

```powershell
# Reachable as nora.local, with the web UI announced as an HTTP service
ucxapp COM13 --exec "wifi.connect MySsid MyPassword" --exec "web.start 80" `
  --exec "mdns.on nora" --exec "mdns.add _http._tcp 80 \"NORA-W36 dashboard\" path=/"

# What a Matter controller looks for while commissioning
ucxapp COM13 --exec "mdns.add _matterc._udp 5540 A1B2C3D4E5F60718 VP=65521+32769 D=3840 CM=1" --exec "mdns.status"
```

### Performance (perf)

Round-trip time and throughput through the module to an echo peer over TCP, UDP or Bluetooth SPS. The peer is
any echo server or a second module running `perf.echo`. u-blox runs one at `echo.u-blox.com` port 7: UDP echoes
every datagram, TCP currently closes the connection after the first reply (perf says so), so use UDP there or a
second module for TCP. Times include the AT link to the module, so they show what an application on this host gets;
at 115200 baud the UART limits throughput to a few tens of kbit/s.

| Command | Arguments | Description |
|---|---|---|
| `perf.latency` | `<tcp\|udp> <host\|xplorer> <port> [count=20] [size=64] \| sps [count=20] [size=64]` | Send `count` packets (default 20) of `size` bytes (default 64, 10-1000) one at a time and wait for each echo: min/avg/max round-trip time, jitter, lost and corrupt packets; fails on any loss |
| `perf.throughput` | `<tcp\|udp> <host\|xplorer> <port> [seconds=10] [size=512] \| sps [seconds=10] [size=512]` | Send for `seconds` (default 10) with 8 packets of `size` bytes (default 512) in flight; prints kbit/s sent and echoed every second and at the end, and the breaks in the echoed 0x00-0xFF pattern (any break fails TCP and SPS) |
| `perf.echo` | `<tcp\|udp> <port> [seconds=60] \| sps [seconds=60]` | Echo peer on a second module (default 60 s): `tcp`/`udp` is `sock.echo`; `sps` advertises, accepts the connection and echoes SPS data |
| `perf.phone` | `<tx\|rx\|echo> [seconds=10] [size=244]` | Bluetooth SPS between the module and the u-connectXplorer phone: the phone finds the module by its name, connects and runs its own perf; the module sends (`tx`), receives (`rx`) or echoes the phone's packets (`echo`, the phone measures RTT) for `seconds` (default 10) with `size`-byte packets (default 244). Prints both ends; fails on a pattern break at the receiving end |

SPS uses the current Bluetooth connection (`bt.connect` first). The data is the s-center data pump pattern: a
0x00-0xFF byte counter that runs on across packets, so every byte value goes through the module and back
(binary-safe `USOWB`/`USOSTB`/`USPSWB` and `USORB`/`USORFB`/`USPSRB`). The echo is compared byte for byte, and
`perf.echo` returns an s-center pump stream unchanged.

```powershell
ucxapp COM13 --exec "wifi.profile.auto" --exec "perf.latency udp echo.u-blox.com 7 20" `
  --exec "perf.throughput udp echo.u-blox.com 7 10 1000"
```

### u-connectXplorer phone (xplorer)

The u-connectXplorer app (App Store, Google Play) has a remote control: Settings > Remote control shows the phone's
address and a token, and serves JSON-RPC over a WebSocket on port 8765. ucxapp drives the module over AT and the
phone over this link, so one command runs both ends of a test. Keep the app in the foreground on the same Wi-Fi.
The token is stored sealed in `storage/xplorer.cfg`. The echo peer needs an app with `socketEcho` and `perf.phone`
one with `perfSps`; `xplorer.info` says when the app needs an update. These commands are on Windows and Linux only.

| Command | Arguments | Description |
|---|---|---|
| `xplorer.connect` | `<phone-ip[:port]> <token>` | Connect, show the app version, OS and Bluetooth state, and save the phone |
| `xplorer.info` | | The same for the saved phone |
| `xplorer.call` | `<method> [key=value]...` | Call any remote-control method and print its JSON result; numbers, `true`/`false`/`null` and `{..}`/`[..]` values are sent as they are, others as strings |
| `xplorer.echo` | `<tcp\|udp> <port> \| state \| stop` | Make the phone an echo server; `state` shows the bytes it got and the breaks in its 0x00-0xFF check |

With host `xplorer`, `perf.latency` and `perf.throughput` start the echo server on the phone, run against it and print what the
phone received, including its own pattern check (a break fails TCP). `perf.phone` tests Bluetooth SPS between the
module and the phone.

```powershell
ucxapp --exec "xplorer.connect 192.168.1.200 <token>"
ucxapp COM13 --exec "wifi.profile.auto" --exec "perf.latency udp xplorer 7000" --exec "perf.throughput tcp xplorer 7001 10"
ucxapp COM13 --exec "perf.phone tx 10" --exec "perf.phone rx 10" --exec "perf.phone echo 10"
```

### Matter

A Matter node on the connected module. The node itself (the Matter SDK) is an optional plug-in next to the app, `ucxmatter.dll` (`libucxmatter.so` on Linux), built with `ucxapp --matter --build`; without it `matter.start` says so and everything else works as before. The plug-in uses the app's connection to the module, so the other commands, `at.trace`, the events and the web page keep working while the node runs. What the node stores (fabrics, keys) is sealed in the secure store as `matter.*.key`.

The node is a root node with a dimmable light, a temperature sensor and a contact sensor. Without a fabric it opens a commissioning window for 15 minutes and advertises over Bluetooth LE; its discriminator and passcode are its own, drawn at random the first time it starts and kept sealed in the store, so two nodes never share codes and nothing can be worked out from the module's address. To a controller it is the product `u-connectXpressApp` of vendor `u-blox`, with the module's type code as hardware version and part number, the module's serial number and the module's firmware version as software version. The log shows commissioning as six steps, one line each (`[matter] commissioning [##----] 2/6 fabric installed`); `matter.log detail` adds the Matter SDK's own log. A commissioner pairs with the node over Bluetooth LE and gives it a Wi-Fi network, which the node joins with the module; a module that is on Wi-Fi already (`wifi.connect`) is used as it is. On the network the node is reached over UDP through the module and announced by the module's own mDNS. `matter.status` shows the node's network. On the web page the node is the Matter tab of What's happening (`matter.card`): state, commissioning steps, endpoint controls, and buttons for start, stop, QR code, open window and factory default.

| Command | Arguments | Description |
|---|---|---|
| `matter.start` | `[all\|light\|temp\|contact\|<endpoints>]` | Load the plug-in and start the node on the module. `all` (the default) builds the default set, light, temp and contact; one kind or a list of kinds as `matter.endpoints` names them (`light light contact`) builds those |
| `matter.stop` | | Stop the node (leaving the app or disconnecting the module does the same) |
| `matter.status` | | Stopped or running, the plug-in and its file, the module's radios, the URCs passed on to the node, and what the node says about itself |
| `matter.card` | | The node at a glance, one `TAG value` line each: state, commissioning step, Bluetooth LE, commissioning window, network, setup codes, every endpoint with its value and what `matter.set` takes, and the log level. The web page shows the same card as the Matter tab of What's happening, with a picture, a button, a slider or a number field with - and + for each endpoint value, and buttons for start, stop, QR code, open window, demo, SDK log and factory default (`matter.reset`) |
| `matter.endpoints` | | Endpoints of the running node with their values, and the kinds the plug-in can build |
| `matter.set` | `<endpoint> <value...>` | Change an endpoint from this side, as a button or a sensor would: `matter.set light on`, `matter.set temp 23.5`, `matter.set contact open`. The node writes the value to its data model, so a controller sees it |
| `matter.demo` | `[on\|off]` | The node moves its sensors itself: the temperature goes round 19..24 C in two minutes and the contact opens or closes every 30 s, as a controller shows them; without an argument it shows whether the demo runs. The web page has a Demo button for it |
| `matter.qr` | | Setup QR code and manual pairing code; it does not advertise (`matter.open` does) |
| `matter.open` | `[seconds]` | Open the commissioning window again (180 to 900 seconds, default 900), advertise over Bluetooth LE and show the codes |
| `matter.reset` | `[all]` | Forget every fabric and remove what the node stored, disconnect Wi-Fi and reboot the module; a node that ran starts again with the same endpoints and opens its commissioning window, so it can be commissioned again. Its setup codes stay, as the label on a product does; `all` draws new ones and factory-resets the module instead of rebooting it (the UART goes back to 115200 baud) |
| `matter.selftest` | | Check the plug-in, the module (Bluetooth LE peripheral role, Wi-Fi or Thread), the sealed store, and the node (random generator, its own setup codes, Bluetooth LE advertising or link, network); starts a node for the test when none runs and stops it again; fails when a check fails |
| `matter.bridge` | `tcps [port] \| tls [port] \| tcpc <host> <port> \| udpc <host> <port> [local] \| udps <port> \| ble [nus\|sps\|custom] \| uart <port> [baud] \| cell \| mqtt [prefix\|off] \| off [n] \| status` | The node over a bridge (default port 7540, up to 4 at once): every change of the card goes out as a line (`EP light = on`, `STATE ...`, `STEP ...`) and the node's events as `EVT ...`; the far side sends `set <endpoint> <value...>` (answer `OK` or `ERR <why>`) and `get` (the whole card, then `OK`). `mqtt` (after `mqtt.connect`) publishes each endpoint retained to `<prefix>/<endpoint>`, the node lines to `<prefix>/node/<tag>`, events to `<prefix>/event`, and takes `<prefix>/<endpoint>/set`; prefix default `ucxapp/matter` |
| `matter.log` | `<info\|detail\|error>` | Set how much of the node's log is printed: `info` (default) is its milestones and the commissioning steps, `detail` adds the Matter SDK's log, `error` only failures; without a level it shows the one in use. On the web page the Matter tab switches between the two with SDK log on / off, and the log pane has Copy and Clear |

```powershell
ucxapp COM13 --exec "matter.start" --exec "matter.qr" --exec "matter.status"
```

### MQTT

| Command | Arguments | Description |
|---|---|---|
| `mqtt.connect` | `[host] [port] [id=<client>] [user=<name>] [pass=<pw>] [tls\|ca=<name> [cert=<name> key=<name>]]` | Connect to an MQTT broker (default `broker.emqx.io:1883`, no TLS); waits up to 10 s. `user=`/`pass=` log in, `id=` sets the client ID. `ca=` turns on TLS (port 8883 unless given) and checks the server with that CA certificate; `cert=`/`key=` add a client certificate. A bare `tls` encrypts without checking the server (tests only). Certificates are uploaded with `cert.*`; keep passwords out of scripts with `${MQTT_PASS}` |
| `mqtt.disconnect` | | Disconnect from the broker |
| `mqtt.will` | `[<topic> <message> [qos=<0-2>] [retain] \| off]` | Last will and testament (`AT+UMQLWT`): the broker publishes `<message>` on `<topic>` if the module drops off without `mqtt.disconnect`. Set it before `mqtt.connect`; the module keeps it until `off`. Without arguments: the current will |
| `mqtt.subscribe` | `<topic> [qos=0]` | Subscribe (wildcards allowed, qos 0-2) |
| `mqtt.unsubscribe` | `<topic>` | Unsubscribe |
| `mqtt.publish` | `<topic> [qos=0] [retain] <text...>\|<topic> [qos=0] [retain] file=<name>` | Publish typed text (up to one shell line, 511 bytes) or a stored file of up to 5000 bytes, the module's limit for `AT+UMQPB` (MQTT 3.1.1). `qos=` 0-2; `retain` keeps it on the broker for new subscribers (sensor state for Home Assistant) |
| `mqtt.read` | `[wait_s=10]` | Wait (default 10 s) for messages and print them |

```powershell
ucxapp COM13 --exec "wifi.profile.auto" --exec "mqtt.connect" `
  --exec "mqtt.subscribe ucx/demo/#" --exec "mqtt.publish ucx/demo/hello hi there" --exec "mqtt.read 5"

# TLS with server check and login (CA uploaded once with cert.upload)
ucxapp COM13 --exec "wifi.profile.auto" --exec 'mqtt.connect broker.emqx.io ca=emqx-ca user=demo pass=${MQTT_PASS}'
```

Known firmware behaviour: on NORA-W36 an MQTT publish made while a subscription is active returned ERROR:15 in testing, and back-to-back MQTT operations get ERROR:15 while the previous one is in flight; the commands retry the latter automatically.

### Thread

For NORA-B26 and ALMA-W1 with Thread firmware (not released yet; the commands follow the u-connectXpress 3.5 Thread API). On a module without Thread firmware they are hidden in the UI and refused with `needs Thread`; `dev.caps` shows `Thread: yes/no`. Full builds only (not F103/G071 lite).

| Command | Arguments | Description |
|---|---|---|
| `thread.status` | | Thread on or off, the network, the role (disabled, detached, child, router, leader), RLOC16, partition and extended address |
| `thread.start` | `[wait_s=30]` | Turn Thread on and wait for `+UETHU` (attached); `0` does not wait |
| `thread.stop` | | Turn Thread off |
| `thread.network` | `[new\|<name> <channel> <pan_id> <ext_pan_id> <network_key>]` | Channel 11-26, ext_pan_id 16 hex digits, network_key 32. Show the credentials, make a new network with random ones (this module becomes the leader), or set them |
| `thread.dataset` | `[<hex>\|clear]` | Show or set the active operational dataset as TLV hex (max 254 bytes; on an OpenThread border router: `ot-ctl dataset active -x`) |
| `thread.type` | `[router\|end\|sleepy [poll_ms]]` | Device type: router-capable, end device, or sleepy end device polling its parent |
| `thread.scan` | | Thread networks nearby: name, channel, PAN ID, extended PAN ID, RSSI, LQI, joinable |
| `thread.join` | `<pskd> [wait_s=120]\|stop` | Joiner: a commissioner admits the module with its PSKd (6-32 of A-Z and 0-9 without I, O, Q, Z; lower case is converted); waits for `+UETHJ` |
| `thread.addr` | | IPv6 addresses and their origin (thread, slaac, dhcpv6, manual) |
| `thread.neighbors` | | Neighbours: extended address, RLOC16, role, RSSI, link quality |
| `thread.routers` | | Router table: id, RLOC16, next hop, path cost, link quality in/out, age |
| `thread.leader` | | Leader data: partition, leader router id, weighting, data versions |
| `thread.counters` | `[all\|reset]` | Link counters (attach attempts, parent changes, tx/rx frames, retries, CCA, security and FCS errors); `all` lists every id |
| `thread.srp` | `[on\|off\|host <name>\|add <_type._udp> <port> <instance>\|remove <id>\|txt <id> <key> <value>]` | SRP client: register services with the border router for DNS-SD; no argument lists the services and their state |
| `thread.ping` | `<ipv6>\|leader [count=4]` | ICMPv6 ping over Thread; `leader` pings the partition leader, off-mesh hosts and IPv4 go through the border router (NAT64) |
| `thread.dns` | `browse <_type._udp>\|resolve <instance> <_type._udp>\|host <name>` | DNS over the Thread network |

Events (`app.log.events.on`, web Events pane): `thread: up, <role>`, `thread: down`, `thread: role <role>`, `thread: srp service <id> state <n>`, `thread: joiner result <n>`.

```powershell
# New network led by this module
ucxapp COM13 --exec "thread.network new" --exec "thread.start" --exec "thread.status"

# Join a border router's network with its dataset
ucxapp COM13 --exec "thread.dataset 0e08...<hex from ot-ctl dataset active -x>" --exec "thread.start" --exec "thread.addr"
```

### HTTP

| Command | Arguments | Description |
|---|---|---|
| `http.get` | `<url> [show_bytes] [ca=<name> [cert=<name> key=<name>]]` | GET over HTTP or HTTPS (scheme optional, default HTTP); prints the status line and up to `show_bytes` (default 512) of the body. For HTTPS, `ca=` checks the server with that CA certificate and `cert=`/`key=` add a client certificate (mutual TLS); without `ca=` HTTPS encrypts without checking the server. Certificates come from `cert.upload`; `http.post` and `http.download` take the same options |
| `http.download` | `<url> <file>` | Download to a file, following up to 5 redirects in the app (the module firmware does not); needs a 2xx response, checks `Content-Length` when present, deletes the file on failure |
| `http.post` | `<url> <body> [content_type]` | POST a body (default `application/x-www-form-urlencoded`), prints the status line and up to 512 bytes of the reply |
| `wifi.portal` | `<check\|login> [probe_url] [field=value ...]` | Hotel/guest Wi-Fi login page (captive portal). `check` fetches `http://connectivitycheck.gstatic.com/generate_204` (or `probe_url`): 204 = online, a redirect or a page = login page in the way (both succeed; only no answer at all fails). `login` loads the login page (keeping its cookie), fills in its first `<form>` (hidden fields, checkboxes ticked, your `field=value` overrides), submits it and checks again. Pages built by JavaScript or with a captcha need a browser. Test with `tests/harness/captive_portal.py` |
| `wifi.portal.form` | `<html_file> <page_url> [field=value ...]` | Shows what `wifi.portal login` would submit for a saved login page (method, resolved action URL, url-encoded body, field names); no module needed. Use it to check a real hotel page saved from a browser |

```powershell
ucxapp COM13 --exec "wifi.profile.auto" --exec "http.get https://example.com 300"
ucxapp COM13 --exec "wifi.profile.auto" --exec "http.download https://example.com/file.bin file.bin"
```

REST and geolocation examples (the module needs Wi-Fi; replies are JSON text, so raise `show_bytes`; not run against
the services here, which may change or rate-limit):

```powershell
# Position from the public IP address, then the QR code for it (copy lat/lon from the reply)
ucxapp COM13 --exec "wifi.profile.auto" --exec "http.get http://ip-api.com/json 600"
ucxapp COM13 --exec "utils.qr.geo 59.33 18.07"
# Any REST endpoint works the same way
ucxapp COM13 --exec "wifi.profile.auto" --exec "http.get https://api.github.com/zen 200"
```

NORA-W36 has no GNSS; for a position from a receiver on the PC use `gnss.open` and `gnss.status`.

### Security

| Command | Arguments | Description |
|---|---|---|
| `cert.list` | | List installed certificates and keys with their type (`ca`, `client`, `key`) |
| `cert.info` | `<name>` | Size and SHA-256 fingerprint |
| `cert.upload` | `<ca\|client\|key> <file> [name] [pw]` | Upload a PEM file (max 8192 bytes); the name defaults to the file name, `pw` is for encrypted keys. For `ca` and `client` the fingerprint is checked against the module afterwards |
| `cert.remove` | `<name>` | Remove a certificate or key (type is looked up automatically) |
| `cert.import` | `<file> [name] [pw]` | Convert a PKCS#12 (`.p12`/`.pfx`), PKCS#7 (`.p7b`), DER (`.cer`/`.der`) or PEM file and upload it: client chain as `<name>`, key as `<name>-key`, CA certificates as `<name>-ca` (without a key all certificates go up as CA `<name>`). The name defaults to the file name, max 28 characters. Not on STM32 |
| `dtls.selftest` | `[server-cert]` | Run a DTLS client and server in-process (offline) and check an echo |
| `dtls.client` | `<host> <port> [text]` | DTLS client over a module UDP socket: handshake, send text, print the reply |
| `dtls.server` | `<port> [cert]` | DTLS server over a module UDP socket: one client, echo one message |

```powershell
ucxapp COM13 --exec "cert.upload ca root-ca.pem" --exec "cert.upload client device.pem" --exec "cert.upload key device-key.pem" --exec "cert.list"
```

Certificates often come as PKCS#12 from Windows or the IT department. The module only takes PEM, so convert first or import in one go.
PKCS#12 must use AES (PBES2); files with the legacy 3DES/RC2 encryption get a re-export hint
(`openssl pkcs12 -legacy -in old.p12 -nodes | openssl pkcs12 -export -out new.p12`, or AES256-SHA256 in the Windows export wizard):

```powershell
ucxapp COM13 --exec "utils.cert.info device.p12 MyPassword" --exec "cert.import device.p12 device MyPassword" --exec "cert.list"
```

#### App TLS (HTTPS and TLS bridges)

The module's TLS only covers outgoing connections, so the app terminates TLS itself (mbedTLS) for
`web.start` (HTTPS by default) and `*.bridge tls`. The `tls.*` commands keep a small local CA and certificates in the
app store as `tls-<name>.crt` / `tls-<name>.key` (PEM, ECDSA P-256, SHA-256). No device is needed.

| Command | Arguments | Description |
|---|---|---|
| `utils.tls.ca.create` | `[cn] [force]` | Create the local CA (`tls-ca.*`, valid 20 years). `force` replaces it; certificates it issued stop working |
| `utils.tls.ca.trust` | `[check]` | Trust the local CA on this computer: adds it to the Windows user root store (Windows asks to confirm), prints the steps on Linux/macOS/Firefox. `check` only reports |
| `utils.tls.server.create` | `<name> <host> [...]` | Server certificate signed by the CA for up to 8 host names / IPv4 addresses (the first is the common name), valid 825 days (the most Apple devices accept) with the `serverAuth` usage |
| `utils.tls.client.create` | `<name>` | Client certificate for `clientauth` bridges |
| `utils.tls.list` | | Certificates with subject, issuer and validity |
| `utils.tls.pem` | `<name> [key]` | Print a certificate, or its key, in PEM (copy `utils.tls.pem ca` to clients) |
| `utils.tls.remove` | `<name>` | Remove a certificate and its key |
| `utils.tls.selftest` | | Run the mbedTLS built-in known-answer tests (hashes, AES/GCM/CCM, ChaCha20-Poly1305, DRBGs, bignum, RSA, ECP), one line per algorithm with its time |

```powershell
ucxapp COM13 --shell
> utils.tls.ca.create
> utils.tls.server.create server 192.168.1.42 nora.local
> web.start https                 # https://192.168.1.42/
> utils.tls.client.create laptop
> gnss.bridge tls 2947 server clientauth
```

`web.start https` typed at the prompt on Windows trusts the local CA automatically the first time (one Windows
confirmation), so the browser on this PC does not warn; scripts only get a hint. On other PCs, or other systems,
import `tls-ca.crt` as a trusted root (`utils.tls.ca.trust` prints how).
Validity starts today, or at the app's build date when the clock is not set, so no real-time clock is needed. The
`web.start` certificate is renewed by itself 30 days before it expires and whenever the module's address changes. A test client
(the stored key is sealed, so save the output of `utils.tls.pem laptop key` as `laptop-key.pem` first):
`openssl s_client -connect 192.168.1.42:2947 -CAfile tls-ca.crt -cert tls-laptop.crt -key laptop-key.pem`.

### GNSS and IMU sensors (PC only, no module needed except for bridges)

A sensor on a PC serial port is read by a background thread. `gnss.*` handles u-blox receivers (NMEA and UBX); `imu.*` handles WitMotion serial IMUs (WT61, WT901 and similar, including the 20-byte BLE packet) and Bosch BNO055 over UART. Bosch BMI160/BMI270/BMI323 and BNO08x have no PC serial interface (I2C or SPI only) and are not supported.

A bridge forwards the raw byte stream through the connected module: `tcps` listens on the module's IP address (up to 4 clients), `tcpc` connects to a server, `udpc` sends datagrams to a host, `udps` listens for them, `ble` notifies on the Nordic UART TX characteristic (needs a connected client with notifications enabled). The module must be on Wi-Fi for these four. The AT link limits throughput to a few kB/s; excess data is dropped and counted.

| Command | Arguments | Description |
|---|---|---|
| `gnss.detect` | `[port]` | Find a GNSS receiver on the serial ports (tries 9600 to 921600 baud) |
| `gnss.open` | `[port] [baud]` | Open a receiver; detects port and baud when omitted. Already open: says so and succeeds |
| `gnss.close` | | Close the receiver |
| `gnss.status` | | Fix, position, time, satellites, DOP, receiver version and counters. `RTK:` shows the correction state (DGNSS, RTK float, RTK fixed from UBX-NAV-PVT or the GGA quality), the age of the corrections and the NAV-PVT accuracy estimate; with UBX-NAV-HPPOSLLH (`gnss.cfg msg nav-hpposllh 1`, F9 and later) `Position:` shows 1e-9 degrees and 0.1 mm height (`(high precision)`) and the accuracy is given to 0.1 mm. `Security:` shows jamming and spoofing (UBX-SEC-SIG, MON-RF, NAV-STATUS; polled from real receivers that speak UBX), the CW jamming indicator and the antenna supervisor; `Backup:` how the last start used the save-on-shutdown backup. While a receiver runs, `JAM:`, `SPOOF:` and `ANT:` lines report each change |
| `gnss.sats` | | Satellites with elevation, azimuth, signal strength and used flag |
| `gnss.watch` | `[seconds=10]` | Live card once a second: fix bars, used/seen satellites as dots, HDOP bars with a rating, position, UTC time and time to first fix since `gnss.open`; an `RTK` line with corrections and accuracy, an `ALERT` line on jamming, spoofing or an antenna fault. Default 10 s, any key stops. The web page pins the same card above "What's happening" while a receiver is open and sending, and hides it otherwise |
| `gnss.log` | `<file\|off> [nmea\|ubx]` | Record the raw receiver stream to a file; `nmea` keeps only valid NMEA sentences, `ubx` only valid UBX frames (a `.ubx` file u-center and `gnss.replay` open) |
| `gnss.replay` | `<file.nmea\|file.ubx> [speed]` | Virtual receiver without hardware: plays an NMEA, UBX or mixed recording in real time (epochs paced by the RMC/GGA/GNS/ZDA time, or the UBX-NAV-PVT time of week) or at `speed` 0.1 to 100, from the start again at the end. Record one with `gnss.log <file> nmea\|ubx`, in u-center, or draw a route on [nmeagen.org](https://nmeagen.org/). Status, watch, fence, bridges and the use cases work on it as on a receiver; `gnss.send`, `rate` and `restart` are accepted and dropped. `gnss.close` stops |
| `gnss.virtual` | `[host\|malmo\|<lat> <lon> [alt]\|off]` | Virtual receiver standing still: RMC, GGA, GSA and GSV once a second with the PC's UTC time and ten satellites. The position is the host's (Windows Location service; GeoClue `where-am-i` on Linux), the u-blox office in Malmo when the host has none (or with `malmo`), or the given one. Bridges, log, fence and the use cases work on it; with `gnss.cfg msg sec-sig 1` or `mon-rf 1` it reports a scripted episode on a 10 s cycle (CW jamming from 1 to 3 s, spoofing indicated from 4 to 6 s), so `gnss.watch` shows the `JAM:` and `SPOOF:` lines; `off` or `gnss.close` stops |
| `gnss.fence` | `<file.kml\|off\|status>` | Geofence from the first polygon (or line string) of a KML file: draw the area in Google My Maps and use *Export to KML/KMZ* with *Export as KML*, or save a place from Google Earth. While a receiver, replay or virtual receiver runs, `FENCE: outside <name> at <lat>, <lon>` and `FENCE: back inside <name>` report the crossings; `gnss.status` shows the state and how often it was left |
| `gnss.export` | `<recording> <out.kml\|out.gpx>` | Track of an NMEA, UBX or mixed recording, one point per epoch with a fix: KML (Google Earth, import into Google My Maps) or GPX (with UTC times) |
| `gnss.send` | `<hex...> \| <$NMEA> \| ubx <cls> <id> [hex...]` | Send raw bytes, an NMEA sentence (checksum added) or a UBX frame |
| `gnss.restart` | `<hot\|warm\|cold>` | Restart the receiver (UBX-CFG-RST) |
| `gnss.rate` | `<hz=1>` | Navigation rate 1 to 25 Hz (UBX-CFG-RATE) |
| `gnss.cfg` | `gnss <list> \| model <name> \| msg <name> <rate> \| baud <rate> \| save \| reset` | Configure the receiver and check its UBX-ACK (an ACK-NAK or no answer is an error). UBX-CFG-VALSET in RAM on PROTVER 23 and later (u-blox F9/M10), the older CFG-NAV5, CFG-MSG and CFG-PRT before. `gnss` enables only the listed systems (`gps,gal,bds,glo,qzss,sbas,navic`; VALSET only); `model` sets the dynamic model (`portable`, `stationary`, `pedestrian`, `automotive`, `sea`, `air1g`, `air2g`, `air4g`, `wrist`, `bike`, `mower`, `escooter`); `msg` sets a message rate on UART1 and USB in epochs (0 = off): `gga gll gsa gsv rmc vtg gst zda gns nav-pvt nav-hpposllh nav-svin nav-relposned nav-sat nav-status mon-rf sec-sig`; `baud` switches UART1 (9600, 38400, 115200, 460800 or 921600: the rates `gnss.detect` tries), reopens the port at that rate, checks that data still arrives and remembers it for `gnss.open` (not on a virtual receiver or while bridges or a raw log run); `save` writes the configuration to battery-backed RAM and flash (UBX-CFG-CFG); `reset` restores the factory defaults |
| `gnss.setup` | `standard \| rover \| base survey [min_s=120] [acc_m=2] \| base fixed <lat> <lon> <height_m> [acc_m=0.05] \| base off` | Basic setup in one step, in RAM (`gnss.cfg save` keeps it); every part is a UBX-CFG-VALSET checked by its UBX-ACK and printed as `ok`, `refused` or `not on this receiver` (optional parts). `standard`: NMEA GGA GSA GSV RMC VTG and UBX NAV-PVT every epoch, GLL off, portable model; on a high-precision receiver also base mode, high-precision NMEA, NAV-HPPOSLLH and RTCM3 output off (any u-blox receiver; before PROTVER 23 with UBX-CFG-MSG). `rover`: RTCM3 and SPARTN corrections in on UART1 and USB (SPARTN is optional: ZED-F9P HPG 1.30 and later), NAV-PVT, NAV-HPPOSLLH and GGA every epoch, high-precision NMEA (CFG-NMEA-HIGHPREC), RTK fixed mode (CFG-NAVHPG-DGNSSMODE 3), base mode and RTCM3 output off; then feed corrections with `gnss.bridge ntrip`, `pointperfect` or `tcpc` from a base. `base survey`: CFG-TMODE survey-in until at least `min_s` seconds have passed and the mean position is better than `acc_m`; `base fixed`: CFG-TMODE fixed at a latitude, longitude (degrees, to 1e-9) and ellipsoid height (metres, to 0.1 mm) with its accuracy. Both send RTCM 1005 1074 1084 1094 1124 every epoch and 1230 every 5 on UART1 and USB, turn on NAV-SVIN and set the stationary model; `gnss.bridge tcps [port]` serves the corrections to rovers. `base off` stops base mode and the RTCM3 output. RTK base and rover need a ZED-F9P or ZED-X20P. `gnss.status` then shows a `Base:` line: survey-in progress (time, observations, mean accuracy) or done, and the RTCM3 frames the receiver sends (CRC-24Q checked) with their message numbers; after a survey-in it prints the position and the `gnss.setup base fixed` line that keeps it. The virtual receiver plays along: a survey-in that converges as 20 m / (1 + s), then RTCM 1005 |
| `gnss.backup` | `<save\|clear\|status>` | Save-on-shutdown (UBX-UPD-SOS, u-blox M8 and later): `save` stops navigation, writes the receiver state to flash and waits for the confirmation; power off then, and the next start restores it for a fast fix (`gnss.restart hot` carries on instead). `status` shows how the last start went (restored, no backup, failed); `clear` deletes the backup |
| `gnss.assist` | `<file.ubx>\|online [gnss] [data]\|register <ztp-token>\|chipcode <code>\|id [ztp-token]\|legacy <token> [gnss]` | u-blox AssistNow for a fast first fix (u-blox M8 and later). `online` downloads UBX-MGA data over the module's Wi-Fi with `http.download` from `assistnow.services.u-blox.com` into `assistnow.ubx` and sends every message to the receiver unchanged and in order; `gnss` defaults to `gps,gal,bds,glo`, `data` to Live Orbits `ulorb_l1,ukion,usvht` (or `uporb_1,ualm` when the plan only has Predictive Orbits). It needs a chipcode: `register <ztp-token>` reads UBX-SEC-UNIQID and UBX-MON-VER from the receiver, posts them with the device profile token from the Thingstream portal (Location Services, AssistNow device profile) to the zero-touch provisioning API and saves the chipcode and allowed data types in `ucxapp.assistnow` (private); `chipcode <code>` saves one taken from the portal; `id [ztp-token]` only prints the ZTP request body. `legacy <token> [gnss]` uses an old AssistNow Online token (`online-live1.services.u-blox.com`, default `gps,glo`). `<file.ubx>` sends the UBX-MGA messages of a file (an earlier download, or one fetched elsewhere); other bytes are skipped and an answer without messages is shown as the error |
| `gnss.bridge` | `<tcps [port]\|tls [port] [cert] [clientauth]\|tcpc <host> <port>\|ntrip <host> <port> <mount> [user pass] [ble] [cell]\|pointperfect <client-id> [eu\|us\|kr\|au\|jp] [cert]\|udpc <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off [n]\|status>` | Two-way bridges of the receiver stream; up to 4 run at once (each with its own buffer, at most one BLE bridge and one NTRIP client), `status` lists them as `bridge 1:`, `bridge 2:` and so on, `off` stops all of them and `off <n>` only bridge n. `tcps` serves (default 2947), `tls` serves the same over TLS with certificate `tls-<cert>` (default `server`; `clientauth` requires a client certificate from the local CA), `tcpc` connects and reconnects, `ntrip` is an NTRIP v1 client (plain TCP, e.g. PointPerfect Flex/Live on port 2101 or RTK2go): corrections (RTCM or SPARTN) go to the receiver and its latest GGA goes to the caster every 10 s; `ble` also sends the corrected NMEA to a phone over BLE Nordic UART (RTK rover); `cell` runs the NTRIP client over a cellular modem instead of Wi-Fi (it uses the `cell.connect` data link, or opens one to `<host> <port>`; needs `cell.open` and `cell.attach on`; a dropped cellular link is reopened with backoff from 1 s up to 30 s); keep credentials out of scripts with `${NTRIP_USER}` `${NTRIP_PASS}`, `pointperfect` subscribes to u-blox PointPerfect over MQTT (`pp.services.u-blox.com:8883`, TLS 1.2) with the client ID of the Thingstream PointPerfect thing and its certificates uploaded as `<cert>` (client, default `pointperfect`), `<cert>-key` and `<cert>-ca` (`cert.upload`); the SPARTN keys (`/pp/ubx/0236/ip`) and the corrections of the region (`/pp/ip/<region>`, default `eu`) go to the receiver unchanged, after SPARTN input from IP is enabled (UBX-CFG-VALSET in RAM; F9P, F9R, X20P); it reconnects every 10 s when the broker drops and uses the module's one MQTT connection (shared with `mqtt.*`), `udpc` sends and listens on `local`, `udps` listens and answers the last sender, `ble` is Nordic UART (`nus`), u-blox SPS or your own service (`custom`, below), `uart` a second PC port, `cell` the data link opened by `cell.connect`. Data from the far side goes to the receiver. For `ntrip` and `pointperfect`, `status` also checks every correction frame on its way to the receiver, like ubxlib's `uSpartnValidate`: RTCM3 (CRC-24Q), SPARTN (frame CRC-4 and message CRC) and UBX (checksum), shown as `(RTCM 120, SPARTN 40 (1 bad), UBX 2, 37 bytes outside frames)`; a bad count means the data was damaged on the way (caster, network or the AT link) |
| `imu.detect` | `[port]` | Find a WitMotion or Bosch BNO055 IMU |
| `imu.open` | `[port] [baud] [wit\|bno055]` | Open an IMU; detects port, baud and type when omitted |
| `imu.close` | | Close the IMU |
| `imu.status` | | Accel, gyro, angle, magnetometer, quaternion, pressure, steps and counters |
| `imu.log` | `<file\|off>` | Record the raw IMU byte stream to a file |
| `imu.send` | `<hex...>` | Send raw bytes (WitMotion configuration frames) |
| `imu.rate` | `<hz=10>` | WitMotion output rate (0.2 to 200 Hz) |
| `imu.bridge` | `<tcps [port]\|tcpc <host> <port>\|udpc <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off [n]\|status>` | Two-way bridges of the IMU stream, same modes as `gnss.bridge` (up to 4 at once) |

### Power Profiler Kit II (PC only)

Measures the module's current with a Nordic Power Profiler Kit II (PPK2, USB 1915:C00A) on its USB data port, using the PPK2's serial protocol directly (100 kS/s, 200 nA to 1 A over five ranges, with the factory calibration read from the kit). `ppk.open` starts safely as an ampere meter with DUT power off. The other detectors skip PPK2 ports. `virt:ppk` simulates one: 2 mA with 60 mA bursts.

| Command | Arguments | Description |
|---|---|---|
| `ppk.detect` | `[port]` | Find PPK2s (every 1915:C00A port is asked for its metadata) |
| `ppk.open` | `[port]` | Open a PPK2, detected when omitted; ampere meter, DUT power off |
| `ppk.close` | | Close the PPK2; the DUT power stays as it was |
| `ppk.status` | | Mode, VDD, DUT power and the calibration per range |
| `ppk.mode` | `<amp\|source>` | Ampere meter (DUT on its own supply) or source meter (the PPK2 powers the DUT) |
| `ppk.vdd` | `<mV>` | Supply voltage, 800 to 5000 mV; the output in source mode, and used for the calibration in both |
| `ppk.power` | `<on\|off>` | DUT power switch (source mode needs `ppk.vdd` first) |
| `ppk.measure` | `[ms=1000] [file.csv]` | Average, min/max, charge and energy over a window (up to 600 s); the CSV has every sample (`t_us,current_uA,range,logic`). The `Chart:` line holds min, average and max in uA for 200 time slots; the web page draws it as a chart with a hover readout |
| `ppk.meta` | | The raw metadata the PPK2 reports |

```text
# Module on its own 3.3 V supply through the PPK2's VIN/VOUT
ppk.open
ppk.vdd 3300
ppk.measure 5000 idle.csv
```

### Bridges

Each source has its own bridges (`gnss.bridge`, `imu.bridge`, `cell.bridge`, `can.bridge`, `hw.bridge`), up to 4 per source, and they all run at the same time.

| Command | Arguments | Description |
|---|---|---|
| `bridge.list` | | Every open source and where its bridges go, one line per bridge |
| `bridge.off` | | Stop every running bridge |

### Cellular modules

Basic access to a cellular module on a PC serial port, using the generic 3GPP AT commands (TS 27.007) that
Quectel, SIMCom, u-blox, Telit, Sierra and Nordic nRF91 modules share. Verified on a SARA-R510S (XPLR-IOT-1);
`cell.ping`, `cell.connect`, `cell.rat` and `cell.profile` are vendor specific (picked from the module's ATI and
`AT+CGMI` maker). `cell.at` is there for anything else.

On an XPLR-IOT-1 the NORA-B106 powers the SARA-R5: when no modem answers, `cell.detect` and `cell.open` send
`modules SARAR5 power_on` to its shell (FT4232 interface A) and wait for the SARA-R5 on interface B; if it still
does not answer they power cycle it (`power_off`, 5 s, `power_on`). A port left in data mode by an earlier
`cell.connect` is brought back with `+++` and its leftover sockets are closed.

`.\ucxapp --detect` lists the modem and sets `UCX_CELL_PORT` and `UCX_CELL_BAUD` for the hardware tests in
`tests/hardware/test_cell.py`; their echo server is `UCX_CELL_ECHO` (default `tcpbin.com 4242`, line based).

| Command | Arguments | Description |
|---|---|---|
| `cell.detect` | `[port]` | Find a module answering AT (115200, 9600, 921600, 460800 baud); powers on the SARA-R5 of an XPLR-IOT-1 |
| `cell.open` | `[port] [baud=115200]` | Open a module (default 115200); without a port, the first one answering `AT+CGMI` (on a virtual module: `virt:cell` first, so a demo never attaches a real modem). Already open: says so and succeeds |
| `cell.close` |  | Close it |
| `cell.status` |  | SIM, ICCID, signal in dBm, registration, the reject cause when denied (`AT+CEER`), operator, IP |
| `cell.info` |  | Maker, model, firmware, IMEI, IMSI and ICCID |
| `cell.signal` |  | RSSI (`AT+CSQ`), RSRP and RSRQ (`AT+CESQ`) with a rating: excellent (-90 dBm and up), good (-105), fair (-115), poor |
| `cell.watch` | `[seconds=10]` | The live cellular card once a second (any key stops): modem and port, registration, operator and access technology, RSSI and RSRP, IP, the `cell.connect` data link and how long it is up, and every bridge using it (`gnss.bridge cell`, `cell.bridge` relay) with bytes and reconnects. While the port carries data no AT is sent; the network values then come from before. The same card is in the web page under "What's happening", a `Cell` chip in its status bar, and the second CELL line of the text UI dashboard |
| `cell.operator` | `[scan\|auto\|<mccmnc>]` | Show the network, access technology and selection mode; `scan` lists the networks in range (`AT+COPS=?`, up to 3 minutes); `auto` or an MCC+MNC such as `24007` chooses one |
| `cell.radio` | `[on\|off\|reboot]` | Show or switch the radio (`off` is airplane mode, `AT+CFUN=4`), or restart the modem and wait for it |
| `cell.rat` | `[ltem] [nbiot] [2g]` | u-blox: show or set the radio access technologies in order of preference (`AT+URAT`, set with the radio off) |
| `cell.profile` | `[<name>\|<n>]` | u-blox: show or set the MNO profile (`AT+UMNOPROF`): `global` (90), `europe` (100), `sim` (1, from the SIM), `vodafone`, `dt`, `at&t`, `verizon`, `telstra`, `t-mobile` or a number; the modem reboots |
| `cell.pin` | `<pin>` | Unlock the SIM |
| `cell.power` | `on\|off\|cycle` | XPLR-IOT-1 only: switch the SARA-R5 through the NORA-B106 shell. `on` waits until it answers AT and leaves its port closed (free for EasyFlash); `cycle` is off, 5 s, on (after a firmware update, or when it hangs); refused while a data link or bridge holds the modem |
| `cell.at` | `<command>` | Send a raw AT command (network selection, scan and attach wait up to 3 minutes) |
| `cell.apn` | `[<apn> [ip\|ipv6\|ipv4v6]]` | Show or set the APN of context 1 |
| `cell.attach` | `[on\|off]` | Attach and activate data, detach, or show the state. `on` waits until the modem is attached and has an address, and says why when the network refuses; on u-blox modules it also starts the internal IP stack (PSD profile 0 on context 1) that ping and sockets use |
| `cell.ping` | `<host>` | Ping over the cellular link (Quectel and u-blox only); prints the replies and `4 sent, 4 received, average 70 ms`, fails when none come back (`+UUPINGER 17`: `cell.attach on` first) |
| `cell.bridge` | `<tcps [port]\|tcpc <host> <port>\|udpc <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|off [n]\|status>` | Give the modem's AT console to a peer, same modes as `gnss.bridge` (up to 4 at once; the last `off` returns to command mode). After `cell.connect` it relays the data link instead: a Wi-Fi client of the NORA module (in station mode or on its `wifi.ap.enable` access point) talks to the `cell.connect` host over cellular, byte for byte; the last `off` leaves the link up and `cell.disconnect` ends both. One socket is relayed, not IP: the modem and the NORA module each have their own IP stack, so there is no routing or NAT between them (no hotspot) |
| `cell.connect` | `<host> <port> [tcp\|udp]` | Open a transparent socket through the modem. Vendor paths: u-blox (default, `AT+USOCR`/`AT+USOCO`), Quectel (`AT+QIOPEN` access mode 2), SIMCom (`AT+CIPMODE=1`, `AT+CIPOPEN`), Telit (`AT#SGACT`, `AT#SD`), Nordic nRF91 (`AT#XSOCKET`, `AT#XCONNECT`). The modem's UART then carries the payload: bridge to it with `gnss.bridge cell` or `imu.bridge cell`, or relay it to a Wi-Fi client with `cell.bridge tcps` |
| `cell.disconnect` | | Stop the bridge or relay using the link first; sends `+++` after the guard time and closes the socket |

### Dashboard

| Command | Arguments | Description |
|---|---|---|
| `dash` | `[watch [seconds=10]]` | One card with a line each for Wi-Fi (SSID, RSSI, IP), BLE (links, GATT listen), GNSS (fix, satellites, HDOP), cellular (registration, operator, signal) and IMU (angles or acceleration). Filled dot = active. Each line shows the port; other NORA modules found on USB are listed as `standby (dev.use <port>)` and GNSS receivers found by their USB descriptor but not opened as `found, not open`. `watch` refreshes every second (default 10 s, any key stops). Works without a module; Wi-Fi and BLE then show `no module` |
| `conn.status` | `[raw]` | Every live link: the module (model, firmware, uptime, Bluetooth address), Wi-Fi station (SSID, RSSI, channel, IP, gateway), access point (SSID, channel, IP, stations), each BLE link (address, RSSI, MTU, local role, TX PHY and the latest Channel Sounding distance), Thread (role, RLOC16, neighbours with RSSI and link quality), sockets (protocol, state, remote) and the GNSS receiver and cellular modem on the host. `raw` prints the `|` separated lines the web page's **Connections** view reads (`/api/conn`, refreshed every second while a page shows that view). Works without a module (GNSS and cellular only) |

Custom BLE service: `ble custom <uuid>` defines a 128-bit service with a write characteristic (RX, bytes go to the
receiver) and a notify characteristic (TX). With only the service UUID, RX and TX are the service UUID with the fourth
byte increased by 1 and 2 (Nordic style); give `<rx> <tx>` UUIDs to choose them. UUIDs are 32 hex digits, dashes optional.
The module keeps a service until it restarts, so one custom service per restart. Cellular transport is not verified on
hardware and is mapped for Quectel modules only; the `+QIOPEN` timing and `+++` guard time may need adjusting.

### Hardware I/O (hw.*)

GPIO, I2C, SPI, ADC, PWM and CAN behind one backend. On Windows and Linux an emulated backend runs inside the app
(16 pins, I2C devices at 0x48/0x68/0x76, SPI MISO wired to MOSI, ADC triangle waves, CAN loopback), so scripts and the
bridge can be developed without a board. Writing to a pin in `in` mode sets its simulated level. Real STM32 drivers are
not wired in yet.

| Command | Arguments | Description |
|---|---|---|
| `hw.status` |  | Backend and peripheral summary |
| `hw.profile.load` | `<file.json>` | Load a board profile: pin names, channel counts and emulated I2C devices (shell only) |
| `hw.board` |  | Pin map of the loaded profile |
| `hw.gpio.mode` | `<pin> <in\|out\|pullup\|pulldown>` | Set a GPIO pin mode |
| `hw.gpio.write` | `<pin> <0\|1>` | Drive an output (or simulate an input level) |
| `hw.gpio.read` | `<pin>` | Read a GPIO pin |
| `hw.gpio.toggle` | `<pin>` | Invert a GPIO output |
| `hw.gpio.list` |  | Levels of all GPIO pins |
| `hw.led` | `<pin> <on\|off\|toggle>` | Drive a user LED; configures the pin as an output (pins can be given by name) |
| `hw.i2c.scan` |  | Scan the I2C bus |
| `hw.i2c.write` | `<addr> <hex...>` | Write bytes to a device |
| `hw.i2c.read` | `<addr> <len>` | Read bytes from a device |
| `hw.i2c.reg` | `<addr> <reg> [len]` | Read a register |
| `hw.spi.cfg` | `<mode=0> <hz=1000000>` | Set the SPI mode (0-3) and clock in Hz |
| `hw.spi.transfer` | `<hex...>` | Full duplex transfer, prints the bytes read |
| `hw.adc.read` | `<ch>` | Read an ADC channel (12 bit, 3.3 V reference) |
| `hw.pwm.set` | `<ch> <hz> <duty%>` | Start PWM on a channel |
| `hw.pwm.off` | `<ch>` | Stop PWM on a channel |
| `hw.exec` | `<command...>` | Run one bridge line, for example `hw.exec gpio.read 3`, and print the reply |
| `hw.bridge` | `<tcps [port]\|tcpc <host> <port>\|udpc <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off [n]\|status>` | Same modes as `gnss.bridge` (TCP default port 7000, up to 4 at once) |

The bridge speaks lines: the peer sends a command without the `hw.` prefix (`i2c.reg 0x68 0x75`) and gets `OK <text>`
or `ERR <text>`. While bridged, GPIO input changes arrive as `EVT gpio <pin> <level>`.

CAN has its own commands. On Windows and Linux they run an emulated loopback controller, and the bridge carries
frames to a network peer. On the STM32 boards with a CAN controller (FreeRTOS and Zephyr builds for H753, F767, F439,
F407 and F446) they drive the real controller (FDCAN1 on the H753, bxCAN CAN1 on the others). Wire a 3.3 V CAN
transceiver (for example a TJA1051T/3 or SN65HVD230 board) to RX/TX, and terminate the bus with 120 ÃƒÅ½Ã‚Â© at both ends:

| Board | CAN RX | CAN TX |
|---|---|---|
| NUCLEO-H753ZI, NUCLEO-F767ZI, NUCLEO-F439ZI (CN9), STM32F4DISCOVERY | PD0 | PD1 |
| NUCLEO-F446RE | PB8 (D15) | PB9 (D14) |

The F401 has no CAN controller and the lite boards (F103, G071) leave CAN out. On Linux, `can.open can0` puts can.*
on a SocketCAN interface instead of the emulation, for example a CANable 2.0 (candleLight firmware, it shows up as
`can0`; slcan firmware needs `slcand` first) or a `vcan0` for tests. SocketCAN takes the bit rate and listen-only from
the interface (`sudo ip link set can0 type can bitrate 500000 [listen-only on]; sudo ip link set can0 up`), and
`can.cfg` prints the line to use.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `can.cfg` | `<bitrate=500000> [normal\|loopback\|listen]` | Set the bit rate (10000-1000000) and mode and (re)start the controller. `loopback` receives its own frames without a bus or transceiver (the host emulation is always loopback). `listen` only receives: the controller sends no acknowledge or error frames, so it can sit on a running bus (a car, a machine) without disturbing it, and `can.send` and bridged frames are refused. On a board the controller starts at 500000 normal on the first `can.send` or `can.recv` |
| `can.open` | `[emulated\|<interface>]` | Windows and Linux: the emulated loopback (default) or, on Linux, a SocketCAN interface (`can0`, `vcan0`); without arguments it shows which |
| `can.send` | `<id> [hex...]` | Send a CAN frame (id above 0x7FF is extended). On a bus without another node nothing acknowledges it: the error counters in `can.status` rise until the controller goes error-passive |
| `can.recv` |  | Take the next received frame |
| `can.watch` | `[seconds=10]` | Print every received frame with its time in seconds since the start (default 10 s, any key stops), then the count |
| `can.status` |  | Controller, mode, bit rate and counters (tx, rx, queued, dropped, bad bridge records) and bridge state; on a board also the controller, its state (error-active, error-warning, error-passive, bus-off) with the transmit and receive error counters, and frames lost when the receive queue was full |
| `can.bridge` | same modes as `hw.bridge` plus `cell` (TCP default port 7001) | Binary, both ways: 17-byte records (ts_ms u32 LE, id u32 LE, dlc, data[8]), up to 32 per write. `cell` sends them over the `cell.connect` data link and reconnects it when it drops (backoff from 1 s up to 30 s; `can.bridge status` shows the reconnect count). On a board the bridge runs on a task of its own through the module: `tcps [port]` (the newest client takes over), `tcpc <host> <port>` (reconnects every 2 s), `udpc <host> <port> [local]`, `udps <port>` and `ble sps`, one at a time; `status` shows the bytes sent, received and not delivered |
| `can.selftest` |  | Offline check of the record format |

```text
can.cfg 500000 loopback
can.send 0x123 DE AD BE EF
can.watch 1
```

Board profiles live in `app/boards/` (`h753.json`, `f767.json`, `f439.json`); `scripts/hw_config.py <profile>
--out hw_config.h` validates one (unknown keys, bad or duplicate pins, reserved pins) and generates X-macro pin tables
for a firmware build. `hw.profile.load` reads the same file on the host. Pins can then be given by name (`hw.gpio.write PB0 1`).
Format: `board`, `gpio[]`, `adc[]`, `pwm[{pin,timer}]`, `i2c{scl,sda}`, `spi{sck,miso,mosi,cs}`, `can{rx,tx}`,
`reserved[]` and, for the emulation only, `emu.i2c[{addr,regs{reg:value}}]`.

Default pins on all three Nucleo-144 boards (not verified on hardware; the STM32 firmware does not use them yet):
I2C1 PB8/PB9, SPI4 PE2 (SCK), PE5 (MISO), PE6 (MOSI), PE4 (CS), CAN on PD0 (RX) and PD1 (TX), PWM on TIM1 PE9/PE11/PE13/PE14,
ADC on PA3, PC0, PC3, PF10, GPIO on the user LEDs and button plus PF12-PF15, PD14 and PD15. PB6/PB7, PG9/PG14 and
PD8/PD9 stay reserved. SPI4 is used because PA7 on SPI1 is shared with the Ethernet PHY.

### Tools (no AT equivalent)

| Command | Arguments | Description |
|---|---|---|
| `utils.qr.text` | `<text...> [small\|large]` | Print a QR code for any text. `small` (default) draws two module rows per text line with half blocks, `large` two characters per module for terminals without them |
| `utils.qr.wifi` | `<ssid> [password] [WPA\|WEP\|nopass] [hidden] [small\|large]` | Print a Wi-Fi join QR code; the keywords may come in any order, and without a password (or with `nopass`) the code is for an open network |
| `utils.qr.geo` | `<lat> <lon> [small\|large]` | Print a location QR code |
| `utils.zip` | `<archive.zip> <file> [file...]` | Create a zip archive from up to 6 files (deflate; no device needed) |
| `utils.unzip` | `<archive.zip> [dir]` | Extract a zip archive, default into the current directory (no device needed) |
| `utils.cert.info` | `<file> [pw]` | Show what a PKCS#12, PKCS#7, DER or PEM file holds: subject, issuer, validity, which certificate matches the key, key type and size (no device needed, not on STM32) |
| `utils.cert.convert` | `<file> [out] [pw]` | Convert PKCS#12, PKCS#7, DER or PEM to the PEM the module takes: `<out>.crt` (client chain, or all certificates without a key), `<out>.key` (not encrypted) and `<out>-ca.crt`; `out` defaults to the file name without extension (no device needed, not on STM32) |
| `utils.encrypt` | `<in> <out> <passphrase>` | Encrypt a file with AES-256-GCM; the key comes from the passphrase (PBKDF2-HMAC-SHA256, 600000 iterations, random salt). Max 64 MB (no device needed, not on STM32) |
| `utils.decrypt` | `<in> <out> <passphrase>` | Decrypt a `utils.encrypt` file; fails on a wrong passphrase or a changed file (no device needed, not on STM32) |
| `utils.secure` | | Show the secure store: how the install key `ucxapp.key` is kept (Windows: DPAPI for this user; Linux/macOS: owner-only file; STM32 H753/F767: internal flash and the readout protection level) and whether every private key (`*.key`) is sealed. Fails if one is still plain |

Private keys saved in the store (`*.key`: `utils.tls`, `utils.cert.convert`, web and BLE uploads) are sealed with AES-256-GCM under the install key and opened again when a command reads them; the file name is authenticated, so a sealed key cannot be renamed or edited. Keys already in the store are sealed at start. Paths outside the store (`utils.cert.convert x.p12 C:\certs\dev`) are written as plain PEM. Wi-Fi passwords in `ucxapp.wifi` use the same key; on STM32 the H753/F767 seal them too and convert older obfuscated profiles on first use. Copying `storage/` to another PC or user does not carry the keys along: export them first with `utils.tls.pem <name> key` or `utils.encrypt`. See [docs/SECURITY.md](docs/SECURITY.md).
| `app.log.at.on` | | Show the AT TX/RX trace |
| `app.log.at.off` | | Hide the AT TX/RX trace (default) |
| `app.log.warn.on` | | Show library warnings such as `Unexpected data` (refused in the text UI, where they would garble the screen) |
| `app.log.warn.off` | | Hide library warnings |
| `app.log.events.on` | | Also print module events (Wi-Fi, Bluetooth, MQTT ...) in the log as `[event] ...` |
| `app.log.events.off` | | Keep events out of the log (default); they still show in the Events pane |
| `app.log.level` | `<off\|warn\|all>` | One switch: `off` hides warnings, AT trace and events; `warn` shows warnings only; `all` shows warnings, AT trace and events |
| `app.log.save` | `<file>` | Save everything the log shows to a file; the file is written when `app.log.stop` runs |
| `app.log.stop` | | Finish and write the file started by `app.log.save` |
| `app.log.clear` | | Clear the log (in the text UI the Log pane) |
| `app.defaults` | `<confirm>` | Forget all app settings: log switches, Wi-Fi and BLE profiles, last used profile and Wi-Fi region (the module keeps its own; see `dev.factory`) |

The `app.log.at`, `app.log.warn`, `app.log.events` and `app.log.level` switches are saved in `ucxapp.log` in the store and restored at the next start or reboot.
| `session.record` | `<file.ucx>` | Record every command from now on to a script, with its output as `# ` comments, pauses as `#+` and failures as `#!`; passwords and tokens are written as `${UCX_WIFI_PASS}` / `${UCX_SECRET}`. Send the file to support |
| `session.stop` | | Stop recording |
| `session.replay` | `<file.ucx> [all] [stop]` | Run the commands of a recorded session or script (`${NAME}` expanded); firmware, factory reset and removals are skipped unless `all`; `stop` ends at the first failure. A line `expect <regex>` after a command checks its output (`.`, `\d \w \s`, classes, groups with `\|`, `* + ?`, `^ $` per line), `expect-fail` that it fails |
| `session.selftest` | | Offline check of secret masking (scripts, AT trace) and report file names |
| `prod.test` | `[script.ucx\|-] [dir]` | Production test: runs a script (or `-`: `dev.info`, `dev.caps`, `dev.diagnose`; `expect`/`expect-fail` lines as in `session.replay`), writes `<dir>/<serial>_<date>.txt` with all output and adds a line to `<dir>/report.csv` (date, serial, model, firmware, script, steps, failed, PASS/FAIL); default dir `reports` |
| `file.hash` | `<md5\|sha256> <file>` | Print the hash of a file (no device needed) |
| `utils.hash.text` | `<md5\|sha256> <text...>` | Print the hash of a text |
| `utils.hash.selftest` | | Offline check of md5 and sha256 against RFC 1321 / FIPS 180-2 vectors. Not in lite builds |
| `file.view` | `<file> [lines=40]` | Show the first lines of a text file, read only (default 40 lines; non-printable bytes shown as dots) |
| `file.list` | | List the files in the working folder, numbered, with sizes |
| `file.remove` | `<file\|number>` | Delete a file by name or by its number from `file.list` |
| `file.recv` | `[name]` | Receive a file over the console with YMODEM (1K blocks, CRC-16) and store it under its own name or `name`; STM32 console only (Tera Term, ExtraPuTTY, `sz`, `ucxapp --stm32-freertos --put`) |
| `file.send` | `<file>` | Send a file over the console with YMODEM; STM32 console only (Tera Term, ExtraPuTTY, `rz`, `ucxapp --stm32-freertos --get`) |
| `file.selftest` | | Offline check of the YMODEM encoder and decoder (CRC-16, 1K and 128-byte blocks, retry) |
| `stats.show` | `[tcp\|udp\|sps\|gatt\|http\|mqtt\|web] [rx\|tx]` | Payload bytes sent and received per channel; `stats.show tcp rx` prints just the number |
| `stats.clear` | `[tcp\|udp\|sps\|gatt\|http\|mqtt\|web]` | Reset one channel's counters, or all |
| `app.info` | | About this app: maker, platform and links |
| `app.complete` | `<text...>` | Show what Tab completes a typed line to: command names first (any case, plus `help`), then the keywords from the command's usage, e.g. `app.complete wifi.sc` prints `1 match: wifi.scan` and `app.complete "gnss.bridge n"` prints `1 match: gnss.bridge ntrip`; quote the line to keep a trailing space. Two or more matches are listed on the next line. The TUI (Tab, Tab again lists) and the web page (Tab) use the same completion |
| `app.memory` | | App RAM, heap and flash (disk on PC) use, plus task stacks on STM32; same rows as `dev.stack` |
| `app.version` | | App version, build time, platform and compiler |
| `app.uptime` | | Time since the app started (`days hh:mm:ss`) |
| `app.snake` | `[demo [steps=300] [seed=1]]` | Break time: the snake eats data packets (`<>`); arrows or WASD steer, `p` pauses, `q` quits, Enter or `r` plays again after the link is lost. Full screen in the console and in the TUI (type it on the input line); `--exec` and the web command line have no keyboard, so they print a hint. `demo` lets the app steer for `steps` moves from a fixed `seed` and prints the board and the score. On the web page type `snake` outside a field, or right-click or hold FX and pick Snake |
| `app.hunt` | `[ssid] [rounds=20]` | Signal hunt: walk the module (a laptop with the EVK, or an STM32 board on a power bank) towards an access point, the strongest one or `ssid`. Every Wi-Fi scan prints the RSSI, a bar from -95 to -40 dBm and `hotter!`, `colder`, `same` (3 dB steps) or `out of range`; -40 dBm wins. A good way to feel how antenna, walls and body change the signal. Any key stops; ends with the best RSSI against the start |
| `app.time` | `[sync [ntp\|http\|module\|gnss] [host] \| set <unix\|YYYY-MM-DDTHH:MM:SS> \| push \| geo \| zone [<+HH:MM>\|off] \| selftest]` | App wall clock in UTC with its source and age, the local time when a zone is set, and the place (GNSS fix, else the last `wifi.locate` or `geo` answer, else unknown). `geo` asks `ip-api.com` (plain HTTP over the module) for the public IP, the city and the time zone with its current UTC offset, daylight saving included; later `sync`s refresh it. `zone` sets a fixed offset by hand (no daylight saving) or `off`. `sync` uses SNTP over a module UDP socket (default `pool.ntp.org`), falling back to the `Date` header of an HTTP `HEAD` (default `www.google.com`); `sync module` reads `AT+USYTU`; `sync gnss` takes the UTC of the open receiver's fix. With a fresh GNSS fix the clock follows the receiver by itself (checked once a minute, source `gnss`) unless it was `set` by hand; a replayed recording is never used. `set` sets it by hand, `push` writes it to the module (`AT+USYTU`). On STM32 it sets the clock `time()` reads; on a PC the OS keeps the clock and the offset is shown |
| `app.sbom` | | Software bill of materials: build target and compiler, AT API variant, then each component with version (u-connectClient with its commit, mbedTLS and TF-PSA-Crypto on Windows/Linux/browser, FreeRTOS or Zephyr on STM32), licence and source |
| `app.license` | `[name]` | Licence of each component; with a name (e.g. `qrcodegen`), the full notice |
| `web.start` | `[port] [token] [http\|https [cert]]` | Serve a web UI from the module's TCP listener (needs Wi-Fi) over HTTPS on port 443; prints `https://<ip>:<port>/`. HTTPS needs no access token unless one is given (`token` makes a random 32-digit hex one; the URL then ends in `#t=<token>` and the page asks for it when it is missing or wrong); plain `http` always uses one (random unless given). The first start (or a new module address) makes certificate `tls-web` for the module's IP, signed by the local CA `tls-ca` (created when missing; trust it once with `utils.tls.pem ca`). `https <cert>` uses `tls-<cert>` instead; `http` serves plain HTTP on port 80. On the NUCLEO-H753ZI and NUCLEO-F767ZI (FreeRTOS and Zephyr) the page is served over HTTPS only (`http` is refused) with keys from the board's hardware TRNG (see [docs/SECURITY.md](docs/SECURITY.md)); other boards have no web UI. Files: private keys (`*.key`) are never downloadable; uploading keys or `tls-*` certificates needs `https` |
| `web.stop` |  | Stop the web server |
| `web.status` |  | Show whether the web server is running, its port and SSE client count |
| `web.export` | `<dir>` | Write the page's JSON (`groups.json`, `commands-<group>.json`, `examples-<group>.json`) for a preview or a static demo; `.\ucxapp --web-preview` uses it to serve the page on the PC |
| `usecase.list` | | Use cases with a fit check and ready-made steps, in sections. Industry: `tool` (connected tool, steps in `usecase.example`), `iiot` (factory machine: Enterprise Wi-Fi, TLS to the MES, CAN data, one configuration per line, firmware lifecycle), `medical` (IEC 62304: module as a SOUP item, secure bonded BLE, traces and JUnit reports as evidence), `site` (construction tools: technician's phone over BLE, fleet found by name, site and depot Wi-Fi), `implement` (farm implement: ISOBUS CAN to the cab tablet over the module's AP or BLE, work record over MQTT), `condition` (motor condition monitoring: IMU vibration baseline, stream to the analysis PC, alarm over MQTT), `agv` (AGV or mobile robot: roaming between access points, route survey, latency to the fleet manager, BLE service link; the E-stop stays wired), `eol` (end-of-line test: identity, golden settings, radio check, report per serial number), `blegw` (BLE sensor gateway: sensors found by name and read over GATT, readings to the plant broker over Wi-Fi). Edge AI: `edge` (IMU training data, I2C sensors, results to the cloud; ALMA-W161/W166 run the same u-connectXpress steps, ALMA-W111/W116 run the model on their NPU). Industry and Edge AI are not in lite builds. Location: `boat` (GPS to a chart app over the module's AP or BLE), `rtk` (NTRIP corrections over Wi-Fi, position over BLE), `mower` (robotic lawn mower: phone setup and status over BLE, HP GNSS with PointPerfect over the garden Wi-Fi or cellular, theft alert over cellular), `farm` (tractor auto-steer: PointPerfect over cellular, corrected NMEA to the cab tablet over BLE, log upload over the farmyard Wi-Fi), `tracker` (position to a server over Wi-Fi or cellular). Cellular: `fieldkit` (Wi-Fi to cellular relay: laptops on the module's soft AP reach your server over LTE through `cell.bridge tcps`; one socket is relayed, not IP, so it is not a hotspot), `blecell` (a phone or BLE device on Nordic UART relayed to your server over LTE, no Wi-Fi on site), `vehicle` (CAN frames to your server over LTE with `can.bridge cell`, reconnecting after dead zones), `xplr` (XPLR-IOT-1 kit: SARA-R5 and MAX-M10S, position over LTE, `cell.power` and EasyFlash for the modem firmware); cellular cases are not in lite builds. Everyday: `home` (MQTT to Home Assistant), `provision` (Wi-Fi set up from a phone over BLE), `matter` (a Matter node commissioned from a home app over BLE: setup codes, card, second commissioning window; Windows, Linux and the browser), `hotel` (guest Wi-Fi login page), `notify` (iPhone/Android notifications), `remote` (BLE keyboard clicker and media remote), `finder` (item finder: distance to a BLE tag with Channel Sounding, `bt.cs` and `bt.cs.range`; not in lite builds); their steps are in `examples uc-<name>` |
| `usecase.demo` | `[name]` | Runs every step of every use case (or of one) on a virtual module, the Virtual module on the web page or vucx, and prints how many examples ran to the end and which stopped early. Steps that watch or serve for a while get 3 s each. Without the Matter plug-in the `matter` case is skipped (`skipped: matter`). Refuses a real module: the steps change its settings |
| `demo` | `[all]` | Shows the module off on a virtual module: a tour of the highlight use cases across the radios (Wi-Fi: phone setup, a Matter node commissioned over BLE, MQTT, hotel portal; GNSS to a chart app; Wi-Fi to LTE relay; BLE: sensor gateway, phone notifications, remote; Thread: mesh and border router; IMU vibration monitor), 3 s per watch step; `all` runs every use case like `usecase.demo`. On the web page the ▶ Demo button runs it and connects the Virtual module first |
| `usecase.check` | `<name>` | Fit check of a use case: `[ok]`, `[limit]`, `[check]` or `[gap]` per requirement, a `[gap]` when the module has no Wi-Fi or a command is not in this build (STM32), the open GNSS receiver and the station state. `usecase.check tool` is `usecase.tool` |
| `usecase.tool` | `[mbit/s]` | Fit check of the connected tool use case (factory Wi-Fi with Enterprise security, AP for laptops, HTTPS page on the host, 900 MB history in 30 min, 8 socket trays, BLE) against the NORA-W36 limits: `[ok]`, `[limit]`, `[check]` or `[gap]` per requirement, plus the station state when a module is connected. With a measured rate (e.g. from `net.iperf`) it prints the download time |

```powershell
ucxapp COM13 --exec "utils.qr.wifi MySsid MyPassword WPA"
ucxapp COM13 --shell        # then: wifi.profile.auto, web.start
```

#### Web UI

`web.start` serves a page with two views. **Toolbox** has four parts: a **Commands** panel generated from the command table
(a `MODULE` part for the groups the NORA module executes and a `HOST` part for the work this PC does, each in
sections: Device, Bluetooth, Wi-Fi and Thread, IP services; App, Protocols, Peripherals; one fold per group with its
description as tooltip, big groups fold `gatt.client.*`, `gatt.server.*` and the like into sub-menus, and one form
per command built from its usage string, so a new command shows up
without touching the page), a **Log** pane that mirrors everything the app prints (the
same text as the console), an **Events** pane that shows module events as they arrive (Wi-Fi link and
network up/down, MQTT, Bluetooth, GATT, SPS, HTTP and ping results), and a command line with history
(up/down arrows) and quick buttons. Handle fields (`[handle]` of `sock.*`, `[conn]`) are lists of the open
sockets and links (empty means the current one), fields remember the last value typed (never passwords), and
the last commands sit as one-click chips above the groups. **Use cases** has a page per use case (picked at the
top right): a live diagram (lit by the Wi-Fi station, AP, BLE links and sockets), the `usecase.tool` or
`usecase.check <name>` fit check and the steps (`usecase.example` or `examples uc-<name>`) as
cards that run with one click. Commands run one at a time (a second tab gets "busy"); every open tab receives the
output, events and state, each from its own place in the streams, and the module's 6 sockets (2 for the
listener) allow two tabs at once. The `web.*` commands are not allowed
from the page. Open the printed URL; with a token (always over `http`, over HTTPS only when `web.start` was given one) it ends in `#t=`. The token sits in the URL fragment, so it is not
sent with the page request; the page removes it from the address bar and keeps it for the tab only.
API calls send it in an `X-Token` header (or `?t=`); the `/events` stream uses `?t=`, as EventSource cannot set headers.

#### In the browser (WebAssembly + Web Serial)

`.\ucxapp --wasm` builds the whole app as WebAssembly (`build/wasm/site`) and serves it on
`http://127.0.0.1:8098/`. The page is the same as `web.start`, but nothing runs on a server: the app answers the
page inside the tab, and **Connect module** opens the EVK's serial port with Web Serial (Chrome or Edge,
115200 baud) and runs `dev.connect webserial`. Module commands, sockets and bridges work as on the PC; host-only parts (serial port list, GNSS/IMU/cellular on PC ports, the terminal UI) do not.
Files live in the tab's memory. **Virtual module** needs no hardware: it connects the same `webserial` device to a
simulated ALMA-W1 with Wi-Fi, Thread and Bluetooth that answers from `app/wasm/valma-w1.json`, the
`tests/harness/vucx/alma-w1-thread.yaml` test profile (the NORA-W36 and NORA-B26 Thread profiles merged), so every
example and use case runs on it.
Regenerate with `python -m harness.vucx --json alma-w1-thread.yaml` from `tests/`; `test_wasm.py` checks it matches.
The profile's `peer:` block is a BLE peer with every service the `gatt.client.*` and `hid.client` commands look for,
and an `ft.on` file server (notes.txt, big.bin); its `sockets:` block is the network behind the module, in the
browser as in the Python vucx used by the tests: an echo, an NTRIP caster on 2101, CoAP on 5683, SNTP on 123, TFTP
on 69, HTTP on 80 and the ft TCP server on 6970; a listener on 8000 gets a client and a UDP socket bound to 4433 a
datagram. Our own GATT server hands out handles as the module does (from 40, a service on the next multiple of 20).
There is no DTLS peer, so on the virtual module `dtls.client` and `dtls.server` skip the handshake and say so
(`dtls.selftest` runs a real one). The virtual GNSS, IMU, modem and PC (`virt:gnss`, `virt:imu`,
`virt:cell`, `virt:pc`) are what `gnss.detect`, `imu.detect` and `cell.detect` find there, so the examples run end
to end; missing demo files and unset `${VAR}`s get demo values.
The site is static: `.github/workflows/pages.yml` publishes it on GitHub
Pages, with `coi-serviceworker.js` adding the cross-origin isolation headers threads need.
#### Text UI

`--tui` opens the same three panes in a terminal window (Windows console, Linux terminal) and is the
default when there is no `--exec` or `--shell`. The layout widens with the window up to 180 columns by 45
rows; the first Log line shows the app version and build date (`--version` prints the same). Log lines wrap at word boundaries.
**Commands** (the same groups and sections as the web page, module groups first; Right/Enter expands a group or puts a command on the input line),
**Log** and **Events** (colored by source), with a hint line that shows the usage of the selected or
typed command.

```powershell
ucxapp COM13 --tui
```

| Key | Action |
|---|---|
| Enter | Run the input line (`exit` or `quit` leaves, `clear` empties the log) |
| Up / Down | Input history; in the Commands pane, move the selection |
| Tab | Complete the command name |
| Esc | Switch between the input line and the Commands pane |
| PgUp / PgDn | Scroll the log |
| F10, Ctrl-C | Quit |

The text UI and the web page share one core (`ucxapp_ui.c`): the command table and its usage parsing, the
log and event streams and the one-at-a-time command runner. A command added to a pack appears in both.

## Exit codes and errors

Commands return `UCXAPP_OK` (0) or a negative code (`src/ucxapp_cmd.h`):

| Code | Meaning |
|---|---|
| `-100` | Unknown command |
| `-101` | Bad arguments; the usage line is printed |
| `-102` | Command needs a module and none is connected |
| `-103` | The module reported a failure or a wait timed out |

In `--exec` mode the process exits with 1 on the first non-zero result, otherwise 0.

## Architecture

| File | Role |
|---|---|
| `ucxapp.h` | `ucxapp_t` context, IO layer, URC event flags |
| `ucxapp_cmd.h/.c` | Command registry, tokenizer, `help`, shell loop |
| `ucxapp_io.c`, `ucxapp_io_stdio.c` | `ucxapp_print` / `ucxapp_readline` and the stdio backend |
| `ucxapp_events.c` | URC event flags with a mutex; `ucxapp_events_wait()` blocks until a flag is set |
| `ucxapp_cmds_device.c`, `_wifi.c`, `_sock.c`, `_net.c`, `_sys.c`, `_mqtt.c`, `_bt.c` | Command packs |
| `ucxapp_platform.h` | Host OS services: console init, tick, key press, executable directory, serial-port listing, HTTPS GET |
| `ucxapp_platform_win.c` | Windows backend (registry, SetupAPI, FTDI D2XX, WinHTTP, `conio`) |

Rules for portable code: use `ucxapp_print()` / `ucxapp_readline()` for all I/O, and only
u-connectClient APIs and its `U_CX_PORT_*` macros for OS services.

## Adding a command

A command is a table entry. Register the table once at startup.

```c
static int cmdHello(ucxapp_t *app, int argc, char *argv[])
{
    /* argv[0] is the command name; use app->ucx for the module */
    ucxapp_print("hello\n");
    return UCXAPP_OK;            /* or UCXAPP_ERR_USAGE / UCXAPP_ERR_FAILED */
}

static const ucxapp_cmd_t gMyCmds[] = {
    { "my.hello", "[name]", "Say hello", UCXAPP_CMD_NEEDS_DEVICE, cmdHello },
};

void ucxapp_register_my_cmds(void)
{
    ucxapp_cmd_register(gMyCmds, sizeof(gMyCmds) / sizeof(gMyCmds[0]));
}
```

Then call the register function in `main()` and add the file to `CMakeLists.txt`.
`UCXAPP_CMD_NEEDS_DEVICE` makes the dispatcher reject the command when no module is connected,
so the handler can use `app->ucx` without checking. Returning `UCXAPP_ERR_USAGE` prints
`usage: <name> <usage>` automatically. To wait for a URC use
`ucxapp_events_clear(flag)` before the AT command and `ucxapp_events_wait(flag, seconds)` after it.

## Porting to another target

1. Provide a `ucxapp_io_t` (`write` and `readline`, for example over a UART) and pass it to `ucxapp_io_set()`.
2. Create the `uCxHandle_t` with the u-connectClient port for your platform and set `app.ucx`.
3. Call `ucxapp_events_init()` and register the URC handlers that call `ucxapp_events_signal()`.
4. Register the command packs and call `ucxapp_shell_run(&app)` or `ucxapp_cmd_exec()`.

Steps 2 and 3 are done by `src/ucxapp_main.c` (and `src/ucxapp_urc.c`), which is the entry point on both Windows and POSIX.
