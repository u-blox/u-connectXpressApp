"""Sockets and IP services between the two modules and to public servers: TCP, UDP, MQTT, HTTP, CoAP, DTLS, iperf."""

import queue
import re
import subprocess
import threading
import time

import pytest
from hwutil import _args, _background, _close_listeners, _finish, _run, _wifi_connect_command


@pytest.mark.two_devices
def test_tcp_socket_between_two_modules(app_exe, port_a, port_b):
    """Module A hosts the web server (a TCP listener); module B opens a raw TCP socket to it."""
    wifi = _wifi_connect_command()
    for port in (port_a, port_b):
        _run(app_exe, port, "device.reboot")
    time.sleep(5)
    args = _args(app_exe, port_a, wifi, "web.start 80 abc123") + ["--shell"]
    server = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in server.stdout], daemon=True).start()
    try:
        host = None
        deadline = time.time() + 60
        while time.time() < deadline and host is None:
            try:
                match = re.search(r"http://([\d.]+):80/", lines.get(timeout=1))
            except queue.Empty:
                continue
            host = match.group(1) if match else None
        assert host, "module A did not print its web URL"
        request = "sock.send 0 GET /api/groups?t={} HTTP/1.0\\r\\n\\r\\n"
        out = _run(app_exe, port_b, wifi,
                   "sock.open tcp", f"sock.connect 0 {host} 80", request.format("abc123"), "sock.read 0 120 10",
                   "sock.close 0",
                   "sock.open tcp", f"sock.connect 0 {host} 80", request.format("wrong"), "sock.read 0 120 10",
                   "sock.close 0", timeout=240)
    finally:
        server.stdin.write("web.stop\nexit\n")
        server.stdin.flush()
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
    assert out.returncode == 0, out.stdout
    assert "200" in out.stdout, out.stdout   # valid token
    assert "403" in out.stdout, out.stdout   # wrong token


@pytest.mark.hardware
def test_udp_socket_opens_and_closes(app_exe, port_a):
    wifi = _wifi_connect_command()
    _run(app_exe, port_a, "device.reboot")
    time.sleep(5)
    out = _run(app_exe, port_a, wifi, "sock.open udp", "sock.list", "sock.close 0",
               timeout=120)
    assert out.returncode == 0, out.stdout
    assert "socket 0: UDP" in out.stdout, out.stdout


@pytest.mark.hardware
def test_mqtt_publish_is_received_back(app_exe, port_a):
    wifi = _wifi_connect_command()
    topic = f"ucxapp/test/{int(time.time())}"
    out = _run(app_exe, port_a, wifi, "mqtt.connect", f"mqtt.subscribe {topic} 0",
               f"mqtt.publish {topic} hello-mqtt", "mqtt.read 10", f"mqtt.unsubscribe {topic}", "mqtt.disconnect",
               timeout=180)
    if "connect failed" in out.stdout or "timeout waiting for the broker" in out.stdout:
        pytest.skip("public MQTT broker unreachable")
    assert out.returncode == 0, out.stdout
    assert f"{topic}: hello-mqtt" in out.stdout, out.stdout
    assert "no subscribe confirmation" not in out.stdout, out.stdout
    assert "read failed" not in out.stdout, out.stdout
    assert "publish failed" not in out.stdout, out.stdout


@pytest.mark.hardware
def test_http_download_saves_a_file(app_exe, port_a, tmp_path):
    wifi = _wifi_connect_command()
    target = tmp_path / "example.html"
    out = _run(app_exe, port_a, wifi, f"http.download http://example.com/ {target}",
               timeout=180)
    assert out.returncode == 0, out.stdout
    assert "bytes saved" in out.stdout, out.stdout
    assert b"Example Domain" in target.read_bytes()


@pytest.mark.hardware
def test_coap_get_public_server(app_exe, port_a):
    out = _run(app_exe, port_a, _wifi_connect_command(), "coap.get coap.me 5683 test", timeout=180)
    assert out.returncode == 0, out.stdout
    assert "coap 2.05" in out.stdout, out.stdout


