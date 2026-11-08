#include "ucxapp_cmd.h"

#include <stdio.h>
#include <string.h>

/* Connected tool: a host with a NORA-W36 on the factory network, a local AP for laptops, BLE for accessories.
   The limits are the NORA-W36 3.5.0 user guide and data sheet figures. */
#define HISTORY_MBIT      7200   /* 900 MB */
#define WINDOW_S          1800   /* 30 min */
#define UART_TRANSPARENT  25     /* Mbit/s x10 at 4 Mbaud */
#define UART_AT           10

/* "2.5" -> 25; false unless it is a positive number with at most one decimal. */
static bool parseTenths(const char *s, int32_t *out)
{
    int32_t v = 0;
    int dec = -1;

    for (; *s != '\0'; s++) {
        if (*s == '.' && dec < 0) {
            dec = 0;
        } else if (*s >= '0' && *s <= '9' && dec < 1 && v < 10000) {
            v = v * 10 + (*s - '0');
            dec = dec >= 0 ? dec + 1 : dec;
        } else {
            return false;
        }
    }
    *out = dec == 1 ? v : v * 10;
    return *out > 0;
}

static int32_t minutesAt(int32_t tenths)
{
    return (HISTORY_MBIT * 10 / tenths + 59) / 60;
}

static void row(const char *verdict, const char *what, const char *how)
{
    ucxapp_print(" %-8s %-15s %s\n", verdict, what, how);
}

static int cmdTool(ucxapp_t *app, int argc, char *argv[])
{
    int32_t measured = 0;
    bool wifi = true;
    char ssid[40] = "", ip[48] = "";

    if (argc > 2 || (argc == 2 && !parseTenths(argv[1], &measured))) {
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx != NULL) {
        wifi = ucxapp_module_has_wifi(app);
    }
    ucxapp_print("Connected tool: factory Wi-Fi, local AP for laptops, BLE for accessories (NORA-W36 3.5.0 limits)\n");
    row("[ok]", "Host", "STM32H7 with Zephyr runs this app: ucxapp --stm32-zephyr --board h753");
    if (!wifi) {
        row("[gap]", "Wi-Fi", "this module has no Wi-Fi (NORA-B26); use a NORA-W36");
    } else {
        row("[ok]", "Station + AP", "both at once: wifi.peap or wifi.eap, then wifi.ap.enable (WPA2, max 5 stations)");
        row("[check]", "Enterprise", "EAP-TLS (wifi.eap) and PEAP (wifi.peap); confirm WPA3-Enterprise with the factory AP");
        row("[ok]", "Roaming", "the tool moves between factory APs (same SSID): wifi.roaming on before joining");
    }
    row("[ok]", "Protocols", "the host runs them over module sockets (sock.*) or MQTT (mqtt.*)");
    row("[limit]", "HTTPS, 5 PCs", "TLS ends on the host (web.start, HTTPS by default); 6 module sockets, the listener takes 2");
    if (measured > 0) {
        char line[96];

        snprintf(line, sizeof(line), "measured %d.%d Mbit/s: 900 MB in %d min (needs 4.0 Mbit/s for 30 min)",
                 (int)(measured / 10), (int)(measured % 10), (int)minutesAt(measured));
        row(measured * WINDOW_S >= HISTORY_MBIT * 10 ? "[ok]" : "[gap]", "900 MB/30 min", line);
    } else {
        char line[112];

        snprintf(line, sizeof(line), "needs 4.0 Mbit/s; 4 Mbaud UART gives 2.5 transparent, 1.0 in AT mode (%d / %d min)",
                 (int)minutesAt(UART_TRANSPARENT), (int)minutesAt(UART_AT));
        row("[gap]", "900 MB/30 min", line);
    }
    row("[limit]", "8 trays", "the AP takes 5 stations: trays on the factory network over one UDP socket, or BLE (3 links)");
    row("[ok]", "BLE", "phone setup (gatt.server.wps), service link (gatt.server.sps), accessories (bt.connect)");
    ucxapp_print("Coexistence: at most 1 TLS client, 1 BLE link and 2 TCP clients at the same time\n");
    if (app->ucx != NULL && wifi) {
        if (ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
            ucxapp_print("Now: station on %s (%s)\n", ssid, ip);
        } else {
            ucxapp_print("Now: station not connected\n");
        }
    }
    if (measured == 0) {
        ucxapp_print("Measure with net.iperf, then: usecase.tool <Mbit/s>\n");
    }
    return UCXAPP_OK;
}

/* Use cases by section (industry, edge AI, Thread, location, cellular, everyday). Row kinds: 'w' needs Wi-Fi on the
   module, 't' Thread firmware on the module, 'g' a GNSS receiver,
   anything else is fixed. A row with a command is a gap when that command is not in this build. Names stay under
   15 characters. The industry, edge AI, Thread and cellular cases are not in lite builds. */
typedef struct {
    char kind;
    const char *verdict;
    const char *what;
    const char *how;
    const char *cmd;
} ucRow_t;

typedef struct {
    const char *id;
    const char *title;
    const ucRow_t *rows;
    size_t rowCount;
} ucCase_t;

