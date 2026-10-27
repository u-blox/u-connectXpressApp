# Command Layer

A small, portable layer for driving a u-connectXpress module with text commands instead of the
menu. It lives in `app/src/ucxapp_*` and has no Windows dependencies, so the same
commands can run in a PC console, a Linux terminal or a UART shell on an embedded target.

## Using it

```powershell
# One-shot: run commands in order, stop at the first failure (exit code 1)
ucxapp COM13 --exec "dev.info" --exec "wifi.scan"

# Interactive shell
ucxapp COM13 --shell

# A single command can be given directly (no spaces, no quotes), port optional
ucxapp COM13 dev.info
ucxapp dev.info
```

- Add `--tui` for a three-pane text UI (see [Text UI](#text-ui)).

- Without a port argument, `--exec` and `--shell` use the saved port from the ini file.
- `--offline` skips the module auto-detection, for commands that need no device (`tls.*`, `help`).
- If the module cannot be connected, commands that need it fail with `not connected`; `help` still works.
- Arguments are whitespace separated; use double quotes for values with spaces
  (`wifi.connect "My Network" "my password"`). `\"` and `\\` escape inside quotes.
- Lines starting with `#` are comments. Maximum 8 arguments per command.
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
| `<group>.ex [n]`, `examples [group] [n]` | | Real scenarios with ready-to-type commands for gnss, imu, wifi, net, http, sock, mqtt, cert, bt, gatt, hid, web, fw, dev, cell, can, hw, coap, tftp, ft, dtls, log, file and utils (`gnss.ex`); only groups in the build are listed. With `n`, runs example n line by line and stops at the first error or before a command that changes data (`wifi.ex 1`); the web page's "Run all" uses this |
| `at` | `[command]` | Send a raw AT command; the `AT` prefix is optional (`at +GMI`). With no command it enters AT mode: every line is sent as typed (quotes kept) until you type `back` |
| `at.reset` | | Reboot the module (`AT+CPWROFF`); same as `dev.reboot` |
| `at.factory` | `<confirm>` | Factory reset (`AT+USYFR`) and reboot; same as `dev.factory`, the word `confirm` is required |

### System

| Command | Arguments | Description |
|---|---|---|
| `dev.info` | | Manufacturer, model, firmware version |
| `dev.caps` | | Capabilities: model, firmware, Wi-Fi, Bluetooth LE, and the sockets/MQTT/TLS/HTTP/GATT features this module and build support |
| `dev.uptime` | | Time since the module started, counted from its last `+STARTUP` |
| `dev.stack` | | Developer: stack size, used bytes and usage % of every module task |
| `dev.crash` | | Developer: last crash info stored in module flash; reading it clears it |
| `dev.diagnose` | `[host]` | Support report: `dev.caps`, uptime, Wi-Fi, DNS and ping of the host (default u-blox.com), certificates; ends with `Result: PASS` or `FAIL` |
| `dev.status` | | Overview: module info, Wi-Fi state and IP, open sockets, BLE connections, certificates |
| `dev.reboot` | | Reboot the module (clears GATT services and connections); use as the last command |
| `dev.factory` | `<confirm>` | Reset the module to factory settings (erases Wi-Fi, Bluetooth and certificate settings) and reboot; the word `confirm` is required |
| `dev.connect` | `[port]` | Connect to the module (default: saved port). Windows only |
| `dev.disconnect` | | Disconnect. Windows only |
| `dev.use` | `<port>` | Switch the active module to another serial port; the current one is released (and kept if the new one fails). One module is active at a time |
| `fw.download` | `[latest [file]]` or `<url> <file> [sha256]` | With no arguments (same as `latest`) looks up the newest u-connectXpress GitHub release for the connected module family (e.g. NORA-W36) and saves its zip (Windows only); flash it with `fw.flash`. With a URL: download via the module (follows redirects), print SHA-256, delete the file on mismatch |
| `fw.update` | `[force] [baud]` | Compares the module firmware with the newest GitHub release for its family; if that is newer (or `force`), downloads the zip into the app store (reused if already there), flashes it and checks the module reports the new version. Download needs Windows |
| `fw.bootloader` | `<port> <file.bin\|file.zip> [baud]` | Flash a module that is in bootloader mode (no AT interface): waits for the `>` prompt, sends `x`, then XMODEM; run `dev.disconnect` first |
| `fw.flash` | `<file.bin\|file.zip> [baud]` | XMODEM firmware update (default 921600 baud); a zip is only extracted (built-in unzip), and only the `.bin` inside is flashed |

```powershell
ucxapp COM13 --exec "dev.info"
ucxapp COM13 --exec "fw.download latest" --exec "fw.flash NORA-W36X-3.1.0.zip"
ucxapp COM13 --exec "at +GMR"
```

### Bluetooth

| Command | Arguments | Description |
|---|---|---|
| `bt.scan` | `[full]` | Scan for BLE devices (up to 30 s). One line per device (duplicates merged, named first, strongest first) with what it advertises: services, manufacturer, iBeacon/Eddystone, appearance. `full` decodes the whole advertising data. The results are kept for `bt.connect` |
| `bt.connect` | `[addr[,random]\|n\|name]` | With no argument: connect to the last used profile, else scan and take the strongest known device (same as `bt.profile.auto`). Otherwise connect by address, by the number from the last `bt.scan`, or by profile or scanned device name (a unique start of the name is enough). Suggests `bt.profile.add` for devices not yet saved |
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
| `gatt.client.<svc>` | `[conn]` (`sps`: `[flow]`) | Discover a predefined service, read, decode and subscribe; `<svc>` is `hrs`, `cts`, `ess`, `lns`, `aio`, `bas`, `dis`, `ras` (Ranging Service, Channel Sounding), `nus`, `sps` or `wit` (WitMotion BLE IMU; decodes the 0x55 0x61 packets) |
| `gatt.send` | `[conn] <nus\|sps> <text>` | Send text to the remote NUS or SPS service |
| `gatt.credits` | `[conn] <1-127>` | Send SPS credits |
| `gatt.wps` | `[conn] <status\|scan\|connect\|forget\|config <ssid> <pw> [OPEN\|WPA2\|WPA3]\|raw <text>>` | Send a Wi-Fi provisioning command to a remote device and wait for the response |
| `gatt.server.<svc>` | `[seconds]` | Define a predefined local service (once per module restart); with seconds, simulate values for a connected client. `<svc>` is `hrs`, `cts`, `ess`, `lns`, `aio`, `bas`, `ras`, `nus` or `sps` |
| `gatt.server.nus` / `.sps` / `.wps` | `[seconds]` | Nordic UART, u-blox Serial Port and Wi-Fi Provisioning servers; with seconds, serve a client for that long (client writes are printed, WPS commands are executed) |
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
| `wifi.connect` | `[ssid\|n] [password\|-]` | With no argument: connect like `wifi.profile.auto` (the last used profile, else the strongest known network). Otherwise connect by SSID or by the number from the last `wifi.scan`; `-` prompts for the password; waits up to 20 s and shows IP, mask, gateway, channel and signal. A saved profile for the SSID supplies the password; open networks need none |
| `wifi.status` | | Connection state, SSID, RSSI, IP |
| `wifi.eap` | `<ssid> <ca> <cert> <key> <identity> [1.2\|1.3\|any]` | Connect to a WPA2/WPA3-Enterprise network with EAP-TLS. `ca`, `cert` and `key` are names on the module (`cert.upload`, `cert.list`); the module requires an identity; TLS version defaults to `any`. Waits up to 30 s. Not in lite builds |
| `wifi.peap` | `<ssid> <user> <password\|-> <ca> [1.2\|1.3\|any]` | Connect to a WPA2/WPA3-Enterprise network with PEAP (MSCHAPv2); `-` prompts for the password. The module requires the CA that verifies the RADIUS server. Not in lite builds |
| `wifi.disconnect` | | Leave the network |
| `wifi.ip` | `[dhcp \| <ip> <mask> <gw> [dns1 [dns2]]]` | Show, or set, DHCP or static IP; applies on the next `wifi.connect` |
| `wifi.rssi` | `[count] [interval_s]` | Read the signal strength, repeatedly if requested |
| `wifi.roaming` | `[on\|off]` | Show or set roaming between access points |
| `wifi.regdomain` | `[world\|etsi\|fcc\|ic\|nz\|mkk\|ncc\|acma\|kcc\|sa\|br]` | Show, or set and save, the regulatory domain used by `wifi.profile.auto` (default `etsi`) |
| `wifi.profile.add` | `<name> <ssid> [password]` | Save or update a Wi-Fi profile in `ucxapp.wifi` (current directory; passwords are obfuscated, not encrypted; max 16) |
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
| `net.ping` | `<host> [count]` | ICMP ping, count 1-10 (default 4); fails if no reply |
| `net.iperf` | `<server\|client\|stop> [args...]` | iperf throughput test run by the module: `server [tcp\|udp] [port]` keeps running until `stop`; `client <ip> [tcp\|udp] [seconds] [port]` runs for the given time (default 10 s, port 5001). Needs a Wi-Fi connection. PC side: iperf 2 (`iperf -c <module-ip>` / `iperf -s`) |
| `net.dns` | `<host>` | Resolve a host name |
| `net.ntp` | `<status\|on\|off\|server> [args...]` | NTP client: `status`, `on [manual\|auto]`, `off`, `server <0\|1> <host\|->` (`-` removes) |
| `coap.get` | `<host> <port> [path]` | CoAP GET over UDP via a module socket (confirmable, 3 tries, 2 s each) |
| `coap.selftest` | | Offline check of CoAP request encoding and response parsing |
| `tftp.on` | `[rw] [port] [seconds]` | TFTP server (RFC 1350, with `blksize`/`tsize` options) on a module UDP socket, default port 69; serves plain file names from the working folder, read only unless `rw`. Uploads land as `<name>.part` and are renamed when complete. With `seconds` it blocks that long, then stops. No authentication: trusted networks only |
| `tftp.off` | | Stop the TFTP server |
| `tftp.status` | | Server port, mode, transfer count and the last transfer |
| `tftp.get` | `<host> <remote> [local] [port]` | Download a file from a TFTP server |
| `tftp.put` | `<host> <local> [remote] [port]` | Upload a file to a TFTP server |
| `tftp.selftest` | | Offline check of TFTP name rules, options and packet encoding |
| `ft.on` | `[rw] [tcp[:port]] [seconds]` | File transfer server serving plain file names from the working folder; read only unless `rw`. Over BLE: a custom GATT service (`7a3f0001-5c1e-4d2b-9f6a-3e8b2c4d5e6f`, CTRL `...0002`, DATA `...0003`), needs an encrypted link, blocks up to 242 bytes. With `tcp`: a module TCP socket (default port 6970), the same packets framed as `[len u16][channel u8][packet]`, blocks up to 1455 bytes, one client at a time, no authentication (trusted networks only). Go-back-N with a window of 8; every file is checked with CRC-32 and size, uploads land as `<name>.part` and are renamed when verified |
| `ft.off` | | Stop the file transfer server |
| `ft.status` | | Server link, mode, transfer count and the last transfer |
| `ft.get` | `<conn\|host[:port]> <remote> [local] [blk] [resume]` | Download a file from a peer's `ft` server: a BLE connection number, or a host for TCP (blk 16-242 over BLE, default 240; up to 1455 over TCP, the default). An interrupted download keeps `<local>.part`; `resume` continues from it |
| `ft.put` | `<conn\|host[:port]> <local> [remote] [blk] [resume]` | Upload a file to a peer's `ft` server (needs `rw` there); `resume` continues from the `.part` the server kept |
| `ft.ls` | `<conn\|host[:port]>` | List the files on a peer's `ft` server |
| `ft.rm` | `<conn\|host[:port]> <name>` | Delete a file on a peer's `ft` server (needs `rw` there) |
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
| `sock.read` | `[handle] [max] [wait_s]` | Wait for data (default 5 s) and print up to `max` bytes (default and maximum 512) |
| `sock.bind` | `[handle] <port>` | Bind a socket to a local port (UDP receive) |
| `sock.listen` | `[handle] [port]` | Listen for TCP connections |
| `sock.accept` | `[wait_s]` | Wait for an incoming TCP connection (default 30 s) and print its handle |
| `sock.sendto` | `[handle] <host> <port> <text...>` | Send a UDP datagram |
| `sock.recvfrom` | `[handle] [max] [wait_s]` | Receive a UDP datagram and print the sender |
| `sock.close` | `[handle] \| all` | Close a socket, or every socket on the module |
| `sock.list` | | List open sockets |
| `sock.echo` | `<tcp\|udp> <port> [seconds]` | Echo server for latency/throughput tests (default 60 s, one TCP client) |

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

### MQTT

| Command | Arguments | Description |
|---|---|---|
| `mqtt.connect` | `[host] [port]` | Connect to an MQTT broker (default `broker.emqx.io:1883`, no TLS); waits up to 10 s |
| `mqtt.disconnect` | | Disconnect from the broker |
| `mqtt.subscribe` | `<topic> [qos]` | Subscribe (wildcards allowed, qos 0-2) |
| `mqtt.unsubscribe` | `<topic>` | Unsubscribe |
| `mqtt.publish` | `<topic> <text...>` | Publish a QoS 0, non-retained message (max 511 bytes) |
| `mqtt.read` | `[wait_s]` | Wait (default 10 s) for messages and print them |

```powershell
ucxapp COM13 --exec "wifi.profile.auto" --exec "mqtt.connect" `
  --exec "mqtt.subscribe ucx/demo/#" --exec "mqtt.publish ucx/demo/hello hi there" --exec "mqtt.read 5"
```

Known firmware behaviour: on NORA-W36 an MQTT publish made while a subscription is active returned ERROR:15 in testing, and back-to-back MQTT operations get ERROR:15 while the previous one is in flight; the commands retry the latter automatically.

### HTTP

| Command | Arguments | Description |
|---|---|---|
| `http.get` | `<url> [show_bytes]` | GET over HTTP or HTTPS (scheme optional, default HTTP); prints the status line and up to `show_bytes` (default 512) of the body |
| `http.download` | `<url> <file>` | Download to a file, following up to 5 redirects in the app (the module firmware does not); needs a 2xx response, checks `Content-Length` when present, deletes the file on failure |
| `http.post` | `<url> <body> [content_type]` | POST a body (default `application/x-www-form-urlencoded`), prints the status line and up to 512 bytes of the reply |
| `wifi.portal` | `<check\|login> [probe_url] [field=value ...]` | Hotel/guest Wi-Fi login page (captive portal). `check` fetches `http://connectivitycheck.gstatic.com/generate_204` (or `probe_url`): 204 = online, a redirect or a page = login page in the way. `login` loads the login page (keeping its cookie), fills in its first `<form>` (hidden fields, checkboxes ticked, your `field=value` overrides), submits it and checks again. Pages built by JavaScript or with a captcha need a browser. Test with `tests/harness/captive_portal.py` |
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
| `dtls.selftest` | `[server-cert]` | Run a DTLS client and server in-process (offline) and check an echo |
| `dtls.client` | `<host> <port> [text]` | DTLS client over a module UDP socket: handshake, send text, print the reply |
| `dtls.server` | `<port> [cert]` | DTLS server over a module UDP socket: one client, echo one message |

```powershell
ucxapp COM13 --exec "cert.upload ca root-ca.pem" --exec "cert.upload client device.pem" --exec "cert.upload key device-key.pem" --exec "cert.list"
```

#### App TLS (HTTPS and TLS bridges)

The module's TLS only covers outgoing connections, so the app terminates TLS itself (mbedTLS) for
`web.start https` and `*.bridge tls`. The `tls.*` commands keep a small local CA and certificates in the
app store as `tls-<name>.crt` / `tls-<name>.key` (PEM, ECDSA P-256, SHA-256). No device is needed.

| Command | Arguments | Description |
|---|---|---|
| `utils.tls.ca.create` | `[cn] [force]` | Create the local CA (`tls-ca.*`, valid 20 years). `force` replaces it; certificates it issued stop working |
| `utils.tls.server.create` | `<name> <host\|ip> [...]` | Server certificate signed by the CA for up to 8 host names / IPv4 addresses (the first is the common name), valid 10 years |
| `utils.tls.client.create` | `<name>` | Client certificate for `clientauth` bridges |
| `utils.tls.list` | | Certificates with subject, issuer and validity |
| `utils.tls.pem` | `<name> [key]` | Print a certificate, or its key, in PEM (copy `utils.tls.pem ca` to clients) |
| `utils.tls.remove` | `<name>` | Remove a certificate and its key |
| `utils.tls.selftest` | | Run the mbedTLS built-in known-answer tests (hashes, AES/GCM/CCM, ChaCha20-Poly1305, DRBGs, bignum, RSA, ECP), one line per algorithm with its time |

```powershell
ucxapp COM13 --shell
> utils.tls.ca.create
> utils.tls.server.create server 192.168.1.42 nora.local
> web.start https                 # https://192.168.1.42/#t=<token>
> utils.tls.client.create laptop
> gnss.bridge tls 2947 server clientauth
```

Install `tls-ca.crt` as a trusted root in the browser or OS to avoid the certificate warning.
Validity starts at the app's build date, so no clock is needed. A test client:
`openssl s_client -connect 192.168.1.42:2947 -CAfile tls-ca.crt -cert tls-laptop.crt -key tls-laptop.key`.

### GNSS and IMU sensors (PC only, no module needed except for bridges)

A sensor on a PC serial port is read by a background thread. `gnss.*` handles u-blox receivers (NMEA and UBX); `imu.*` handles WitMotion serial IMUs (WT61, WT901 and similar, including the 20-byte BLE packet) and Bosch BNO055 over UART. Bosch BMI160/BMI270/BMI323 and BNO08x have no PC serial interface (I2C or SPI only) and are not supported.

A bridge forwards the raw byte stream through the connected module: `tcp` listens on the module's IP address (up to 4 clients), `udp` sends datagrams, `ble` notifies on the Nordic UART TX characteristic (needs a connected client with notifications enabled). The module must be on Wi-Fi for `tcp` and `udp`. The AT link limits throughput to a few kB/s; excess data is dropped and counted.

| Command | Arguments | Description |
|---|---|---|
| `gnss.detect` | `[port]` | Find a GNSS receiver on the serial ports (tries 9600 to 921600 baud) |
| `gnss.open` | `[port] [baud]` | Open a receiver; detects port and baud when omitted |
| `gnss.close` | | Close the receiver |
| `gnss.status` | | Fix, position, time, satellites, DOP, receiver version and counters |
| `gnss.sats` | | Satellites with elevation, azimuth, signal strength and used flag |
| `gnss.watch` | `[seconds]` | Live card once a second: fix bars, used/seen satellites as dots, HDOP bars with a rating, position, UTC time and time to first fix since `gnss.open`. Default 10 s, any key stops |
| `gnss.log` | `<file\|off>` | Record the raw receiver stream to a file |
| `gnss.send` | `<hex..\|$NMEA\|ubx cls id [hex]>` | Send raw bytes, an NMEA sentence (checksum added) or a UBX frame |
| `gnss.restart` | `<hot\|warm\|cold>` | Restart the receiver (UBX-CFG-RST) |
| `gnss.rate` | `<hz>` | Navigation rate 1 to 25 Hz (UBX-CFG-RATE) |
| `gnss.bridge` | `<tcp [port]\|tls [port] [cert] [clientauth]\|tcpc <host> <port>\|ntrip <host> <port> <mount> [user pass] [ble]\|udp <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off\|status>` | Two-way bridge of the receiver stream. `tcp` serves (default 2947), `tls` serves the same over TLS with certificate `tls-<cert>` (default `server`; `clientauth` requires a client certificate from the local CA), `tcpc` connects and reconnects, `ntrip` is an NTRIP v1 client (plain TCP, e.g. PointPerfect Flex/Live on port 2101 or RTK2go): corrections (RTCM or SPARTN) go to the receiver and its latest GGA goes to the caster every 10 s; `ble` also sends the corrected NMEA to a phone over BLE Nordic UART (RTK rover); keep credentials out of scripts with `${NTRIP_USER}` `${NTRIP_PASS}`, `udp` sends and listens on `local`, `udps` listens and answers the last sender, `ble` is Nordic UART (`nus`), u-blox SPS or your own service (`custom`, below), `uart` a second PC port, `cell` the data link opened by `cell.connect`. Data from the far side goes to the receiver |
| `imu.detect` | `[port]` | Find a WitMotion or Bosch BNO055 IMU |
| `imu.open` | `[port] [baud] [wit\|bno055]` | Open an IMU; detects port, baud and type when omitted |
| `imu.close` | | Close the IMU |
| `imu.status` | | Accel, gyro, angle, magnetometer, quaternion, pressure, steps and counters |
| `imu.log` | `<file\|off>` | Record the raw IMU byte stream to a file |
| `imu.send` | `<hex..>` | Send raw bytes (WitMotion configuration frames) |
| `imu.rate` | `<hz>` | WitMotion output rate (0.2 to 200 Hz) |
| `imu.bridge` | `<tcp [port]\|tcpc <host> <port>\|udp <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off\|status>` | Two-way bridge of the IMU stream, same modes as `gnss.bridge` |

### Bridges

Each source has its own bridge (`gnss.bridge`, `imu.bridge`, `cell.bridge`, `can.bridge`, `hw.bridge`), and they run at the same time.

| Command | Arguments | Description |
|---|---|---|
| `bridge.list` | | Every open source and where its bridge goes |
| `bridge.off` | | Stop every running bridge |

### Cellular modules

Basic access to a cellular module on a PC serial port, using the generic 3GPP AT commands (TS 27.007) that
Quectel, SIMCom, u-blox, Telit, Sierra and Nordic nRF91 modules share. Not verified on hardware yet; only
`cell.ping` and `cell.connect` are vendor specific (picked from the module's ATI maker string). Everything else goes through `cell.at`.

| Command | Arguments | Description |
|---|---|---|
| `cell.detect` | `[port]` | Find a module answering AT (115200, 9600, 921600, 460800 baud) |
| `cell.open` | `<port> [baud]` | Open a module (default 115200) |
| `cell.close` |  | Close it |
| `cell.status` |  | SIM, ICCID, signal in dBm, registration (LTE and 2G/3G), operator, IP |
| `cell.at` | `<command>` | Send a raw AT command |
| `cell.apn` | `[apn [ip\|ipv6\|ipv4v6]]` | Show or set the APN of context 1 |
| `cell.attach` | `[on\|off]` | Attach and activate data, detach, or show the state |
| `cell.ping` | `<host>` | Ping over the cellular link (Quectel and u-blox only) |
| `cell.bridge` | `<tcp [port]\|tcpc <host> <port>\|udp <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|off\|status>` | Give the modem's AT console to a peer, same modes as `gnss.bridge` (`off` returns to command mode) |
| `cell.connect` | `<host> <port> [tcp\|udp]` | Open a transparent socket through the modem. Vendor paths: u-blox (default, `AT+USOCR`/`AT+USOCO`), Quectel (`AT+QIOPEN` access mode 2), SIMCom (`AT+CIPMODE=1`, `AT+CIPOPEN`), Telit (`AT#SGACT`, `AT#SD`), Nordic nRF91 (`AT#XSOCKET`, `AT#XCONNECT`). The modem's UART then carries the payload: bridge to it with `gnss.bridge cell` or `imu.bridge cell` |
| `cell.disconnect` | | Stop the bridge using the link first; sends `+++` after the guard time and closes the socket |

### Dashboard

| Command | Arguments | Description |
|---|---|---|
| `dash` | `[watch [seconds]]` | One card with a line each for Wi-Fi (SSID, RSSI, IP), BLE (links, GATT listen), GNSS (fix, satellites, HDOP), cellular (registration, operator, signal) and IMU (angles or acceleration). Filled dot = active. Each line shows the port; other NORA modules found on USB are listed as `standby (dev.use <port>)` and GNSS receivers found by their USB descriptor but not opened as `found, not open`. `watch` refreshes every second (default 10 s, any key stops). Works without a module; Wi-Fi and BLE then show `no module` |

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
| `hw.load` | `<file.json>` | Load a board profile: pin names, channel counts and emulated I2C devices (shell only) |
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
| `hw.spi.cfg` | `<mode 0-3> <hz>` | Set the SPI mode and clock |
| `hw.spi.xfer` | `<hex...>` | Full duplex transfer, prints the bytes read |
| `hw.adc.read` | `<ch>` | Read an ADC channel (12 bit, 3.3 V reference) |
| `hw.pwm.set` | `<ch> <hz> <duty%>` | Start PWM on a channel |
| `hw.pwm.off` | `<ch>` | Stop PWM on a channel |
| `hw.exec` | `<command...>` | Run one bridge line, for example `hw.exec gpio.read 3`, and print the reply |
| `hw.bridge` | `<tcp [port]\|tcpc <host> <port>\|udp <host> <port> [local]\|udps <port>\|ble [nus\|sps\|custom <uuid> [rx tx]]\|uart <port> [baud]\|cell\|off\|status>` | Same modes as `gnss.bridge` (TCP default port 7000) |

The bridge speaks lines: the peer sends a command without the `hw.` prefix (`i2c.reg 0x68 0x75`) and gets `OK <text>`
or `ERR <text>`. While bridged, GPIO input changes arrive as `EVT gpio <pin> <level>`.

CAN has its own commands and bridge (emulated loopback controller until a bxCAN/FDCAN driver is added):

| Command | Arguments | Description |
|---------|-----------|-------------|
| `can.cfg` | `<bitrate>` | Set the CAN bit rate |
| `can.send` | `<id> [hex...]` | Send a CAN frame (id above 0x7FF is extended) |
| `can.recv` |  | Take the next received frame |
| `can.status` |  | Bit rate, counters and bridge state |
| `can.bridge` | same modes as `hw.bridge` (TCP default port 7001) | Binary, both ways: 17-byte records (ts_ms u32 LE, id u32 LE, dlc, data[8]), up to 32 per write |
| `can.selftest` |  | Offline check of the record format |

Board profiles live in `app/boards/` (`h753.json`, `f767.json`, `f439.json`); `scripts/hw_config.py <profile>
--out hw_config.h` validates one (unknown keys, bad or duplicate pins, reserved pins) and generates X-macro pin tables
for a firmware build. `hw.load` reads the same file on the host. Pins can then be given by name (`hw.gpio.write PB0 1`).
Format: `board`, `gpio[]`, `adc[]`, `pwm[{pin,timer}]`, `i2c{scl,sda}`, `spi{sck,miso,mosi,cs}`, `can{rx,tx}`,
`reserved[]` and, for the emulation only, `emu.i2c[{addr,regs{reg:value}}]`.

Default pins on all three Nucleo-144 boards (not verified on hardware; the STM32 firmware does not use them yet):
I2C1 PB8/PB9, SPI4 PE2 (SCK), PE5 (MISO), PE6 (MOSI), PE4 (CS), CAN on PD0 (RX) and PD1 (TX), PWM on TIM1 PE9/PE11/PE13/PE14,
ADC on PA3, PC0, PC3, PF10, GPIO on the user LEDs and button plus PF12-PF15, PD14 and PD15. PB6/PB7, PG9/PG14 and
PD8/PD9 stay reserved. SPI4 is used because PA7 on SPI1 is shared with the Ethernet PHY.

### Tools (no AT equivalent)

| Command | Arguments | Description |
|---|---|---|
| `utils.qr.text` | `<text...>` | Print a QR code for any text |
| `utils.qr.wifi` | `<ssid> [password] [WPA\|WEP\|nopass] [hidden]` | Print a Wi-Fi join QR code |
| `utils.qr.geo` | `<lat> <lon>` | Print a location QR code |
| `utils.zip` | `<archive.zip> <file> [file...]` | Create a zip archive from up to 6 files (deflate; no device needed) |
| `utils.unzip` | `<archive.zip> [dir]` | Extract a zip archive, default into the current directory (no device needed) |
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
| `session.replay` | `<file.ucx> [all] [stop]` | Run the commands of a recorded session or script (`${NAME}` expanded); firmware, factory reset and removals are skipped unless `all`; `stop` ends at the first failure |
| `session.selftest` | | Offline check of secret masking and report file names |
| `prod.test` | `[script.ucx\|-] [dir]` | Production test: runs a script (or `-`: `dev.info`, `dev.caps`, `dev.diagnose`), writes `<dir>/<serial>_<date>.txt` with all output and adds a line to `<dir>/report.csv` (date, serial, model, firmware, script, steps, failed, PASS/FAIL); default dir `reports` |
| `file.hash` | `<md5\|sha256> <file>` | Print the hash of a file (no device needed) |
| `utils.hash.text` | `<md5\|sha256> <text...>` | Print the hash of a text |
| `utils.hash.selftest` | | Offline check of md5 and sha256 against RFC 1321 / FIPS 180-2 vectors |
| `file.view` | `<file> [lines]` | Show the first lines of a text file, read only (default 40 lines; non-printable bytes shown as dots) |
| `file.list` | | List the files in the working folder, numbered, with sizes |
| `file.remove` | `<file\|number>` | Delete a file by name or by its number from `file.list` |
| `file.recv` | `[name]` | Receive a file over the console with YMODEM (1K blocks, CRC-16) and store it under its own name or `name`; STM32 console only (Tera Term, ExtraPuTTY, `sz`, `ucxapp --stm32 --put`) |
| `file.send` | `<file>` | Send a file over the console with YMODEM; STM32 console only (Tera Term, ExtraPuTTY, `rz`, `ucxapp --stm32 --get`) |
| `file.selftest` | | Offline check of the YMODEM encoder and decoder (CRC-16, 1K and 128-byte blocks, retry) |
| `stats.show` | `[tcp\|udp\|sps\|gatt\|http\|mqtt\|web] [rx\|tx]` | Payload bytes sent and received per channel; `stats.show tcp rx` prints just the number |
| `stats.clear` | `[tcp\|udp\|sps\|gatt\|http\|mqtt\|web]` | Reset one channel's counters, or all |
| `app.info` | | About this app: maker, platform and links |
| `app.complete` | `<prefix>` | Show what Tab would complete a typed start of a command to, e.g. `app.complete wifi.sc` prints `1 match: wifi.scan` (for scripts and tests) |
| `app.memory` | | App RAM, heap and flash (disk on PC) use, plus task stacks on STM32; same rows as `dev.stack` |
| `app.version` | | App version, build time, platform and compiler |
| `app.uptime` | | Time since the app started (`days hh:mm:ss`) |
| `app.time` | `[sync [ntp\|http\|module] [host] \| set <unix\|YYYY-MM-DDTHH:MM:SS> \| push \| selftest]` | App wall clock in UTC with its source and age. `sync` uses SNTP over a module UDP socket (default `pool.ntp.org`), falling back to the `Date` header of an HTTP `HEAD` (default `www.google.com`); `sync module` reads `AT+USYTU`. `set` sets it by hand, `push` writes it to the module (`AT+USYTU`). On STM32 it sets the clock `time()` reads; on a PC the OS keeps the clock and the offset is shown |
| `app.sbom` | | Software bill of materials: components, versions, licences and sources |
| `app.license` | `[name]` | Licence of each component; with a name (e.g. `qrcodegen`), the full notice |
| `web.start` | `[port] [token] [https [cert]]` | Serve a web UI from the module's TCP listener (needs Wi-Fi; default port 80); prints `http://<ip>:<port>/#t=<token>` (random 12-digit hex token unless one is given; the page asks for it when it is missing or wrong). `https` serves over TLS (default port 443, certificate `tls-server`, see App TLS). Files: private keys (`*.key`) are never downloadable; uploading keys or `tls-*` certificates needs `https` |
| `web.stop` |  | Stop the web server |
| `web.status` |  | Show whether the web server is running, its port and SSE client count |

```powershell
ucxapp COM13 --exec "utils.qr.wifi MySsid MyPassword WPA"
ucxapp COM13 --shell        # then: wifi.profile.auto, web.start
```

#### Web UI

`web.start` serves a page with four parts: a **Commands** panel generated from the command table
(one section per group, one form per command built from its usage string, so a new command shows up
without touching the page), a **Log** pane that mirrors everything the app prints (the
same text as the console), an **Events** pane that shows module events as they arrive (Wi-Fi link and
network up/down, MQTT, Bluetooth, GATT, SPS, HTTP and ping results), and a command line with history
(up/down arrows) and quick buttons. Commands run one at a time; the `web.*` commands are not allowed
from the page. Open the printed URL, including the `#t=` token. The token sits in the URL fragment, so it is not
sent with the page request; the page removes it from the address bar and keeps it for the tab only.
API calls send it in an `X-Token` header (or `?t=`); the `/events` stream uses `?t=`, as EventSource cannot set headers.

#### Text UI

`--tui` opens the same three panes in a terminal window (Windows console, Linux terminal) and is the
default when there is no `--exec` or `--shell`. The layout widens with the window up to 180 columns by 45
rows; the first Log line shows the app version and build date (`--version` prints the same). Log lines wrap at word boundaries.
**Commands** (groups in AT manual order; Right/Enter expands a group or puts a command on the input line),
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
