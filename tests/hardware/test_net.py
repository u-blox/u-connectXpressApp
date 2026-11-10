"""Sockets and IP services between the two modules and to public servers: TCP, UDP, MQTT, HTTP, CoAP, DTLS, iperf."""

import queue
import re
import socket
import subprocess
import threading
import time

import pytest
from hwutil import _args, _background, _close_listeners, _finish, _join_wifi, _reboot, _run, _wifi_connect_command

pytestmark = pytest.mark.wifi


@pytest.mark.two_devices
def test_tcp_socket_between_two_modules(app_exe, port_a, port_b):
    """Module A hosts the web server (a TCP listener); module B opens a raw TCP socket to it."""
    wifi = _wifi_connect_command()
    _reboot(app_exe, port_a, port_b)
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
                   "sock.open tcp", f"sock.connect 0 {host} 80", request.format("abc123"), "sock.read 0 1000 10",
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
    _reboot(app_exe, port_a)
    out = _run(app_exe, port_a, wifi, "sock.open udp", "sock.list", "sock.close 0",
               timeout=120)
    assert out.returncode == 0, out.stdout
    assert "socket 0: UDP" in out.stdout, out.stdout


MCAST_GROUP = "239.255.77.77"
MCAST_PORT = 7010


@pytest.mark.hardware
def test_udp_multicast_group_receives_a_datagram_from_the_pc(app_exe, port_a):
    """The module joins a multicast group on a bound UDP socket; a datagram the PC sends to the group arrives."""
    wifi = _wifi_connect_command()
    _reboot(app_exe, port_a)
    ip_a, _ = _join_wifi(app_exe, port_a, wifi)
    proc, seen, lines = _background(app_exe, port_a, wifi, "sock.open udp", f"sock.bind 0 {MCAST_PORT}",
                                    f"sock.mcast 0 join {MCAST_GROUP}", "sock.mcast 0 join ff02::fb",
                                    "sock.recvfrom 0 64 20", f"sock.mcast 0 leave {MCAST_GROUP}", "sock.close 0",
                                    ready=f"joined {MCAST_GROUP}")
    try:
        # Send from the interface that reaches the module, so a VPN or virtual adapter does not swallow the group.
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((ip_a, MCAST_PORT))
        local_ip = probe.getsockname()[0]
        probe.close()
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(local_ip))
        for _ in range(5):
            tx.sendto(b"ucx-mcast", (MCAST_GROUP, MCAST_PORT))
            time.sleep(1)
        tx.close()
    finally:
        text = _finish(proc, seen, lines)
    assert "socket 0 joined ff02::fb" in text, text
    assert "ucx-mcast" in text, f"no datagram from the group reached the module (does the access point pass " \
                                f"multicast?):\n{text}"
    assert f"socket 0 left {MCAST_GROUP}" in text, text


@pytest.mark.hardware
def test_mdns_hostname_and_service_round_trip(app_exe, port_a):
    """Enable the responder with a host name, advertise a service with TXT records, edit them, remove it."""
    wifi = _wifi_connect_command()
    _reboot(app_exe, port_a)
    name = f"ucxtest-{int(time.time()) % 100000}"
    out = _run(app_exe, port_a, wifi, "mdns.status", timeout=120)
    if "needs NORA-W36 firmware 3.4.1" in out.stdout:
        pytest.skip("module firmware has no mDNS responder (3.4.1 or later)")
    assert out.returncode == 0, out.stdout
    try:
        out = _run(app_exe, port_a, wifi, f"mdns.on {name} 15", "mdns.hostname",
                   f"mdns.add _ucxtest._tcp 8080 {name} path=/ fw=test", timeout=120)
        assert out.returncode == 0, out.stdout
        assert f"mDNS up: {name}.local" in out.stdout, out.stdout
        assert re.search(rf"^{re.escape(name)}\.local$", out.stdout, re.M), out.stdout
        added = re.search(r"^Service (\d+) added: " + re.escape(name) + r"\._ucxtest\._tcp port 8080$", out.stdout,
                          re.M)
        assert added, out.stdout
        sid = added.group(1)
        out = _run(app_exe, port_a, wifi, f"mdns.txt {sid} fw changed", f"mdns.txt {sid} remove path", "mdns.status",
                   timeout=120)
        assert out.returncode == 0, out.stdout
        assert re.search(rf"^\s*{sid}  {re.escape(name)}\._ucxtest\._tcp port 8080$", out.stdout, re.M), out.stdout
        assert re.search(r"^      fw=changed$", out.stdout, re.M), out.stdout
        assert "path=/" not in out.stdout, out.stdout
        assert re.search(rf"^Host name:      {re.escape(name)}\.local$", out.stdout, re.M), out.stdout
    finally:
        out = _run(app_exe, port_a, wifi, "mdns.remove all", "mdns.status", "mdns.off", timeout=120)
    assert out.returncode == 0, out.stdout
    assert "0 service(s)" in out.stdout and "mDNS responder off" in out.stdout, out.stdout