#ifndef UCXAPP_LITE
static const ucRow_t kIiot[] = {
    { 'w', "[ok]", "Enterprise", "WPA2/WPA3-Enterprise with certificates on the module: wifi.eap", "wifi.eap" },
    { 0, "[ok]", "TLS to the MES", "a TCP socket with TLS, CA and client certificate from cert.upload: sock.tls", "sock.tls" },
    { 0, "[ok]", "Machine data", "CAN frames from the machine to the MES: can.bridge tcpc <host> <port>", "can.bridge" },
    { 0, "[ok]", "MQTT", "TLS with CA and client certificate: mqtt.connect <host> ca=<ca> cert=<c> key=<k>", "mqtt.connect" },
    { 0, "[ok]", "Baseline", "save one unit's settings, then cfg.diff every unit on the line against it", "cfg.diff" },
    { 0, "[ok]", "Health", "one PASS/FAIL report for support: dev.diagnose <mes-host>", "dev.diagnose" },
    { 0, "[ok]", "Firmware", "fw.download the newest firmware, fw.update in a service window", "fw.download" },
    { 0, "[check]", "Lifecycle", "plan field updates around u-blox product change notices", NULL },
};

static const ucRow_t kMedical[] = {
    { 0, "[ok]", "SOUP item", "the module and its firmware version are a SOUP item: dev.info for the record", "dev.info" },
    { 0, "[ok]", "Secure pairing", "LE Secure Connections with numeric comparison: bt.defaults secure", "bt.defaults" },
    { 0, "[ok]", "Bonds", "only bonded phones or gateways: bt.bonds lists them", "bt.bonds" },
    { 0, "[ok]", "Readings", "to the phone app over the u-blox Serial Port service: gatt.server.sps", "gatt.server.sps" },
    { 0, "[ok]", "Verification", "scripted tests with a JUnit report: ucxapp --script <file> --junit <xml>", NULL },
    { 0, "[ok]", "Evidence", "every command and reply with its time: at.trace, at.trace.pcap for Wireshark", "at.trace.pcap" },
    { 0, "[ok]", "Fixed config", "cfg.diff against the released settings fails when one changed", "cfg.diff" },
    { 0, "[check]", "Safety class", "the IEC 62304 class and risk control stay with the device maker", NULL },
};

static const ucRow_t kSite[] = {
    { 0, "[ok]", "Service app", "a technician's phone reads the tool over the u-blox Serial Port service", "gatt.server.sps" },
    { 0, "[ok]", "Secure pairing", "only paired phones: bt.defaults secure, bt.pairing on", "bt.pairing" },
    { 0, "[ok]", "Tool name", "each tool advertises its own name: bt.name ToolCo-1234", "bt.name" },
    { 0, "[ok]", "Tool tracking", "find the fleet by name from a gateway: bt.filter 0 9 ToolCo, bt.scan filter", "bt.filter" },
    { 'w', "[ok]", "Site Wi-Fi", "profiles for the site office and the depot: wifi.profile.add", "wifi.profile.add" },
    { 0, "[ok]", "Depot update", "at the depot: fw.download, then fw.update overnight", "fw.download" },
    { 0, "[ok]", "Health", "dev.diagnose before the tool goes out", "dev.diagnose" },
    { 0, "[check]", "Find a tool", "NORA-B26/B27: bt.cs on, a Channel Sounding phone measures the distance", "bt.cs" },
    { 0, "[check]", "RF range", "steel and concrete cut the range: check the antenna and test on site", NULL },
};

static const ucRow_t kImplement[] = {
    { 0, "[ok]", "CAN bus", "ISOBUS and J1939 run at 250 kbit/s: can.cfg 250000, can.recv", "can.cfg" },
    { 0, "[ok]", "CAN adapter", "an STM32 board with a CAN transceiver runs can.* on the bus; the PC emulates it", "can.cfg" },
    { 'w', "[ok]", "Cab Wi-Fi", "the module is the cab's access point: wifi.ap.enable", NULL },
    { 0, "[ok]", "Cab tablet", "CAN frames both ways over TCP: can.bridge tcp 9100", "can.bridge" },
    { 0, "[ok]", "Phone", "or over BLE to the operator's phone: can.bridge ble nus", "can.bridge" },
    { 0, "[ok]", "Yard upload", "back at the yard: wifi.connect, then mqtt.publish the work record", "mqtt.publish" },
    { 0, "[ok]", "Field link", "frames over cellular in the field: can.bridge cell (reconnects after dead zones)", "can.bridge" },
    { 0, "[limit]", "Decoding", "frames are raw (id, dlc, data); ISOBUS and J1939 decoding is in the tablet app", NULL },
};

static const ucRow_t kCondition[] = {
    { 0, "[ok]", "Vibration", "an IMU on the motor housing at up to 200 Hz: imu.open, imu.rate 100", "imu.rate" },
    { 0, "[ok]", "Baseline", "record the healthy motor first: imu.log baseline.bin", "imu.log" },
    { 0, "[ok]", "Raw stream", "to the analysis PC or edge model: imu.bridge tcp 9000", "imu.bridge" },
    { 'w', "[ok]", "Plant Wi-Fi", "Enterprise Wi-Fi with certificates: wifi.eap", "wifi.eap" },
    { 0, "[ok]", "Alarms", "the host's verdict to the plant broker: mqtt.publish", "mqtt.publish" },
    { 0, "[limit]", "Bearing faults", "200 Hz finds imbalance and misalignment; bearings need a kHz sensor on the host", NULL },
    { 0, "[check]", "Hazardous area", "ATEX or IECEx approval stays with the device maker and the enclosure", NULL },
};

