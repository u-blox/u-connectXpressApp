#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

/* Real scenarios per command group, shown by "<group>.ex" or "examples <group>". Lines starting with '#' are notes. */
typedef struct {
    const char *group;
    const char *title;
    const char *lines;
} example_t;

static const example_t kExamples[] = {
    { "gnss", "First fix check",
      "gnss.open\ngnss.status\ngnss.sats" },
    { "gnss", "u-center or gpsd on the PC over Wi-Fi",
      "wifi.connect MyNetwork MyPassword\ngnss.bridge tcp\n# connect u-center / gpsd to <module-ip>:2947" },
    { "gnss", "Phone app over BLE",
      "gnss.bridge ble nus\n# open nRF Connect or a Serial Bluetooth Terminal app and connect" },
    { "gnss", "Stream to a server or chart plotter",
      "gnss.bridge tcpc myserver.example.com 5000\n# or NMEA over UDP to OpenCPN:\ngnss.bridge udp 192.168.1.10 10110" },
    { "gnss", "Secure remote access (TLS)",
      "utils.tls.server.create gnss 192.168.1.50\ngnss.bridge tls 2948 gnss" },
    { "gnss", "Log a drive test",
      "gnss.rate 5\ngnss.log drive.nmea\n# ...drive...\ngnss.log off" },
    { "gnss", "Recover a stuck receiver",
      "gnss.restart cold\ngnss.status" },
    { "gnss", "PC tool through a second COM port",
      "gnss.bridge uart COM20 115200\n# open COM20's partner port in u-center" },
    { "gnss", "Send positions over cellular",
      "cell.open COM30\ncell.attach on\ncell.connect myserver.example.com 5000\ngnss.bridge cell" },

    { "imu", "First look at the sensor",
      "imu.detect\nimu.open\nimu.status" },
    { "imu", "Stream to a PC over Wi-Fi",
      "imu.rate 50\nimu.bridge tcp 9000\n# connect a TCP client to <module-ip>:9000" },
    { "imu", "Live data to a phone over BLE",
      "imu.bridge ble nus" },
    { "imu", "Record a session",
      "imu.log motion.bin\n# ...move...\nimu.log off" },

    { "wifi", "Join a network and check it",
      "wifi.scan\nwifi.connect 1 MyPassword\nwifi.status\nnet.ping u-blox.com" },
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

    { "net", "Is the internet reachable?",
      "net.dns u-blox.com\nnet.ping u-blox.com 4\ndevice.diagnose" },
    { "net", "Correct time from NTP",
      "net.ntp server 0 pool.ntp.org\nnet.ntp on\nnet.ntp status" },
    { "net", "Throughput with iperf on the PC",
      "# PC: iperf -s\nnet.iperf client 192.168.1.10 tcp 10" },

    { "http", "Fetch a page and download a file",
      "http.get https://www.u-blox.com 200\nhttp.download https://example.com/file.bin file.bin\nfile.hash sha256 file.bin" },

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

    { "cert", "Install and check certificates",
      "cert.upload ca ca.pem myca\ncert.upload client client.pem mycert\ncert.upload key client.key mykey\ncert.list\ncert.info myca" },

    { "bt", "Find and connect a device",
      "bt.scan\nbt.connect 1\nbt.list\nbt.disconnect" },
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
      "device.info\nfw.update\ndevice.info" },
    { "fw", "Flash a given file",
      "fw.flash NORA-W36X-3.5.0.zip" },
    { "fw", "Rescue a module stuck in bootloader",
      "device.disconnect\nfw.bootloader COM13 NORA-W36X-3.5.0.zip" },

    { "device", "Support report",
      "device.info\ndevice.status\ndevice.diagnose" },
    { "device", "Start clean",
      "device.factory confirm" },

    { "cell", "Cellular data link",
      "cell.detect\ncell.open COM30\ncell.apn internet\ncell.attach on\ncell.status\ncell.ping u-blox.com" },
    { "cell", "Modem AT console from a PC tool over TCP",
      "cell.open COM30\ncell.bridge tcp 7000" },

    { "can", "Loopback test",
      "can.cfg 500000\ncan.send 0x123 11 22 33\ncan.recv\ncan.status" },
    { "can", "CAN frames to a PC over UDP",
      "can.bridge udp 192.168.1.10 6000" },

    { "hw", "Blink an LED and read a pin",
      "hw.load app/boards/h753.json\nhw.led 0 on\nhw.gpio.read 3" },
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
      "bt.scan\nbt.connect 1\nbt.confirm yes\nft.ls 0\nft.get 0 notes.txt\nft.put 0 log.txt\nft.rm 0 log.txt" },
    { "ft", "File server over Wi-Fi (TCP, faster; an alternative to TFTP with CRC-32 checks)",
      "wifi.connect\nft.on rw tcp\n# on the other module: ft.ls 192.168.1.10\n# ft.get 192.168.1.10 notes.txt\n# ft.put 192.168.1.10:6970 log.txt\nft.off" },
    { "ft", "Continue an interrupted transfer",
      "ft.get 192.168.1.10 big.bin big.bin 1455 resume\nft.put 192.168.1.10 big.bin big.bin 1455 resume" },
    { "ancs", "iPhone notifications (pair 'ucx ANCS' from the phone's Bluetooth settings)",
      "ancs.on\nbt.list\nancs.sub 0\nancs.list\nancs.show 42\nancs.action 42 neg\nancs.off" },
    { "notify", "Android notifications from a companion app",
      "bt.defaults secure\nbt.pairing on\nnotify.on\n# the app writes 17|Messages|Anna|See you at 5\nnotify.list\nnotify.action 17 dismiss" },
    { "session", "Record a session for support, replay it elsewhere",
      "session.record issue.ucx\nwifi.connect\nnet.ping 8.8.8.8\nsession.stop\nsession.replay issue.ucx stop" },
    { "prod", "Production test with a report per serial number",
      "prod.test\nprod.test examples/automation/production-test.ucx reports" },
    { "app", "Keep the clock in sync and set the module",
      "wifi.connect MyNetwork MyPassword\napp.time sync\napp.time\napp.time push" },
    { "dtls", "Secure UDP echo",
      "dtls.client 192.168.1.10 4433 hello\ndtls.server 4433" },

    { "log", "Capture a session for a bug report",
      "log.level all\nlog.save session.txt\n# ...reproduce...\nlog.stop" },
    { "file", "Look at local files",
      "file.list\nfile.view session.txt 20\nfile.hash sha256 session.txt" },
    { "file", "Copy files to and from an STM32 board over its console (YMODEM)",
      "file.recv\n# ...start the YMODEM upload in Tera Term, or: ucxapp --stm32 --put fw.bin\nfile.hash sha256 fw.bin\n"
      "file.send fw.bin\n# ...start the YMODEM download, or: ucxapp --stm32 --get fw.bin" },
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

