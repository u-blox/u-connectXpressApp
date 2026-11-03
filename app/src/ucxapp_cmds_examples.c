#include "ucxapp_cmd.h"
#include "ucxapp_xplorer.h"

#include <stdio.h>
#include <string.h>

/* Real scenarios per command group, shown by "<group>.example" or "examples <group>". Lines starting with '#' are notes. */
typedef struct {
    const char *group;
    const char *title;
    const char *lines;
} example_t;

static const example_t kExamples[] = {
    { "bridge", "Sensor hub: GNSS to the PC over Wi-Fi, IMU to a phone over BLE",
      "wifi.profile.auto\ngnss.open\nimu.open\ngnss.bridge tcp\nimu.bridge ble nus\nbridge.list" },
    { "bridge", "Asset tracker: GNSS over cellular, CAN over Wi-Fi",
      "cell.open COM30\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.open\ngnss.bridge cell\n"
      "can.bridge udp 192.168.1.10 6000\nbridge.list" },
    { "bridge", "RTK rover: NTRIP in over Wi-Fi, position out over BLE",
      "wifi.profile.auto\ngnss.open\ngnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\nbridge.list" },
    { "bridge", "Remote modem console over Wi-Fi",
      "wifi.profile.auto\ncell.open COM30\ncell.bridge tcp 7000\n# telnet <module-ip> 7000 and type AT commands" },
    { "bridge", "Stop everything",
      "bridge.list\nbridge.off" },

    { "gnss", "First fix check",
      "gnss.open\ngnss.status\ngnss.sats" },
    { "gnss", "Watch the fix come in (time to first fix)",
      "gnss.open\ngnss.restart cold\ngnss.watch 60" },
    { "gnss", "u-center or gpsd on the PC over Wi-Fi",
      "wifi.connect MyNetwork MyPassword\ngnss.bridge tcp\n# connect u-center / gpsd to <module-ip>:2947" },
    { "gnss", "Phone app over BLE",
      "gnss.bridge ble nus\n# open nRF Connect or a Serial Bluetooth Terminal app and connect" },
    { "gnss", "Stream to a server or chart plotter",
      "gnss.bridge tcpc myserver.example.com 5000\n# or NMEA over UDP to OpenCPN:\ngnss.bridge udp 192.168.1.10 10110" },
    { "gnss", "Several bridges at once (up to 4): u-center, OpenCPN and a phone",
      "gnss.bridge tcp\ngnss.bridge udp 192.168.1.10 10110\ngnss.bridge ble nus\ngnss.bridge status\n"
      "# stop one by its number, or all of them\ngnss.bridge off 2\ngnss.bridge off" },
    { "gnss", "RTK / PPP corrections from an NTRIP caster (PointPerfect, RTK2go)",
      "wifi.profile.auto\ngnss.open\ngnss.bridge ntrip ppntrip.services.u-blox.com 2101 EU ${NTRIP_USER} ${NTRIP_PASS}\n"
      "gnss.bridge status\ngnss.watch 60" },
    { "gnss", "RTK rover: corrections over Wi-Fi, position to a phone over BLE",
      "wifi.profile.auto\ngnss.open\ngnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\n"
      "# connect a GNSS app (e.g. SW Maps) to the module's Nordic UART service" },
    { "gnss", "Secure remote access (TLS)",
      "utils.tls.server.create gnss 192.168.1.50\ngnss.bridge tls 2948 gnss" },
    { "gnss", "Log a drive test",
      "gnss.rate 5\ngnss.log drive.nmea nmea\n# ...drive...\ngnss.log off" },
    { "gnss", "Replay a track without a receiver (recorded, or drawn on nmeagen.org)",
      "gnss.replay drive.nmea 2\ngnss.watch 10\ngnss.bridge udp 192.168.1.10 10110" },
    { "gnss", "No receiver at hand: a virtual one at the PC's location (u-blox Malmo if unknown)",
      "gnss.virtual\ngnss.watch 5\ngnss.bridge tcp\n# or a fixed spot: gnss.virtual 55.6125 12.9925 10" },
    { "gnss", "u-center recording: log UBX, replay it, view the track in Google Earth",
      "gnss.log drive.ubx ubx\n# ...drive...\ngnss.log off\ngnss.export drive.ubx drive.kml\ngnss.replay drive.ubx 5" },
    { "gnss", "Geofence from Google My Maps (draw the area, Export to KML)",
      "gnss.fence garden.kml\ngnss.open\ngnss.fence status\n# FENCE: lines report leaving and coming back" },
    { "gnss", "Fast first fix with u-blox AssistNow (token from the Thingstream portal)",
      "wifi.profile.auto\ngnss.open\ngnss.assist register ${ASSISTNOW_TOKEN}\ngnss.assist online\ngnss.watch 30\n"
      "# later runs only need: gnss.assist online" },
    { "gnss", "Recover a stuck receiver",
      "gnss.restart cold\ngnss.status" },
    { "gnss", "PC tool through a second COM port",
      "gnss.bridge uart COM20 115200\n# open COM20's partner port in u-center" },
    { "gnss", "Send positions over cellular",
      "cell.open COM30\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.bridge cell" },
    { "gnss", "Centimetre RTK with PointPerfect over MQTT (certificates and client ID from the Thingstream thing)",
      "cert.upload client device-cert.pem pointperfect\ncert.upload key device-key.pem pointperfect-key\n"
      "cert.upload ca AmazonRootCA1.pem pointperfect-ca\nwifi.profile.auto\ngnss.open\n"
      "gnss.bridge pointperfect ${PP_CLIENT_ID} eu\ngnss.watch 120\n# the RTK line goes DGNSS -> RTK float -> RTK fixed" },
    { "gnss", "Configure the receiver and keep it",
      "gnss.cfg gnss gps,gal,bds,glo\ngnss.cfg model automotive\ngnss.cfg msg gsv 5\ngnss.cfg msg sec-sig 1\n"
      "gnss.cfg msg mon-rf 1\ngnss.cfg save" },
    { "gnss", "Jamming and spoofing watch",
      "gnss.cfg msg sec-sig 1\ngnss.cfg msg mon-rf 1\ngnss.status\ngnss.watch 600\n"
      "# JAM:, SPOOF: and ANT: lines report each change" },
    { "gnss", "Fast start after power off (save-on-shutdown)",
      "gnss.backup save\n# power the receiver off and on, then:\ngnss.open\ngnss.backup status" },

    { "imu", "First look at the sensor",
      "imu.detect\nimu.open\nimu.status" },
    { "imu", "Stream to a PC over Wi-Fi",
      "imu.rate 50\nimu.bridge tcp 9000\n# connect a TCP client to <module-ip>:9000" },
    { "imu", "Live data to a phone over BLE",
      "imu.bridge ble nus" },
    { "imu", "Record a session",
      "imu.log motion.bin\n# ...move...\nimu.log off" },

    { "wifi", "Join a network and check it (choose your country's regulatory domain once first)",
      "wifi.regdomain etsi\nwifi.scan\nwifi.connect 1 MyPassword\nwifi.status\nnet.ping u-blox.com" },
    { "wifi", "Faster repeated scans: only the channels of the last scan, full scan every 10th; 'full' clears it",
      "wifi.channels mixed\nwifi.scan\nwifi.scan\nwifi.channels full" },
    { "wifi", "Save networks and reconnect automatically",
      "wifi.profile.add home MyNetwork MyPassword\nwifi.profile.add office OfficeNet OfficePass\nwifi.profile.auto" },
    { "wifi", "Static IP",
      "wifi.ip 192.168.1.50 255.255.255.0 192.168.1.1 8.8.8.8\nwifi.connect MyNetwork MyPassword" },
    { "wifi", "Access point with a join QR code",
      "wifi.ap.enable MyAP secret123 6\nwifi.ap.status" },
    { "wifi", "Enterprise network with EAP-TLS certificates",
      "cert.upload ca ca.pem ca\ncert.upload client client.pem client\ncert.upload key client.key key\n"
      "wifi.eap CorpNet ca client key device@corp.example" },
    { "wifi", "Enterprise network with PEAP user and password",
      "cert.upload ca ca.pem ca\nwifi.peap CorpNet alice - ca" },
    { "wifi", "Watch the signal while walking around",
      "wifi.rssi 30 1" },
    { "wifi", "Is the Wi-Fi at the install spot good enough? (verdict with advice)",
      "wifi.connect MyNetwork MyPassword\nwifi.survey 30" },
    { "wifi", "Hotel or guest Wi-Fi with a login page",
      "wifi.connect HotelGuest\nwifi.portal check\nwifi.portal login room=101 surname=Smith\n"
      "# test on the PC: py -3 tests/harness/captive_portal.py --room\n"
      "# then: wifi.portal login http://192.168.1.10:8080/generate_204 room=101 surname=Smith" },
    { "wifi", "Where am I? Position from the access points around (Google, Skyhook, HERE or Combain API key)",
      "wifi.profile.auto\nwifi.locate key google ${GOOGLE_API_KEY}\nwifi.locate\n"
      "# another service once: wifi.locate combain ${COMBAIN_API_KEY}\n"
      "# see what is sent: wifi.locate body here" },

    { "net", "Is the internet reachable?",
      "net.dns u-blox.com\nnet.ping u-blox.com 4\ndev.diagnose" },
    { "net", "Correct time from NTP",
      "net.ntp server 0 pool.ntp.org\nnet.ntp on\nnet.ntp status" },
    { "net", "Throughput with iperf on the PC",
      "# PC: iperf -s\nnet.iperf client 192.168.1.10 tcp 10" },

    { "http", "Fetch a page and download a file",
      "http.get https://www.u-blox.com 200\nhttp.download https://example.com/file.bin file.bin\nfile.hash sha256 file.bin" },
    { "http", "Post form data",
      "http.post http://httpbin.org/post name=ucx&value=1" },

    { "sock", "TCP client",
      "sock.open tcp\nsock.connect example.com 80\nsock.send \"GET / HTTP/1.0\\r\\n\\r\\n\"\nsock.read\nsock.close" },
    { "sock", "TLS client with your CA",
      "cert.upload ca ca.pem myca\nsock.open tcp\nsock.tls myca\nsock.connect example.com 443" },
    { "sock", "Close every socket left open on the module", "sock.list\nsock.close all" },
    { "sock", "UDP send and receive",
      "sock.open udp\nsock.bind 5000\nsock.sendto 192.168.1.10 5000 hello\nsock.recvfrom" },
    { "sock", "TCP server",
      "sock.open tcp\nsock.listen 8000\nsock.accept 60\nsock.read" },
    { "sock", "Latency test with an echo server",
      "sock.echo udp 7 120\n# send datagrams from the PC to <module-ip>:7" },

    { "mqtt", "Publish and subscribe on a public broker",
      "mqtt.connect\nmqtt.subscribe ucx/demo/#\nmqtt.publish ucx/demo/hello \"hi from NORA\"\nmqtt.read 10" },
    { "mqtt", "Own broker",
      "mqtt.connect mybroker.local 1883\nmqtt.subscribe sensors/+/temp 1\nmqtt.read 30" },
    { "mqtt", "TLS with a server check and a login",
      "cert.upload ca broker-ca.pem broker-ca\nmqtt.connect mybroker.example.com ca=broker-ca user=device1 pass=${MQTT_PASS}\n"
      "mqtt.publish sensors/device1/temp 21.5" },
    { "mqtt", "Mutual TLS with a client certificate (AWS IoT, Azure IoT Hub)",
      "cert.upload ca root-ca.pem root-ca\ncert.upload client device.pem device\ncert.upload key device.key device-key\n"
      "mqtt.connect xxxxxx-ats.iot.eu-west-1.amazonaws.com ca=root-ca cert=device key=device-key id=device1" },

    { "cert", "Install and check certificates",
      "cert.upload ca ca.pem myca\ncert.upload client client.pem mycert\ncert.upload key client.key mykey\ncert.list\ncert.info myca" },
    { "cert", "Install a PKCS#12 (.p12/.pfx) file from the IT department",
      "utils.cert.info device.p12 MyPassword\ncert.import device.p12 device MyPassword\ncert.list" },
    { "utils", "Convert PKCS#12, PKCS#7 or DER to PEM files",
      "utils.cert.convert device.pfx device MyPassword\nutils.cert.info chain.p7b\nutils.cert.convert root.cer" },
    { "utils", "Encrypt a file with a passphrase and get it back",
      "utils.encrypt device.key device.key.enc MyPassphrase\nutils.decrypt device.key.enc device.key MyPassphrase" },

    { "bt", "Find and connect a device",
      "bt.scan\nbt.connect 1\nbt.list\nbt.disconnect" },
    { "bt", "Connect by name without a scan first (filtered scan on the name, case must match)",
      "bt.connect NORA-W36\nbt.list" },
    { "bt", "Read and set the module's own name and address",
      "bt.name\nbt.addr\nbt.name MySensor" },
#ifndef UCXAPP_LITE
    { "bt", "Scan only for NORA-W36 names or u-blox manufacturer data (text or hex)",
      "bt.filter 0 9 NORA-W36\nbt.filter 1 0xFF 0x1701\nbt.filter\nbt.scan filter\nbt.filter clear" },
    { "bt", "Channel Sounding: let a phone or tag reader measure the distance (NORA-B26/B27)",
      "bt.cs on\nbt.pairing on\n# connect from the initiator; procedure events are printed as they come\nbt.cs off" },
    { "bt", "Channel Sounding initiator: measure the distance to a reflector (NORA-B26 cs_initiator evaluation firmware)",
      "bt.cs initiator\nbt.connect reflector\nbt.bond 0\nbt.cs.range 0 30\n# smoother or raw distances\nbt.cs.filter 1 500\nbt.cs.filter off\nbt.cs.rate 10" },
#endif
    { "bt", "Save a device and reconnect later",
      "bt.profile.add sensor C0:FF:EE:00:11:22\nbt.profile.connect sensor" },
    { "bt", "Secure pairing",
      "bt.defaults secure\nbt.pairing on\nbt.connect 1\nbt.confirm yes\nbt.bonds" },

    { "gatt", "Explore a remote device",
      "bt.connect 1\ngatt.services\ngatt.read 0x0003\ngatt.subscribe 0x0010 notify\ngatt.listen 30" },
    { "gatt", "Heart rate sensor (client)",
      "bt.connect 1\ngatt.client.hrs" },
    { "gatt", "Simulate a heart rate sensor (server)",
      "gatt.server.hrs 60\n# connect with nRF Connect and enable notifications" },
    { "gatt", "Serial link with NUS",
      "bt.connect 1\ngatt.client.nus\ngatt.send nus hello" },
    { "gatt", "Provision Wi-Fi over BLE",
      "bt.connect 1\ngatt.client.wps\ngatt.wps scan\ngatt.wps config MyNetwork MyPassword WPA2\ngatt.wps connect" },
    { "gatt", "Ranging Service (Channel Sounding)",
      "gatt.server.ras 60\n# on the other module (RAS needs an encrypted link, so pair first):\nbt.connect 1\n"
      "gatt.client.ras\n# features and Data Ready indications; ranging data needs a Channel Sounding radio" },

    { "hid", "BLE keyboard",
      "hid.setup\nbt.pairing on\n# pair from the PC or phone\nhid.text Hello from NORA\nhid.key enter" },
    { "hid", "BLE mouse and media keys",
      "hid.setup\n# pair from the PC or phone\nhid.mouse 100 -50\nhid.click left double\nhid.mouse 0 0 -3\nhid.media volup\nhid.media play" },
    { "hid", "Read a BLE keyboard or mouse (HID host)",
      "bt.scan\nbt.connect 1\nhid.client 0\n# keys, mouse moves and media keys are printed\nhid.client off" },

    { "web", "Web UI from the module",
      "wifi.connect MyNetwork MyPassword\nweb.start 80 mytoken\nweb.status\n# browse to http://<module-ip>/?token=mytoken" },
    { "web", "HTTPS web UI",
      "utils.tls.server.create web 192.168.1.50\nweb.start 443 mytoken https web" },

    { "fw", "Update to the newest firmware",
      "dev.info\nfw.update\ndev.info" },
    { "fw", "Flash a given file",
      "fw.flash NORA-W36X-3.5.0.zip" },
    { "fw", "Rescue a module stuck in bootloader",
      "dev.disconnect\nfw.bootloader COM13 NORA-W36X-3.5.0.zip" },

    { "dev", "Support report",
      "dev.info\ndev.status\ndev.diagnose" },
    { "dev", "Start clean",
      "dev.factory confirm" },
    { "dev", "Save power: NORA-W36 power save, NORA-B26 light sleep",
      "dev.power save 1 1000\ndev.power\n# NORA-B26: links stay up while it sleeps\ndev.power sleep\ndev.power wake" },
    { "dev", "Deep sleep until the wake-up pin is pulled low",
      "dev.power deep\n# the module starts as after a reset" },

    { "cell", "Cellular data link",
      "cell.detect\ncell.open COM30\ncell.apn internet\ncell.attach on\ncell.status\ncell.ping u-blox.com" },
    { "cell", "Modem AT console from a PC tool over TCP",
      "cell.open COM30\ncell.bridge tcp 7000" },

    { "can", "Loopback test",
      "can.cfg 500000 loopback\ncan.send 0x123 11 22 33\ncan.recv\ncan.status" },
    { "can", "Listen on a real bus without disturbing it (STM32 board with a CAN transceiver)",
      "can.cfg 250000 listen\ncan.watch 30\ncan.status" },
    { "can", "CANable on Linux (SocketCAN, set up with: sudo ip link set can0 type can bitrate 500000 up)",
      "can.open can0\ncan.send 0x123 11 22 33\ncan.watch 10\ncan.open emulated" },
    { "can", "CAN frames to a PC over UDP (Windows, Linux or the STM32 board)",
      "can.bridge udp 192.168.1.10 6000\ncan.bridge status" },
    { "can", "CAN frames to a phone over SPS from the STM32 board",
      "can.cfg 500000 listen\ncan.bridge ble sps\ncan.bridge status" },
    { "can", "CAN frames to a server over cellular (reconnects when the link drops)",
      "cell.open COM30\ncell.attach on\ncell.connect logger.example.com 7001\ncan.bridge cell\ncan.bridge status" },

    { "hw", "Blink an LED and read a pin",
      "hw.profile.load app/boards/h753.json\nhw.led 0 on\nhw.gpio.read 3" },
    { "hw", "I2C sensor",
      "hw.i2c.scan\nhw.i2c.reg 0x76 0xD0" },
    { "hw", "Remote control over TCP",
      "hw.bridge tcp 7100\n# send 'gpio.read 3' lines from the PC" },

    { "coap", "CoAP GET",
      "coap.get coap.me 5683 test" },
    { "tftp", "TFTP server for the working folder",
      "wifi.connect MyNetwork MyPassword\ntftp.on rw\n# from a PC: curl -T fw.bin tftp://<module-ip>/\n"
      "# or: tftp -i <module-ip> GET notes.txt\ntftp.status\ntftp.off" },
    { "tftp", "TFTP client",
      "tftp.get 192.168.1.10 config.txt\ntftp.put 192.168.1.10 log.txt" },
    { "ft", "BLE file server (on one module)",
      "bt.defaults secure\nbt.pairing on\nft.on rw\nft.status\nft.off" },
    { "ft", "BLE file client (on the other module)",
      "bt.scan\nbt.connect 1\nbt.confirm yes\nft.list 0\nft.get 0 notes.txt\nft.put 0 log.txt\nft.remove 0 log.txt" },
    { "ft", "File server over Wi-Fi (TCP, faster; an alternative to TFTP with CRC-32 checks)",
      "wifi.connect\nft.on rw tcp\n# on the other module: ft.list 192.168.1.10\n# ft.get 192.168.1.10 notes.txt\n# ft.put 192.168.1.10:6970 log.txt\nft.off" },
    { "ft", "Continue an interrupted transfer",
      "ft.get 192.168.1.10 big.bin big.bin 1455 resume\nft.put 192.168.1.10 big.bin big.bin 1455 resume" },
    { "ancs", "iPhone notifications (pair 'ucx ANCS' from the phone's Bluetooth settings)",
      "ancs.on\nbt.list\nancs.sub 0\nancs.list\nancs.show 42\nancs.action 42 neg\nancs.off" },
    { "notify", "Android notifications from a companion app",
      "bt.defaults secure\nbt.pairing on\nnotify.on\n# the app writes 17|Messages|Anna|See you at 5\nnotify.list\nnotify.action 17 dismiss" },
    { "session", "Record a session for support, replay it elsewhere",
      "session.record issue.ucx\nwifi.connect\nnet.ping 8.8.8.8\nsession.stop\nsession.replay issue.ucx stop" },
    { "cfg", "Back up a module's settings and compare later",
      "cfg.show\ncfg.save golden.cfg\n# ... later, or on another module\ncfg.diff golden.cfg" },
    { "cfg", "Clone one module's settings to another",
      "cfg.save golden.cfg\n# connect to the second module, then\ncfg.load golden.cfg\ncfg.diff golden.cfg" },
    { "cfg", "Compare two saved files", "cfg.diff golden.cfg field.cfg" },
    { "at", "Record the AT traffic, then read it decoded",
      "at.trace boot.trace\nwifi.connect\nat.trace.stop\nat.trace.show boot.trace" },
    { "at", "Decode a URC or response", "at.decode +UEWSNU\nat.decode +UEWLD:0,3" },
    { "at", "Replay a recorded trace and compare the replies", "at.replay boot.trace" },
    { "at", "Open a trace in Wireshark (next to a BLE or Wi-Fi sniffer capture)",
      "at.trace.pcap boot.trace\n# or record straight to pcapng:\nat.trace boot.pcapng\nwifi.connect\nat.trace.stop" },
    { "perf", "Round-trip time and throughput through the u-blox echo server (TCP and UDP)",
      "wifi.connect MyNetwork MyPassword\nperf.latency tcp echo.u-blox.com 7 20\nperf.throughput tcp echo.u-blox.com 7 10 512\n"
      "perf.latency udp echo.u-blox.com 7 20\nperf.throughput udp echo.u-blox.com 7 10 512" },
    { "perf", "Module to module over Wi-Fi (B echoes, A measures)",
      "# on module B\nperf.echo udp 7000 120\n# on module A, with B's address\nperf.latency udp 192.168.1.20 7000 50\nperf.throughput udp 192.168.1.20 7000 10 1000" },
    { "perf", "Bluetooth SPS between two modules (B echoes, A measures)",
      "# on module B\nperf.echo sps 120\n# on module A\nbt.connect <address-of-B>\nperf.latency sps 20\nperf.throughput sps 10 244" },
#if UCXAPP_XPLORER
    { "perf", "The u-connectXplorer phone as the other end (Wi-Fi echo and Bluetooth SPS)",
      "# phone: u-connectXplorer > Settings > Remote control on, note address and token\n"
      "xplorer.connect 192.168.1.200 <token>\nwifi.profile.auto\nperf.latency udp xplorer 7000\n"
      "perf.throughput tcp xplorer 7001 10\nperf.phone tx 10\nperf.phone rx 10\nperf.phone echo 10" },
    { "xplorer", "Look at and drive the phone app",
      "xplorer.connect 192.168.1.200 <token>\nxplorer.info\nxplorer.call ble.getState\n"
      "xplorer.call ble.startScan\nxplorer.call ble.getDevices\nxplorer.call ble.stopScan\n"
      "xplorer.echo udp 7000\nxplorer.echo state\nxplorer.echo stop" },
#endif
    { "prod", "Production test with a report per serial number",
      "prod.test\nprod.test examples/automation/production-test.ucx reports" },
    { "app", "Keep the clock in sync and set the module",
      "wifi.connect MyNetwork MyPassword\napp.time sync\napp.time\napp.time push" },
    { "dtls", "Secure UDP echo",
      "dtls.client 192.168.1.10 4433 hello\ndtls.server 4433" },

    { "app", "Capture a session for a bug report",
      "app.log.level all\napp.log.save session.txt\n# ...reproduce...\napp.log.stop" },
    { "file", "Look at local files",
      "file.list\nfile.view session.txt 20\nfile.hash sha256 session.txt" },
    { "file", "Copy files to and from an STM32 board over its console (YMODEM)",
      "file.recv\n# ...start the YMODEM upload in Tera Term, or: ucxapp --stm32 --put fw.bin\nfile.hash sha256 fw.bin\n"
      "file.send fw.bin\n# ...start the YMODEM download, or: ucxapp --stm32 --get fw.bin" },
    { "usecase", "Fit check, then with a measured rate",
      "usecase.tool\nusecase.tool 2.5" },
    { "usecase", "Join the factory network with a user name (PEAP)",
      "cert.list\nwifi.peap FactoryNet tool17 Secret123 ca.pem\nwifi.status" },
    { "usecase", "Join the factory network with certificates (EAP-TLS)",
      "cert.list\nwifi.eap FactoryNet ca.pem client.pem client.key tool17\nwifi.status" },
    { "usecase", "Access point for the laptops, next to the station",
      "wifi.ap.enable Tool17 Tool17Pass\nwifi.ap.status" },
    { "usecase", "HTTPS monitoring page for the laptops",
      "# web.start 443 <token> https  (from the shell or --exec, not from this page)\nweb.status" },
    { "usecase", "Throughput for the history download",
      "net.iperf server tcp\n# on a laptop: iperf -c <tool-ip> -t 10\n# iperf measures the radio; the download also crosses the UART\nnet.iperf stop" },
    { "usecase", "Socket trays: one UDP socket for all of them",
      "sock.open udp\nsock.bind 5000\nsock.recvfrom" },
    { "usecase", "BLE: set up the tool from a phone",
      "bt.defaults secure\nbt.pairing on\ngatt.server.wps 120" },
    { "usecase", "BLE: service link for a technician",
      "gatt.server.sps 60" },
    { "usecase", "BLE: accessory such as a battery pack or scanner",
      "bt.scan\nbt.connect 1\ngatt.client.bas" },
#ifndef UCXAPP_LITE
    /* Industry and edge AI use cases (usecase.check <name>); run with examples uc-<name> <number>. */
    { "uc-iiot", "Join the plant network with certificates (EAP-TLS)",
      "cert.list\nwifi.eap PlantNet ca.pem client.pem client.key machine17\nwifi.status" },
    { "uc-iiot", "TLS connection to the MES",
      "# once: cert.upload ca ca.pem mesca\nsock.open tcp\nsock.tls mesca\nsock.connect mes.example.com 8883\nsock.list" },
    { "uc-iiot", "Machine data from the CAN bus to the MES",
      "can.cfg 500000\ncan.bridge tcpc mes.example.com 9100\ncan.status" },
    { "uc-iiot", "Same settings on every unit of the line",
      "# on the golden unit: cfg.save line.cfg\ncfg.diff line.cfg\n# when it differs: cfg.load line.cfg" },
    { "uc-iiot", "Health report and firmware",
      "dev.diagnose mes.example.com\nfw.download\n# in the service window: fw.update" },
    { "uc-medical", "Record the module as a SOUP item",
      "dev.info\ndev.caps" },
    { "uc-medical", "Secure link to the phone app",
      "bt.defaults secure\nbt.pairing on\n# pair from the phone, confirm the number\nbt.bonds" },
    { "uc-medical", "Readings over the Serial Port service, with a trace as evidence",
      "at.trace evidence.txt\ngatt.server.sps 60\nat.trace.stop\nat.trace.pcap evidence.txt" },
    { "uc-medical", "Check the released settings",
      "# at release: cfg.save release.cfg\ncfg.diff release.cfg\n"
      "# the whole check as a script with a report: ucxapp COM13 --script verify.ucx --junit verify.xml" },
    { "uc-site", "Name the tool and allow only paired phones",
      "bt.name ToolCo-1234\nbt.defaults secure\nbt.pairing on" },
    { "uc-site", "Service link for the technician's phone",
      "gatt.server.sps 120" },
    { "uc-site", "Gateway: find the fleet by name",
      "bt.filter 0 9 ToolCo\nbt.scan filter\nbt.filter clear" },
    { "uc-site", "Site office and depot Wi-Fi",
      "wifi.profile.add site SiteOffice SitePass123\nwifi.profile.add depot Depot DepotPass123\nwifi.profile.list" },
    { "uc-site", "At the depot: check and update",
      "dev.diagnose\nfw.download\n# overnight: fw.update" },
    { "uc-implement", "ISOBUS at 250 kbit/s",
      "can.cfg 250000\ncan.status\ncan.recv" },
    { "uc-implement", "Cab Wi-Fi and machine data to the tablet",
      "wifi.ap.enable Implement Implement123\ncan.bridge tcp 9100\n# the tablet app connects to <ap-ip>:9100\ncan.bridge status" },
    { "uc-implement", "Machine data to the operator's phone over BLE",
      "can.bridge ble nus\ncan.bridge status" },
    { "uc-implement", "Back at the yard: the work record to the cloud",
      "can.bridge off\nwifi.ap.disable\nwifi.connect FarmYard MyPassword\n"
      "mqtt.connect broker.example.com 1883\nmqtt.publish farm/implement/7 hectares=12.4" },
    { "uc-edge", "Record training data from the IMU",
      "imu.detect\nimu.open\nimu.rate 100\nimu.log motion.bin\n# ...move the sensor...\nimu.log off" },
    { "uc-edge", "Live stream to a notebook",
      "imu.bridge tcp 9000\n# in Python: socket.create_connection((\"<module-ip>\", 9000))\nimu.bridge status" },
    { "uc-edge", "I2C sensors on the host",
      "hw.i2c.scan\nhw.i2c.reg 0x76 0xD0" },
    { "uc-edge", "The model's result to the cloud",
      "mqtt.connect broker.example.com 1883\nmqtt.publish edge/sensor1/state walking" },
    /* Everyday use cases (usecase.check <name>); run with examples uc-<name> <number>. */
    { "uc-boat", "Receiver check and fix",
      "gnss.open\ngnss.watch 10" },
    { "uc-boat", "Boat Wi-Fi for the tablets",
      "wifi.ap.enable BoatNet BoatPass123\nwifi.ap.status" },
    { "uc-boat", "NMEA to the chart app over TCP 10110",
      "gnss.bridge tcp 10110\n# in OpenCPN or Navionics: add a TCP connection to <ap-ip>:10110\ngnss.bridge status" },
    { "uc-boat", "NMEA to a phone over BLE as well",
      "gnss.bridge ble nus\n# connect a GNSS app to the Nordic UART service\ngnss.bridge status\n"
      "# bridge 1 is the TCP server, bridge 2 the BLE link; gnss.bridge off 2 stops only the phone" },
    { "uc-boat", "Log the trip",
      "gnss.log trip.nmea nmea\n# ...sail...\ngnss.log off\n# at home: gnss.close, then gnss.replay trip.nmea 10" },
    { "uc-rtk", "Home Wi-Fi and the receiver",
      "wifi.connect MyNetwork MyPassword\ngnss.open\ngnss.status" },
    { "uc-rtk", "Corrections from a caster, position to the robot over BLE",
      "gnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\ngnss.bridge status" },
    { "uc-rtk", "Watch for an RTK fixed solution",
      "gnss.watch 30" },
    { "uc-mower", "First setup from the owner's phone over BLE",
      "bt.defaults secure\nbt.pairing on\ngatt.server.wps 120\nwifi.status" },
    { "uc-mower", "PointPerfect over the garden Wi-Fi, position to the phone over BLE",
      "gnss.open\ngnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble\n"
      "gnss.bridge status\ngnss.status" },
    { "uc-mower", "Edge of the garden: the same corrections over cellular",
      "gnss.bridge off\ncell.open\ncell.attach on\n"
      "gnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble cell\n"
      "gnss.watch 3\ngnss.bridge status" },
    { "uc-mower", "Theft alert: position to your server over cellular",
      "gnss.bridge off\ncell.disconnect\ncell.connect myserver.example.com 5000\ngnss.bridge cell\ngnss.bridge status" },
    { "uc-mower", "Corrections over Wi-Fi and the theft link over cellular together",
      "gnss.bridge off\ncell.disconnect\n"
      "gnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble\n"
      "cell.connect myserver.example.com 5000\ngnss.bridge cell\ngnss.bridge status" },
    { "uc-farm", "HP receiver and the cellular modem in the field",
      "gnss.open\ngnss.status\ncell.open\ncell.attach on" },
    { "uc-farm", "PointPerfect over cellular, corrected NMEA to the cab tablet over BLE",
      "gnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble cell\n"
      "gnss.watch 3\ngnss.bridge status" },
    { "uc-farm", "Log the field",
      "gnss.log field.nmea nmea\n# ...drive the field...\ngnss.log off" },
    { "uc-farm", "Back at the farmyard: Wi-Fi, then upload the log",
      "gnss.bridge off\ncell.disconnect\nwifi.connect FarmYard MyPassword\n"
      "# tftp.put <pc> field.nmea, or ft.put <pc> field.nmea to a PC running ft.on rw" },
    { "uc-tracker", "Position to your server over Wi-Fi",
      "wifi.connect MyNetwork MyPassword\ngnss.open\ngnss.bridge tcpc myserver.example.com 5000\ngnss.bridge status" },
    { "uc-tracker", "Position over cellular on the road",
      "gnss.bridge off\ncell.open\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.bridge cell" },
    { "uc-home", "Join the home network",
      "wifi.connect MyNetwork MyPassword\nwifi.profile.add home MyNetwork MyPassword" },
    { "uc-home", "Publish a state to Home Assistant",
      "mqtt.connect 192.168.1.20 1883\nmqtt.publish home/garage/door open" },
    { "uc-home", "Take commands from Home Assistant",
      "mqtt.subscribe home/garage/cmd\nmqtt.read 30" },
    { "uc-provision", "Wait for a phone to send the Wi-Fi settings",
      "bt.defaults secure\nbt.pairing on\ngatt.server.wps 120\nwifi.status" },
    { "uc-provision", "Or: own access point with a join QR code",
      "wifi.ap.enable Setup-1234 setup1234\nwifi.ap.status" },
    { "uc-hotel", "Find and join the guest network",
      "wifi.scan\nwifi.connect HotelGuest" },
    { "uc-hotel", "Log in on the portal page",
      "wifi.portal login room=412 surname=Smith\nwifi.portal check\nnet.ping 8.8.8.8" },
    { "uc-notify", "iPhone notifications",
      "ancs.on\n# pair 'ucx ANCS' from the phone's Bluetooth settings\nancs.sub 0\nancs.list" },
    { "uc-notify", "Read and answer one",
      "ancs.show 42\nancs.action 42 pos" },
    { "uc-notify", "Android notifications from a companion app",
      "bt.defaults secure\nbt.pairing on\nnotify.on\nnotify.list" },
    { "uc-remote", "Pair as a keyboard",
      "hid.setup\nbt.pairing on\n# pair from the PC or phone\nhid.status" },
    { "uc-remote", "Presentation clicker",
      "hid.key space\nhid.key backspace" },
    { "uc-remote", "Media remote",
      "hid.media volup\nhid.media play\nhid.text hello" },
#endif
    { "utils", "Handy tools",
      "utils.qr.wifi MyNetwork MyPassword\nutils.zip logs.zip session.txt\nutils.hash.text sha256 hello" },
};