static const ucRow_t kAgv[] = {
    { 'w', "[ok]", "Roaming", "between the hall's APs on one SSID: wifi.roaming on before joining", "wifi.roaming" },
    { 'w', "[ok]", "Coverage", "drive the route and rate the signal: wifi.survey, wifi.rssi", "wifi.survey" },
    { 0, "[ok]", "Latency", "round trip to the fleet manager while driving: perf.latency udp <host> <port>", "perf.latency" },
    { 0, "[ok]", "Fleet link", "position and battery to the fleet manager: mqtt.publish", "mqtt.publish" },
    { 0, "[ok]", "Service", "a technician's phone reads the robot over BLE: gatt.server.sps", "gatt.server.sps" },
    { 0, "[check]", "Handover gap", "the gap while roaming depends on the APs; measure it with perf.latency on the route", NULL },
    { 0, "[limit]", "Safety", "the emergency stop stays wired or on a certified safety radio, never on Wi-Fi", NULL },
};

static const ucRow_t kEol[] = {
    { 0, "[ok]", "Identity", "serial number, model and firmware of each unit: dev.info", "dev.info" },
    { 0, "[ok]", "Firmware", "the released firmware on every unit: fw.update <file>", "fw.update" },
    { 0, "[ok]", "Settings", "the golden settings from a reference unit: cfg.load line.cfg", "cfg.load" },
    { 0, "[ok]", "Verify", "cfg.diff fails the unit when a setting differs", "cfg.diff" },
    { 0, "[ok]", "Radio check", "the unit hears the line's beacon and AP: bt.scan, wifi.scan", "bt.scan" },
    { 0, "[ok]", "Report", "one file per serial number and a line in report.csv: prod.test", "prod.test" },
    { 0, "[check]", "RF tests", "conducted RF and RED/FCC tests stay with the test house and the module approvals", NULL },
};

static const ucRow_t kBleGw[] = {
    { 0, "[ok]", "Find sensors", "by name prefix: bt.filter 0 9 Sensor, bt.scan filter", "bt.filter" },
    { 0, "[ok]", "Read", "connect and read the standard services: bt.connect, gatt.client.bas", "gatt.client.bas" },
    { 0, "[ok]", "Beacons", "sensors that only advertise need no link: the values are in the scan", "bt.scan" },
    { 'w', "[ok]", "Plant Wi-Fi", "station on the plant network: wifi.connect or wifi.eap", NULL },
    { 0, "[ok]", "To MQTT", "each reading to the plant broker: mqtt.publish plant/hall2/sensor-17 21.5", "mqtt.publish" },
    { 0, "[limit]", "Links", "3 BLE links at a time: poll more sensors in turns or use advertising sensors", NULL },
    { 0, "[check]", "Range", "metal racks and machines cut the range: scan on site before placing the gateway", NULL },
};

static const ucRow_t kEdge[] = {
    { 0, "[ok]", "Training data", "record the motion for the model: imu.open, imu.log motion.bin", "imu.log" },
    { 0, "[ok]", "Live stream", "the IMU stream to a PC or notebook: imu.bridge tcp 9000", "imu.bridge" },
    { 0, "[ok]", "I2C sensors", "find and read sensors on the host: hw.i2c.scan, hw.i2c.reg", "hw.i2c.scan" },
    { 0, "[ok]", "Results", "the model's result to the cloud: mqtt.publish", "mqtt.publish" },
    { 0, "[check]", "ALMA-W16x", "ALMA-W161/W166 run u-connectXpress on Wi-Fi 6: these steps carry over", NULL },
    { 0, "[check]", "ML on module", "the model on an NPU: ALMA-W111/W116 (open CPU, your own firmware)", NULL },
    { 0, "[check]", "SPI host link", "SPI for more throughput is coming in u-connectClient", NULL },
};

static const ucRow_t kMesh[] = {
    { 't', "[ok]", "Form", "the first node makes the network: thread.network new, thread.start", "thread.network" },
    { 't', "[ok]", "Join", "the others take its dataset: thread.dataset <hex>, thread.start", "thread.dataset" },
    { 't', "[ok]", "Mesh view", "leader, router table and neighbours: thread.leader, thread.routers", "thread.routers" },
    { 't', "[ok]", "Battery nodes", "sleepy end devices poll their parent: thread.type sleepy 5000", "thread.type" },
    { 0, "[check]", "Self-healing", "power off the leader; another router takes over within about 2 minutes", NULL },
    { 0, "[limit]", "Size", "32 active routers per partition; end devices attach to a router", NULL },
    { 0, "[limit]", "Local only", "without a border router the IPv6 traffic stays inside the mesh", NULL },
};

static const ucRow_t kBorder[] = {
    { 't', "[ok]", "Commission", "the border router adds the PSKd as a joiner: thread.join <pskd>", "thread.join" },
    { 't', "[ok]", "Or dataset", "from the border router (ot-ctl dataset active -x): thread.dataset <hex>", "thread.dataset" },
    { 't', "[ok]", "Be found", "register with the border router: thread.srp add _sensor._udp 5683 sensor-1", "thread.srp" },
    { 't', "[ok]", "Find others", "DNS-SD over the mesh: thread.dns browse _sensor._udp", "thread.dns" },
    { 0, "[check]", "Border router", "Apple TV, HomePod, Nest Hub or OpenThread Border Router in Home Assistant", NULL },
    { 0, "[check]", "Matter", "plain Thread only; Matter commissioning is not covered", NULL },
};

