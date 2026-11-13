"""A fake u-connectXplorer remote control: JSON-RPC 2.0 over a WebSocket, as the phone app serves it.

    phone = FakePhone()                       # listens on 127.0.0.1:phone.port, token TOKEN
    phone.bluetooth = "poweredOn"             # what app.getPlatform and ble.getState report
    phone.handlers["ble.connect"] = lambda params: {"mtu": 247}   # a result, or {"error": {...}} to refuse
    phone.notify("perf.onComplete", {...})    # an event to the connected client
"""

import base64
import hashlib
import hmac
import json
import socket
import struct
import threading

TOKEN = "TEST1234"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B65"
# What a client may call before app.authenticate
OPEN = ("app.ping", "app.version", "app.getCapabilities", "app.getPlatform", "app.getChallenge")
# The features of a phone with remote control protocol version 2, on top of socketEcho and perfSps
V2 = {"socketEcho": True, "perfSps": True, "authChallenge": True, "stopOnDisconnect": True, "waitForDevice": True,
      "perfEndReason": True, "errorCodes": True}


class FakePhone:
    """Accepts any number of WebSocket clients and answers the methods in `handlers`, then the built-in ones.
    The default is a phone from before protocol version 2 (token only); FakePhone(features=V2, protocol=2) is one
    that takes the answer to a challenge."""

    def __init__(self, features=None, protocol=None):
        self.features = {"socketEcho": True, "perfSps": True} if features is None else features
        self.protocol = protocol  # protocolVersion of app.getCapabilities; None: the field is not there (version 1)
        self.configured = []     # params of every app.configure
        self.nonce = "00112233445566778899aabbccddeeff"
        self.bluetooth = "poweredOff"
        self.breaks = 0          # patternBreaks of the echo session (socket.getState)
        self.handlers = {}       # method -> callable(params) returning the result, or {"error": {...}}
        self.calls = []
        self.echo = None
        self._conn = None        # the client that called last, for notify()
        self._wlock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def close(self):
        self.sock.close()

    def called(self, method):
        return [params for name, params in self.calls if name == method]

    def notify(self, method, params):
        """An event (a message without an id) to the client that called last; False when it has left."""
        conn = self._conn
        if conn is None:
            return False
        try:
            self._send(conn, {"jsonrpc": "2.0", "method": method, "params": params})
        except OSError:
            return False
        return True

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

    def _send(self, conn, obj):
        data = json.dumps(obj).encode()
        head = bytes([0x81, len(data)]) if len(data) < 126 else bytes([0x81, 126]) + struct.pack(">H", len(data))
        with self._wlock:   # notify() runs in another thread: one lock keeps each frame whole
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
                self._conn = conn
                if method == "app.authenticate":
                    answer = hmac.new(TOKEN.encode(), self.nonce.encode(), hashlib.sha256).hexdigest()
                    authed = (params.get("token") == TOKEN or
                              (self.features.get("authChallenge") and params.get("response") == answer))
                    reply = {"result": {"ok": True}} if authed else {"error": {"code": -32001,
                                                                                  "message": "Invalid token"}}
                elif method == "app.getChallenge" and self.features.get("authChallenge"):
                    reply = {"result": {"nonce": self.nonce, "algorithm": "HMAC-SHA256"}}
                elif not authed and method not in OPEN:
                    reply = {"error": {"code": -32000, "message": "Not authenticated"}}
                elif method == "app.configure" and self.features.get("stopOnDisconnect"):
                    self.configured.append(params)
                    reply = {"result": {"ok": True, **params}}
                elif method in self.handlers:
                    result = self.handlers[method](params)
                    reply = result if isinstance(result, dict) and "error" in result else {"result": result}
                else:
                    reply = self._answer(method, params)
                self._send(conn, {"jsonrpc": "2.0", "id": msg["id"], **reply})
        except (ConnectionError, OSError, IndexError):
            pass
        finally:
            if self._conn is conn:
                self._conn = None
            conn.close()

    def _answer(self, method, params):
        if method == "app.version":
            return {"result": {"fullVersion": "9.9.9+99"}}
        if method == "app.getPlatform":
            return {"result": {"platform": "ios", "osVersion": "18.1", "bluetoothState": self.bluetooth}}
        if method == "app.getCapabilities":
            version = {} if self.protocol is None else {"protocolVersion": self.protocol}
            return {"result": {"methods": [], "features": self.features, **version}}
        if method == "socket.startServer":
            self.echo = params
            return {"result": {"ok": True, "port": params["port"]}}
        if method == "socket.getState":
            if self.echo is None:
                return {"result": {"active": False}}
            return {"result": {"active": True, "protocol": self.echo["protocol"], "state": "listening",
                               "port": self.echo["port"], "echo": True, "bytesReceived": 2048,
                               "bytesSent": 2048, "packetsReceived": 4, "packetsSent": 4,
                               "patternBreaks": self.breaks}}
        if method == "socket.stop":
            self.echo = None
            return {"result": {"ok": True}}
        if method == "ble.getState":
            return {"result": {"scanning": False, "bluetoothState": self.bluetooth, "note": "a \"quoted\" text"}}
        if method == "echo.params":
            return {"result": params}
        return {"error": {"code": -32601, "message": f"Method not found: {method}"}}
