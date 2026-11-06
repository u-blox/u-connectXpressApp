#include "ucxapp_cmd.h"
#include "ucxapp_demo.h"
#include "ucxapp_xplorer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real scenarios per command group, shown by "<group>.example" or "examples <group>". Lines starting with '#' are notes. */
typedef struct {
    const char *group;
    const char *title;
    const char *lines;
} example_t;

static const example_t kExamples[] = {
#if UCXAPP_HOST   /* peripherals and tools of the PC builds */
    { "bridge", "Sensor hub: GNSS to the PC over Wi-Fi, IMU to a phone over BLE",
      "wifi.profile.auto\ngnss.open\nimu.open\ngnss.bridge tcp\nimu.bridge ble nus\nbridge.list" },
    { "bridge", "Asset tracker: GNSS over cellular, CAN over Wi-Fi",
      "cell.open\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.open\ngnss.bridge cell\n"
      "can.bridge udp 192.168.1.10 6000\nbridge.list" },
    { "bridge", "RTK rover: NTRIP in over Wi-Fi, position out over BLE",
      "wifi.profile.auto\ngnss.open\ngnss.bridge off\ngnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\nbridge.list" },
    { "bridge", "Remote modem console over Wi-Fi",
      "wifi.profile.auto\ncell.open\ncell.bridge tcp 7000\n# telnet <module-ip> 7000 and type AT commands" },
    { "bridge", "Stop everything",
      "bridge.list\nbridge.off" },

    { "gnss", "First fix check",
      "gnss.open\ngnss.status\ngnss.sats" },
    { "gnss", "Watch the fix come in (time to first fix)",
      "gnss.open\ngnss.restart cold\ngnss.watch 60" },
    { "gnss", "u-center or gpsd on the PC over Wi-Fi",
      "wifi.connect MyNetwork MyPassword\ngnss.open\ngnss.bridge tcp\n# connect u-center / gpsd to <module-ip>:2947" },
    { "gnss", "Phone app over BLE",
      "gnss.open\ngnss.bridge ble nus\n# open nRF Connect or a Serial Bluetooth Terminal app and connect" },
    { "gnss", "Stream to a server or chart plotter",
      "gnss.open\ngnss.bridge tcpc myserver.example.com 5000\n# or NMEA over UDP to OpenCPN:\ngnss.bridge udp 192.168.1.10 10110" },
    { "gnss", "Several bridges at once (up to 4): u-center, OpenCPN and a phone",
      "gnss.open\ngnss.bridge tcp\ngnss.bridge udp 192.168.1.10 10110\ngnss.bridge ble nus\ngnss.bridge status\n"
      "# stop one by its number, or all of them\ngnss.bridge off 2\ngnss.bridge off" },
    { "gnss", "RTK / PPP corrections from an NTRIP caster (PointPerfect, RTK2go)",
      "wifi.profile.auto\ngnss.open\ngnss.bridge off\ngnss.bridge ntrip ppntrip.services.u-blox.com 2101 EU ${NTRIP_USER} ${NTRIP_PASS}\n"
      "gnss.bridge status\ngnss.watch 60" },
    { "gnss", "RTK rover: corrections over Wi-Fi, position to a phone over BLE",
      "wifi.profile.auto\ngnss.open\ngnss.bridge off\ngnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\n"
      "# connect a GNSS app (e.g. SW Maps) to the module's Nordic UART service" },
    { "gnss", "Secure remote access (TLS)",
      "# once: utils.tls.ca.create\nutils.tls.server.create gnss 192.168.1.50\ngnss.open\ngnss.bridge tls 2948 gnss" },
    { "gnss", "Log a drive test",
      "gnss.open\ngnss.rate 5\ngnss.log drive.nmea nmea\n# ...drive...\ngnss.watch 10\ngnss.log off" },
    { "gnss", "Replay a track without a receiver (recorded, or drawn on nmeagen.org)",
      "gnss.close\ngnss.replay drive.nmea 2\ngnss.watch 10\ngnss.bridge udp 192.168.1.10 10110" },
    { "gnss", "No receiver at hand: a virtual one at the PC's location (u-blox Malmo if unknown)",
      "gnss.close\ngnss.virtual\ngnss.watch 5\ngnss.bridge tcp\n# or a fixed spot: gnss.virtual 55.61276 12.98625 10" },
    { "gnss", "u-center recording: log UBX, replay it, view the track in Google Earth",
      "gnss.open\ngnss.log drive.ubx ubx\n# ...drive...\ngnss.watch 5\ngnss.log off\ngnss.export drive.ubx drive.kml\ngnss.close\ngnss.replay drive.ubx 5" },
    { "gnss", "Geofence from Google My Maps (draw the area, Export to KML)",
      "gnss.fence garden.kml\ngnss.open\ngnss.fence status\n# FENCE: lines report leaving and coming back" },
    { "gnss", "Fast first fix with u-blox AssistNow (token from the Thingstream portal)",
      "wifi.profile.auto\ngnss.open\ngnss.assist register ${ASSISTNOW_TOKEN}\ngnss.assist online\ngnss.watch 30\n"
      "# later runs only need: gnss.assist online" },
    { "gnss", "Recover a stuck receiver",
      "gnss.open\ngnss.restart cold\ngnss.status" },
    { "gnss", "PC tool through a second COM port",
      "gnss.open\ngnss.bridge uart COM20 115200\n# open COM20's partner port in u-center" },
    { "gnss", "Send positions over cellular",
      "cell.open\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.open\ngnss.bridge cell" },
    { "gnss", "Centimetre RTK with PointPerfect over MQTT (certificates and client ID from the Thingstream thing)",
      "cert.upload client device-cert.pem pointperfect\ncert.upload key device-key.pem pointperfect-key\n"
      "cert.upload ca AmazonRootCA1.pem pointperfect-ca\nwifi.profile.auto\ngnss.open\n"
      "gnss.bridge pointperfect ${PP_CLIENT_ID} eu\ngnss.watch 120\n# the RTK line goes DGNSS -> RTK float -> RTK fixed" },
    { "gnss", "Configure the receiver and keep it",
      "gnss.open\ngnss.cfg gnss gps,gal,bds,glo\ngnss.cfg model automotive\ngnss.cfg msg gsv 5\ngnss.cfg msg sec-sig 1\n"
      "gnss.cfg msg mon-rf 1\ngnss.cfg save" },
    { "gnss", "Jamming and spoofing watch",
      "gnss.open\ngnss.cfg msg sec-sig 1\ngnss.cfg msg mon-rf 1\ngnss.status\ngnss.watch 120\n"
      "# JAM:, SPOOF: and ANT: lines report each change" },
    { "gnss", "Fast start after power off (save-on-shutdown)",
      "gnss.open\ngnss.backup save\n# power the receiver off and on, then:\ngnss.open\ngnss.backup status" },

    { "imu", "First look at the sensor",
      "imu.detect\nimu.open\nimu.status" },
    { "imu", "Stream to a PC over Wi-Fi",
      "imu.open\nimu.rate 50\nimu.bridge tcp 9000\n# connect a TCP client to <module-ip>:9000" },
    { "imu", "Live data to a phone over BLE",
      "imu.open\nimu.bridge ble nus" },
    { "imu", "Record a session",
      "imu.open\nimu.log motion.bin\n# ...move...\nimu.log off" },
#endif

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
      "cert.upload ca ca.pem ca\nwifi.peap CorpNet alice ${PEAP_PASSWORD} ca\n# or - instead of the password to type it" },
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
    { "sock", "Listen to mDNS queries on the local link (multicast group)",
      "sock.open udp\nsock.bind 5353\nsock.mcast join 224.0.0.251\nsock.recvfrom 512 30\nsock.mcast leave 224.0.0.251" },

    { "mdns", "Be found as a name on the LAN: ping nora.local from any PC",
      "wifi.connect MySsid MyPassword\nmdns.on nora\nmdns.status" },
    { "mdns", "Advertise the web UI as a DNS-SD service with TXT records",
      "web.stop\nweb.start 80\nmdns.on nora\nmdns.add _http._tcp 80 \"NORA-W36 dashboard\" path=/ fw=3.5.0\n"
      "# on a PC: dns-sd -B _http._tcp  or  avahi-browse -t _http._tcp" },
    { "mdns", "Matter commissionable node advertisement (what a Matter controller browses for)",
      "mdns.on\nmdns.add _matterc._udp 5540 A1B2C3D4E5F60718 VP=65521+32769 D=3840 CM=1 DT=257 DN=Light\n"
      "mdns.status\n# change a record later, then stop advertising\nmdns.txt 0 CM 0\nmdns.remove all" },

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

    { "thread", "Start a new Thread network on this module (it becomes the leader)",
      "thread.network new\nthread.start\nthread.status\nthread.network" },
    { "thread", "Join a border router's network with its dataset",
      "# on the border router: ot-ctl dataset active -x\n"
      "thread.dataset 0e080000000000010000000300000f010212340208dead00beef00cafe051000112233445566778899aabbccddeeff"
      "0309767563782d6d657368\n"
      "thread.start\nthread.addr" },
    { "thread", "Join through a commissioner with a PSKd",
      "# on the commissioner: ot-ctl commissioner start; ot-ctl commissioner joiner add * J01NME\n"
      "thread.join J01NME\nthread.start" },
    { "thread", "Sleepy end device and link health",
      "thread.type sleepy 2000\nthread.start\nthread.neighbors\nthread.counters" },
    { "thread", "Publish a service over SRP and find it with DNS-SD",
      "thread.srp host sensor1\nthread.srp add _coap._udp 5683 sensor1\nthread.srp on\nthread.srp\n"
      "thread.dns browse _coap._udp" },
    { "thread", "Ping the leader, then the internet through the border router (NAT64)",
      "thread.ping leader\nthread.ping www.google.com\nthread.ping 8.8.8.8" },

    { "cert", "Install and check certificates",
      "cert.upload ca ca.pem myca\ncert.upload client client.pem mycert\ncert.upload key client.key mykey\ncert.list\ncert.info myca" },