static const ucRow_t kBuilding[] = {
    { 't', "[ok]", "One network", "every unit gets the same dataset on the line: thread.dataset <hex>", "thread.dataset" },
    { 0, "[ok]", "Baseline", "cfg.diff each unit against a reference unit", "cfg.diff" },
    { 't', "[ok]", "Roles", "mains-powered lights route (thread.type router), sensors sleep", "thread.type" },
    { 't', "[ok]", "Placement", "RSSI and link quality to each neighbour: thread.neighbors", "thread.neighbors" },
    { 't', "[ok]", "Health", "attach attempts, retries and errors per unit: thread.counters", "thread.counters" },
    { 0, "[limit]", "Routers", "32 active routers per partition; the rest stay end devices", NULL },
};

static const ucRow_t kGateway[] = {
    { 't', "[ok]", "Thread side", "sensors join the gateway's network: thread.network new, thread.start", "thread.start" },
    { 'w', "[ok]", "Wi-Fi side", "station on the site network: wifi.connect", NULL },
    { 't', "[ok]", "Find sensors", "sensors register over SRP, the gateway browses: thread.dns browse _sensor._udp", "thread.dns" },
    { 0, "[ok]", "To the cloud", "the host relays the readings: mqtt.publish site/sensor-1 21.5", "mqtt.publish" },
    { 0, "[check]", "Routing", "the module does not route IPv6 between Thread and Wi-Fi; the host relays", NULL },
    { 0, "[check]", "Module", "Wi-Fi and Thread on one module: ALMA-W1 with Thread firmware", NULL },
};
#endif

#ifndef UCXAPP_LITE
static const ucRow_t kSwarm[] = {
    { 't', "[ok]", "Mesh transport", "start with 5-10 devices using one Thread dataset; inspect thread.neighbors", "thread.neighbors" },
    { 't', "[ok]", "UDP exchange", "one bound UDP socket per node; exchange small timestamped position records", "sock.sendto" },
    { 0, "[check]", "Nearest peers", "edge host selects a few fresh nearby peers, not every node; radio neighbours are not positions", NULL },
    { 0, "[check]", "Neighbour DB", "edge-owned bounded table: node ID, frame, position, velocity, uncertainty, sequence, timestamp, expiry", NULL },
    { 0, "[check]", "CS ranging", "validate Channel Sounding initiator, reflector, firmware and antenna support; RSSI is not distance", "bt.cs" },
    { 0, "[check]", "Time basis", "shared time or measured clock offsets; reject replayed, duplicate and stale records", "app.time" },
    { 0, "[limit]", "Edge ownership", "ALMA edge ML device fuses IMU, GNSS and other sensors; ucxapp demonstrates transport only", NULL },
    { 0, "[check]", "Scale safely", "measure airtime and loss at 5, then 10 nodes before scaling; no flight-control guarantee", NULL },
};

static const ucRow_t kRecovery[] = {
    { 't', "[ok]", "Link health", "watch neighbours and counters while a node is powered off and returns", "thread.counters" },
    { 'g', "[check]", "GNSS health", "watch loss of fix and jamming/spoofing alarms; do not trust a flagged fix", "gnss.status" },
    { 0, "[ok]", "IMU input", "stream host sensor data to the external edge estimator", "imu.bridge" },
    { 0, "[check]", "Fallback", "edge estimator uses live peer positions and measured ranges; never substitute a peer's fix as its own", NULL },
    { 0, "[check]", "Observability", "enough independent anchors and valid geometry; common GNSS jamming can affect all peers", NULL },
    { 0, "[check]", "Confidence", "expire missing peers, grow uncertainty, flag estimated vs measured; stop claiming a fix when unobservable", NULL },
    { 0, "[check]", "Rejoin", "fresh sequence/session on return; reset stale state and blend valid local sensors back in", NULL },
    { 0, "[limit]", "Two recoveries", "Thread repairs connectivity; position recovery is a separate edge algorithm, not implemented here", NULL },
};

static const ucRow_t kHandover[] = {
    { 'w', "[ok]", "Indoor scan", "Wi-Fi scan fingerprints; a location service needs its API key and coverage", "wifi.locate" },
    { 0, "[check]", "Indoor ranging", "CS anchors and a surveyed coordinate frame need suitable ranging hardware", "bt.cs" },
    { 'g', "[check]", "Outdoor fix", "activate the receiver and verify a fresh valid GNSS fix before switching", "gnss.open" },
    { 0, "[check]", "Hysteresis", "edge host uses dwell time and confidence to avoid switching repeatedly at a doorway", NULL },
    { 0, "[check]", "Same frame", "survey indoor anchors into the GNSS frame; retain source, age and uncertainty", NULL },
    { 0, "[limit]", "Power policy", "receiver duty cycling and automatic source selection belong on the edge host", NULL },
};

static const ucRow_t kLivestock[] = {
    { 'g', "[check]", "Field position", "GNSS on a cow or lamb tag; indoor barn scan uses the handover scenario", "gnss.open" },
    { 0, "[ok]", "Geofence", "load a paddock polygon from KML; gnss.fence reports entering and leaving", "gnss.fence" },
    { 0, "[ok]", "Find the tag", "after an area event, advertise its tag ID for a nearby phone or gateway", "bt.adv" },
    { 0, "[check]", "Coded PHY", "long range: 125 kbit/s S8 links reach about 4x 1M (NORA-B26, ALMA-W1; not NORA-W36)", "at" },
    { 0, "[check]", "Notify", "a cellular modem and SIM send the alert and position to the farm server", "cell.connect" },
    { 0, "[check]", "Alert policy", "edge host applies accuracy margin, dwell time, event ID, rate limit and offline queue", NULL },
    { 0, "[limit]", "Automation", "steps demonstrate components; automatic fence-to-BLE/cellular actions are not implemented", NULL },
};
#endif

