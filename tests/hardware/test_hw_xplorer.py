"""u-connectXplorer as the phone: its Remote Control (JSON-RPC over WebSocket) drives the BLE side against ucxapp.

Start u-connectXplorer (phone or desktop), turn on Settings > Remote Control and set
UCX_XPLORER=ws://<host>:8765 and UCX_XPLORER_TOKEN=<token shown there>. Wi-Fi provisioning also needs
UCX_WIFI_SSID and UCX_WIFI_PASS. See u-connectXplorer/docs/REMOTE_CONTROL.md for the protocol.
"""

import itertools
import json
import os
import re
import subprocess
import threading
import time

import pytest
from hwutil import _args, _reboot, _run, _stop, _wifi_connect_command
from test_gatt_isolate import _address, _Reader

pytestmark = [pytest.mark.hardware, pytest.mark.xplorer]

BAS = "0000180f-0000-1000-8000-00805f9b34fb"
BAS_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"
WPS = "14387800-130c-49e7-b877-2881c89cb258"
WPS_CTRL = "14387802-130c-49e7-b877-2881c89cb258"
WPS_DATA = "14387803-130c-49e7-b877-2881c89cb258"
NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


class Xplorer:
    """A JSON-RPC 2.0 client for u-connectXplorer; events (no id) are kept in self.events.

    The phone serves one remote at a time (close code 4001 for the older one), so each ucxapp run takes the phone
    over; the next call here connects again."""

    def __init__(self, url, token):
        self.url, self.token = url, token
        self.ids = itertools.count(1)
        self.replies = {}
        self.events = []
        self.cond = threading.Condition()
        self.ws = None
        self._connect()

    def _connect(self):
        from websockets.sync.client import connect

        self.ws = connect(self.url, open_timeout=10, max_size=None)
        threading.Thread(target=self._read, args=(self.ws,), daemon=True).start()
        if self.token:
            assert self.call("app.authenticate", token=self.token).get("authenticated"), "wrong UCX_XPLORER_TOKEN"

    def _read(self, ws):
        try:
            for raw in ws:
                msg = json.loads(raw)
                with self.cond:
                    if "id" in msg and msg["id"] is not None:
                        self.replies[msg["id"]] = msg
                    else:
                        self.events.append(msg)
                    self.cond.notify_all()
        except Exception:
            pass
        with self.cond:
            self.cond.notify_all()

    def _replaced(self):
        from websockets.protocol import State

        return self.ws.protocol.state is not State.OPEN and self.ws.protocol.close_rcvd is not None \
            and self.ws.protocol.close_rcvd.code == 4001

    def call(self, method, timeout=30, **params):
        if self._replaced():
            self._connect()
        rid = next(self.ids)
        ws = self.ws
        ws.send(json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}))
        with self.cond:
            self.cond.wait_for(lambda: rid in self.replies or ws.protocol.close_rcvd is not None, timeout)
            assert rid in self.replies, f"{method}: no reply in {timeout} s ({ws.protocol.close_rcvd or 'open'})"
            msg = self.replies.pop(rid)
        assert "error" not in msg, f"{method}: {msg['error']}"
        return msg["result"]

    def event(self, method, match=lambda p: True, timeout=30):
        def find():
            return next((e["params"] for e in self.events if e.get("method") == method and match(e["params"])), None)

        with self.cond:
            self.cond.wait_for(lambda: find() is not None, timeout)
            return find()

    def close(self):
        try:
            self.call("ble.disconnect", timeout=10)
        finally:
            self.ws.close()


@pytest.fixture
def xplorer():
    pytest.importorskip("websockets")
    try:
        xp = Xplorer(os.environ["UCX_XPLORER"], os.environ.get("UCX_XPLORER_TOKEN", ""))
    except OSError as e:
        pytest.fail(f"cannot reach u-connectXplorer at {os.environ['UCX_XPLORER']} ({e}); open the app with "
                    "Remote Control on and keep the screen unlocked (iOS stops the server in the background)",
                    pytrace=False)
    xp.call("ble.disconnect")
    yield xp
    xp.close()


