"""Hardware stress: repeated reboots, port reopen, socket churn, Wi-Fi cycles, BLE scans and TCP bursts.

Run with `.\\ucxapp --detect --test tests/hardware -m stress`; each test checks the module is still clean afterwards.
"""

import re

import pytest
from hwutil import _background, _finish, _reboot, _run, _wifi_connect_command

pytestmark = [pytest.mark.stress, pytest.mark.hardware]


def _assert_clean(exe, port, wifi=True):
    out = _run(exe, port, *(["sock.list"] if wifi else []), "bt.list", "dev.info")
    assert out.returncode == 0, out.stdout
    assert "Model:" in out.stdout, out.stdout
    if wifi:
        assert "0 socket(s)" in out.stdout, f"socket leak:\n{out.stdout}"
    assert "0 connection(s)" in out.stdout, f"link leak:\n{out.stdout}"


def test_port_reopen_50_times(app_exe, port_a):
    for i in range(50):
        out = _run(app_exe, port_a, "at +GMM", timeout=30)
        assert out.returncode == 0 and "NORA-" in out.stdout, f"run {i}:\n{out.stdout[-600:]}"


def test_500_commands_in_one_session(app_exe, port_a):
    out = _run(app_exe, port_a, *(["at +GMM", "dev.uptime", "bt.list"] * 167), timeout=600)
    assert out.returncode == 0, out.stdout[-1500:]
    assert out.stdout.count("NORA-") >= 167, out.stdout[-1500:]
    assert "ERROR" not in out.stdout, re.findall(r".*ERROR.*", out.stdout)[:5]


def test_reboot_10_times(app_exe, port_a):
    for i in range(10):
        out = _run(app_exe, port_a, "dev.reboot", "dev.uptime", timeout=60)
        assert "module rebooted" in out.stdout, f"reboot {i}:\n{out.stdout[-600:]}"
        assert re.search(r"^0 days 00:00:\d\d$", out.stdout, re.M), f"reboot {i}:\n{out.stdout[-600:]}"
    _assert_clean(app_exe, port_a, wifi=False)


def test_ble_scan_10_times(app_exe, port_a):
    for i in range(10):
        out = _run(app_exe, port_a, "bt.scan", timeout=60)
        assert out.returncode == 0, f"scan {i}:\n{out.stdout[-600:]}"
        assert "ERROR" not in out.stdout, f"scan {i}:\n{out.stdout[-600:]}"
    _assert_clean(app_exe, port_a, wifi=False)


@pytest.mark.wifi
def test_wifi_connect_disconnect_10_cycles(app_exe, port_a):
    wifi = _wifi_connect_command()
    _reboot(app_exe, port_a)
    for i in range(10):
        out = _run(app_exe, port_a, wifi, "wifi.status", "wifi.disconnect", timeout=180)
        assert out.returncode == 0, f"cycle {i}:\n{out.stdout[-800:]}"
        assert re.search(r"ip=\d+\.\d+\.\d+\.\d+", out.stdout), f"cycle {i}:\n{out.stdout[-800:]}"
    _assert_clean(app_exe, port_a)


@pytest.mark.wifi
def test_socket_open_close_churn(app_exe, port_a):
    wifi = _wifi_connect_command()
    _reboot(app_exe, port_a)
    churn = [c for _ in range(40) for c in ("sock.open udp", "sock.close 0")]
    churn += [c for _ in range(20) for c in ("sock.open tcp", "sock.close 0")]
    out = _run(app_exe, port_a, wifi, *churn, "sock.list", "wifi.disconnect", timeout=300)
    assert out.returncode == 0, out.stdout[-1500:]
    assert "ERROR" not in out.stdout, re.findall(r".*ERROR.*", out.stdout)[:5]
    assert "0 socket(s)" in out.stdout, f"socket leak:\n{out.stdout[-800:]}"


@pytest.mark.two_devices
@pytest.mark.wifi
@pytest.mark.parametrize("proto", ["udp", "tcp"])
def test_echo_burst_between_two_modules(app_exe, wifi_peers, port_a, port_b, proto):
    """B sends 100 numbered messages to A's echo server; every one must come back."""
    ip_a = wifi_peers[port_a][0]
    count = 100
    proc, seen, lines = _background(app_exe, port_a, f"sock.echo {proto} 7011 120", ready="echo on port")
    try:
        if proto == "udp":
            setup = ["sock.open udp", "sock.bind 0 7012"]
            burst = [c for i in range(count) for c in (f"sock.sendto 0 {ip_a} 7011 msg{i:04d}",
                                                        "sock.recvfrom 0 64 5")]
        else:
            setup = ["sock.open tcp", f"sock.connect 0 {ip_a} 7011"]
            burst = [c for i in range(count) for c in (f"sock.send 0 msg{i:04d}", "sock.read 0 64 5")]
        out = _run(app_exe, port_b, *setup, *burst, "sock.close 0", timeout=600)
    finally:
        _finish(proc, seen, lines, stop=True)
    assert out.returncode == 0, out.stdout[-1500:]
    got = set(re.findall(r"received \d+ bytes.*?msg(\d{4})", out.stdout))
    missing = sorted({f"{i:04d}" for i in range(count)} - got)
    assert not missing, f"{len(missing)} lost: {missing[:10]}\n{out.stdout[-800:]}"