static const ucRow_t kBoat[] = {
    { 'g', "[check]", "GNSS receiver", "any NMEA receiver on a serial port: gnss.open finds port and baud", "gnss.open" },
    { 'w', "[ok]", "Onboard Wi-Fi", "the module is the access point: wifi.ap.enable (WPA2, max 5 tablets or phones)", NULL },
    { 0, "[ok]", "Chart app", "NMEA over TCP port 10110 (OpenCPN, Navionics, iNavX): gnss.bridge tcp 10110", "gnss.bridge" },
    { 0, "[ok]", "Phone over BLE", "NMEA over Nordic UART instead: gnss.bridge ble nus", "gnss.bridge" },
    { 0, "[ok]", "Trip log", "the raw receiver stream to a file: gnss.log trip.nmea", "gnss.log" },
    { 0, "[ok]", "Both at once", "tablet and phone together: both bridges run side by side (up to 4)", "gnss.bridge" },
};

static const ucRow_t kRtk[] = {
    { 'g', "[check]", "RTK receiver", "needs an RTK receiver (ZED-F9P, ZED-X20P); a plain GNSS gets metres, not cm", "gnss.open" },
    { 'w', "[ok]", "Home Wi-Fi", "the corrections come over the station link: wifi.connect", NULL },
    { 0, "[ok]", "NTRIP client", "RTCM or SPARTN from a caster to the receiver, GGA back every 10 s", "gnss.bridge" },
    { 0, "[check]", "Caster", "a base within about 20 km (RTK2go) or a PointPerfect plan", NULL },
    { 0, "[ok]", "Phone or robot", "the corrected NMEA over BLE: add ble to gnss.bridge ntrip", "gnss.bridge" },
};

static const ucRow_t kMower[] = {
    { 0, "[ok]", "Owner's phone", "first setup over BLE, the phone sends the home Wi-Fi: gatt.server.wps 120", "gatt.server.wps" },
    { 'w', "[ok]", "Garden Wi-Fi", "station on the home network: wifi.connect, wifi.profile.add for next time", NULL },
    { 'g', "[check]", "HP GNSS", "a high precision receiver (ZED-F9P, ZED-X20P) for cm; plain GNSS gets metres", "gnss.open" },
    { 0, "[ok]", "PointPerfect", "SPARTN over NTRIP: gnss.bridge ntrip <caster> 2101 NEAR-SPARTN <user> <pass>", "gnss.bridge" },
    { 0, "[ok]", "Phone status", "add ble: the corrected position to the owner's app over Nordic UART", "gnss.bridge" },
    { 0, "[check]", "Edge of garden", "corrections stop where Wi-Fi ends: add cell to gnss.bridge ntrip", "cell.connect" },
    { 0, "[ok]", "Theft alert", "position to your server over cellular: cell.connect, gnss.bridge cell", "gnss.bridge" },
    { 0, "[ok]", "Both at once", "NTRIP over Wi-Fi and the theft link run side by side (up to 4 bridges)", "gnss.bridge" },
    { 0, "[limit]", "One cell link", "the modem carries one socket: NTRIP over cell or the theft link, not both", NULL },
};

static const ucRow_t kFarm[] = {
    { 'g', "[check]", "HP GNSS", "a high precision receiver (ZED-F9P, ZED-X20P) for auto-steer at 2-3 cm", "gnss.open" },
    { 0, "[check]", "Field link", "no Wi-Fi in the field: a cellular modem and SIM, cell.open, cell.attach on", "cell.attach" },
    { 0, "[ok]", "PointPerfect", "NTRIP over cellular: gnss.bridge ntrip <caster> 2101 NEAR-SPARTN <user> <pass> cell", "gnss.bridge" },
    { 0, "[ok]", "Cab tablet", "add ble: the corrected NMEA to the guidance app over Nordic UART", "gnss.bridge" },
    { 0, "[ok]", "Field log", "the track to a file while driving: gnss.log field.nmea nmea", "gnss.log" },
    { 'w', "[ok]", "Farmyard Wi-Fi", "back at the yard: wifi.connect, then upload the log with tftp.put or ft.put", NULL },
    { 0, "[ok]", "Coverage", "the cellular link reconnects after a dead zone (1 s backoff, up to 30 s)", NULL },
};

static const ucRow_t kTracker[] = {
    { 'g', "[check]", "GNSS receiver", "open it first: gnss.open, then gnss.watch for the fix", "gnss.open" },
    { 'w', "[ok]", "At home", "Wi-Fi to your server: gnss.bridge tcpc <host> <port> (reconnects)", "gnss.bridge" },
    { 0, "[check]", "On the road", "a cellular modem (u-blox, Quectel, SIMCom, Telit, nRF91): cell.connect", "cell.connect" },
    { 0, "[ok]", "Server", "receives plain NMEA lines over TCP; any TCP listener or GPS logger app", NULL },
    { 0, "[limit]", "Power", "dev.power save 1 idles the module; the host and receiver still run all the time", "dev.power" },
};