#if UCXAPP_HOST
    { "cert", "Install a PKCS#12 (.p12/.pfx) file from the IT department",
      "utils.cert.info device.p12 MyPassword\ncert.import device.p12 device MyPassword\ncert.list" },
    { "utils", "Convert PKCS#12, PKCS#7 or DER to PEM files",
      "utils.cert.convert device.pfx device MyPassword\nutils.cert.info chain.p7b\nutils.cert.convert root.cer" },
    { "utils", "Encrypt a file with a passphrase and get it back",
      "utils.encrypt device.key device.key.enc MyPassphrase\nutils.decrypt device.key.enc device.key MyPassphrase" },
#endif

    { "bt", "Find and connect a device",
      "bt.scan\nbt.connect 1\nbt.list\nbt.disconnect" },
    { "bt", "Connect by name without a scan first (filtered scan on the name, case must match)",
      "bt.connect NORA-W36\nbt.list" },
    { "bt", "Read and set the module's own name and address",
      "bt.name\nbt.addr\nbt.name MySensor" },
    { "bt", "Custom advertising: a Matter commissionable (CHIPoBLE) service data record, then a broadcast-only beacon",
      "bt.adv\nbt.adv 0B16F6FF000F001234567800\nbt.adv on\n# non-connectable: flags + complete name\n"
      "bt.adv 02010605094E4F5241 broadcast\nbt.adv interval 100 200\nbt.adv off" },