#define EX_COUNT (sizeof(kExamples) / sizeof(kExamples[0]))
#define GROUP_MAX 40

bool ucxapp_example_get(size_t index, const char **group, const char **title, const char **lines)
{
    if (index >= EX_COUNT) {
        return false;
    }
    *group = kExamples[index].group;
    *title = kExamples[index].title;
    *lines = kExamples[index].lines;
    return true;
}

static ucxapp_cmd_t gCmds[GROUP_MAX + 1];
static char gNames[GROUP_MAX][24];

static bool hasGroup(const char *group);

/* "gnss.example 2" for a command group, "examples uc-boat 2" for a use case. */
static void runHint(const char *group, char *out, size_t size)
{
    snprintf(out, size, hasGroup(group) ? "%s.example" : "examples %s", group);
}

static void printGroup(const char *group)
{
    int n = 0;

    ucxapp_print("Examples for %s%s\n", group, hasGroup(group) ? ".*" : "");
    for (size_t i = 0; i < EX_COUNT; i++) {
        const char *p = kExamples[i].lines;

        if (strcmp(kExamples[i].group, group) != 0) {
            continue;
        }
        ucxapp_print("\n %d. %s\n", ++n, kExamples[i].title);
        while (*p != '\0') {
            const char *e = strchr(p, '\n');
            int len = e != NULL ? (int)(e - p) : (int)strlen(p);

            ucxapp_print("    %s%.*s\n", p[0] == '#' ? "" : "> ", len, p);
            p += len + (e != NULL);
        }
    }
    if (n > 0) {
        char hint[32];

        runHint(group, hint, sizeof(hint));
        ucxapp_print("\nRun one with %s <number> (stops at the first error)\n", hint);
    }
}

