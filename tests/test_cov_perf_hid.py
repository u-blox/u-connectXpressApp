"""perf.*, hid.* and the advertising decoder (ucxapp_adv.c) against the virtual module: echo runs over TCP, UDP
and SPS, a HID device whose host writes protocol mode, LEDs and the control point, a HID host decoding keyboard,
mouse, media and vendor reports from a report map, and scan/advertising payloads with every AD type."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.app

PEER = "C0:FF:EE:00:11:22"


def _module(tmp_path, yaml_text=None, profile=None):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    if yaml_text is not None:
        path = tmp_path / "profile.yaml"
        path.write_text("include: [nora-w36.yaml]\n" + yaml_text, encoding="utf-8")
        profile = str(path)
    module = Vucx(profile or "nora-w36.yaml")
    module.start()
    return module


def _shell(app_exe, module, commands, cwd, timeout=60):
    try:
        return subprocess.run([str(app_exe), module.port, "--shell"], input="\n".join(commands) + "\n",
                              capture_output=True, text=True, timeout=timeout, errors="replace", cwd=cwd)
    except subprocess.TimeoutExpired as e:
        text = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"hung after {timeout} s:\n{text[-3000:]}\nlast AT: {module.log[-15:]}")


def _exec(app_exe, module, command, cwd, timeout=60):
    return subprocess.run([str(app_exe), module.port, "--exec", command], capture_output=True, text=True,
                          timeout=timeout, errors="replace", stdin=subprocess.DEVNULL, cwd=cwd)


# ---- perf ----------------------------------------------------------------------------------------------------


def test_perf_latency_and_throughput_over_tcp_and_udp(app_exe, tmp_path):
    module = _module(tmp_path)
    try:
        out = _shell(app_exe, module, ["perf.latency tcp 192.168.1.10 7 4 64",
                                       "perf.latency udp 192.168.1.10 7 4 100",
                                       "perf.throughput tcp 192.168.1.10 7 1 200",
                                       "perf.throughput udp 192.168.1.10 7 1 100"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "RTT over TCP, 4 packets of 64 bytes" in text and "RTT over UDP, 4 packets of 100 bytes" in text, text
    assert text.count("4 sent, 4 received, 0 lost (0%), 0 corrupt") == 2, text
    assert text.count("rtt min ") == 2, text
    assert "throughput over TCP for 1 s, 200-byte packets" in text, text
    assert "throughput over UDP for 1 s, 100-byte packets" in text, text
    assert text.count("pattern 0x00-0xFF: 0 breaks") == 2, text


def test_perf_over_sps_to_the_connected_peer(app_exe, tmp_path):
    module = _module(tmp_path)
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "perf.latency sps 3 20", "perf.throughput sps 1 50"],
                     tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "AT+USPSWB=0" in module.log, module.log[-20:]
    assert "RTT over SPS, 3 packets of 20 bytes" in text and "3 sent, 3 received" in text, text
    assert "throughput over SPS for 1 s, 50-byte packets" in text, text


def test_perf_sps_needs_a_connection_and_the_sps_service(app_exe, tmp_path):
    module = _module(tmp_path, 'commands:\n  "AT+USPSRM=*": ["ERROR"]\n')
    try:
        out = _shell(app_exe, module, ["perf.latency sps", f"bt.connect {PEER}", "perf.throughput sps 1"], tmp_path)
    finally:
        module.stop()
    assert out.stdout.count("ERROR: cannot set SPS buffered mode") == 2, out.stdout


def test_perf_sps_that_never_connects(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("nora-w36.yaml")
    del profile["sockets"]   # USPSC then comes from the command table: OK without +UESPSC
    module = _module(tmp_path, profile=profile)
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "perf.latency sps 1"], tmp_path)
    finally:
        module.stop()
    assert "ERROR: SPS did not connect on connection 0" in out.stdout, out.stdout


def test_perf_without_an_echo_reports_loss(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("nora-w36.yaml")
    profile["sockets"] = {"servers": {9: "sink"}}
    module = _module(tmp_path, profile=profile)
    try:
        out = _shell(app_exe, module, ["perf.latency udp 192.168.1.10 9 2 10",
                                       "perf.throughput udp 192.168.1.10 9 1 10"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "packet 0: no echo within 1000 ms" in text and "2 sent, 0 received, 2 lost (100%)" in text, text
    assert "echoed 0 bytes" in text and "100.0% not echoed" in text, text


def test_perf_socket_failures(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("nora-w36.yaml")
    del profile["sockets"]
    profile["commands"].update({"AT+USOCR=17*": ["ERROR"], "AT+USOCR=6*": ["+USOCR:0", "OK"],
                                "AT+USOC=*": ["ERROR"], "AT+UDNSRN=*": ["ERROR"]})
    module = _module(tmp_path, profile=profile)
    try:
        out = _shell(app_exe, module, ["perf.latency udp 192.168.1.10 7", "perf.throughput tcp 192.168.1.10 7",
                                       "perf.latency udp no.such.host 7"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert text.count("ERROR: send failed at packet 0") == 2, text
    assert "ERROR: cannot connect to 192.168.1.10:7" in text, text


def test_perf_sps_echo_returns_what_the_peer_sends(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("nora-w36.yaml")
    del profile["sockets"]
    profile["commands"].update({"AT+USPSRB=*": [["+USPSRB:0\x01hello", "OK"], ["+USPSRB:0\x01", "OK"]],
                                "AT+USPSWB=*": ["+USPSWB:0,5", "OK"]})
    profile["urcs"]["AT+USPSRM=1"] = {"after": 0.3, "lines": ["+UESPSDA:0,5"]}
    module = _module(tmp_path, profile=profile)
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "perf.echo sps 2", "perf.echo sps 1 2",
                                       "perf.echo sps x"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "AT+USPS=1" in module.log, module.log[-20:]
    assert "SPS echo for 2 s" in text and re.search(r"echoed [1-9]\d* bytes", text), text
    assert any(c.startswith("AT+USPSWB=0") for c in module.log), module.log[-20:]
    assert text.lower().count("usage") >= 2, text


def test_perf_sps_echo_without_the_service(app_exe, tmp_path):
    module = _module(tmp_path, 'commands:\n  "AT+USPS=*": ["ERROR"]\n')
    try:
        out = _exec(app_exe, module, "perf.echo sps 1", tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "ERROR: cannot enable the SPS service" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["perf.latency sps 1 2 3", "perf.latency sps 0", "perf.latency sps 1 5",
                                     "perf.throughput sps 0", "perf.throughput sps 1 2000",
                                     "perf.latency udp h 70000", "perf.throughput ftp h 7",
                                     "perf.latency tcp h 7 1 64 9", "perf.echo tcp 7000 1 2 3"])
def test_perf_bad_arguments(app_exe, tmp_path, command):
    module = _module(tmp_path)
    try:
        out = _exec(app_exe, module, command, tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout
    assert not any(c.startswith(("AT+USOCR", "AT+USPS")) for c in module.log), module.log


# ---- hid device ----------------------------------------------------------------------------------------------

HID_HOST_WRITES = """urcs:
  "AT+UBTGSA":
    - "+UEBTC:0,C0FFEE001122p"
    - "+UEBTGCW:0,{c:2A22},0100,0"
    - "+UEBTGCW:0,{c:2A33},0100,0"
    - "+UEBTGCW:0,{h:2A4E},00,0"
    - "+UEBTGCW:0,{h:2A32},07,0"
    - "+UEBTGCW:0,{h:2A4C},00,0"
    - "+UEBTGCW:0,{h:2A4C},01,0"