def _serve(app_exe, port, *commands, cwd=None):
    proc = subprocess.Popen(_args(app_exe, port, *commands) + ["--shell"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", cwd=cwd)
    return proc, _Reader(proc)


def _send(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def _find_and_connect(xp, address, name):
    """Scan until the module shows up (by address, or by name where the OS hides addresses) and connect."""
    xp.call("ble.startScan")
    device = None
    deadline = time.time() + 30
    while device is None and time.time() < deadline:
        time.sleep(1)
        device = next((d for d in xp.call("ble.getDevices")["devices"]
                       if d["id"].upper() == address or (name and d.get("name") == name)), None)
    xp.call("ble.stopScan")
    assert device, f"u-connectXplorer did not see {address} ({name})"
    result = xp.call("ble.connect", timeout=40, deviceId=device["id"])
    assert result["state"] == "connected" and result["services"] > 0, result
    return device["id"]


def _module(app_exe, port):
    _reboot(app_exe, port)
    _run(app_exe, port, "bt.unbond all")
    name = re.search(r"\S+$", _run(app_exe, port, "bt.name").stdout.strip())
    return _address(app_exe, port), name.group(0) if name else ""


def _service(xp, dev, uuid):
    services = {s["uuid"].lower(): s for s in xp.call("ble.getServices", deviceId=dev)["services"]}
    assert uuid in services, f"{uuid} not in {sorted(services)}"
    return {c["uuid"].lower(): c for c in services[uuid]["characteristics"]}


def test_xplorer_reads_and_is_notified_by_a_gatt_server(app_exe, port_a, xplorer):
    address, name = _module(app_exe, port_a)
    server, out = _serve(app_exe, port_a, "gatt.server.bas", "gatt.server.set bas.level 37")
    try:
        assert out.wait(r"service active", 60), out.text
        dev = _find_and_connect(xplorer, address, name)
        assert _service(xplorer, dev, BAS)[BAS_LEVEL]["notifiable"]
        assert xplorer.call("ble.readCharacteristic", deviceId=dev, serviceId=BAS,
                            characteristicId=BAS_LEVEL)["value"] == [0x37]
        assert xplorer.call("ble.subscribe", deviceId=dev, serviceId=BAS, characteristicId=BAS_LEVEL)["notifying"]
        time.sleep(1)
        _send(server, "gatt.server.notify 0 bas.level 42")
        got = xplorer.event("ble.onNotification", lambda p: p["characteristicId"].lower() == BAS_LEVEL)
        assert got and got["value"] == [0x42], f"{xplorer.events[-5:]}\n{out.drain()}"
    finally:
        _stop(server)


def test_xplorer_sps_both_ways(app_exe, port_a, xplorer):
    address, name = _module(app_exe, port_a)
    server, out = _serve(app_exe, port_a, "gatt.server.sps")
    try:
        assert out.wait(r"service active", 60), out.text
        dev = _find_and_connect(xplorer, address, name)
        assert xplorer.call("sps.connect", deviceId=dev, useFlowControl=False)["ok"]
        xplorer.call("sps.write", text="hello-from-xplorer")
        assert out.wait(r"hello-from-xplorer", 20), out.drain()
        _send(server, "gatt.server.notify 0 sps.fifo text:hello-from-ucxapp")
        got = xplorer.event("sps.onData", lambda p: "hello-from-ucxapp" in p.get("text", ""))
        assert got, f"{xplorer.events[-5:]}\n{out.drain()}"
    finally:
        xplorer.call("sps.disconnect")
        _stop(server)


def _pb(field, value):
    """One protobuf field: an int as a varint, bytes length-delimited (what u-connectXplorer's provisioning sends)."""
    def varint(v):
        out = bytearray()
        while True:
            out.append((v & 0x7F) | (0x80 if v > 0x7F else 0))
            v >>= 7
            if not v:
                return bytes(out)
    if isinstance(value, int):
        return varint(field << 3) + varint(value)
    return varint(field << 3 | 2) + varint(len(value)) + value


def _phone():
    """host:port and token of UCX_XPLORER, as xplorer.connect takes them."""
    url = os.environ["UCX_XPLORER"]
    return re.sub(r"^wss?://", "", url).rstrip("/"), os.environ.get("UCX_XPLORER_TOKEN", "")


def _with_phone(app_exe, port, cwd, *commands, timeout=180):
    """ucxapp on `port` (or --offline) with the phone saved in `cwd` first, then `commands`."""
    address, token = _phone()
    connect = f"xplorer.connect {address} {token}"
    if port is None:
        args = [str(app_exe), "--offline", "--exec", connect]
        for command in commands:
            args += ["--exec", command]
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace", cwd=cwd,
                              stdin=subprocess.DEVNULL)
    return _run(app_exe, port, connect, *commands, timeout=timeout, cwd=cwd)


# What ucxapp uses from remote control protocol version 1 (u-connectXplorer 1.2.0)
V1_FEATURES = ("socketEcho", "perfSps", "authChallenge", "stopOnDisconnect", "waitForDevice", "perfEndReason",
               "errorCodes")


def test_xplorer_speaks_protocol_version_1(app_exe, xplorer, tmp_path):
    caps = xplorer.call("app.getCapabilities")
    assert caps.get("protocolVersion") == 1, caps
    missing = [f for f in V1_FEATURES if not caps.get("features", {}).get(f)]
    assert not missing, f"the phone lacks {missing}: {caps.get('features')}"
    out = _with_phone(app_exe, None, tmp_path, "xplorer.info")
    assert out.returncode == 0, out.stdout
    assert "saved;" in out.stdout and "note:" not in out.stdout, out.stdout
    assert re.search(r"u-connectXplorer \S+ .* on \w+", out.stdout), out.stdout


def test_xplorer_answers_the_challenge_and_refuses_a_wrong_token(app_exe, tmp_path):
    """One wrong token only: the phone locks every client out after ten wrong answers in a minute."""
    address, _ = _phone()
    args = [str(app_exe), "--offline", "--exec", f"xplorer.connect {address} WRONG0000"]
    out = subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path,
                         stdin=subprocess.DEVNULL)
    assert out.returncode != 0 and "(-32001)" in out.stdout, out.stdout
    assert "check the token on the Remote Control screen" in out.stdout, out.stdout
    assert "saved;" not in out.stdout, out.stdout
    out = _with_phone(app_exe, None, tmp_path, "xplorer.call ble.getState")
    assert out.returncode == 0 and "saved;" in out.stdout, out.stdout
    assert re.search(r"^\{.*\}$", out.stdout, re.M), out.stdout


def test_xplorer_echo_starts_reports_and_stops(app_exe, xplorer, tmp_path):
    out = _with_phone(app_exe, None, tmp_path, "xplorer.echo udp 7400", "xplorer.echo state")
    assert out.returncode == 0, out.stdout
    assert "the phone echoes udp on" in out.stdout, out.stdout
    state = xplorer.call("socket.getState")
    assert state.get("active") and state.get("echo") and state.get("port") == 7400, state
    out = _with_phone(app_exe, None, tmp_path, "xplorer.echo stop")
    assert out.returncode == 0, out.stdout
    assert not xplorer.call("socket.getState").get("active"), "the echo server still runs after xplorer.echo stop"


@pytest.mark.parametrize("mode", ["tx", "rx", "echo"])
def test_perf_phone_over_sps(app_exe, port_a, xplorer, tmp_path, mode):
    """Each mode starts from a phone the earlier run left clean (stopOnDisconnect): no link, no SPS, no run."""
    _module(app_exe, port_a)
    out = _with_phone(app_exe, port_a, tmp_path, f"perf.phone {mode} 5")
    assert out.returncode == 0, out.stdout
    assert "SPS with the phone" in out.stdout and not _failed(out.stdout), out.stdout
    assert "module: sent" in out.stdout, out.stdout
    if mode == "rx":
        assert re.search(r"pattern 0x00-0xFF: 0 breaks", out.stdout), out.stdout
    assert not xplorer.call("socket.getState").get("active"), "perf.phone left a socket session on the phone"


@pytest.mark.wifi
@pytest.mark.parametrize("command", ["perf.latency udp xplorer 7401 20", "perf.latency tcp xplorer 7402 20",
                                     "perf.throughput udp xplorer 7403 5", "perf.throughput tcp xplorer 7404 5"])
def test_perf_over_wifi_with_the_phone_as_echo_peer(app_exe, port_a, xplorer, tmp_path, command):
    _module(app_exe, port_a)
    connect = _wifi_connect_command(tmp_path)
    out = _with_phone(app_exe, port_a, tmp_path, connect, command, timeout=240)
    assert out.returncode == 0, out.stdout
    assert "phone " in out.stdout and " echoes " in out.stdout, out.stdout
    assert not _failed(out.stdout), out.stdout
    assert not xplorer.call("socket.getState").get("active"), "the phone still echoes after the run"


@pytest.mark.wifi
def test_xplorer_provisions_wifi_with_the_nordic_protocol(app_exe, port_a, xplorer):
    ssid, password = os.environ.get("UCX_WIFI_SSID"), os.environ.get("UCX_WIFI_PASS", "")
    if not ssid:
        pytest.skip("set UCX_WIFI_SSID and UCX_WIFI_PASS")
    address, name = _module(app_exe, port_a)
    _run(app_exe, port_a, "wifi.disconnect")
    server, out = _serve(app_exe, port_a, "gatt.server.wps 120")
    try:
        assert out.wait(r"serving .* for 120 s", 60), out.text
        dev = _find_and_connect(xplorer, address, name)
        chars = _service(xplorer, dev, WPS)
        assert chars[WPS_CTRL]["indicatable"] and chars[WPS_DATA]["notifiable"], chars
        for char in (WPS_DATA, WPS_CTRL):
            assert xplorer.call("ble.subscribe", deviceId=dev, serviceId=WPS, characteristicId=char)["notifying"]
        info = _pb(1, ssid.encode()) + _pb(2, b"\xff" * 6) + _pb(5, 4 if password else 0)
        config = _pb(1, info) + (_pb(2, password.encode()) if password else b"")
        xplorer.call("ble.writeCharacteristic", deviceId=dev, serviceId=WPS, characteristicId=WPS_CTRL,
                     value=list(_pb(1, 4) + _pb(11, config)))
        ok = xplorer.event("ble.onNotification", lambda p: p["characteristicId"].lower() == WPS_CTRL)
        assert ok and ok["value"][:4] == [0x08, 0x04, 0x10, 0x00], f"{xplorer.events[-5:]}\n{out.drain()}"
        state = xplorer.event("ble.onNotification",
                              lambda p: p["characteristicId"].lower() == WPS_DATA and p["value"][:1] == [0x10], 60)
        assert state and state["value"][:2] == [0x10, 4], f"not connected: {state}\n{out.drain()}"   # CONNECTED
        assert password not in out.drain() or not password
    finally:
        _stop(server)
    assert "wifi: connected" in _run(app_exe, port_a, "wifi.status").stdout.lower()


def _needs(xp, *flags):
    features = xp.call("app.getCapabilities").get("features", {})
    missing = [f for f in flags if not features.get(f)]
    if missing:
        pytest.skip(f"this u-connectXplorer has no {missing}")


def _phone_ip():
    return re.sub(r"^wss?://", "", os.environ["UCX_XPLORER"]).split("/")[0].split(":")[0]


def _who(xp, address, name):
    """How ble.waitForDevice finds the module: by address, or by name on iOS/macOS where the phone hides it."""
    if xp.call("app.getPlatform").get("platform") in ("ios", "macos"):
        return {"name": name}
    return {"id": address}


def _text(p):
    return p.get("text") or bytes(p.get("value") or []).decode(errors="replace")


def _failed(stdout):
    """ucxapp prints a failed command as "ERROR: ..."; a warning naming an error code is not one."""
    return re.search(r"(?m)^ERROR", stdout)


def test_xplorer_answers_ping_network_info_and_log(xplorer):
    assert xplorer.call("app.ping").get("pong") is True
    info = xplorer.call("app.getNetworkInfo")
    port = int(re.search(r":(\d+)", re.sub(r"^wss?://", "", os.environ["UCX_XPLORER"]) + ":8765").group(1))
    assert info["running"] and info["port"] == port, info
    assert _phone_ip() in [info["primaryIp"]] + [a["ip"] for a in info.get("addresses", [])], info
    log = xplorer.call("app.getLog", limit=50)
    assert log["count"] > 0 and "app.ping" in json.dumps(log["entries"]), log


def test_xplorer_waits_for_the_module_and_reads_the_link(app_exe, port_a, xplorer):
    _needs(xplorer, "waitForDevice")
    address, name = _module(app_exe, port_a)
    server, out = _serve(app_exe, port_a, "gatt.server.bas")
    try:
        assert out.wait(r"service active", 60), out.text
        found = xplorer.call("ble.waitForDevice", timeout=40, timeoutMs=30000, **_who(xplorer, address, name))
        assert found.get("found") and found.get("id"), found
        dev = found["id"]
        assert xplorer.call("ble.connect", timeout=40, deviceId=dev)["state"] == "connected"
        state = xplorer.call("ble.getState", deviceId=dev)
        assert dev in json.dumps(state), state
        rssi = xplorer.call("ble.readRssi", deviceId=dev)
        assert -110 < rssi["rssi"] < 0, rssi
        link = xplorer.call("perf.getLink", deviceId=dev)
        assert link["mtu"] >= 23 and -110 < link["rssi"] < 0 and link.get("phyEstimate"), link
    finally:
        _stop(server)


def test_xplorer_writes_a_characteristic_and_unsubscribes(app_exe, port_a, xplorer):
    address, name = _module(app_exe, port_a)
    server, out = _serve(app_exe, port_a, "gatt.server.nus")
    try:
        assert out.wait(r"service active", 60), out.text
        dev = _find_and_connect(xplorer, address, name)
        chars = _service(xplorer, dev, NUS)
        assert NUS_RX in chars and chars[NUS_TX]["notifiable"], chars
        xplorer.call("ble.writeCharacteristic", deviceId=dev, serviceId=NUS, characteristicId=NUS_RX,
                     value=list(b"hello-nus"), withResponse=False)
        assert out.wait(r"hello-nus", 20), out.drain()
        assert xplorer.call("ble.subscribe", deviceId=dev, serviceId=NUS, characteristicId=NUS_TX)["notifying"]
        time.sleep(1)
        _send(server, "gatt.server.notify 0 nus.tx text:one")
        got = xplorer.event("ble.onNotification", lambda p: p["value"] == list(b"one"))
        assert got, f"{xplorer.events[-5:]}\n{out.drain()}"
        xplorer.call("ble.unsubscribe", deviceId=dev, serviceId=NUS, characteristicId=NUS_TX)
        assert out.wait(r"\] 0000", 20), f"the module saw no CCCD write turning notifications off\n{out.drain()}"
        _send(server, "gatt.server.notify 0 nus.tx text:two")
        time.sleep(3)
        assert not xplorer.event("ble.onNotification", lambda p: p["value"] == list(b"two"), 0), \
            "the phone still got a notification after ble.unsubscribe"
    finally:
        _stop(server)


@pytest.mark.parametrize("mode", ["sps", "nus"])
def test_module_connects_to_the_phone_as_a_peripheral(app_exe, port_a, xplorer, mode):
    """The module as central: it finds the phone by its advertised name, connects and talks both ways."""
    _needs(xplorer, "blePeripheral")
    name = f"ucx-{mode}-{os.getpid() % 1000}"
    _module(app_exe, port_a)
    options = {"spsUseFlowControl": False} if mode == "sps" else {}
    adv = xplorer.call("peripheral.startAdvertising", timeout=30, mode=mode, name=name, **options)
    assert adv.get("ok") and adv.get("advertising"), adv
    server, out = _serve(app_exe, port_a)
    try:
        _send(server, f"bt.connect {name}")
        assert out.wait(r"connection \d+ \(current\)", 40), out.drain()
        _send(server, f"gatt.client.{mode}")
        assert out.wait(r"[1-9] subscription", 30), out.drain()
        # iOS reports a central only once it subscribes
        assert xplorer.event("peripheral.onConnectionChange", lambda p: p["connectionCount"] >= 1, 20), \
            out.drain()
        _send(server, f"gatt.send {mode} hello-from-module")
        got = xplorer.event("peripheral.onData", lambda p: "hello-from-module" in _text(p), 20)
        assert got, f"{xplorer.events[-5:]}\n{out.drain()}"
        xplorer.call("peripheral.send", text="hello-from-phone")
        assert out.wait(r"hello-from-phone", 20), f"phone: {xplorer.call('peripheral.getState')}\n{out.drain()}"
        state = xplorer.call("peripheral.getState")
        assert state["connectionCount"] >= 1 and state["bytesReceived"] >= len("hello-from-module"), state
    finally:
        _stop(server)
        xplorer.call("peripheral.stopAdvertising")


def _module_on_wifi(app_exe, port, tmp_path):
    """ucxapp shell with the module on Wi-Fi; returns the process, its output and the module's IP address."""
    _module(app_exe, port)
    server, out = _serve(app_exe, port, _wifi_connect_command(tmp_path), cwd=tmp_path)
    ip = out.wait(r"ip=(\d+\.\d+\.\d+\.\d+)", 90)
    if not ip:
        _stop(server)
        pytest.fail(f"the module did not join Wi-Fi\n{out.drain()}", pytrace=False)
    return server, out, ip.group(1)


@pytest.mark.wifi
@pytest.mark.parametrize("protocol", ["tcp", "udp"])
def test_module_talks_to_a_phone_socket_server(app_exe, port_a, xplorer, tmp_path, protocol):
    _needs(xplorer, "ipServer")
    port = 7410 if protocol == "tcp" else 7411
    server, out, _ = _module_on_wifi(app_exe, port_a, tmp_path)
    try:
        assert xplorer.call("socket.startServer", protocol=protocol, port=port, force=True)["ok"]
        _send(server, f"sock.open {protocol}")
        if protocol == "tcp":
            _send(server, f"sock.connect {_phone_ip()} {port}")
            assert out.wait(r"connected to", 30), out.drain()
            _send(server, "sock.send hello-from-module")
        else:
            # an unbound UDP socket only sends; the reply needs a local port (AT+USOB)
            _send(server, f"sock.bind {port + 1}")
            _send(server, f"sock.sendto {_phone_ip()} {port} hello-from-module")
        got = xplorer.event("socket.onData", lambda p: "hello-from-module" in _text(p), 20)
        assert got, f"{xplorer.events[-5:]}\n{out.drain()}"
        xplorer.call("socket.send", text="hello-from-phone")
        _send(server, "sock.read" if protocol == "tcp" else "sock.recvfrom")
        assert out.wait(r"received \d+ bytes.*hello-from-phone", 20), out.drain()
    finally:
        xplorer.call("socket.stop")
        _stop(server)


@pytest.mark.wifi
@pytest.mark.parametrize("protocol", ["tcp", "udp"])
def test_phone_socket_client_reaches_the_module(app_exe, port_a, xplorer, tmp_path, protocol):
    _needs(xplorer, "ipClient")
    port = 7412 if protocol == "tcp" else 7413
    server, out, ip = _module_on_wifi(app_exe, port_a, tmp_path)
    try:
        _send(server, f"sock.open {protocol}")
        _send(server, f"sock.listen {port}" if protocol == "tcp" else f"sock.bind {port}")
        assert out.wait(r"listening|bound to port", 20), out.drain()
        if protocol == "tcp":
            _send(server, "sock.accept 30")
        time.sleep(0.5)
        assert xplorer.call("socket.startClient", timeout=40, protocol=protocol, host=ip, port=port, force=True)["ok"]
        if protocol == "tcp":
            assert out.wait(r"accepted socket", 30), out.drain()
        sent = xplorer.call("socket.send", text="hello-from-phone")
        _send(server, "sock.read" if protocol == "tcp" else "sock.recvfrom")
        got = out.wait(r"received \d+ bytes(?: from ([\d.]+):(\d+))?.*hello-from-phone", 20)
        assert got, f"phone send: {sent}\nphone state: {xplorer.call('socket.getState')}\n{out.drain()}"
        if protocol == "tcp":
            _send(server, "sock.send hello-from-module")
        else:
            _send(server, f"sock.sendto {got.group(1)} {got.group(2)} hello-from-module")
        assert xplorer.event("socket.onData", lambda p: "hello-from-module" in _text(p), 20), \
            f"{xplorer.events[-5:]}\n{out.drain()}"
    finally:
        xplorer.call("socket.stop")
        _stop(server)


@pytest.mark.wifi
def test_module_talks_tls_to_the_phone(app_exe, port_a, xplorer, tmp_path):
    """The phone makes its own server certificate; the module trusts it as its CA and connects over TLS."""
    _needs(xplorer, "tls", "ipServer")
    cert = xplorer.call("tls.generateSelfSigned", timeout=60, commonName="ucxapp-test")
    assert "BEGIN CERTIFICATE" in cert.get("certPem", ""), cert
    pem = tmp_path / "xplorer-ca.pem"
    pem.write_bytes(cert["certPem"].encode())   # as sent: the module cannot parse a PEM with CRLF line ends
    server, out, _ = _module_on_wifi(app_exe, port_a, tmp_path)
    try:
        _send(server, "cert.remove xplorer-ca")
        _send(server, f"cert.upload ca {pem} xplorer-ca")
        assert xplorer.call("socket.startServer", protocol="tls", port=7414, force=True)["ok"]
        _send(server, "sock.open tcp")
        _send(server, "sock.tls xplorer-ca")
        assert out.wait(r"uses TLS with ca xplorer-ca", 30), out.drain()
        _send(server, f"sock.connect {_phone_ip()} 7414")
        assert out.wait(r"connected to", 40), out.drain()
        _send(server, "sock.send hello-over-tls")
        got = xplorer.event("socket.onData", lambda p: "hello-over-tls" in _text(p), 20)
        assert got, f"{xplorer.events[-5:]}\n{out.drain()}"
        xplorer.call("socket.send", text="hello-from-phone")
        _send(server, "sock.read")
        assert out.wait(r"received \d+ bytes.*hello-from-phone", 20), out.drain()
    finally:
        xplorer.call("socket.stop")
        _send(server, "sock.close all")
        _send(server, "cert.remove xplorer-ca")
        time.sleep(1)
        _stop(server)
