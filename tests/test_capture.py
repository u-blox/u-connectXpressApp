"""sniff.* (nRF Sniffer through nrfutil) and wlanpi.* (tcpdump on a WLAN Pi over ssh) with stand-in tools
(tests/capture_fake.py): command lines, pcap files, live view and error hints; no sniffer or Pi needed."""

import os
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.app

FAKE = Path(__file__).with_name("capture_fake.py")


def _tool(name):
    args = [sys.executable, str(FAKE), name]
    return subprocess.list2cmdline(args) if os.name == "nt" else " ".join(f"'{a}'" for a in args)


PASS = 'wlanpi.setup "secret pw"'


def _run(app_exe, tmp_path, *commands, fail=None, extra=None):
    env = dict(os.environ, UCX_FAKE_DIR=str(tmp_path), UCXAPP_NRFUTIL=_tool("nrfutil"), UCXAPP_SSH=_tool("ssh"),
               UCXAPP_WIRESHARK=_tool("wireshark"), **(extra or {}))
    env.pop("UCX_FAKE_FAIL", None)
    if fail:
        env["UCX_FAKE_FAIL"] = fail
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    out = subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=tmp_path,
                         stdin=subprocess.DEVNULL, env=env)
    calls = tmp_path / "calls.txt"
    return out, calls.read_text(encoding="utf-8") if calls.exists() else ""


def _packets(path):
    data = path.read_bytes()
    n, pos = 0, 24
    while pos + 16 <= len(data):
        pos += 16 + struct.unpack_from("<I", data, pos + 8)[0]
        n += 1
    return n


def test_sniff_start_stop_follow(app_exe, tmp_path):
    out, calls = _run(app_exe, tmp_path, "sniff.start ble.pcap port=COM99 follow=AA:BB:CC:DD:EE:FF adv rssi=-70",
                      "sniff.status", "app.wait 1500", "sniff.stop", "sniff.status")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "Sniffing on COM99 into ble.pcap, following AA:BB:CC:DD:EE:FF" in out.stdout
    line = calls.splitlines()[0]
    for part in ("ble-sniffer sniff", "--output-pcap-file", "ble.pcap", "--port", "COM99",
                 "--follow AA:BB:CC:DD:EE:FF", "--only-advertising", "--rssi-cut-off -70"):
        assert part in line, line
    packets = _packets(tmp_path / "ble.pcap")
    assert packets > 3
    assert f"ble.pcap: {packets} packets" in out.stdout
    assert out.stdout.rstrip().endswith("No capture running")


def test_sniff_follow_by_name_and_tool_failure(app_exe, tmp_path):
    out, calls = _run(app_exe, tmp_path, "sniff.start x.pcap port=COM99 follow=name:NORA coded", "app.wait 1500",
                      "sniff.status", fail="nrfutil")
    assert "--follow-by-name NORA" in calls and "--coded" in calls
    assert "nrfutil ended (exit code 2)" in out.stdout
    assert "could not open port" in out.stdout


def test_sniff_usage_errors(app_exe, tmp_path):
    for bad in ("sniff.start port=COM99 rssi=5", "sniff.start port=COM99 bogus=1", "sniff.view"):
        out, _ = _run(app_exe, tmp_path, bad)
        assert "usage: " + bad.split()[0] in out.stdout, out.stdout
    out, _ = _run(app_exe, tmp_path, "sniff.start port=COM99 follow=module")
    assert "follow=module needs a connected module" in out.stdout


def test_sniff_live_feeds_wireshark(app_exe, tmp_path):
    out, calls = _run(app_exe, tmp_path, "sniff.start live.pcap port=COM99 live", "app.wait 1500", "sniff.stop",
                      "app.wait 500")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "wireshark -k -i -" in calls
    assert (tmp_path / "wireshark.pcap").read_bytes() == (tmp_path / "live.pcap").read_bytes()