"""


def test_hid_device_with_a_boot_protocol_host(app_exe, tmp_path):
    module = _module(tmp_path, HID_HOST_WRITES)
    try:
        out = _shell(app_exe, module, [
            "hid.key a", "hid.status", "hid.setup", "hid.setup", "hid.setup auth", "hid.status",
            "hid.key enter", "hid.key esc", "hid.key backspace", "hid.key tab", "hid.key space ctrl alt gui shift",
            "hid.key Z", "hid.text 0 .,-/!@?:_ Ab9", "hid.text a~", "hid.text " + "x" * 210,
            "hid.mouse 300 -300 5", "hid.click right", "hid.click middle double",
            "hid.media play", "hid.media search"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "ERROR: HID not set up (run hid.setup)" in text and "hid: not set up" in text, text
    assert "HID service active: keyboard, mouse and media keys" in text, text
    assert "HID service is already active" in text, text
    assert "restart the module to define it again" in text, text
    assert "[HID] protocol mode: boot" in text, text
    assert "[HID] keyboard LEDs 0x07 (num=1 caps=1 scroll=1)" in text, text
    assert "[HID] control point: suspend" in text and "[HID] control point: exit suspend" in text, text
    assert "hid: service " in text and "protocol boot" in text, text
    assert text.count("key sent") == 6, text
    assert "typed 15 character(s)" in text, text
    assert "ERROR: no key mapping for '~'" in text and "ERROR: text too long" in text, text
    assert "mouse moved" in text and text.count("clicked") == 2, text
    assert "host connected without a subscribe event; sending anyway" in text, text
    assert "play sent" in text and "search sent" in text, text
    boot_mouse = [c for c in module.log if c.startswith("AT+UBTGNS=0,") and len(c.split(",")[-1]) == 6]
    assert boot_mouse, module.log[-30:]


def test_hid_reports_fail_when_notifications_are_refused(app_exe, tmp_path):
    module = _module(tmp_path, 'commands:\n  "AT+UBTGNS=*": ["ERROR"]\n' + HID_HOST_WRITES)
    try:
        out = _shell(app_exe, module, ["hid.setup", "hid.key a", "hid.text ab", "hid.mouse 1 1", "hid.click",
                                       "hid.media mute"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert text.count("report notification failed") >= 5, text
    assert "key sent" not in text and "mouse moved" not in text and "clicked" not in text, text


@pytest.mark.parametrize("command", ["hid.setup now", "hid.key", "hid.key ab", "hid.key ~", "hid.key a meta",
                                     "hid.text", "hid.mouse 1", "hid.mouse 1 x", "hid.mouse 20000 0",
                                     "hid.mouse 0 0 200", "hid.click up", "hid.media", "hid.media louder",
                                     "hid.client", "hid.client x", "hid.client 0 legacy"])
def test_hid_bad_arguments(app_exe, tmp_path, command):
    module = _module(tmp_path)
    try:
        out = _exec(app_exe, module, command, tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


def test_hid_setup_failures(app_exe, tmp_path):
    for bad, message in (('"AT+UBTGS=1812": ["ERROR"]', "ERROR: cannot define HID service"),
                         ('"AT+UBTGC=2A4A*": ["ERROR"]', "ERROR: cannot add characteristic 0x2A4A"),
                         ('"AT+UBTGSA": ["ERROR"]', "ERROR: cannot activate HID service")):
        module = _module(tmp_path, f"commands:\n  {bad}\n")
        try:
            out = _exec(app_exe, module, "hid.setup", tmp_path)
        finally:
            module.stop()
        assert out.returncode != 0 and message in out.stdout, (bad, out.stdout)


# ---- hid host ------------------------------------------------------------------------------------------------

REPORT_MAP = (
    "FE0000"                                                              # a long item, skipped
    "05010906A1018501"                                                    # keyboard, report 1
    "050719E029E715002501750195088102"                                    # modifiers
    "950175088101"                                                        # reserved
    "950675081500256505071900296581" "00"                                 # 6 keys
    "C0"
    "05010902A1018502" "0901A100"                                         # mouse, report 2
    "0509190129031500250195037501" "8102"                                 # 3 buttons
    "950175058101"                                                        # padding
    "05010930093109381581257F750895038106"                                # x, y, wheel
    "C0C0"
    "0B01000C00A1018503050C150026FF031900" "2AFF03751095018100C0"     # media, report 3 (4-byte usage)
    "0600FF0901A1018504150026FF0075089502" "09018102C0"                   # vendor, report 4
)

# handles from 100: service 100, map 102, protocol mode 104, keyboard 106 (CCCD 107), mouse 110 (111),
# media 114 (115), vendor 118 (119), output report 122, boot keyboard 126 (127), boot mouse 129 (130)
HID_PEER = """peer:
  start: 100
  services:
    - uuid: "1812"
      chars:
        - ["2A4B", {map_props}, "{map}"]
        - ["2A4E", R w, "01"]
        - ["2A4D", R N, "0000000000000000", {{"2908": "0101"}}]
        - ["2A4D", R N, "00000000", {{"2908": "0201"}}]
        - ["2A4D", R N, "0000", {{"2908": "0301"}}]
        - ["2A4D", R N, "0000", {{"2908": "0401"}}]
        - ["2A4D", R w N, "00", {{"2908": "0502"}}]
        - ["2A22", R N, "0000000000000000"]
        - ["2A33", R N, "000000"]