#ifndef UCXAPP_LITE
    { "bt", "Scan only for NORA-W36 names or u-blox manufacturer data (text or hex)",
      "bt.filter 0 9 NORA-W36\nbt.filter 1 0xFF 0x1701\nbt.filter\nbt.scan filter\nbt.filter clear" },
    { "bt", "Channel Sounding: let a phone or tag reader measure the distance (NORA-B26/B27)",
      "bt.cs on\nbt.pairing on\n# connect from the initiator; procedure events are printed as they come\nbt.cs off" },
    { "bt", "Channel Sounding initiator: measure the distance to a reflector (NORA-B26 cs_initiator evaluation firmware)",
      "bt.cs initiator\nbt.connect reflector\nbt.bond 0\nbt.cs.range 0 30\n# smoother or raw distances\nbt.cs.filter 1 500\nbt.cs.filter off\nbt.cs.rate 10" },
#endif
    { "bt", "Save a device and reconnect later",
      "bt.profile.add sensor C0:FF:EE:00:11:22\nbt.profile.connect sensor\nbt.list\nbt.disconnect" },
    { "bt", "Secure pairing: check the code yourself",
      "bt.defaults secure\nbt.pairing on\nbt.autoconfirm off\nbt.connect 1\n# both sides show the same code\n"
      "bt.confirm yes\nbt.bonds\nbt.autoconfirm on" },

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

#if UCXAPP_HOST
    { "hid", "BLE keyboard",
      "hid.setup\nbt.pairing on\n# pair from the PC or phone\nhid.text Hello from NORA\nhid.key enter" },
    { "hid", "BLE mouse and media keys",
      "hid.setup\n# pair from the PC or phone\nhid.mouse 100 -50\nhid.click left double\nhid.mouse 0 0 -3\nhid.media volup\nhid.media play" },
    { "hid", "Read a BLE keyboard or mouse (HID host)",
      "bt.scan\nbt.connect 1\nhid.client 0\n# keys, mouse moves and media keys are printed\nhid.client off" },

    { "web", "Web UI from the module",
      "wifi.connect MyNetwork MyPassword\n# a running server keeps its settings\nweb.stop\nweb.start 80 mytoken\nweb.status\n"
      "# browse to http://<module-ip>/?token=mytoken" },
    { "web", "HTTPS web UI",
      "utils.tls.server.create web 192.168.1.50\n# a running server keeps its settings\nweb.stop\nweb.start 443 mytoken https web" },

    { "fw", "Update to the newest firmware",
      "dev.info\nfw.update\ndev.info" },
    { "fw", "Update to the newest firmware, evaluation builds included",
      "dev.info\nfw.update eval\ndev.info" },
    { "fw", "List the firmware releases on GitHub for the connected module",
      "fw.github.list" },
    { "fw", "List the firmware already downloaded to the store",
      "fw.list" },
    { "fw", "Flash a given file",
      "fw.flash NORA-W36X-3.5.0.zip" },
    { "fw", "Rescue a module stuck in bootloader",
      "dev.disconnect\nfw.bootloader COM13 NORA-W36X-3.5.0.zip" },
#endif

    { "dev", "Support report",
      "dev.info\ndev.status\ndev.diagnose" },
    { "dev", "Start clean",
      "dev.factory confirm" },
    { "dev", "Save power: NORA-W36 power save, NORA-B26 light sleep",
      "dev.power save 1 1000\ndev.power\n# NORA-B26: links stay up while it sleeps\ndev.power sleep\ndev.power wake" },
    { "dev", "Deep sleep until the wake-up pin is pulled low",
      "dev.power deep\n# the module starts as after a reset" },

#if UCXAPP_HOST
    { "cell", "Cellular data link",
      "cell.detect\ncell.open\ncell.apn internet\ncell.attach on\ncell.status\ncell.ping u-blox.com" },
    { "cell", "Check the modem, SIM and signal without AT commands",
      "cell.open\ncell.info\ncell.signal\ncell.operator\ncell.radio" },
    { "cell", "Live card: network, signal, the data link and who uses it",
      "cell.open\ncell.attach on\ncell.watch 5\ncell.connect myserver.example.com 5000\ngnss.bridge cell\ncell.watch 5\n"
      "# the web page shows the same card under What's happening" },
    { "cell", "u-blox: LTE-M first, then NB-IoT, global profile, then a network in range",
      "cell.open\ncell.profile global\ncell.rat ltem nbiot\ncell.operator scan\ncell.operator auto\ncell.attach on" },
    { "cell", "Modem AT console from a PC tool over TCP",
      "cell.open\ncell.bridge tcp 7000" },
    { "cell", "Wi-Fi to cellular relay: a laptop on the module's access point reaches a server over LTE",
      "wifi.disconnect\nwifi.ap.enable FieldKit FieldKit123\ncell.open\ncell.attach on\ncell.connect myserver.example.com 5000\n"
      "cell.bridge tcp 7000\n# the laptop joins FieldKit and connects to <ap-ip>:7000\ncell.bridge status\n"
      "cell.disconnect" },
    { "cell", "XPLR-IOT-1: update the SARA-R5 firmware with EasyFlash, then restore the settings",
      "cell.power on\n# EasyFlash: SARA-R5, the port shown, 3000000 baud or slower, the .dof file\ncell.power cycle\n"
      "cell.open\ncell.at AT+UFACTORY=0,2\ncell.power cycle\ncell.open\ncell.info\ncell.profile global\n"
      "cell.rat ltem nbiot\ncell.attach on" },
#endif
#if UCXAPP_HOST || defined(UCXAPP_CAN_HW)
    { "can", "Loopback test",
      "can.cfg 500000 loopback\ncan.send 0x123 11 22 33\ncan.recv\ncan.status" },
    { "can", "Listen on a real bus without disturbing it (STM32 board with a CAN transceiver)",
      "can.cfg 250000 listen\ncan.watch 30\ncan.status" },
    { "can", "CANable on Linux (SocketCAN, set up with: sudo ip link set can0 type can bitrate 500000 up)",
      "can.open can0\ncan.cfg 500000 normal\ncan.send 0x123 11 22 33\ncan.watch 10\ncan.open emulated" },
    { "can", "CAN frames to a PC over UDP (Windows, Linux or the STM32 board)",
      "can.bridge udp 192.168.1.10 6000\ncan.bridge status" },
    { "can", "CAN frames to a phone over SPS from the STM32 board",
      "can.cfg 500000 listen\ncan.bridge ble sps\ncan.bridge status" },
