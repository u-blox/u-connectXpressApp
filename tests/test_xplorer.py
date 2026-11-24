"""xplorer.* and perf ... xplorer against a fake u-connectXplorer remote control (JSON-RPC 2.0 over a WebSocket),
so the client, its JSON reading and the error paths run without a phone. tests/hardware drives the real app."""

import json
import socket
import subprocess
import threading

import pytest
from harness.xplorer import TOKEN, V1, FakePhone

pytestmark = pytest.mark.app


@pytest.fixture
def phone():
    fake = FakePhone()
    yield fake
    fake.close()


_MODULE = {}


@pytest.fixture(autouse=True)
def module():
    """The virtual NORA-W36: the app reaches the (fake) phone through its TCP sockets, as on the real module."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    vucx = Vucx("nora-w36.yaml")
    vucx.net.relay = {"127.0.0.1"}
    vucx.start()
    _MODULE["port"] = vucx.port
    yield vucx
    vucx.stop()


def _run(app_exe, cwd, *commands, offline=False):
    args = [str(app_exe), "--offline" if offline else _MODULE["port"]]
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


def test_connect_reads_the_documented_version_field(app_exe, tmp_path, phone):
    """REMOTE_CONTROL.md documents only "version"; fullVersion is an extra some app builds send."""
    phone.handlers["app.version"] = lambda params: {"version": "2.1.0", "app": "u-connectXplorer"}
    out = _connect(app_exe, tmp_path, phone)
    assert "u-connectXplorer 2.1.0 on ios" in out.stdout, out.stdout


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


def test_a_challenge_is_answered_so_the_token_is_never_sent(app_exe, tmp_path):
    """A phone with authChallenge gets the HMAC-SHA256 of its nonce under the token; the token itself does not
    cross the wire, on xplorer.connect or on any later command, and a wrong token is still refused."""
    fake = FakePhone(features=V1, protocol=1)
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert "saved" in out.stdout and "note:" not in out.stdout, out.stdout
        assert _run(app_exe, tmp_path, "xplorer.info").returncode == 0
        answers = fake.called("app.authenticate")
        assert len(answers) == 2 and all(set(params) == {"response"} for params in answers), answers
        assert len(fake.called("app.getChallenge")) == 2 and TOKEN not in json.dumps(fake.calls), fake.calls
        out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{fake.port} WRONG")
        assert out.returncode != 0 and "Invalid token" in out.stdout, out.stdout
        assert "check the token on the Remote Control screen" in out.stdout, out.stdout
    finally:
        fake.close()


def test_a_phone_with_a_newer_protocol_gets_a_note(app_exe, tmp_path):
    fake = FakePhone(protocol=2)
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert "note: this u-connectXplorer speaks remote control version 2, this app knows version 1" in out.stdout
        assert "saved" in out.stdout, out.stdout
    finally:
        fake.close()


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
    assert json.loads(out.stdout.split("\n", 1)[1])["note"] == 'a "quoted" text'   # after "connected to ..."
    out = _run(app_exe, tmp_path, "xplorer.call no.such")
    assert out.returncode != 0 and "ERROR: no.such: Method not found: no.such (-32601)" in out.stdout, out.stdout


def test_another_remote_taking_the_phone_over_is_named(app_exe, tmp_path, phone):
    """The phone serves one remote at a time and closes the older one with 4001; say so instead of 'lost'."""
    _connect(app_exe, tmp_path, phone)
    phone.replace_on.add("ble.getState")
    out = _run(app_exe, tmp_path, "xplorer.call ble.getState")
    assert out.returncode != 0, out.stdout
    assert "another remote took over the phone (Replaced by a newer client)" in out.stdout, out.stdout


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


@pytest.mark.parametrize("command", ["perf.latency udp xplorer 7000", "perf.throughput tcp xplorer 7001",
                                     "perf.phone rx",
                                     "perf.phone", "perf.phone up", "perf.phone tx 0"])
def test_perf_with_the_phone_needs_a_device(app_exe, tmp_path, phone, command):
    _connect(app_exe, tmp_path, phone)
    calls = len(phone.calls)
    out = _run(app_exe, tmp_path, command, offline=True)
    assert out.returncode != 0 and "not connected to a module" in out.stdout, out.stdout
    assert len(phone.calls) == calls, "nothing may start on the phone without a module"


def test_connect_needs_the_module(app_exe, tmp_path, phone, module):
    """The phone is reached through the module's Wi-Fi, never the host network."""
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{phone.port} {TOKEN}", offline=True)
    assert out.returncode != 0 and "no module connected" in out.stdout, out.stdout
    assert not phone.calls
    _connect(app_exe, tmp_path, phone)
    assert any(c.startswith('AT+USOC=0,"127.0.0.1"') or c.startswith("AT+USOC=0,127.0.0.1") for c in module.log), \
        module.log