@pytest.mark.hardware
def test_mqtt_publish_is_received_back(app_exe, port_a):
    wifi = _wifi_connect_command()
    topic = f"ucxapp/test/{int(time.time())}"
    out = _run(app_exe, port_a, wifi, f"mqtt.will {topic}/status offline", "mqtt.will", "mqtt.connect",
               f"mqtt.subscribe {topic} 0", f"mqtt.publish {topic} hello-mqtt", "mqtt.read 10",
               f"mqtt.unsubscribe {topic}", "mqtt.disconnect", "mqtt.will off", timeout=90)
    if "connect failed" in out.stdout or "timeout waiting for the broker" in out.stdout:
        pytest.skip("public MQTT broker unreachable")
    assert out.returncode == 0, out.stdout
    assert f"last will: 'offline' on {topic}/status" in out.stdout and "last will cleared" in out.stdout, out.stdout
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
def test_wifi_portal_logs_in_to_fake_hotel_page(app_exe, port_a):
    """tests/harness/captive_portal.py on this PC stands in for a hotel login page; http.post is exercised too."""
    from harness.captive_portal import serve

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 53))
        pc_ip = s.getsockname()[0]
    server = serve(0, "0.0.0.0", cookie=True, room=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    probe = f"http://{pc_ip}:{server.server_address[1]}/generate_204"
    try:
        out = _run(app_exe, port_a, _wifi_connect_command(), f"wifi.portal check {probe}",
                   f"wifi.portal login {probe} room=101 surname=Smith",
                   f"http.post http://{pc_ip}:{server.server_address[1]}/login accept=no", timeout=180)
    finally:
        server.shutdown()
    if "no answer from" in out.stdout:
        pytest.skip("module cannot reach this PC (firewall or another network)")
    assert "login page:" in out.stdout, out.stdout
    assert "answered 204" in out.stdout, out.stdout
    assert "HTTP/1.0 403" in out.stdout, out.stdout   # http.post without the terms accepted


@pytest.mark.hardware
@pytest.mark.parametrize("token", ["", "hw-token"])
def test_https_web_server_files_and_key_protection(app_exe, port_a, tmp_path, token):
    """web.start https with a local-CA certificate; the PC trusts the CA, uploads, lists and downloads a file.
    Without a token argument HTTPS needs none; with one, a request without it is refused."""
    import ssl
    import urllib.error
    import urllib.request

    wifi = _wifi_connect_command(tmp_path)
    ip_a, _ = _join_wifi(app_exe, port_a, wifi)
    made = _run(app_exe, port_a, "utils.tls.ca.create", f"utils.tls.server.create server {ip_a}", cwd=tmp_path)
    assert made.returncode == 0, made.stdout
    proc = subprocess.Popen(_args(app_exe, port_a, wifi, f"web.start 8443 {token} https server".replace("  ", " "))
                            + ["--shell"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", cwd=tmp_path)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    ctx = None
    base = f"https://{ip_a}:8443"

    def call(path, data=None, tok=token):
        req = urllib.request.Request(base + path, data, {"X-Token": tok} if tok else {},
                                     method="POST" if data else "GET")
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=20) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    try:
        ctx = ssl.create_default_context(cafile=str(tmp_path / "storage" / "tls-ca.crt"))
        deadline, started = time.time() + 90, False
        while time.time() < deadline and not started:
            try:
                started = "web server on" in lines.get(timeout=1)
            except queue.Empty:
                pass
        assert started, "web.start https did not come up"
        try:
            status, _ = call("/api/files")
        except OSError as e:
            pytest.skip(f"this PC cannot reach the module at {ip_a}: {e}")
        assert status == 200
        if token:
            assert call("/api/files", tok="")[0] == 403
        payload = bytes(range(256)) * 4
        assert call("/api/upload?n=hw.bin&o=0&e=1", payload)[0] == 200
        assert b'"hw.bin"' in call("/api/files")[1]
        status, body = call("/api/file?n=hw.bin&o=0")
        assert status == 200 and body == payload
        assert call("/api/file?n=tls-server.key&o=0")[0] == 403   # private keys never leave the device
        assert call("/api/file?n=tls-ca.crt&o=0")[0] == 200
    finally:
        try:
            proc.stdin.write("web.stop\nexit\n")
            proc.stdin.flush()
            proc.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()


@pytest.mark.hardware
def test_web_mode_serves_the_page_until_interrupted(app_exe, port_a, tmp_path):
    """--web: no TUI or shell, Wi-Fi from the saved profiles, the page served, Ctrl+C (Ctrl+Break) stops it."""
    import os
    import signal
    import urllib.request

    wifi = _wifi_connect_command(tmp_path)
    ip_a, _ = _join_wifi(app_exe, port_a, wifi)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen(_args(app_exe, port_a, "web.start 8080 web-token") + ["--web"], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            cwd=tmp_path, creationflags=flags)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    seen = []
    try:
        deadline = time.time() + 90
        while time.time() < deadline and not any("web UI only" in s for s in seen):
            try:
                seen.append(lines.get(timeout=1))
            except queue.Empty:
                pass
        assert any("web UI only" in s for s in seen), "".join(seen)
        assert f"http://{ip_a}:8080/#t=web-token" in "".join(seen)
        try:
            with urllib.request.urlopen(f"http://{ip_a}:8080/api/groups?t=web-token", timeout=20) as r:
                assert r.status == 200 and b"wifi" in r.read()
        except OSError as e:
            pytest.skip(f"this PC cannot reach the module at {ip_a}: {e}")
    finally:
        proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    while not lines.empty():
        seen.append(lines.get())
    assert proc.returncode == 0, "".join(seen)
    assert "web server stopped" in "".join(seen)


@pytest.mark.hardware
def test_two_pages_both_get_the_whole_output(app_exe, port_a, tmp_path):
    """Two browser tabs on the module-hosted page: each event stream has its own place in the log, so a tab whose
    last event is still draining over the UART does not lose the chunk the other tab took (one shared cursor did)."""
    import json
    import os
    import signal
    import urllib.request

    wifi = _wifi_connect_command(tmp_path)
    ip_a, _ = _join_wifi(app_exe, port_a, wifi)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen(_args(app_exe, port_a, "web.start 8080 web-token") + ["--web"], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            cwd=tmp_path, creationflags=flags)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    seen = []
    base = f"http://{ip_a}:8080"

    def stream(out):
        """Reads one event stream until its 'done' event: out gets the joined 'out' data and the 'done' value."""
        try:
            with urllib.request.urlopen(f"{base}/events?t=web-token", timeout=60) as r:
                name, text = "", ""
                for raw in r:
                    line = raw.decode(errors="replace").rstrip("\n")
                    if line.startswith("event: "):
                        name = line[7:]
                    elif line.startswith("data: "):
                        if name == "out":
                            text += json.loads(line[6:])
                        elif name == "done":
                            out.update(text=text, done=line[6:])
                            return
        except OSError as e:
            out["error"] = str(e)

    a, b = {}, {}
    try:
        deadline = time.time() + 90
        while time.time() < deadline and not any("web UI only" in s for s in seen):
            try:
                seen.append(lines.get(timeout=1))
            except queue.Empty:
                pass
        assert any("web UI only" in s for s in seen), "".join(seen)
        try:
            urllib.request.urlopen(f"{base}/api/groups?t=web-token", timeout=20).read()
        except OSError as e:
            pytest.skip(f"this PC cannot reach the module at {ip_a}: {e}")
        readers = [threading.Thread(target=stream, args=(d,), daemon=True) for d in (a, b)]
        for t in readers:
            t.start()
        time.sleep(3)   # both streams open before the output starts
        req = urllib.request.Request(f"{base}/api/exec?cmd=help%20wifi", method="POST",
                                     headers={"X-Token": "web-token"})
        assert urllib.request.urlopen(req, timeout=20).read() == b"accepted"
        for t in readers:
            t.join(timeout=120)
    finally:
        proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
    while not lines.empty():
        seen.append(lines.get())
    assert a.get("done") == "0" and b.get("done") == "0", (a.get("error"), b.get("error"), "".join(seen)[-2000:])
    assert "wifi.scan" in a["text"] and a["text"].count("\n") > 20, a["text"]
    assert a["text"] == b["text"], "the two tabs saw different output"


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
