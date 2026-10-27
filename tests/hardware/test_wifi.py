"""Wi-Fi: scan, profiles, regulatory domain, access point, ping and link status."""

import os
import subprocess
import time

import pytest
from hwutil import _args, _assert_ping, _run, _wifi_connect_command

pytestmark = pytest.mark.wifi


@pytest.mark.hardware
def test_wifi_scan_finds_networks(app_exe, port_a):
    out = _run(app_exe, port_a, "wifi.scan")
    assert out.returncode == 0, out.stdout


@pytest.mark.hardware
def test_wifi_enterprise_settings_are_accepted(app_exe, port_a):
    """The module takes the EAP-TLS and PEAP settings; with no such network both time out cleanly."""
    eap = _run(app_exe, port_a, "wifi.eap ucxapp-no-such-net ca0 cert0 key0 dev@example 1.2", timeout=120)
    peap = _run(app_exe, port_a, "wifi.peap ucxapp-no-such-net alice secret12 ca0 1.3", timeout=120)
    for out in (eap, peap):
        assert out.returncode == 1, out.stdout
        assert "failed to set" not in out.stdout, out.stdout
        assert "timeout waiting for network up" in out.stdout, out.stdout
    again = _run(app_exe, port_a, _wifi_connect_command(), "wifi.status", timeout=180)
    assert again.returncode == 0, again.stdout


@pytest.mark.hardware
def test_wifi_profile_auto_falls_back_to_scan(app_exe, port_a, tmp_path):
    ssid, password = os.environ.get("UCX_WIFI_SSID"), os.environ.get("UCX_WIFI_PASS", "")
    if not ssid:
        pytest.skip("set UCX_WIFI_SSID (and UCX_WIFI_PASS) to run the Wi-Fi profile test")
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "ucxapp.wifilast").write_text("bogus")
    out = subprocess.run(
        _args(app_exe, port_a, "wifi.profile.add bogus no-such-network-xyz pw12345678",
              f"wifi.profile.add real {ssid} {password}", "wifi.profile.auto", "wifi.status"),
        capture_output=True, text=True, timeout=180, errors="replace", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    assert f"Strongest known network: 'real ({ssid})'" in out.stdout, out.stdout
    assert (tmp_path / "storage" / "ucxapp.wifilast").read_text() == "real"


@pytest.mark.hardware
def test_wifi_regdomain_set_and_read_back(app_exe, port_a, tmp_path):
    out = subprocess.run(_args(app_exe, port_a, "wifi.regdomain fcc", "wifi.regdomain"), capture_output=True, text=True,
                         timeout=60, errors="replace", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("regdomain=fcc") == 2, out.stdout
    assert (tmp_path / "storage" / "ucxapp.regdomain").read_text() == "fcc"
    restore = subprocess.run(_args(app_exe, port_a, "wifi.regdomain etsi"), capture_output=True, text=True, timeout=60,
                             errors="replace", cwd=tmp_path)
    assert "regdomain=etsi" in restore.stdout, restore.stdout


@pytest.mark.hardware
def test_access_point_enable_status_disable(app_exe, port_a):
    out = _run(app_exe, port_a, "dev.reboot")
    assert out.returncode == 0, out.stdout
    time.sleep(1)
    out = _run(app_exe, port_a, "wifi.ap.enable ucxtest-ap 12345678 6", "wifi.ap.status", "wifi.ap.disable",
               "dev.reboot")
    assert out.returncode == 0, out.stdout
    assert "ucxtest-ap" in out.stdout, out.stdout
    assert "0 station(s)" in out.stdout, out.stdout


@pytest.mark.hardware
def test_network_commands_over_wifi(app_exe, port_a):
    out = _run(app_exe, port_a, _wifi_connect_command(), "wifi.status", "net.dns example.com",
               "net.ping 8.8.8.8 2", "http.get http://example.com 64", "wifi.disconnect", timeout=180)
    assert out.returncode == 0, out.stdout
    assert "wifi: connected" in out.stdout, out.stdout
    assert "200" in out.stdout, out.stdout


@pytest.mark.two_devices
def test_wifi_ping_gateway_from_both_modules(app_exe, wifi_peers, port_a, port_b):
    for port in (port_a, port_b):
        _, gateway = wifi_peers[port]
        _assert_ping(_run(app_exe, port, f"net.ping {gateway} 4"), gateway)


@pytest.mark.two_devices
def test_wifi_ping_between_modules_both_directions(app_exe, wifi_peers, port_a, port_b):
    ip_a, ip_b = wifi_peers[port_a][0], wifi_peers[port_b][0]
    for source, target in ((port_a, ip_b), (port_b, ip_a)):
        _assert_ping(_run(app_exe, source, f"net.ping {target} 4"), target)


@pytest.mark.hardware
def test_status_commands_over_wifi(app_exe, port_a):
    out = _run(app_exe, port_a, _wifi_connect_command(), "dev.status", "wifi.rssi 2 1", "wifi.ip",
               "net.ntp status", "sock.list", "stats.show", timeout=180)
    assert out.returncode == 0, out.stdout
    assert "NTP client:" in out.stdout, out.stdout


@pytest.mark.hardware
def test_profiles_roaming_and_unbond(app_exe, port_a, tmp_path):
    out = subprocess.run(_args(app_exe, port_a, "wifi.roaming", "bt.unbond all", "bt.bonds",
                               "bt.profile.add ghost 11:22:33:44:55:66", "bt.profile.connect ghost"),
                         capture_output=True, text=True, timeout=120, errors="replace", cwd=tmp_path)
    assert "roaming" in out.stdout.lower(), out.stdout
    assert out.returncode != 0, "connecting to a missing device must fail: " + out.stdout
    ssid = os.environ.get("UCX_WIFI_SSID")
    if ssid:
        add = f"wifi.profile.add ucxtest {ssid} {os.environ.get('UCX_WIFI_PASS', '')}"
        out = subprocess.run(_args(app_exe, port_a, add, "wifi.profile.connect ucxtest", "wifi.status"),
                             capture_output=True, text=True, timeout=180, errors="replace", cwd=tmp_path)
        assert "wifi: connected" in out.stdout, out.stdout
