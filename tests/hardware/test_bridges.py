"""Stream bridges between the two modules: GNSS/IMU over TCP, TLS and BLE, and CAN records over UDP."""

import re
import subprocess
import time

import pytest
from hwutil import (
    SOURCES,
    _args,
    _background,
    _check_stream,
    _finish,
    _join_wifi,
    _run,
    _start_bridge,
    _stop,
    _wifi_connect_command,
)


@pytest.mark.two_devices
@pytest.mark.parametrize("source", SOURCES)
def test_bridge_over_wifi_between_two_modules(app_exe, port_a, port_b, source, request):
    """A bridges the source over TCP on Wi-Fi; B connects a socket and reads the stream."""
    wifi = _wifi_connect_command()
    proc, seen = _start_bridge(app_exe, port_a, source, request.getfixturevalue(source), [wifi],
                               f"{source}.bridge tcp 2947")
    try:
        match = re.search(r"connect a TCP client to ([\d.]+):2947", "".join(seen))
        assert match, "".join(seen)
        out = _run(app_exe, port_b, wifi, "sock.open tcp", f"sock.connect 0 {match.group(1)} 2947",
                   "sock.read 0 400 10", "sock.close 0", timeout=240)
    finally:
        _stop(proc)
    assert out.returncode == 0, out.stdout
    _check_stream(source, out.stdout)


@pytest.mark.two_devices
@pytest.mark.parametrize("source", SOURCES)
@pytest.mark.parametrize("clientauth", [False, True], ids=["server-auth", "mutual-auth"])
def test_secure_bridge_over_wifi_between_two_modules(app_exe, port_a, port_b, source, clientauth, request, tmp_path):
    """A serves the source over TLS with a certificate from the app's local CA; B trusts that CA and reads."""
    wifi = _wifi_connect_command(tmp_path)
    ip_a, _ = _join_wifi(app_exe, port_a, wifi)
    out = subprocess.run(_args(app_exe, port_a, "utils.tls.ca.create", f"utils.tls.server.create server {ip_a}",
                               "utils.tls.client.create rover"), capture_output=True, text=True, timeout=60,
                         errors="replace", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    bridge = f"{source}.bridge tls 2947 server" + (" clientauth" if clientauth else "")
    proc, _ = _start_bridge(app_exe, port_a, source, request.getfixturevalue(source),
                            [wifi], bridge, cwd=tmp_path)
    store = tmp_path / "storage"
    upload = [f"cert.upload ca {store / 'tls-ca.crt'} ucxtestca"]
    tls = "sock.tls ucxtestca"
    if clientauth:
        upload += [f"cert.upload client {store / 'tls-rover.crt'} ucxtestrover",
                   f"cert.upload key {store / 'tls-rover.key'} ucxtestroverkey"]
        tls += " ucxtestrover ucxtestroverkey"
    try:
        _run(app_exe, port_b, "cert.remove ucxtestca", "cert.remove ucxtestrover", "cert.remove ucxtestroverkey")
        out = _run(app_exe, port_b, wifi, *upload, "sock.open tcp", tls,
                   f"sock.connect 0 {ip_a} 2947", "sock.read 0 400 15", "sock.close 0", timeout=240)
        if clientauth:
            # B without a client certificate must be refused by A
            refused = _run(app_exe, port_b, wifi, "sock.open tcp", "sock.tls ucxtestca",
                           f"sock.connect 0 {ip_a} 2947", "sock.read 0 400 5", timeout=240)
    finally:
        _stop(proc)
        _run(app_exe, port_b, "cert.remove ucxtestca", "cert.remove ucxtestrover", "cert.remove ucxtestroverkey")
    assert out.returncode == 0, out.stdout
    _check_stream(source, out.stdout)
    if clientauth:
        assert refused.returncode != 0 or "received 0 bytes" in refused.stdout, refused.stdout


@pytest.mark.two_devices
@pytest.mark.parametrize("source", SOURCES)
def test_bridge_over_ble_between_two_modules(app_exe, port_a, port_b, source, request):
    """A bridges the source over Nordic UART; B scans, connects by name and listens for notifications."""
    name = "ucxbridge"
    proc, _ = _start_bridge(app_exe, port_a, source, request.getfixturevalue(source), [f'at +UBTLN="{name}"'],
                            f"{source}.bridge ble nus")
    try:
        out = _run(app_exe, port_b, "bt.scan", f"bt.connect {name}", "gatt.client.nus", "gatt.listen 15",
                   "bt.disconnect", timeout=240)
    finally:
        _stop(proc)
    assert out.returncode == 0, out.stdout
    assert "[NUS RX]" in out.stdout, out.stdout
    if source == "gnss":
        assert "$G" in out.stdout, out.stdout


@pytest.mark.two_devices
def test_can_bridge_record_reaches_other_module(app_exe, wifi_peers, port_a, port_b):
    """A bridges CAN over UDP and loops one emulated frame; B receives one 17-byte record."""
    ip_b = wifi_peers[port_b][0]
    wifi = _wifi_connect_command()
    proc, seen, lines = _background(app_exe, port_b, wifi, "sock.open udp", "sock.bind 0 7008",
                                    "sock.recvfrom 0 600 20",
                                    "sock.close 0", ready="bound to port 7008")
    try:
        bridge = subprocess.Popen(_args(app_exe, port_a, wifi, f"can.bridge udp {ip_b} 7008") + ["--shell"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  errors="replace")
        try:
            time.sleep(3)
            bridge.stdin.write("can.send 0x123 11 22 33\ncan.status\n")
            bridge.stdin.flush()
            time.sleep(2)
        finally:
            _stop(bridge)
        out = bridge.stdout.read()
    finally:
        text = _finish(proc, seen, lines)
    assert "sent 0x123 [3]" in out, out
    assert "received 17 bytes" in text, text