static void printGroup(const char *group)
{
    int n = 0;

    ucxapp_print("Examples for %s.*\n", group);
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
}

static int cmdExamples(ucxapp_t *app, int argc, char *argv[])
{
    char group[16];
    const char *dot = strchr(argv[0], '.');

    (void)app;
    if (argc > 2) {
        return UCXAPP_ERR_USAGE;
    }
    if (dot == NULL) {
        if (argc == 2) {
            snprintf(group, sizeof(group), "%s", argv[1]);
            printGroup(group);
            return UCXAPP_OK;
        }
        ucxapp_print("Groups with examples (type <group>.ex):\n ");
        for (size_t i = 0; gCmds[i].name != NULL; i++) {
            if (strchr(gCmds[i].name, '.') != NULL) {
                ucxapp_print(" %.*s", (int)(strchr(gCmds[i].name, '.') - gCmds[i].name), gCmds[i].name);
            }
        }
        ucxapp_print("\n");
        return UCXAPP_OK;
    }
    snprintf(group, sizeof(group), "%.*s", (int)(dot - argv[0]), argv[0]);
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
        snprintf(gNames[n], sizeof(gNames[n]), "%s.ex", g);
        gCmds[n] = (ucxapp_cmd_t){ gNames[n], "", "Real-world scenarios with ready-to-type commands", 0, cmdExamples };
        n++;
    }
    gCmds[n] = (ucxapp_cmd_t){ "examples", "[group]", "List the groups with examples, or show one group's examples", 0, cmdExamples };
    (void)ucxapp_cmd_register(gCmds, n + 1);
}