/* Runs example number n of a group line by line; stops at the first error or at a dangerous command. */
static int runExample(ucxapp_t *app, const char *group, int n)
{
    int k = 0;

    for (size_t i = 0; i < EX_COUNT; i++) {
        const char *p = kExamples[i].lines;

        if (strcmp(kExamples[i].group, group) != 0 || ++k != n) {
            continue;
        }
        ucxapp_print("Running %s example %d: %s\n", group, n, kExamples[i].title);
        while (*p != '\0') {
            const char *e = strchr(p, '\n');
            int len = e != NULL ? (int)(e - p) : (int)strlen(p);
            char line[256], word[48];
            const ucxapp_cmd_t *c;

            snprintf(line, sizeof(line), "%.*s", len, p);
            p += len + (e != NULL);
            if (line[0] == '#' || line[0] == '\0') {
                continue;
            }
            snprintf(word, sizeof(word), "%.*s", (int)strcspn(line, " "), line);
            c = ucxapp_cmd_find(word);
            if (c != NULL && (c->flags & UCXAPP_CMD_DANGER)) {
                ucxapp_print("stopped before '%s': it changes or deletes data, run it by hand\n", line);
                return UCXAPP_ERR_FAILED;
            }
            ucxapp_print("> %s\n", line);
            int rc = ucxapp_cmd_exec(app, line);
            if (rc != UCXAPP_OK) {
                ucxapp_print("stopped: '%s' failed (%d); edit the placeholders and run the rest by hand\n", line, rc);
                return rc;
            }
        }
        return UCXAPP_OK;
    }
    char hint[32];

    runHint(group, hint, sizeof(hint));
    ucxapp_print("no example %d for %s (see %s)\n", n, group, hint);
    return UCXAPP_ERR_USAGE;
}