#endif
#if UCXAPP_HOST
    { "can", "CAN frames to a server over cellular (reconnects when the link drops)",
      "cell.open\ncell.attach on\ncell.connect logger.example.com 7001\ncan.bridge cell\ncan.bridge status" },

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
      "bt.scan\n# the pairing code is accepted (bt.autoconfirm off to check it with bt.confirm)\nbt.connect 1\nft.list 0\nft.get 0 notes.txt\nft.put 0 log.txt\nft.remove 0 log.txt" },
    { "ft", "File server over Wi-Fi (TCP, faster; an alternative to TFTP with CRC-32 checks)",
      "wifi.connect\nft.on rw tcp\n# on the other module: ft.list 192.168.1.10\n# ft.get 192.168.1.10 notes.txt\n# ft.put 192.168.1.10:6970 log.txt\nft.off" },
    { "ft", "Continue an interrupted transfer",
      "wifi.connect\nft.get 192.168.1.10 big.bin big.bin 1455 resume\nft.put 192.168.1.10 big.bin big.bin 1455 resume" },
    { "ancs", "iPhone notifications (pair 'ucx ANCS' from the phone's Bluetooth settings)",
      "ancs.on\nbt.list\nancs.sub 0\nancs.list\nancs.show 42\nancs.action 42 neg\nancs.off" },
    { "notify", "Android notifications from a companion app",
      "bt.defaults secure\nbt.pairing on\nnotify.on\n# the app writes 17|Messages|Anna|See you at 5\nnotify.list\nnotify.action 17 dismiss" },
    { "session", "Record a session for support, replay it elsewhere",
      "session.record issue.ucx\nwifi.connect\nnet.ping 8.8.8.8\nsession.stop\nsession.replay issue.ucx stop" },
#endif
    { "cfg", "Back up a module's settings and compare later",
      "cfg.show\ncfg.save golden.cfg\n# ... later, or on another module\ncfg.diff golden.cfg" },
    { "cfg", "Clone one module's settings to another",
      "cfg.save golden.cfg\n# connect to the second module, then\ncfg.load golden.cfg\ncfg.diff golden.cfg" },
    { "cfg", "Compare two saved files", "cfg.diff golden.cfg field.cfg" },
#if UCXAPP_HOST
    { "at", "Record the AT traffic, then read it decoded",
      "at.trace boot.trace\nwifi.connect\nat.trace.stop\nat.trace.show boot.trace" },
    { "at", "Decode a URC or response", "at.decode +UEWSNU\nat.decode +UEWLD:0,3" },
    { "at", "Replay a recorded trace and compare the replies", "at.replay boot.trace" },
    { "at", "Open a trace in Wireshark (next to a BLE or Wi-Fi sniffer capture)",
      "at.trace.pcap boot.trace\n# or record straight to pcapng:\nat.trace boot.pcapng\nwifi.connect\nat.trace.stop" },
#endif
    { "perf", "Round-trip time and throughput through the u-blox echo server (TCP and UDP)",
      "wifi.connect MyNetwork MyPassword\nperf.latency tcp echo.u-blox.com 7 20\nperf.throughput tcp echo.u-blox.com 7 10 512\n"
      "perf.latency udp echo.u-blox.com 7 20\nperf.throughput udp echo.u-blox.com 7 10 512" },
    { "perf", "Module to module over Wi-Fi (B echoes, A measures)",
      "# on module B: perf.echo udp 7000 120\n# here on module A, with B's address\nperf.latency udp 192.168.1.20 7000 50\nperf.throughput udp 192.168.1.20 7000 10 1000" },
    { "perf", "Bluetooth SPS between two modules (B echoes, A measures)",
      "# on module B: perf.echo sps 120\n# here on module A, B found by its name\nbt.connect NORA-W36\nperf.latency sps 20\nperf.throughput sps 10 244" },
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
#if UCXAPP_HOST
    { "prod", "Production test with a report per serial number",
      "prod.test\nprod.test examples/automation/production-test.ucx reports" },
#endif
    { "app", "Keep the clock in sync and set the module",
      "wifi.connect MyNetwork MyPassword\napp.time sync\napp.time\napp.time push" },
    { "app", "Local time with daylight saving and the place from the public IP",
      "wifi.profile.auto\napp.time sync\napp.time geo\napp.time" },
    { "app", "A fixed time zone by hand, then back to UTC",
      "app.time zone +01:00\napp.time\napp.time zone off" },
#if UCXAPP_HOST
    { "dtls", "Secure UDP echo",
      "dtls.client 192.168.1.10 4433 hello\ndtls.server 4433" },
#endif

    { "app", "Capture a session for a bug report",
      "app.log.level all\napp.log.save session.txt\n# ...reproduce...\napp.log.stop\n"
      "# the level is remembered: back to the default\napp.log.level warn" },
#if UCXAPP_HOST
    { "app", "Break time: snake eats data packets",
      "# in the console or the TUI (arrows/WASD, p pause, q quit)\napp.snake\n"
      "# or watch the app play\napp.snake demo 500" },
    { "app", "Signal hunt: find an access point by walking with the module",
      "# the strongest one, hotter or colder after every scan; -40 dBm wins\napp.hunt\n"
      "# a named one, 30 scans\napp.hunt MyNetwork 30" },
#endif
    { "app", "Run the use cases on the Virtual module (web page) or vucx",
      "# the tour, one use case, then all of them; watch and serve steps get 3 s each\n"
      "demo\nusecase.demo boat\ndemo all" },
    { "file", "Look at local files",
      "file.list\nfile.view session.txt 20\nfile.hash sha256 session.txt" },
