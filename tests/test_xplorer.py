"""xplorer.* and perf ... xplorer against a fake u-connectXplorer remote control (JSON-RPC 2.0 over a WebSocket),
so the client, its JSON reading and the error paths run without a phone. tests/hardware drives the real app."""

import base64
import hashlib
import json
import socket
import struct
import subprocess
import threading

import pytest

pytestmark = pytest.mark.app

TOKEN = "TEST1234"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B65"


class FakePhone:
    """Accepts any number of WebSocket clients and answers the methods in `results`."""

    def __init__(self, features=None):
        self.features = {"socketEcho": True, "perfSps": True} if features is None else features
        self.calls = []
        self.echo = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def close(self):
        self.sock.close()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    @staticmethod
    def _read_exact(conn, n):
        data = b""
        while len(data) < n:
            chunk = conn.recv(n - len(data))
            if not chunk:
                raise ConnectionError
            data += chunk
        return data

    def _read_frame(self, conn):
        b0, b1 = self._read_exact(conn, 2)
        size = b1 & 0x7F
        if size == 126:
            size = struct.unpack(">H", self._read_exact(conn, 2))[0]
        elif size == 127:
            size = struct.unpack(">Q", self._read_exact(conn, 8))[0]
        mask = self._read_exact(conn, 4) if b1 & 0x80 else b"\0\0\0\0"
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(self._read_exact(conn, size)))
        return b0 & 0x0F, data

    @staticmethod
    def _send(conn, obj):
        data = json.dumps(obj).encode()
        head = bytes([0x81, len(data)]) if len(data) < 126 else bytes([0x81, 126]) + struct.pack(">H", len(data))
        conn.sendall(head + data)

    def _client(self, conn):
        try:
            request = b""
            while b"\r\n\r\n" not in request:
                request += conn.recv(1024)
            key = [line.split(b":", 1)[1].strip() for line in request.split(b"\r\n")
                   if line.lower().startswith(b"sec-websocket-key:")][0]
            accept = base64.b64encode(hashlib.sha1(key + WS_GUID.encode()).digest()).decode()
            conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            # A ping and an unrelated event first: the client must answer the one and skip the other.
            conn.sendall(bytes([0x89, 0]))
            self._send(conn, {"jsonrpc": "2.0", "method": "app.onLog", "params": {"text": "hello"}})
            authed = False
            while True:
                op, data = self._read_frame(conn)
                if op == 8:
                    return
                if op != 1:
                    continue
                msg = json.loads(data)
                method, params = msg["method"], msg.get("params") or {}
                self.calls.append((method, params))
                if method == "app.authenticate":
                    authed = params.get("token") == TOKEN
                    reply = {"result": {"ok": True}} if authed else {"error": {"code": -32001,
                                                                                  "message": "Invalid token"}}
                elif not authed:
                    reply = {"error": {"code": -32000, "message": "Not authenticated"}}
                else:
                    reply = self._answer(method, params)
                self._send(conn, {"jsonrpc": "2.0", "id": msg["id"], **reply})
        except (ConnectionError, OSError, IndexError):
            pass
        finally:
            conn.close()

    def _answer(self, method, params):
        if method == "app.version":
            return {"result": {"fullVersion": "9.9.9+99"}}
        if method == "app.getPlatform":
            return {"result": {"platform": "ios", "osVersion": "18.1", "bluetoothState": "poweredOff"}}
        if method == "app.getCapabilities":
            return {"result": {"methods": [], "features": self.features}}
        if method == "socket.startServer":
            self.echo = params
            return {"result": {"ok": True, "port": params["port"]}}
        if method == "socket.getState":
            if self.echo is None:
                return {"result": {"active": False}}
            return {"result": {"active": True, "protocol": self.echo["protocol"], "state": "listening",
                               "port": self.echo["port"], "echo": True, "bytesReceived": 2048,
                               "bytesSent": 2048, "packetsReceived": 4, "packetsSent": 4, "patternBreaks": 0}}
        if method == "socket.stop":
            self.echo = None
            return {"result": {"ok": True}}
        if method == "ble.getState":
            return {"result": {"scanning": False, "bluetoothState": "poweredOff", "note": "a \"quoted\" text"}}
        if method == "echo.params":
            return {"result": params}
        return {"error": {"code": -32601, "message": f"Method not found: {method}"}}


@pytest.fixture
def phone():
    fake = FakePhone()
    yield fake
    fake.close()


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def _connect(app_exe, tmp_path, phone):
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{phone.port} {TOKEN}")
    assert out.returncode == 0, out.stdout
    return out


def test_connect_shows_the_phone_and_saves_it(app_exe, tmp_path, phone):
    out = _connect(app_exe, tmp_path, phone)
    assert "u-connectXplorer 9.9.9+99 on ios 18.1 at 127.0.0.1" in out.stdout
    assert "phone Bluetooth: poweredOff" in out.stdout
    assert "saved" in out.stdout and "note:" not in out.stdout
    saved = (tmp_path / "storage" / "xplorer.cfg").read_text()
    assert "127.0.0.1" in saved and TOKEN not in saved, "the token must be stored sealed"
    out = _run(app_exe, tmp_path, "xplorer.info")
    assert out.returncode == 0 and "9.9.9+99" in out.stdout, out.stdout


