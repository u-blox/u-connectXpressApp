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
from hwutil import _args, _reboot, _run, _stop
from test_gatt_isolate import _address, _Reader

pytestmark = [pytest.mark.hardware, pytest.mark.xplorer]

BAS = "0000180f-0000-1000-8000-00805f9b34fb"
BAS_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"
WPS = "14387800-130c-49e7-b877-2881c89cb258"
WPS_CTRL = "14387802-130c-49e7-b877-2881c89cb258"
WPS_DATA = "14387803-130c-49e7-b877-2881c89cb258"


class Xplorer:
    """A JSON-RPC 2.0 client for u-connectXplorer; events (no id) are kept in self.events."""

    def __init__(self, url, token):
        from websockets.sync.client import connect

        self.ws = connect(url, open_timeout=10, max_size=None)
        self.ids = itertools.count(1)
        self.replies = {}
        self.events = []
        self.cond = threading.Condition()
        threading.Thread(target=self._read, daemon=True).start()
        if token:
            assert self.call("app.authenticate", token=token).get("authenticated"), "wrong UCX_XPLORER_TOKEN"

    def _read(self):
        try:
            for raw in self.ws:
                msg = json.loads(raw)
                with self.cond:
                    if "id" in msg and msg["id"] is not None:
                        self.replies[msg["id"]] = msg
                    else:
                        self.events.append(msg)
                    self.cond.notify_all()
        except Exception:
            pass

    def call(self, method, timeout=30, **params):
        rid = next(self.ids)
        self.ws.send(json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}))
        with self.cond:
            assert self.cond.wait_for(lambda: rid in self.replies, timeout), f"{method}: no reply in {timeout} s"
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
    xp = Xplorer(os.environ["UCX_XPLORER"], os.environ.get("UCX_XPLORER_TOKEN", ""))
    xp.call("ble.disconnect")
    yield xp
    xp.close()


def _serve(app_exe, port, *commands):
    proc = subprocess.Popen(_args(app_exe, port, *commands) + ["--shell"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
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