#if !UCXAPP_HOST   /* file.recv and file.send need the board's own console */
    { "file", "Copy files to and from an STM32 board over its console (YMODEM)",
      "file.recv\n# ...start the YMODEM upload in Tera Term, or: ucxapp --stm32 --put fw.bin\nfile.hash sha256 fw.bin\n"
      "file.send fw.bin\n# ...start the YMODEM download, or: ucxapp --stm32 --get fw.bin" },
#endif
    { "usecase", "Fit check, then with a measured rate",
      "usecase.tool\nusecase.tool 2.5" },
    { "usecase", "Join the factory network with a user name (PEAP)",
      "cert.list\nwifi.roaming on\nwifi.peap FactoryNet tool17 Secret123 ca.pem\nwifi.status" },
    { "usecase", "Join the factory network with certificates (EAP-TLS)",
      "cert.list\nwifi.roaming on\nwifi.eap FactoryNet ca.pem client.pem client.key tool17\nwifi.status" },
    { "usecase", "Access point for the laptops, next to the station",
      "wifi.ap.enable Tool17 Tool17Pass\nwifi.ap.status" },
#if UCXAPP_HOST
    { "usecase", "HTTPS monitoring page for the laptops",
      "# web.start 443 <token> https  (from the shell or --exec, not from this page)\nweb.status" },
#endif
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
      "dev.diagnose mes.example.com\nfw.list\n# from the Windows app: fw.download\n# in the service window: fw.update" },
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
      "gatt.server.sps 60" },
    { "uc-site", "Gateway: find the fleet by name",
      "bt.filter 0 9 ToolCo\nbt.scan filter\nbt.filter clear" },
    { "uc-site", "Site office and depot Wi-Fi",
      "wifi.profile.add site SiteOffice SitePass123\nwifi.profile.add depot Depot DepotPass123\nwifi.profile.list" },
    { "uc-site", "At the depot: check and update",
      "dev.diagnose\nfw.list\n# from the Windows app: fw.download\n# overnight: fw.update" },
    { "uc-implement", "ISOBUS at 250 kbit/s",
      "can.cfg 250000\ncan.status\ncan.recv" },
    { "uc-implement", "Cab Wi-Fi and machine data to the tablet",
      "wifi.ap.enable Implement Implement123\ncan.bridge tcp 9100\n# the tablet app connects to <ap-ip>:9100\ncan.bridge status" },
    { "uc-implement", "Machine data to the operator's phone over BLE",
      "can.bridge ble nus\ncan.bridge status" },
    { "uc-implement", "Back at the yard: the work record to the cloud",
      "can.bridge off\nwifi.ap.disable\nwifi.connect FarmYard MyPassword\n"
      "mqtt.connect broker.example.com 1883\nmqtt.publish farm/implement/7 hectares=12.4" },
    { "uc-condition", "Vibration sensor on the motor",
      "imu.detect\nimu.open\nimu.rate 100\nimu.status" },
    { "uc-condition", "Record a healthy baseline, then stream to the analysis PC",
      "imu.log baseline.bin\n# ...a few minutes of normal running...\nimu.log off\nimu.bridge tcp 9000\n"
      "# the PC connects to <module-ip>:9000\nimu.bridge status" },
    { "uc-condition", "Alarm to the plant broker",
      "mqtt.connect broker.example.com 1883\nmqtt.publish plant/line1/motor7/vibration \"rms=7.1 mm/s state=alarm\"\n"
      "mqtt.disconnect" },
    { "uc-agv", "Join the hall network with roaming on",
      "wifi.roaming on\nwifi.connect HallNet HallPass123\nwifi.status" },
    { "uc-agv", "Drive the route and rate the coverage",
      "wifi.survey 10\nwifi.rssi 5 1" },
    { "uc-agv", "Latency to the fleet manager while driving",
      "# the fleet manager echoes UDP: perf.echo udp 7000 on a second module, or any UDP echo\n"
      "perf.latency udp echo.u-blox.com 7 20" },
    { "uc-agv", "Position and battery to the fleet manager",
      "mqtt.connect broker.example.com 1883\nmqtt.publish fleet/agv7/state \"x=12.4 y=3.1 battery=78\"\nmqtt.disconnect" },
    { "uc-eol", "Identify the unit",
      "dev.info\ndev.caps" },
    { "uc-eol", "Golden settings, then verify",
      "# once, on the reference unit: cfg.save line.cfg\ncfg.load line.cfg\ncfg.diff line.cfg" },
    { "uc-eol", "Radio check: the unit hears the line",
      "bt.scan\nwifi.scan" },
#if UCXAPP_HOST
    { "uc-eol", "Production test with a report per serial number",
      "prod.test - reports\nfile.list\n# firmware first when needed: fw.update <file>" },