#ifndef UCXAPP_LITE
static const ucRow_t kFieldKit[] = {
    { 'w', "[ok]", "Soft AP", "laptops and tablets join the module's own network: wifi.ap.enable (WPA2, max 5)", NULL },
    { 0, "[check]", "Cellular", "a modem with a SIM and a data plan: cell.open, cell.attach on", "cell.attach" },
    { 0, "[ok]", "Server", "one TCP or UDP socket over LTE: cell.connect <host> <port>", "cell.connect" },
    { 0, "[ok]", "Relay", "a laptop on the AP connects to <ap-ip>:7000, byte for byte to the server: cell.bridge tcp 7000",
      "cell.bridge" },
    { 0, "[ok]", "Phone, serial", "or relay a phone (cell.bridge ble nus) or a machine's port (cell.bridge uart <port>)",
      "cell.bridge" },
    { 0, "[ok]", "Modem console", "without cell.connect the same bridge gives the laptop the modem's AT console", "cell.bridge" },
    { 0, "[limit]", "Not a hotspot", "one socket is relayed, not IP: no web browsing or NAT, the two stacks are separate", NULL },
};

static const ucRow_t kBleCell[] = {
    { 0, "[ok]", "BLE link", "a phone app or BLE device on Nordic UART or u-blox SPS: cell.bridge ble nus", "cell.bridge" },
    { 0, "[check]", "Cellular", "a modem with a SIM and a data plan: cell.open, cell.attach on", "cell.attach" },
    { 0, "[ok]", "Server", "cell.connect <host> <port>; the phone's lines go to it and the replies come back", "cell.connect" },
    { 0, "[ok]", "No Wi-Fi", "works where there is no Wi-Fi and the phone has no data plan or roaming", NULL },
    { 0, "[check]", "Phone app", "a UART terminal app (nRF Connect, Serial Bluetooth Terminal) or your own app", NULL },
    { 0, "[limit]", "Links", "one BLE peer and one server socket on the modem", NULL },
};

static const ucRow_t kVehicle[] = {
    { 0, "[check]", "CAN interface", "Linux with SocketCAN (a CANable: can.open can0); Windows has an emulated loopback",
      "can.open" },
    { 0, "[check]", "Cellular", "a modem with a SIM and a data plan: cell.open, cell.attach on", "cell.attach" },
    { 0, "[ok]", "To your server", "17-byte binary records, up to 32 per write: cell.connect, then can.bridge cell",
      "can.bridge" },
    { 0, "[ok]", "Dead zones", "the data link reopens with backoff from 1 s up to 30 s; can.bridge status counts it",
      "can.bridge" },
    { 0, "[ok]", "Coverage", "signal and network along the route: cell.signal, cell.operator", "cell.signal" },
    { 0, "[limit]", "One cell link", "the modem carries one socket: CAN frames or GNSS positions, not both", NULL },
};

static const ucRow_t kXplr[] = {
    { 0, "[ok]", "SARA-R5", "LTE-M and NB-IoT: cell.open finds it and powers it through the NORA-B106 shell", "cell.open" },
    { 'g', "[check]", "MAX-M10S", "the kit's GNSS on its own port: gnss.open finds port and baud", "gnss.open" },
    { 0, "[ok]", "Position", "to your server over LTE: cell.connect <host> <port>, then gnss.bridge cell", "gnss.bridge" },
    { 0, "[ok]", "Power", "switch or reset the SARA-R5 without the button: cell.power on|off|cycle", "cell.power" },
    { 0, "[ok]", "Firmware", "cell.power on, EasyFlash on the SARA port, then cell.at AT+UFACTORY=0,2", "cell.power" },
    { 0, "[check]", "Wi-Fi relay", "this app drives NORA-W36 and NORA-B26, not the NINA-W156: add a NORA-W36 for fieldkit",
      NULL },
};
#endif

static const ucRow_t kHome[] = {
    { 'w', "[ok]", "Home Wi-Fi", "station on the home network: wifi.connect, wifi.profile.add for next time", NULL },
    { 0, "[ok]", "MQTT", "publish and subscribe to a local broker (Mosquitto in Home Assistant)", "mqtt.connect" },
    { 0, "[ok]", "Broker login", "user and password: mqtt.connect <host> user=<name> pass=${MQTT_PASS}", "mqtt.connect" },
    { 0, "[ok]", "Sensor state", "retained, so Home Assistant sees it at once: mqtt.publish home/door retain open", "mqtt.publish" },
    { 0, "[ok]", "Message size", "publish and subscribe up to 5000 bytes, binary (AT+UMQPB/AT+UMQRB); file= for big ones", NULL },
    { 0, "[ok]", "Sensor", "an IMU on the washing machine or door (imu.open, imu.status)", "imu.open" },
};

static const ucRow_t kProvision[] = {
    { 0, "[ok]", "BLE", "every NORA module: the phone connects without a password", NULL },
    { 'w', "[ok]", "Wi-Fi", "the phone sends SSID and password, the module joins", NULL },
    { 0, "[ok]", "Provisioning", "Nordic Wi-Fi Provisioning service (u-connectXplorer can provision): gatt.server.wps 120", "gatt.server.wps" },
    { 0, "[check]", "Phone app", "an app that writes the provisioning service (or a generic GATT app)", NULL },
    { 0, "[ok]", "QR instead", "or join the device's own AP from a QR code: wifi.ap.enable", NULL },
};

static const ucRow_t kHotel[] = {
    { 'w', "[ok]", "Guest Wi-Fi", "open or WPA2-Personal: wifi.connect <ssid> [password]", NULL },
    { 0, "[ok]", "Login page", "wifi.portal check finds it, wifi.portal login fills in and submits the form", "wifi.portal" },
    { 0, "[limit]", "Page types", "plain HTML forms; pages built by JavaScript or with a captcha need a browser", NULL },
    { 0, "[check]", "Session", "many portals tie the login to the MAC address and expire after a day", NULL },
    { 0, "[ok]", "Check", "wifi.portal check again, then net.ping", "net.ping" },
};