def test_wlanpi_needs_host_then_saves_it(app_exe, tmp_path):
    out, _ = _run(app_exe, tmp_path, "wlanpi.host", "wlanpi.start")
    assert "No WLAN Pi set" in out.stdout and "first: wlanpi.host" in out.stdout
    out, _ = _run(app_exe, tmp_path, "wlanpi.host 192.168.1.202 user=pi iface=wlan1", "wlanpi.host bad;host")
    assert "WLAN Pi: pi@192.168.1.202, capture interface wlan1" in out.stdout
    out, _ = _run(app_exe, tmp_path, "wlanpi.host")
    assert "WLAN Pi: pi@192.168.1.202, capture interface wlan1" in out.stdout


def test_wlanpi_capture_and_stop(app_exe, tmp_path):
    out, calls = _run(app_exe, tmp_path, "wlanpi.host 10.0.0.5", PASS, "wlanpi.start wifi.pcap channel=36 width=80 "
                      "\"filter=not port 22\"", "wlanpi.status", "app.wait 1500", "wlanpi.stop")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "Capturing on 10.0.0.5 wlan0 channel 36 80MHz into wifi.pcap" in out.stdout
    start, stop = calls.splitlines()[1], calls.splitlines()[2]
    assert "PreferredAuthentications=password" in start and "secret pw" not in calls and "wlanpi@10.0.0.5" in start
    assert "iw dev wlan0 set channel 36 80MHz" in start
    assert "tcpdump -i wlan0 -U -w - 'not port 22'" in start
    assert "sudo -n kill -INT 4242" in stop
    packets = _packets(tmp_path / "wifi.pcap")
    assert packets > 3 and f"wifi.pcap: {packets} packets" in out.stdout


def test_wlanpi_start_takes_a_bare_channel_number(app_exe, tmp_path):
    out, calls = _run(app_exe, tmp_path, "wlanpi.host 10.0.0.5", PASS, "wlanpi.start 11", "app.wait 1500",
                      "wlanpi.stop")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "Capturing on 10.0.0.5 wlan0 channel 11" in out.stdout
    assert "iw dev wlan0 set channel 11" in calls


def test_wlanpi_sudo_password_hint(app_exe, tmp_path):
    out, _ = _run(app_exe, tmp_path, "wlanpi.host pi", "wlanpi.setup")
    assert "No password saved: wlanpi.setup <WLAN Pi password>" in out.stdout
    out, _ = _run(app_exe, tmp_path, "wlanpi.start")
    assert "first: wlanpi.setup <password>" in out.stdout
    (tmp_path / "storage" / "ucxapp.wlanpi.key").unlink(missing_ok=True)
    out, _ = _run(app_exe, tmp_path, "wlanpi.host pi", PASS, "wlanpi.start", fail="sudo")
    assert "did not start" in out.stdout and "a password is required" in out.stdout
    assert "Run wlanpi.setup" in out.stdout


def test_wlanpi_setup_saves_the_password_sealed_and_adds_the_sudo_rule(app_exe, tmp_path):
    out, _ = _run(app_exe, tmp_path, "wlanpi.host pi", PASS, "wlanpi.status", fail="sudo")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "[ok] login to wlanpi@pi" in out.stdout and "[ok] sudo for tcpdump" in out.stdout
    assert "type monitor" in out.stdout
    assert (tmp_path / "sudo.txt").read_text() == "secret pw"
    assert b"secret pw" not in (tmp_path / "storage" / "ucxapp.wlanpi.key").read_bytes()
    out, _ = _run(app_exe, tmp_path, "wlanpi.setup")   # the saved password, the rule is there now
    assert out.returncode == 0 and "Ready:" in out.stdout
    out, _ = _run(app_exe, tmp_path, "wlanpi.setup wrong")
    assert out.returncode != 0 and "[--] login" in out.stdout and "Permission denied" in out.stdout