urcs:
  "AT+UBTGCCW=0,119,1":
    after: 0.4
    lines:
      - "+UEBTGCN:0,106,0300041E27282D3A"
      - "+UEBTGCN:0,106,0000045250460000"
      - "+UEBTGCN:0,106,0000000000000000"
      - "+UEBTGCN:0,110,05F60AFF"
      - "+UEBTGCN:0,114,E900"
      - "+UEBTGCN:0,114,3412"
      - "+UEBTGCN:0,114,0000"
      - "+UEBTGCN:0,118,ABCD"
  "AT+UBTGCCW=0,130,1":
    after: 0.4
    lines:
      - "+UEBTGCN:0,126,0200050000000000"
      - "+UEBTGCN:0,129,01FE0302"
"""


def _hid_peer(map_props="R"):
    return HID_PEER.format(map_props=map_props, map=REPORT_MAP)


def test_report_map_is_well_formed():
    data = bytes.fromhex(REPORT_MAP)
    assert len(data) < 512 and data[:3] == b"\xfe\x00\x00"


def test_hid_client_decodes_report_protocol_input(app_exe, tmp_path):
    module = _module(tmp_path, _hid_peer())
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "hid.client 0", "gatt.listen 2", "hid.client off"],
                     tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "report map: " in text and "4 report ID(s)" in text, text
    assert any(c.endswith(",104,01") for c in module.log), module.log
    assert "keyboard input, report ID 1" in text and "mouse input, report ID 2" in text, text
    assert "media input, report ID 3" in text and "other input, report ID 4" in text, text
    assert "[HID] keys: ctrl+ shift+ a 1 0 enter - F1" in text, text
    assert "[HID] keys: up left 0x46" in text, text
    assert "[HID] mouse: buttons LM- dx -10 dy 10 wheel -1" in text, text
    assert "[HID] media: volup (0x00E9)" in text and "[HID] media: usage (0x1234)" in text, text
    assert "[HID] report 4: AB CD" in text, text
    assert "HID host stopped" in text and "AT+UBTGCCW=0,107,0" in module.log, module.log[-20:]


def test_hid_client_boot_protocol(app_exe, tmp_path):
    module = _module(tmp_path, _hid_peer())
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "hid.client 0 boot", "gatt.listen 2",
                                       "hid.client 0 boot"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "keyboard input (boot)" in text and "mouse input (boot)" in text, text
    assert "[HID] keys: shift+ b" in text, text
    assert "[HID] mouse: buttons L-- dx -2 dy 3 wheel 2" in text, text
    assert "HID host stopped" in text, text


def test_hid_client_bonds_when_the_map_read_is_refused(app_exe, tmp_path):
    peer = _hid_peer("w") + ('commands:\n  "AT+UBTGR=0,102": [["ERROR"], ["+UBTGR:0,102,' + REPORT_MAP +
                             '", "OK"]]\n')
    module = _module(tmp_path, peer)
    try:
        out = _shell(app_exe, module, [f"bt.connect {PEER}", "hid.client 0", "hid.client off"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert "read refused, bonding for an encrypted link..." in text, text
    assert any(c.startswith("AT+UBTB=") for c in module.log), module.log[-20:]
    assert "4 report ID(s)" in text, text


# ---- advertising data ----------------------------------------------------------------------------------------

ADV_PAYLOADS = {
    "02011F" "05020F18AAAA" "050578563412" "020AF4" "0319C103" "0408414243": [
        "Flags: 0x1F [limited discoverable] [general discoverable] [no BR/EDR] [LE and BR/EDR]",
        "Some 16-bit services:", "0x180F (", "0xAAAA", "32-bit services:", "0x12345678", "TX power: -12 dBm",
        "Appearance: 0x03C1", "Short name: ABC"],
    "1106" + "00112233445566778899AABBCCDDEEFF" + "062415612E6201" + "0330AABB": [
        "Some 128-bit services:", "FFEEDDCC-BBAA-9988-7766-554433221100", "URI: https:a.b.",
        "Type 0x30 (2 bytes): AA BB"],
    "1416AAFE" + "000102030405060708090A0B0C0D0E0F10" + "07FF4C0002150102": [
        "Service data 0xFEAA", "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F ...",
        "Manufacturer 0x004C", "02 15 01 02"],
    "020106" "03030D18" "1107" "00112233445566778899AABBCCDDEEFF" "0304ABCD": [
        "16-bit services:", "128-bit services:", "Some 32-bit services:"],
}


def test_bt_adv_decodes_every_ad_type(app_exe, tmp_path):
    module = _module(tmp_path)
    try:
        out = _shell(app_exe, module, [f"bt.adv {p} broadcast" for p in ADV_PAYLOADS] + ["bt.adv 020100"], tmp_path)
    finally:
        module.stop()
    text = out.stdout
    for payload, expected in ADV_PAYLOADS.items():
        assert f"AT+UBTADL={payload},2" in module.log, (payload, module.log[-10:])
        for item in expected:
            assert item in text, (item, text)
    assert "Flags: 0x00" in text, text


def test_bt_scan_summaries_and_names_from_advertising(app_exe, tmp_path):
    devices = [
        '+UBTD:C0FFEE0000A1p,-40,"",0,0201060B030F180A180D18AAAABBBB07FF4C00021501020319C103',
        '+UBTD:C0FFEE0000A2p,-45,"",0,04084142430509444546470408585858',
        '+UBTD:C0FFEE0000A3p,-50,"eddy",0,0516AAFE001005FFFEFF0102',
        '+UBTD:C0FFEE0000A4p,-55,"",0,0408535054',
        "OK",
    ]
    module = _module(tmp_path, "commands:\n  \"AT+UBTD*\":\n" + "".join(f"    - '{d}'\n" for d in devices))
    try:
        out = _exec(app_exe, module, "bt.scan full", tmp_path)
    finally:
        module.stop()
    text = out.stdout
    assert out.returncode == 0, text
    assert "iBeacon" in text and " ..." in text, text
    assert "0xAAAA" in text, text
    assert re.search(r"DEFG\s", text) and re.search(r"SPT\s", text), text
    assert "Complete name: DEFG" in text and "Manufacturer 0xFFFE" in text, text