static const ucRow_t kNotify[] = {
    { 0, "[ok]", "iPhone", "Apple Notification Center Service: pair from Settings > Bluetooth, then ancs.sub", "ancs.on" },
    { 0, "[ok]", "Actions", "answer or decline a call, dismiss a message: ancs.action <uid> pos|neg", "ancs.action" },
    { 0, "[check]", "Android", "no ANCS: a companion app writes to the notify.on service", "notify.on" },
    { 0, "[limit]", "Links", "one phone at a time, notifications only while it is connected", NULL },
};

static const ucRow_t kRemote[] = {
    { 0, "[ok]", "Keyboard", "BLE HID keyboard, mouse and media keys: hid.setup", "hid.setup" },
    { 0, "[ok]", "Clicker", "next slide with hid.key space, back with hid.key backspace", "hid.key" },
    { 0, "[ok]", "Media", "volume and play/pause for a TV or phone: hid.media volup", "hid.media" },
    { 0, "[check]", "Host", "pair from the Bluetooth settings; HOGP works on Windows, macOS, iOS, Android", NULL },
    { 0, "[limit]", "Keys", "characters, enter, esc, tab, backspace and space; no arrow keys", NULL },
};

static const ucRow_t kFinder[] = {
    { 0, "[ok]", "Tag", "the item carries a NORA-B26/B27 as Channel Sounding reflector: bt.cs on", "bt.cs" },
    { 0, "[ok]", "Finder", "a second module as initiator measures the distance: bt.cs initiator, bt.cs.range", "bt.cs.range" },
    { 0, "[ok]", "Authentication", "CS runs only on an authenticated, encrypted link: bt.defaults secure, then bt.bond <conn>", "bt.bond" },
    { 0, "[ok]", "Phone", "a phone with Channel Sounding (Bluetooth 6.0) ranges the tag the same way", NULL },
    { 0, "[ok]", "Smoothing", "the module filters the distance; bt.cs.filter trades lag for steadiness", "bt.cs.filter" },
    { 0, "[check]", "Accuracy", "about 0.5 m indoors, CS on both ends; RSSI alone is not a distance", NULL },
};

#define CASE(id, title, rows) { id, title, rows, sizeof(rows) / sizeof(rows[0]) }
static const ucCase_t kCases[] = {
#ifndef UCXAPP_LITE
    CASE("iiot", "Factory machine: Enterprise Wi-Fi, TLS to the MES, machine data, firmware lifecycle", kIiot),
    CASE("medical", "Medical device (IEC 62304): secure BLE, test evidence, fixed configuration", kMedical),
    CASE("site", "Construction tools: technician's phone, tool tracking, site Wi-Fi, depot updates", kSite),
    CASE("implement", "Farm implement (ISOBUS/CAN): machine data to the cab tablet and the cloud", kImplement),
    CASE("condition", "Motor condition monitoring: vibration baseline, stream to analysis, alarms over MQTT", kCondition),
    CASE("agv", "AGV or mobile robot: roaming between hall APs, latency to the fleet manager", kAgv),
    CASE("eol", "End-of-line test: identity, golden settings, radio check, report per serial number", kEol),
    CASE("blegw", "BLE sensor gateway: retrofit wireless sensors to the plant broker", kBleGw),
    CASE("edge", "Edge AI sensor (ALMA-W1): training data, sensors, results to the cloud", kEdge),
    CASE("mesh", "Thread sensor mesh: form a network, join nodes, battery nodes, self-healing", kMesh),
    CASE("border", "Home Thread network: join through a border router, SRP and DNS-SD", kBorder),
    CASE("building", "Building lights and sensors: one dataset per line, roles, health", kBuilding),
    CASE("gateway", "Thread to cloud gateway (ALMA-W1): sensors over Thread, MQTT over Wi-Fi", kGateway),
    CASE("swarm", "Distributed location: 5-10 Thread nodes share nearest-neighbour observations", kSwarm),
    CASE("recovery", "Location recovery: sensor outage, stale peers and node rejoin", kRecovery),
    CASE("handover", "Indoor/outdoor location: Wi-Fi or CS anchors to GNSS", kHandover),
    CASE("livestock", "Livestock tracking: paddock geofence, BLE finding and cellular alerts", kLivestock),
#endif
    CASE("boat", "Boat or camper: GPS to the chart app on a tablet", kBoat),
    CASE("rtk", "Survey pole or rover: centimetre position with RTK", kRtk),
    CASE("mower", "Robotic lawn mower: owner's phone, HP GNSS with PointPerfect, cellular theft alert", kMower),
    CASE("farm", "Tractor or field robot: auto-steer with PointPerfect over cellular", kFarm),
    CASE("tracker", "Car, bike or pet tracker: position to your server", kTracker),
#ifndef UCXAPP_LITE
    CASE("fieldkit", "Field kit: laptops on the module's Wi-Fi reach your server over LTE", kFieldKit),
    CASE("blecell", "Phone or BLE device to your server over LTE, no Wi-Fi on site", kBleCell),
    CASE("vehicle", "Vehicle or machine: CAN data to your server over LTE", kVehicle),
    CASE("xplr", "XPLR-IOT-1 kit: SARA-R5 and MAX-M10S, position over LTE, modem firmware", kXplr),
#endif
    CASE("home", "Smart home sensor: MQTT to Home Assistant", kHome),
    CASE("provision", "New smart device: Wi-Fi set up from a phone over BLE", kProvision),
    CASE("hotel", "Hotel or cafe Wi-Fi with a login page", kHotel),
    CASE("notify", "Phone notifications on a small display or wearable", kNotify),
    CASE("remote", "Presentation clicker or media remote (BLE keyboard)", kRemote),
#ifndef UCXAPP_LITE
    CASE("finder", "Item finder: distance to a BLE tag with Channel Sounding", kFinder),
#endif
};
#define CASE_COUNT (sizeof(kCases) / sizeof(kCases[0]))