def _pcap(path, link, frames, big=False):
    e = ">" if big else "<"
    data = struct.pack(e + "IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, link)
    for i, f in enumerate(frames):
        data += struct.pack(e + "IIII", 100, i * 1000, len(f), len(f)) + f
    path.write_bytes(data)


def _ble_adv(addr, name, rssi):
    adv = bytes(reversed(addr)) + bytes([len(name) + 1, 0x09]) + name
    ll = struct.pack("<I", 0x8E89BED6) + bytes([0x40, len(adv)]) + adv + b"\0\0\0"
    hdr = bytes([0x0A, 0x01, 37, rssi, 0, 0, 0, 0, 0, 0])
    return bytes([0x50]) + struct.pack("<H", len(hdr) + len(ll)) + bytes([3, 0, 0, 2]) + hdr + ll


def _beacon(bssid, ssid, dbm):
    rt = struct.pack("<BBHI", 0, 0, 13, (1 << 3) | (1 << 5)) + struct.pack("<HH", 2437, 0) + struct.pack("<b", dbm)
    mac = bytes.fromhex(bssid.replace(":", ""))
    body = bytes(12) + bytes([0, len(ssid)]) + ssid
    return rt + struct.pack("<H", 0x80) + bytes(2) + b"\xff" * 6 + mac + mac + bytes(2) + body


def test_sniff_show_prints_lines_and_a_summary(app_exe, tmp_path):
    a, b = bytes.fromhex("C0FFEE000001"), bytes.fromhex("C0FFEE000002")
    _pcap(tmp_path / "b.pcap", 272, [_ble_adv(a, b"NORA-W36", 50), _ble_adv(b, b"Tag", 70),
                                     _ble_adv(a, b"NORA-W36", 40)], big=True)
    out, _ = _run(app_exe, tmp_path, "sniff.show b.pcap", "sniff.show b.pcap filter=nora count=1 skip=1",
                  "sniff.show b.pcap summary")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ch37  -50 dBm  ADV_IND         C0:FF:EE:00:00:01 r 'NORA-W36'" in out.stdout
    assert "3 of 3 packets shown" in out.stdout
    assert "1 of 3 packets shown, filter nora (skip=/count= for more" in out.stdout
    assert "0.002  ch37  -40 dBm" in out.stdout
    assert "C0:FF:EE:00:00:01 r          2      -40   37  NORA-W36" in out.stdout
    assert "2 devices in 3 packets" in out.stdout


def test_wlanpi_show_decodes_beacons_and_needs_a_file(app_exe, tmp_path):
    _pcap(tmp_path / "w.pcap", 127, [_beacon("02:11:22:33:44:55", b"office", -48),
                                     _beacon("02:11:22:33:44:66", b"", -70)])
    out, _ = _run(app_exe, tmp_path, "wlanpi.show w.pcap", "wlanpi.show w.pcap summary")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ch6    -48 dBm  Beacon            02:11:22:33:44:55 -> ff:ff:ff:ff:ff:ff 'office'" in out.stdout
    assert "'<hidden>'" in out.stdout
    assert "02:11:22:33:44:55            1      -48    6  office" in out.stdout
    (tmp_path / "x.pcap").write_bytes(b"not a pcap at all here....")
    for cmd, msg in (("wlanpi.show", "no capture yet"), ("wlanpi.show x.pcap", "is not a pcap file"),
                     ("wlanpi.show missing.pcap", "cannot open"), ("wlanpi.show count=x", None)):
        out, _ = _run(app_exe, tmp_path, cmd)
        assert out.returncode != 0
        if msg:
            assert msg in out.stdout



def _ble(aa, pdu, flags=0x01, channel=37, rssi=50):
    """Nordic BLE pseudo header (protocol 3) around one link layer packet: access address, PDU, 3 byte CRC."""
    ll = struct.pack("<I", aa) + pdu + b"\0\0\0"
    hdr = bytes([0x0A, flags, channel, rssi, 0, 0, 0, 0, 0, 0])
    return bytes([0x50]) + struct.pack("<H", len(hdr) + len(ll)) + bytes([3, 0, 0, 2]) + hdr + ll


def _data(llid, payload, flags=0x01):
    return _ble(0x50654321, bytes([llid, len(payload)]) + payload, flags=flags, channel=12)


ATT = {0x01: "ATT error", 0x02: "ATT MTU request", 0x03: "ATT MTU response", 0x04: "ATT find information",
       0x05: "ATT information", 0x08: "ATT read by type", 0x09: "ATT read by type response", 0x0A: "ATT read",
       0x0B: "ATT read response", 0x10: "ATT read by group type", 0x11: "ATT read by group type response",
       0x12: "ATT write request", 0x13: "ATT write response", 0x1B: "ATT notification", 0x1D: "ATT indication",
       0x1E: "ATT confirmation", 0x52: "ATT write command", 0x40: "ATT"}


def test_sniff_show_decodes_scan_extended_and_data_packets(app_exe, tmp_path):
    scanner, adv = bytes.fromhex("112233445566"), bytes.fromhex("C0FFEE000003")
    frames = [
        _ble(0x8E89BED6, bytes([0x43, 12]) + bytes(reversed(scanner)) + bytes(reversed(adv))),       # SCAN_REQ
        _ble(0x8E89BED6, bytes([0x47, 12, 7, 0x01]) + bytes(reversed(adv)) + bytes([3, 0xFF, 0x59, 0x00])),
        _ble(0x8E89BED6, bytes([0x40, 6]) + bytes(reversed(adv)), flags=0x00),                       # CRC error
        _data(0x01, b"", flags=0x03),
        _data(0x02, b"\x05\x00\x04\x00\x0a\x03\x00", flags=0x05),                                   # encrypted
        _data(0x03, bytes([0x0C, 0x0B, 0x59, 0x00, 0x01, 0x00])),
        _data(0x03, bytes([0x40, 0, 0, 0, 0, 0])),
        _data(0x01, b"\x01\x02\x03"),
        *[_data(0x02, struct.pack("<HHB", 3, cid, 0x02) + b"\0\0") for cid in (6, 5, 0x40)],
        *[_data(0x02, struct.pack("<HHB", 3, 4, op) + b"\0\0") for op in ATT],
    ]
    _pcap(tmp_path / "d.pcap", 272, frames)
    out, _ = _run(app_exe, tmp_path, "sniff.show d.pcap count=0")
    assert out.returncode == 0, out.stdout + out.stderr
    text = out.stdout
    assert re.search(r"SCAN_REQ\s+C0:FF:EE:00:00:03 <- 11:22:33:44:55:66 r", text), text
    assert re.search(r"ADV_EXT_IND\s+C0:FF:EE:00:00:03 r mfr 0x0059", text), text
    assert re.search(r"-50 dBm  CRC error  ADV_IND\s+C0:FF:EE:00:00:03", text), text
    assert re.search(r"data C->P\s+AA 50654321 empty", text), text
    assert re.search(r"data P->C\s+AA 50654321 encrypted 7 B", text), text
    assert "AA 50654321 VERSION_IND" in text and "AA 50654321 LL control" in text, text
    assert "AA 50654321 3 B" in text, text
    for name in ("SMP", "L2CAP signaling", "L2CAP"):
        assert f"AA 50654321 {name}\n" in text, text
    for name in ATT.values():
        assert f"AA 50654321 {name}\n" in text, text
    assert f"{len(frames)} of {len(frames)} packets shown" in text, text


def _radiotap(mhz, dbm=-40, fcs=False):
    """Radiotap with a second present word (bit 31), flags, channel and signal; FCS flagged when asked."""
    head = struct.pack("<BBH", 0, 0, 19) + struct.pack("<II", (1 << 31) | (1 << 1) | (1 << 3) | (1 << 5), 0)
    return head + bytes([0x10 if fcs else 0, 0]) + struct.pack("<HH", mhz, 0) + struct.pack("<b", dbm)


def _wifi(fc, body=b"", flags=0, mhz=2437, addr2="02:00:00:00:00:02", fcs=False):
    a1 = bytes.fromhex("020000000001")
    a2 = bytes.fromhex(addr2.replace(":", ""))
    frame = struct.pack("<BB", fc, flags) + bytes(2) + a1 + a2 + a2 + bytes(2) + body
    return _radiotap(mhz, fcs=fcs) + frame + (b"\xde\xad\xbe\xef" if fcs else b"")


def _eapol(info, nonce):
    key = bytes([2]) + struct.pack(">H", info) + bytes(2) + bytes(8) + nonce + bytes(16)
    return b"\xAA\xAA\x03\x00\x00\x00\x88\x8E" + bytes([2, 3]) + struct.pack(">H", len(key)) + key


def test_wlanpi_show_decodes_management_control_and_data_frames(app_exe, tmp_path):
    ssid = bytes([1, 1, 0x82, 0, 4]) + b"lab1"
    rts = struct.pack("<BB", 0xB4, 0) + bytes(2) + bytes.fromhex("020000000001") + bytes.fromhex("02000000000a")
    ack = struct.pack("<BB", 0xD4, 0) + bytes(2) + bytes.fromhex("020000000001")
    frames = [
        _wifi(0x00, bytes(4) + ssid, mhz=2484),                     # assoc request, channel 14
        _wifi(0x20, bytes(10) + ssid, mhz=5180),                    # reassoc request
        _wifi(0x40, bytes([0, 3, 0, 0, 0]), mhz=5975),              # probe request, hidden SSID, 6 GHz
        _wifi(0xA0, struct.pack("<H", 8), flags=0x08, mhz=900),     # disassoc, retry, unknown band
        _wifi(0xC0, struct.pack("<H", 3)),                          # deauth
        _wifi(0xB0, struct.pack("<HHH", 0, 2, 0)),                  # auth
        _wifi(0x10, struct.pack("<HHH", 0, 17, 1)),                 # assoc response
        _radiotap(2412) + rts, _radiotap(2412) + ack,
        _wifi(0x08, b"x" * 20, flags=0x40),                         # protected data
        _wifi(0x48, flags=0x08),                                    # null, retry
        _wifi(0xC8, bytes(4)),                                      # QoS null
        _wifi(0x88, bytes(6) + bytes(2) + bytes(4) + b"abc", flags=0x83, fcs=True),   # 4 addresses, QoS, HT
        _wifi(0x08, _eapol(0x008A, b"\x11" * 32)),
        _wifi(0x08, _eapol(0x010A, b"\x22" * 32)),
        _wifi(0x08, _eapol(0x01CA, b"\x33" * 32)),
        _wifi(0x08, _eapol(0x030A, bytes(32))),
        _radiotap(2412) + b"\x08\x00" + bytes(10),                  # too short for a data header
        _radiotap(2412)[:4] + b"\x01",                              # radiotap longer than the packet
        struct.pack("<BBHI", 0, 0, 10, 1 << 3) + bytes(6),          # channel cut off, frame too short
    ]
    _pcap(tmp_path / "w.pcap", 127, frames)
    _pcap(tmp_path / "e.pcap", 1, [bytes.fromhex("ffffffffffff020000000002") + b"\x08\x06" + bytes(28), b"\x01\x02"])
    _pcap(tmp_path / "u.pcap", 105, [b"abcd"])
    out, _ = _run(app_exe, tmp_path, "wlanpi.show w.pcap count=0", "wlanpi.show e.pcap", "wlanpi.show u.pcap")
    assert out.returncode == 0, out.stdout + out.stderr
    text = out.stdout
    expect = [r"ch14\s+-40 dBm  Assoc request\s+02:00:00:00:00:02 -> 02:00:00:00:00:01 'lab1'",
              r"ch36\s+-40 dBm  Reassoc request\s+.* 'lab1'", r"ch5\s+-40 dBm  Probe request\s+.* '<hidden>'",
              r"ch0\s+-40 dBm  Disassoc\s+.* reason 8 retry", r"Deauth\s+.* reason 3",
              r"Auth\s+.* seq 2 status 0", r"Assoc response\s+.* status 17",
              r"RTS\s+02:00:00:00:00:0a -> 02:00:00:00:00:01", r"ACK\s+ -> 02:00:00:00:00:01",
              r"Data\s+.* protected 20 B", r"Null\s+.* retry", r"QoS Null\s+.* 2 B\n",
              r"QoS Data\s+.* 3 B\n", r"EAPOL key 1/4", r"EAPOL key 2/4", r"EAPOL key 3/4", r"EAPOL key 4/4",
              r"31 B \(link type 127\)", r"14 B \(link type 127\)", r"5 B \(link type 127\)",
              r"02:00:00:00:00:02 -> ff:ff:ff:ff:ff:ff  type 0x0806  42 B", r"2 B \(link type 1\)",
              r"4 B \(link type 105\)"]
    for pattern in expect:
        assert re.search(pattern, text), f"{pattern}\n{text}"
