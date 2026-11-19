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


def _run(app_exe, tmp_path, *commands, fail=None):
    env = dict(os.environ, UCX_FAKE_DIR=str(tmp_path), UCXAPP_NRFUTIL=_tool("nrfutil"), UCXAPP_SSH=_tool("ssh"),
               UCXAPP_WIRESHARK=_tool("wireshark"))
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
    out, calls = _run(app_exe, tmp_path, "wlanpi.host 10.0.0.5", "wlanpi.start wifi.pcap channel=36 width=80 "
                      "\"filter=not port 22\"", "wlanpi.status", "app.wait 1500", "wlanpi.stop")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "Capturing on 10.0.0.5 wlan0 channel 36 80MHz into wifi.pcap" in out.stdout
    start, stop = calls.splitlines()[0], calls.splitlines()[1]
    assert "BatchMode=yes" in start and "wlanpi@10.0.0.5" in start
    assert "iw dev wlan0 set channel 36 80MHz" in start
    assert "tcpdump -i wlan0 -U -w - 'not port 22'" in start
    assert "sudo -n kill -INT 4242" in stop
    packets = _packets(tmp_path / "wifi.pcap")
    assert packets > 3 and f"wifi.pcap: {packets} packets" in out.stdout


def test_wlanpi_sudo_password_hint(app_exe, tmp_path):
    out, _ = _run(app_exe, tmp_path, "wlanpi.host pi", "wlanpi.start", fail="sudo")
    assert "did not start" in out.stdout and "a password is required" in out.stdout
    assert "Run wlanpi.setup" in out.stdout


def test_wlanpi_setup_and_status(app_exe, tmp_path):
    out, _ = _run(app_exe, tmp_path, "wlanpi.host pi", "wlanpi.status", "wlanpi.setup", fail="sudo")
    assert "type monitor" in out.stdout
    if "[ok] SSH key" in out.stdout:
        assert "[ok] key login" in out.stdout and "[--] sudo without a password" in out.stdout
    assert "NOPASSWD" in out.stdout and "Then run wlanpi.setup again" in out.stdout