ALL_FEATURES = {**V1, "blePeripheral": True}


def test_selftest_phone_passes_on_an_up_to_date_phone(app_exe, tmp_path):
    fake = FakePhone(features=ALL_FEATURES, protocol=1)
    fake.bluetooth = "poweredOn"
    fake.handlers["app.getNetworkInfo"] = lambda params: {"ip": "127.0.0.1", "wifi": True}
    try:
        _connect(app_exe, tmp_path, fake)
        out = _run(app_exe, tmp_path, "xplorer.selftest phone")
        assert out.returncode == 0, out.stdout
        for step in ("connect to the phone: 127.0.0.1", "phone answers app.version", "phone Bluetooth is on",
                     "protocol version: protocol 1, ucxapp speaks 1", "phone features",
                     "phone answers app.getNetworkInfo"):
            assert f"[PASS] {step}" in out.stdout, out.stdout
        assert "selftest: 6 passed, 0 failed, 0 skipped" in out.stdout, out.stdout
    finally:
        fake.close()


def test_selftest_phone_names_what_an_old_phone_lacks(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    phone.handlers["app.getPlatform"] = lambda params: {"platform": "ios"}
    out = _run(app_exe, tmp_path, "xplorer.selftest phone")
    assert out.returncode != 0, out.stdout
    assert "[FAIL] phone Bluetooth is on: app.getPlatform gave no bluetoothState" in out.stdout, out.stdout
    assert "[FAIL] protocol version: no protocolVersion; update u-connectXplorer" in out.stdout, out.stdout
    assert ("[FAIL] phone features: missing authChallenge, stopOnDisconnect, waitForDevice, blePeripheral"
            in out.stdout), out.stdout
    assert "[FAIL] phone answers app.getNetworkInfo" in out.stdout, out.stdout
    phone.handlers.pop("app.getPlatform")
    out = _run(app_exe, tmp_path, "xplorer.selftest phone")
    assert "[FAIL] phone Bluetooth is on: poweredOff" in out.stdout, out.stdout


def test_selftest_ble_and_wifi_report_each_step(app_exe, tmp_path):
    fake = FakePhone(features=ALL_FEATURES, protocol=1)
    fake.handlers["peripheral.startAdvertising"] = lambda p: {"error": {"code": -32010, "message": "busy"}}
    try:
        _connect(app_exe, tmp_path, fake)
        out = _run(app_exe, tmp_path, "xplorer.selftest ble")
        assert out.returncode != 0, out.stdout
        assert "[FAIL] module as central to the phone (nus both ways): peripheral.startAdvertising failed" \
            in out.stdout, out.stdout
        assert "> perf.phone tx 3" in out.stdout and "[FAIL] perf.phone tx (phone as central over SPS)" \
            in out.stdout, out.stdout
        assert fake.called("peripheral.startAdvertising") == [{"mode": "nus", "name": fake.called(
            "peripheral.startAdvertising")[0]["name"]}]
        assert fake.called("peripheral.startAdvertising")[0]["name"].startswith("ucx-st-")
        # The virtual module is on Wi-Fi: UDP reaches the phone's echo, TCP finds nobody listening on the fake.
        out = _run(app_exe, tmp_path, "xplorer.selftest wifi")
        assert "> perf.latency udp xplorer 7401 10" in out.stdout, out.stdout
        assert "10 sent, 10 received, 0 lost" in out.stdout and "[PASS] perf.latency udp to the phone" in out.stdout
        assert "ERROR: cannot connect to 127.0.0.1:7402" in out.stdout, out.stdout
        assert "[FAIL] perf.latency tcp to the phone" in out.stdout, out.stdout
        assert fake.called("socket.startServer")[0]["port"] == 7401, fake.calls
    finally:
        fake.close()


def test_selftest_ble_skips_central_on_a_phone_without_a_peripheral(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    out = _run(app_exe, tmp_path, "xplorer.selftest ble")
    assert ("[SKIP] module as central to the phone (nus both ways): the phone app has no blePeripheral"
            in out.stdout), out.stdout
    assert not phone.called("peripheral.startAdvertising")


@pytest.mark.parametrize("command", ["xplorer.selftest bogus", "xplorer.selftest phone extra"])
def test_selftest_usage(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


def test_strings_are_unescaped_and_a_long_answer_takes_the_64_bit_length(app_exe, tmp_path, phone):
    phone.handlers["app.version"] = lambda params: {"version": "2.1\t\u00e9A\nB", "app": "u-connectXplorer"}
    phone.handlers["big.data"] = lambda params: {"blob": "x" * 70000}
    out = _connect(app_exe, tmp_path, phone)
    assert "u-connectXplorer 2.1\t?A\nB on ios" in out.stdout, out.stdout
    out = _run(app_exe, tmp_path, "xplorer.call big.data", 'xplorer.call echo.params s=a"b\\c')
    assert out.returncode == 0 and '{"blob": "' + "x" * 500 in out.stdout, out.stdout[-500:]   # printing is capped
    assert phone.calls[-1] == ("echo.params", {"s": 'a"b\\c'}), phone.calls[-1]


def test_selftest_ble_central_sends_both_ways_over_nus(app_exe, tmp_path, module):
    """The module finds the phone's advertising name, connects as central, sends over NUS and the phone gets it (as
    a byte array), then the phone's text arrives at the module; the link is closed and the advertising stopped."""
    fake = FakePhone(features=ALL_FEATURES, protocol=1)

    def advertise(params):
        module.commands["AT+UBTD*"] = [f'+UBTD:C0FFEE001122p,-50,"{params["name"]}",0,020106', "OK"]
        return {"result": {"ok": True}}

    def send(params):
        module._send_quiet([f"+UEBTGCN:0,{nus_tx},{params['text'].encode().hex().upper()}"])
        return {"result": {"ok": True}}

    handle = module.handle

    def written(cmd, data=b""):
        handle(cmd, data)
        if "hello-from-module".encode().hex().upper() in cmd:
            fake.notify("peripheral.onData", {"value": list(b"hello-from-module")})

    nus_tx = 0x92   # the peer's NUS TX value handle in the virtual module (gatt.client.nus lists it)
    module.handle = written
    fake.handlers["peripheral.startAdvertising"] = advertise
    fake.handlers["peripheral.send"] = send
    fake.handlers["peripheral.stopAdvertising"] = lambda p: {"result": {"ok": True}}
    try:
        _connect(app_exe, tmp_path, fake)
        out = _run(app_exe, tmp_path, "xplorer.selftest ble")
    finally:
        fake.close()
    assert "[PASS] module as central to the phone (nus both ways)" in out.stdout, out.stdout
    assert "NUS TX)  handle 0x0092" in out.stdout and "[NUS RX] hello-from-phone" in out.stdout, out.stdout
    name = fake.called("peripheral.startAdvertising")[0]["name"]
    assert f"connecting to {name} (C0:FF:EE:00:11:22)" in out.stdout and "connection 0 closed" in out.stdout
    assert fake.called("peripheral.send") == [{"text": "hello-from-phone"}], fake.calls
    assert fake.called("peripheral.stopAdvertising"), fake.calls
    assert any(c.startswith("AT+UBTDC=") for c in module.log), module.log[-20:]