#endif
    { "uc-blegw", "Find the sensors",
      "bt.scan\n# by name only: bt.filter 0 9 Sensor, bt.scan filter, bt.filter clear" },
    { "uc-blegw", "Read a sensor",
      "bt.connect 1\ngatt.services\ngatt.client.bas\nbt.disconnect" },
    { "uc-blegw", "Readings to the plant broker",
      "mqtt.connect broker.example.com 1883\nmqtt.publish plant/hall2/sensor-17/battery 87\nmqtt.disconnect" },
    { "uc-edge", "Record training data from the IMU",
      "imu.detect\nimu.open\nimu.rate 100\nimu.log motion.bin\n# ...move the sensor...\nimu.log off" },
    { "uc-edge", "Live stream to a notebook",
      "imu.bridge tcp 9000\n# in Python: socket.create_connection((\"<module-ip>\", 9000))\nimu.bridge status" },
    { "uc-edge", "I2C sensors on the host",
      "hw.i2c.scan\nhw.i2c.reg 0x76 0xD0" },
    { "uc-edge", "The model's result to the cloud",
      "mqtt.connect broker.example.com 1883\nmqtt.publish edge/sensor1/state walking" },
    /* Thread use cases; the dataset hex is an example, paste the one thread.dataset shows on the first node. */
    { "uc-mesh", "Form the network on the first node",
      "thread.network new\nthread.start 30\nthread.status\nthread.dataset" },
    { "uc-mesh", "Join another node with that dataset",
      "thread.dataset 0e080000000000010000000300000f\nthread.start 30\nthread.status" },
    { "uc-mesh", "Look at the mesh",
      "thread.leader\nthread.routers\nthread.neighbors" },
    { "uc-mesh", "Battery node (sleepy end device)",
      "thread.type sleepy 5000\nthread.start 30\nthread.type" },
    { "uc-border", "Join through the border router",
      "# on the border router: ot-ctl commissioner joiner add * J01NME\nthread.join J01NME 120\nthread.status" },
    { "uc-border", "Be found: register a service",
      "thread.srp on\nthread.srp host sensor-1\nthread.srp add _sensor._udp 5683 sensor-1\nthread.srp" },
    { "uc-border", "Find others with DNS-SD",
      "thread.dns browse _sensor._udp\nthread.dns resolve sensor-1 _sensor._udp\nthread.dns host sensor-1" },
    { "uc-building", "Provision a unit on the line",
      "thread.dataset 0e080000000000010000000300000f\nthread.type router\nthread.start 30\nthread.status" },
    { "uc-building", "Placement and health",
      "thread.neighbors\nthread.counters\nthread.counters reset" },
    { "uc-gateway", "Start the Thread side",
      "thread.network new\nthread.start 30\nthread.dataset" },
    { "uc-gateway", "Find the sensors",
      "thread.dns browse _sensor._udp\nthread.dns resolve sensor-1 _sensor._udp" },
    { "uc-gateway", "Readings to the cloud (on the site Wi-Fi)",
      "mqtt.connect broker.example.com 1883\nmqtt.publish site/sensor-1 21.5\nmqtt.disconnect" },
    { "uc-swarm", "Form the pilot mesh (first node; copy its dataset to peers)",
      "thread.network new\nthread.start 30\nthread.dataset\nthread.neighbors" },
    { "uc-swarm", "Receive neighbour records (each node, after joining the dataset)",
      "sock.open udp\nsock.bind 5000\nsock.recvfrom\nsock.close\n# edge host maintains the bounded neighbour database" },
    { "uc-swarm", "Send a pilot observation (replace address and sample with live edge data)",
      "sock.open udp\nsock.sendto fd00::2 5000 node=1,seq=1,frame=site,x=0,y=0,z=0,age_ms=0,sigma_m=2\nsock.close\n"
      "# sample only: real records need a shared time basis, freshness and authentication" },
    { "uc-recovery", "Baseline before a sensor or node outage",
      "thread.neighbors\nthread.counters\n"
      "# with a receiver and sensor open (gnss.open, imu.open): gnss.status for jamming flags, imu.status" },
    { "uc-recovery", "Inspect loss and rejoin (remove one peer, then restore it)",
      "thread.neighbors\nthread.counters\n"
      "# external edge estimator expires stale peers, grows uncertainty and flags estimated positions" },
    { "uc-handover", "Indoor fingerprint and source check",
      "wifi.scan\nwifi.locate body\n# edge host maps the fingerprint or surveyed CS anchors to the site frame" },
    { "uc-handover", "Outdoor receiver check (switch only after a valid fix)",
      "gnss.open\ngnss.watch 5\ngnss.status\n# edge host applies confidence and dwell-time hysteresis" },
    { "uc-livestock", "Load the paddock geofence (replace garden.kml with the paddock polygon)",
      "gnss.fence garden.kml\ngnss.open\ngnss.fence status" },
    { "uc-livestock", "Find a tag after a fence event (manual demonstration)",
      "bt.name FarmTag-001\n"
      "# Coded PHY S8 on every link: lowest rate, longest range (NORA-B26, ALMA-W1)\n"
      "at AT+UBTCS4=4\nat AT+UBTCS5=4\n"
      "bt.adv on\nbt.adv\n"
      "# once the gateway connects, at AT+UBTPHYR=<handle> shows tx/rx PHY 4 (Coded)\n"
      "# edge host triggers this after a confirmed area event" },
    { "uc-livestock", "Notify the farm server (the tag position over cellular)",
      "gnss.bridge off\ncell.open\ncell.attach on\ncell.connect farm.example.com 5000\n"
      "gnss.bridge cell\ngnss.bridge status\ngnss.bridge off\ncell.disconnect" },
    /* Cellular use cases: one socket over the modem, relayed to Wi-Fi, BLE, CAN or GNSS. */
    { "uc-fieldkit", "Access point for the laptops (no site Wi-Fi: leave the station first)",
      "wifi.disconnect\nwifi.ap.enable FieldKit FieldKit123\nwifi.ap.status" },
    { "uc-fieldkit", "Modem online, then your server",
      "cell.open\ncell.attach on\ncell.signal\ncell.connect myserver.example.com 5000" },
    { "uc-fieldkit", "Relay: a laptop on FieldKit talks to the server over LTE",
      "cell.bridge tcp 7000\n# the laptop joins FieldKit and connects to <ap-ip>:7000\ncell.bridge status" },
    { "uc-fieldkit", "Done: stop the relay and the link",
      "cell.bridge off\ncell.disconnect" },
    { "uc-fieldkit", "Or: the modem's AT console for the laptop (no cell.connect)",
      "cell.bridge tcp 7000\n# the laptop connects to <ap-ip>:7000 and types AT commands\ncell.bridge off\n"
      "wifi.ap.disable" },
    { "uc-blecell", "Modem online, then your server",
      "cell.open\ncell.attach on\ncell.connect myserver.example.com 5000" },
    { "uc-blecell", "Relay a phone on Nordic UART to the server",
      "cell.bridge ble nus\n# the phone app connects to Nordic UART; its lines go to the server and back\n"
      "cell.bridge status" },
    { "uc-blecell", "Done: stop the relay and the link",
      "cell.bridge off\ncell.disconnect" },
    { "uc-vehicle", "CAN bus (Linux with a CANable: can.open can0 first)",
      "can.cfg 500000\ncan.status" },
    { "uc-vehicle", "Modem online and the coverage here",
      "cell.open\ncell.attach on\ncell.signal\ncell.operator" },
    { "uc-vehicle", "Frames to your server over LTE",
      "cell.connect logger.example.com 7001\ncan.bridge cell\ncan.bridge status" },
    { "uc-vehicle", "Done: stop the bridge and the link",
      "can.bridge off\ncell.disconnect" },
    { "uc-xplr", "Find the SARA-R5, check the SIM and the signal",
      "cell.open\ncell.info\ncell.signal\ncell.operator" },
    { "uc-xplr", "MAX-M10S position to your server over LTE",
      "gnss.open\ngnss.bridge off\ngnss.status\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.bridge cell\n"
      "gnss.bridge status" },
    { "uc-xplr", "Done: stop the bridge and the link",
      "gnss.bridge off\ncell.disconnect" },
    { "uc-xplr", "Modem firmware (steps in cell.example)",
      "# cell.power on, then EasyFlash on the SARA port; cell.power cycle\n"
      "# cell.at AT+UFACTORY=0,2 and cell.power cycle restore the settings\ncell.open\ncell.info" },
    /* Everyday use cases (usecase.check <name>); run with examples uc-<name> <number>. */
    { "uc-boat", "Receiver check and fix",
      "gnss.open\ngnss.watch 10" },
    { "uc-boat", "Boat Wi-Fi for the tablets",
      "wifi.ap.enable BoatNet BoatPass123\nwifi.ap.status" },
    { "uc-boat", "NMEA to the chart app over TCP 10110",
      "gnss.bridge off\ngnss.bridge tcp 10110\n# in OpenCPN or Navionics: add a TCP connection to <ap-ip>:10110\ngnss.bridge status" },
    { "uc-boat", "NMEA to a phone over BLE as well",
      "gnss.bridge ble nus\n# connect a GNSS app to the Nordic UART service\ngnss.bridge status\n"
      "# bridge 1 is the TCP server, bridge 2 the BLE link; gnss.bridge off 2 stops only the phone" },
    { "uc-boat", "Log the trip",
      "gnss.log trip.nmea nmea\n# ...sail...\ngnss.log off\n# at home: gnss.close, then gnss.replay trip.nmea 10" },
    { "uc-rtk", "Home Wi-Fi and the receiver",
      "wifi.connect MyNetwork MyPassword\ngnss.open\ngnss.status" },
    { "uc-rtk", "Corrections from a caster, position to the robot over BLE",
      "gnss.bridge off\ngnss.bridge ntrip rtk2go.com 2101 MOUNT ${NTRIP_USER} ${NTRIP_PASS} ble\ngnss.bridge status" },
    { "uc-rtk", "Watch for an RTK fixed solution",
      "gnss.watch 30" },
    { "uc-mower", "First setup from the owner's phone over BLE",
      "bt.defaults secure\nbt.pairing on\ngatt.server.wps 120\nwifi.status" },
    { "uc-mower", "PointPerfect over the garden Wi-Fi, position to the phone over BLE",
      "gnss.open\ngnss.bridge off\ngnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble\n"
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
      "gnss.bridge off\ngnss.bridge ntrip ppntrip.services.u-blox.com 2101 NEAR-SPARTN ${NTRIP_USER} ${NTRIP_PASS} ble cell\n"
      "gnss.watch 3\ngnss.bridge status" },
    { "uc-farm", "Log the field",
      "gnss.log field.nmea nmea\n# ...drive the field...\ngnss.log off" },
    { "uc-farm", "Back at the farmyard: Wi-Fi, then upload the log",
      "gnss.bridge off\ncell.disconnect\nwifi.connect FarmYard MyPassword\n"
      "# tftp.put <pc> field.nmea, or ft.put <pc> field.nmea to a PC running ft.on rw" },
    { "uc-tracker", "Position to your server over Wi-Fi",
      "wifi.connect MyNetwork MyPassword\ngnss.open\ngnss.bridge off\ngnss.bridge tcpc myserver.example.com 5000\n"
      "gnss.bridge status" },
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
#if UCXAPP_HOST
    { "utils", "Handy tools",
      "utils.qr.wifi MyNetwork MyPassword\nutils.qr.wifi GuestNet nopass large\nutils.zip logs.zip session.txt\nutils.hash.text sha256 hello" },