static int cmdCheck(ucxapp_t *app, int argc, char *argv[])
{
    const ucCase_t *uc = NULL;
    bool wifi = true, thread = true;
    char ssid[40] = "", ip[48] = "", gnss[64] = "";

    if (argc == 2 && strcmp(argv[1], "tool") == 0) {
        return cmdTool(app, 1, argv);
    }
    for (size_t i = 0; argc == 2 && i < CASE_COUNT; i++) {
        if (strcmp(argv[1], kCases[i].id) == 0) {
            uc = &kCases[i];
        }
    }
    if (uc == NULL) {
        if (argc == 2) {
            ucxapp_print("ERROR: no use case %s (see usecase.list)\n", argv[1]);
        }
        return UCXAPP_ERR_USAGE;
    }
    if (app->ucx != NULL) {
        wifi = ucxapp_module_has_wifi(app);
        thread = ucxapp_module_has_thread(app);
    }
    (void)ucxapp_gnss_summary(gnss, sizeof(gnss));
    ucxapp_print("%s\n", uc->title);
    for (size_t i = 0; i < uc->rowCount; i++) {
        const ucRow_t *r = &uc->rows[i];

        if (r->cmd != NULL && ucxapp_cmd_find(r->cmd) == NULL) {
            char how[96];

            snprintf(how, sizeof(how), "%s is not in this build; use Windows or Linux", r->cmd);
            row("[gap]", r->what, how);
        } else if (r->kind == 'w' && !wifi) {
            row("[gap]", r->what, "this module has no Wi-Fi (NORA-B2x); use a NORA-W36 or ALMA-W1");
        } else if (r->kind == 't' && !thread) {
            row("[gap]", r->what, "this module has no Thread firmware (see dev.caps)");
        } else if (r->kind == 'g' && gnss[0] != '\0') {
            char how[96];

            snprintf(how, sizeof(how), "open: %s", gnss);
            row("[ok]", r->what, how);
        } else {
            row(r->verdict, r->what, r->how);
        }
    }
    if (app->ucx == NULL) {
        ucxapp_print("Now: no module connected\n");
    } else if (wifi && ucxapp_wifi_get_status(app->ucx, ssid, sizeof(ssid), ip, sizeof(ip))) {
        ucxapp_print("Now: station on %s (%s)\n", ssid, ip);
    } else {
        ucxapp_print("Now: module connected, station not connected\n");
    }
    ucxapp_print("Steps: examples uc-%s, run one with examples uc-%s <number>\n", uc->id, uc->id);
    return UCXAPP_OK;
}

static int cmdList(ucxapp_t *app, int argc, char *argv[])
{
    (void)app;
    (void)argv;
    if (argc != 1) {
        return UCXAPP_ERR_USAGE;
    }
    size_t first = 0;

#ifndef UCXAPP_LITE
    /* kCases holds the sections in this order; tool is the first industry case */
    static const struct {
        const char *name;
        size_t count;
    } kSections[] = { { "Industry", 8 }, { "Edge AI", 1 }, { "Thread", 4 }, { "Location", 9 }, { "Cellular", 4 },
                    { "Everyday", 0 } };

    for (size_t s = 0; s < sizeof(kSections) / sizeof(kSections[0]); s++) {
        size_t end = kSections[s].count != 0 ? first + kSections[s].count : CASE_COUNT;

        ucxapp_print("%s%s:\n", s == 0 ? "" : "\n", kSections[s].name);
        if (s == 0) {
            ucxapp_print("%-10s%s\n", "tool", "Connected tool: factory Wi-Fi (Enterprise), AP for laptops, HTTPS, trays, BLE");
        }
        for (; first < end; first++) {
            ucxapp_print("%-10s%s\n", kCases[first].id, kCases[first].title);
        }
    }
    ucxapp_print("\n");
#else
    ucxapp_print("%-10s%s\n", "tool", "Connected tool: factory Wi-Fi (Enterprise), AP for laptops, HTTPS, trays, BLE");
#endif
    for (; first < CASE_COUNT; first++) {
        ucxapp_print("%-10s%s\n", kCases[first].id, kCases[first].title);
    }
    ucxapp_print("Fit check: usecase.check <name>; steps: usecase.example (tool) or examples uc-<name>\n");
    return UCXAPP_OK;
}

static const ucxapp_cmd_t gUsecaseCmds[] = {
    { "usecase.list",  "",          "Use cases with a fit check and ready-made steps", 0, cmdList },
    { "usecase.check", "<name>",    "Fit check of a use case against the module and this build; names from usecase.list", 0, cmdCheck },
    { "usecase.tool",  "[mbit/s]",  "Fit check of the connected tool use case; with a measured rate, the download time", 0, cmdTool },
};

void ucxapp_register_usecase_cmds(void)
{
    ucxapp_cmd_register(gUsecaseCmds, sizeof(gUsecaseCmds) / sizeof(gUsecaseCmds[0]));
}