@pytest.mark.two_devices
@pytest.mark.parametrize("proto", ["udp", "tcp"])
def test_sock_echo_between_two_modules(app_exe, wifi_peers, port_a, port_b, proto):
    """A runs sock.echo; B sends a text and must get the same text back."""
    ip_a = wifi_peers[port_a][0]
    proc, seen, lines = _background(app_exe, port_a, f"sock.echo {proto} 7007 30", ready="echo on port")
    try:
        if proto == "udp":
            out = _run(app_exe, port_b, "sock.open udp", "sock.bind 0 7009", f"sock.sendto 0 {ip_a} 7007 ucx-echo",
                       "sock.recvfrom 0 64 5", "sock.close 0")
        else:
            out = _run(app_exe, port_b, "sock.open tcp", f"sock.connect 0 {ip_a} 7007", "sock.send 0 ucx-echo",
                       "sock.read 0 64 5", "sock.close 0")
    finally:
        text = _finish(proc, seen, lines)
    assert out.returncode == 0, out.stdout
    assert re.search(r"received 8 bytes.*ucx-echo", out.stdout), out.stdout
    assert "echoed 8 bytes" in text, text


@pytest.mark.two_devices
def test_dtls_between_two_modules(app_exe, wifi_peers, port_a, port_b, tmp_path):
    """A serves DTLS with a certificate from the app's local CA (store in tmp_path); B sends one message."""
    ip_a = wifi_peers[port_a][0]
    made = subprocess.run(_args(app_exe, port_a, "utils.tls.ca.create", f"utils.tls.server.create server {ip_a}"),
                          capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path)
    assert made.returncode == 0, made.stdout
    proc, seen, lines = _background(app_exe, port_a, "dtls.server 5684", ready="DTLS server on port", cwd=tmp_path)
    try:
        out = _run(app_exe, port_b, f"dtls.client {ip_a} 5684 ucx-dtls", cwd=tmp_path)
    finally:
        text = _finish(proc, seen, lines)
    assert out.returncode == 0, out.stdout + text
    assert "reply: ucx-dtls" in out.stdout, out.stdout
    assert "DTLS established" in text, text


@pytest.mark.two_devices
def test_tftp_between_two_modules(app_exe, wifi_peers, port_a, port_b, tmp_path):
    """A serves its folder over TFTP (rw); B uploads a file larger than one block and downloads it again."""
    ip_a = wifi_peers[port_a][0]
    dir_a, dir_b = tmp_path / "a", tmp_path / "b"
    dir_a.mkdir()
    dir_b.mkdir()
    payload = bytes(range(256)) * 20
    (dir_b / "up.bin").write_bytes(payload)
    proc, seen, lines = _background(app_exe, port_a, "tftp.on rw 69 40", ready="TFTP server on", cwd=dir_a)
    try:
        out = _run(app_exe, port_b, f"tftp.put {ip_a} up.bin", f"tftp.get {ip_a} up.bin down.bin",
                   f"tftp.get {ip_a} missing.bin", cwd=dir_b)
    finally:
        text = _finish(proc, seen, lines)
    assert f"put up.bin -> up.bin: {len(payload)} bytes" in out.stdout, out.stdout + text
    assert (dir_a / "up.bin").read_bytes() == payload, text
    assert (dir_b / "down.bin").read_bytes() == payload, out.stdout
    assert "File not found" in out.stdout, out.stdout
    assert not (dir_b / "missing.bin").exists()


@pytest.mark.two_devices
def test_iperf_between_two_modules(app_exe, wifi_peers, port_a, port_b):
    ip_a = wifi_peers[port_a][0]
    try:
        assert _run(app_exe, port_a, "net.iperf server tcp").returncode == 0
        out = _run(app_exe, port_b, f"net.iperf client {ip_a} tcp 5", timeout=60)
        assert out.returncode == 0, out.stdout
    finally:
        _run(app_exe, port_a, "net.iperf stop")
        _run(app_exe, port_b, "net.iperf stop")


@pytest.mark.two_devices
def test_tcp_listen_accept_between_two_modules(app_exe, wifi_peers, port_a, port_b):
    ip_a = wifi_peers[port_a][0]
    _close_listeners(app_exe, port_a)
    proc, seen, lines = _background(app_exe, port_a, "sock.open tcp", "sock.listen 0 7010", "sock.accept 30",
                                    "sock.read 1 64 10", "sock.close 1", "sock.close 0", ready="listening")
    try:
        out = _run(app_exe, port_b, "sock.open tcp", f"sock.connect 0 {ip_a} 7010", "sock.send 0 ucx-accept",
                   "sock.close 0")
    finally:
        text = _finish(proc, seen, lines)
    assert out.returncode == 0, out.stdout
    assert "accepted socket" in text and "ucx-accept" in text, text