static int cmdExamples(ucxapp_t *app, int argc, char *argv[])
{
    char group[16];
    const char *dot = strchr(argv[0], '.');
    int32_t num = 0;

    if (dot == NULL) {
        if (argc > 3 || (argc == 3 && !ucxapp_parse_int(argv[2], &num))) {
            return UCXAPP_ERR_USAGE;
        }
        if (argc >= 2) {
            snprintf(group, sizeof(group), "%s", argv[1]);
            if (num > 0) {
                return runExample(app, group, (int)num);
            }
            printGroup(group);
            return UCXAPP_OK;
        }
        ucxapp_print("Groups with examples (type <group>.example):\n ");
        for (size_t i = 0; gCmds[i].name != NULL; i++) {
            if (strchr(gCmds[i].name, '.') != NULL) {
                ucxapp_print(" %.*s", (int)(strchr(gCmds[i].name, '.') - gCmds[i].name), gCmds[i].name);
            }
        }
        ucxapp_print("\n");
        return UCXAPP_OK;
    }
    if (argc > 2 || (argc == 2 && !ucxapp_parse_int(argv[1], &num))) {
        return UCXAPP_ERR_USAGE;
    }
    snprintf(group, sizeof(group), "%.*s", (int)(dot - argv[0]), argv[0]);
    if (num > 0) {
        return runExample(app, group, (int)num);
    }
    printGroup(group);
    return UCXAPP_OK;
}

static bool hasGroup(const char *group)
{
    char prefix[24];
    char out[48];

    snprintf(prefix, sizeof(prefix), "%s.", group);
    return ucxapp_cmd_complete(prefix, out, sizeof(out)) > 0;
}

/* Register after every other pack so only groups that exist in this build get examples. */
void ucxapp_register_examples_cmds(void)
{
    size_t n = 0;

    for (size_t i = 0; i < EX_COUNT && n < GROUP_MAX; i++) {
        const char *g = kExamples[i].group;
        bool seen = false;

        for (size_t k = 0; k < i; k++) {
            seen = seen || strcmp(kExamples[k].group, g) == 0;
        }
        if (seen || !hasGroup(g)) {
            continue;
        }
        snprintf(gNames[n], sizeof(gNames[n]), "%s.example", g);
        gCmds[n] = (ucxapp_cmd_t){ gNames[n], "[number]", "Real-world scenarios with ready-to-type commands; with a number, run that example", 0, cmdExamples };
        n++;
    }
    gCmds[n] = (ucxapp_cmd_t){ "examples", "[group] [number]", "List the groups with examples, show one group's examples, or run one", 0, cmdExamples };
    (void)ucxapp_cmd_register(gCmds, n + 1);
}
