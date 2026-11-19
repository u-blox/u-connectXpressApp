"""sniff.* (nRF Sniffer through nrfutil) and wlanpi.* (tcpdump on a WLAN Pi over ssh) with stand-in tools
(tests/capture_fake.py): command lines, pcap files, live view and error hints; no sniffer or Pi needed."""

import os
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

