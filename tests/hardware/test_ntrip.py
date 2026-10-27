"""NTRIP client (gnss.bridge ntrip) against a small fake caster on this PC: login, GGA upload, corrections in."""

import base64
import re
import socket
import threading

import pytest
from hwutil import _run, _wifi_connect_command

pytestmark = pytest.mark.wifi

MOUNT = "UCXTEST"
USER, PASS = "rover", "s3cret"
RTCM = bytes([0xD3, 0x00, 0x13]) + bytes(19) + bytes(3)   # an empty RTCM3 frame, enough to count bytes


class FakeCaster:
    """Answers one request per connection: ICY 200 + RTCM for the right login, 401 otherwise."""

    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(("0.0.0.0", 0))
        self.srv.listen(4)
        self.srv.settimeout(1)
        self.port = self.srv.getsockname()[1]
        self.requests, self.gga = [], []
        self.stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                continue
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        conn.settimeout(1)
        data = b""
        try:
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(512)
                if not chunk:
                    return
                data += chunk
            head = data.split(b"\r\n\r\n")[0].decode(errors="replace")
            self.requests.append(head)
            auth = "Basic " + base64.b64encode(f"{USER}:{PASS}".encode()).decode()
            if f"GET /{MOUNT} " not in head or f"Authorization: {auth}" not in head:
                conn.sendall(b"HTTP/1.0 401 Unauthorized\r\n\r\n")
                return
            conn.sendall(b"ICY 200 OK\r\n")
            while not self.stop:
                conn.sendall(RTCM)
                try:
                    up = conn.recv(512)
                except socket.timeout:
                    continue
                if not up:
                    return
                self.gga += [line for line in up.decode(errors="replace").splitlines() if "GGA," in line]
        except OSError:
            pass
        finally:
            conn.close()

    def close(self):
        self.stop = True
        self.srv.close()


def _pc_ip(gateway):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((gateway, 9))
        return s.getsockname()[0]


@pytest.fixture
def caster():
    c = FakeCaster()
    yield c
    c.close()


@pytest.mark.hardware
@pytest.mark.gnss
def test_ntrip_corrections_reach_the_receiver(app_exe, port_a, gnss, wifi_peers, caster):
    host = _pc_ip(wifi_peers[port_a][1])
    bridge = f"gnss.bridge ntrip {host} {caster.port} {MOUNT} {USER} {PASS}"
    out = _run(app_exe, port_a, _wifi_connect_command(), f"gnss.open {gnss[0]} {gnss[1]}", bridge, "gnss.watch 15",
               "gnss.bridge status", timeout=240)
    if not caster.requests:
        pytest.skip(f"module could not reach {host}:{caster.port} (PC firewall?): {out.stdout[-400:]}")
    assert out.returncode == 0, out.stdout
    assert "streaming corrections" in out.stdout, out.stdout
    assert re.search(r"bridge: NTRIP .* [1-9]\d* bytes of corrections", out.stdout), out.stdout
    assert PASS not in out.stdout, out.stdout
    assert caster.gga, f"no GGA reached the caster: {out.stdout}"


@pytest.mark.hardware
@pytest.mark.gnss
def test_ntrip_wrong_password_is_reported(app_exe, port_a, gnss, wifi_peers, caster):
    host = _pc_ip(wifi_peers[port_a][1])
    out = _run(app_exe, port_a, _wifi_connect_command(), f"gnss.open {gnss[0]} {gnss[1]}",
               f"gnss.bridge ntrip {host} {caster.port} {MOUNT} {USER} wrong", "gnss.watch 5", "gnss.bridge status",
               timeout=240)
    if not caster.requests:
        pytest.skip(f"module could not reach {host}:{caster.port} (PC firewall?): {out.stdout[-400:]}")
    assert "wrong user name or password" in out.stdout, out.stdout
    assert "refused by the caster" in out.stdout, out.stdout
    assert len(caster.requests) == 1, caster.requests   # no reconnect loop after a refusal