def test_connect_again_with_an_existing_key(app_exe, tmp_path, phone):
    """A second run loads ucxapp.key from disk instead of creating it; sealing must still work."""
    _connect(app_exe, tmp_path, phone)
    assert (tmp_path / "storage" / "ucxapp.key").exists()
    out = _connect(app_exe, tmp_path, phone)
    assert "cannot save" not in out.stdout and "saved" in out.stdout, out.stdout


def test_wrong_token(app_exe, tmp_path, phone):
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{phone.port} WRONG")
    assert out.returncode != 0, out.stdout
    assert "Invalid token" in out.stdout and "check the token" in out.stdout
    assert not (tmp_path / "storage" / "xplorer.cfg").exists()


def test_old_app_gets_update_notes(app_exe, tmp_path):
    fake = FakePhone(features={})
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert out.stdout.count("note: update u-connectXplorer") == 2, out.stdout
        out = _run(app_exe, tmp_path, "xplorer.echo udp 7000")
        assert out.returncode != 0 and "cannot echo" in out.stdout, out.stdout
        assert not any(m == "socket.startServer" for m, _ in fake.calls)
    finally:
        fake.close()


def test_call_builds_params(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    out = _run(app_exe, tmp_path, "xplorer.call echo.params n=5 f=-1.5 b=true z=null s=hello o={} id=AB:CD")
    assert out.returncode == 0, out.stdout
    assert phone.calls[-1] == ("echo.params", {"n": 5, "f": -1.5, "b": True, "z": None, "s": "hello", "o": {},
                                               "id": "AB:CD"})
    out = _run(app_exe, tmp_path, "xplorer.call ble.getState")
    assert out.returncode == 0, out.stdout
    assert json.loads(out.stdout)["note"] == 'a "quoted" text'
    out = _run(app_exe, tmp_path, "xplorer.call no.such")
    assert out.returncode != 0 and "ERROR: no.such: Method not found: no.such (-32601)" in out.stdout, out.stdout


def test_echo_start_state_stop(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    out = _run(app_exe, tmp_path, "xplorer.echo tcp 7001")
    assert out.returncode == 0 and "the phone echoes tcp on 127.0.0.1:7001" in out.stdout, out.stdout
    assert phone.echo == {"protocol": "tcp", "port": 7001, "echo": True, "force": True}
    out = _run(app_exe, tmp_path, "xplorer.echo state")
    assert out.returncode == 0, out.stdout
    assert "phone: tcp listening port 7001 echo, received 2048 bytes (4 packets)" in out.stdout
    assert "pattern 0x00-0xFF: 0 breaks" in out.stdout
    out = _run(app_exe, tmp_path, "xplorer.echo stop")
    assert out.returncode == 0 and phone.echo is None, out.stdout
    out = _run(app_exe, tmp_path, "xplorer.echo state")
    assert out.returncode == 0 and "phone: no socket session" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["xplorer.info", "xplorer.echo state", "xplorer.call app.version"])
def test_no_phone_saved(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "no phone saved" in out.stdout, out.stdout


def test_unreachable_phone(app_exe, tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{port} {TOKEN}")
    assert out.returncode != 0 and "cannot reach u-connectXplorer" in out.stdout, out.stdout


def test_not_a_websocket(app_exe, tmp_path):
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def answer():
        conn, _ = srv.accept()
        conn.recv(1024)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
        conn.close()

    threading.Thread(target=answer, daemon=True).start()
    try:
        out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{srv.getsockname()[1]} {TOKEN}")
        assert out.returncode != 0 and "WebSocket upgrade failed" in out.stdout, out.stdout
    finally:
        srv.close()


@pytest.mark.parametrize("command", ["xplorer.connect", "xplorer.connect 1.2.3.4", "xplorer.connect 1.2.3.4:0 t",
                                     "xplorer.connect :8765 t", "xplorer.info x", "xplorer.call",
                                     "xplorer.call nodot", "xplorer.call a.b novalue", "xplorer.echo",
                                     "xplorer.echo tcp", "xplorer.echo sps 7", "xplorer.echo udp 70000"])
def test_usage(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


@pytest.mark.parametrize("command", ["perf.rtt udp xplorer 7000", "perf.tput tcp xplorer 7001", "perf.phone rx",
                                     "perf.phone", "perf.phone up", "perf.phone tx 0"])
def test_perf_with_the_phone_needs_a_device(app_exe, tmp_path, phone, command):
    _connect(app_exe, tmp_path, phone)
    calls = len(phone.calls)
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "not connected to a module" in out.stdout, out.stdout
    assert len(phone.calls) == calls, "nothing may start on the phone without a module"