#endif
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

static ucxapp_cmd_t gCmds[GROUP_MAX + 4];
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

/* While a demo runs, steps that watch or serve for a while get DEMO_STEP_S seconds: the argument at the command's
   seconds position (counted after the command word; perf.throughput sps has it one earlier). */
#define DEMO_STEP_S 3
static bool gDemoFast;

static void fastLine(char *line)
{
    static const struct {
        const char *cmd;
        int arg;
    } kTimed[] = { { "gnss.watch", 1 },  { "cell.watch", 1 },    { "can.watch", 1 },       { "gatt.listen", 1 },
                   { "wifi.survey", 1 }, { "sock.accept", 1 },   { "mqtt.read", 1 },       { "sock.echo", 3 },
                   { "sock.recvfrom", 3 }, { "perf.phone", 2 },  { "perf.throughput", 4 }, { "bt.cs.range", 2 },
                   { "gatt.server.sps", 1 }, { "gatt.server.wps", 1 }, { "gatt.server.hrs", 1 },
                   { "gatt.server.ras", 1 } };
    size_t wordLen = strcspn(line, " ");
    int arg = 0;

    for (size_t i = 0; i < sizeof(kTimed) / sizeof(kTimed[0]); i++) {
        if (strlen(kTimed[i].cmd) == wordLen && strncmp(line, kTimed[i].cmd, wordLen) == 0) {
            arg = kTimed[i].arg;
        }
    }
    if (arg == 4 && strncmp(line + wordLen, " sps", 4) == 0) {
        arg = 2;
    }
    char *p = line + wordLen;

    for (int n = 0; arg > 0 && p != NULL && *p == ' ' && n < arg - 1; n++) {
        p = strchr(p + 1, ' ');
    }
    if (arg == 0 || p == NULL || *p != ' ') {
        return;
    }
    p++;
    size_t len = strcspn(p, " ");

    if (len > 0 && strspn(p, "0123456789") == len && atoi(p) > DEMO_STEP_S) {
        memmove(p + 1, p + len, strlen(p + len) + 1);
        p[0] = (char)('0' + DEMO_STEP_S);
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
#if UCXAPP_HOST
        ucxapp_demo_prepare(app);
#endif
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
#if UCXAPP_HOST
            ucxapp_demo_line(app, line, sizeof(line));
#endif
            if (gDemoFast) {
                fastLine(line);
            }
            snprintf(word, sizeof(word), "%.*s", (int)strcspn(line, " "), line);
            c = ucxapp_cmd_find(word);
            /* on the Virtual module, commands that only change the module run: it is a simulation */
            if (c != NULL && (c->flags & UCXAPP_CMD_DANGER) &&
                !(ucxapp_module_is_virtual(app) && (c->flags & UCXAPP_CMD_NEEDS_DEVICE) && strncmp(word, "fw.", 3) != 0)) {
                ucxapp_print("stopped before '%s': it changes or deletes data, run it by hand\n", line);
                return UCXAPP_ERR_FAILED;
            }
            ucxapp_print("> %s\n", line);
            /* the echo keeps ${VAR}: the value may be a password or token */
            char expanded[512];
            int rc = ucxapp_expand_env(line, expanded, sizeof(expanded)) ? ucxapp_cmd_exec(app, expanded)
                                                                           : UCXAPP_ERR_FAILED;
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

/* Runs every example of the uc-* groups whose id is in ids (all when ids is NULL), each on its own: a failed step only
   fails that example. Steps that watch or serve for a while are cut to DEMO_STEP_S seconds. */
static int runCases(ucxapp_t *app, const char *const *ids, size_t idCount)
{
    char failed[512] = "";
    int ran = 0, ok = 0, cases = 0;

    if (!ucxapp_module_is_virtual(app)) {
        ucxapp_print("ERROR: the demo needs a virtual module: its steps change the module's settings "
                     "(Virtual module on the web page, or python -m harness.vucx)\n");
        return UCXAPP_ERR_FAILED;
    }
    gDemoFast = true;
    for (size_t i = 0; i < EX_COUNT; i++) {
        const char *g = kExamples[i].group;
        bool seen = false, wanted = ids == NULL;
        int count = 0;

        if (strncmp(g, "uc-", 3) != 0) {
            continue;
        }
        for (size_t k = 0; k < idCount; k++) {
            wanted = wanted || strcmp(g + 3, ids[k]) == 0;
        }
        for (size_t k = 0; k < i; k++) {
            seen = seen || strcmp(kExamples[k].group, g) == 0;
        }
        if (!wanted || seen) {
            continue;
        }
        for (size_t k = i; k < EX_COUNT; k++) {
            count += strcmp(kExamples[k].group, g) == 0;
        }
        cases++;
        ucxapp_print("\n=== %s (%d) ===\n", g + 3, count);
        for (int n = 1; n <= count; n++) {
            ran++;
            if (runExample(app, g, n) == UCXAPP_OK) {
                ok++;
            } else if (strlen(failed) + strlen(g) + 8 < sizeof(failed)) {
                snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), " %s/%d", g + 3, n);
            }
        }
    }
    gDemoFast = false;
    if (cases == 0) {
        ucxapp_print("ERROR: no use case %s (see usecase.list)\n", idCount > 0 ? ids[0] : "");
        return UCXAPP_ERR_USAGE;
    }
    ucxapp_print("\ndemo: %d use case%s, %d of %d examples ran to the end\n", cases, cases == 1 ? "" : "s", ok, ran);
    if (failed[0] != '\0') {
        ucxapp_print("stopped early:%s\n", failed);
    }
    return ok == ran ? UCXAPP_OK : UCXAPP_ERR_FAILED;
}

/* usecase.demo [name]: every use case, or one. */
static int cmdDemo(ucxapp_t *app, int argc, char *argv[])
{
    const char *id = argc == 2 ? argv[1] : NULL;

    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    return runCases(app, id != NULL ? &id : NULL, id != NULL ? 1 : 0);
}

/* demo [all]: a short tour of the highlights (or every use case) on a virtual module. */
static int cmdTour(ucxapp_t *app, int argc, char *argv[])
{
    static const char *const kTour[] = { "provision", "home", "boat", "fieldkit", "blegw", "remote" };

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "all") != 0)) {
        return UCXAPP_ERR_USAGE;
    }
    if (argc == 2) {
        return runCases(app, NULL, 0);
    }
    if (ucxapp_module_is_virtual(app)) {
        ucxapp_print("u-connectXpress tour: Wi-Fi set up from a phone, MQTT, GNSS to a chart app, a Wi-Fi to LTE relay, "
                     "a BLE sensor gateway and a BLE remote (demo all runs every use case)\n");
    }
    return runCases(app, kTour, sizeof(kTour) / sizeof(kTour[0]));
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
    gCmds[n + 1] = (ucxapp_cmd_t){ "usecase.demo", "[name]",
                                   "Run every step of every use case (or one) on a virtual module and report each", 0, cmdDemo };
    gCmds[n + 2] = (ucxapp_cmd_t){ "demo", "[all]",
                                   "Show the module off: a tour of the highlight use cases on a virtual module (all: every one)", 0, cmdTour };
    (void)ucxapp_cmd_register(gCmds, n + 3);
}
