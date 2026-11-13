"""perf.phone and perf.latency / perf.throughput ... xplorer with both ends present: the fake u-connectXplorer
(tests/harness/xplorer.py) and the virtual module. The phone's part of a run (scan, connect, SPS, the throughput
session and its result) comes from the fake; when it "connects", the module reports the link and the SPS channel."""

import subprocess
import threading

import pytest
from harness.xplorer import TOKEN, FakePhone

pytestmark = pytest.mark.app

PEER = "C0FFEE001122p"
STATS = {"txBytes": 0, "rxBytes": 5000, "throughputKbps": 40, "patternBreaks": 0}


def _module(profile_changes=None):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["commands"].update(profile_changes or {})
    module = Vucx(profile)
    module.start()
    return module


def _save_phone(app_exe, tmp_path, phone):
    out = subprocess.run([str(app_exe), "--offline", "--exec", f"xplorer.connect 127.0.0.1:{phone.port} {TOKEN}"],
                         capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path,
                         stdin=subprocess.DEVNULL)
    assert out.returncode == 0, out.stdout


def _exec(app_exe, module, command, cwd, timeout=60):
    try:
        return subprocess.run([str(app_exe), module.port, "--exec", command], capture_output=True, text=True,
                              timeout=timeout, errors="replace", stdin=subprocess.DEVNULL, cwd=cwd)
    except subprocess.TimeoutExpired as e:
        text = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"hung after {timeout} s:\n{text[-3000:]}\nlast AT: {module.log[-15:]}")


def _phone(module, stats=None, after=1.0, on_start=None, started=None):
    """A phone with Bluetooth on that finds the module, connects (the module then reports the link and SPS) and
    runs a throughput session: `stats` come as perf.onComplete `after` seconds into it (None: never, so the app
    asks with perf.stop)."""
    phone = FakePhone()
    phone.bluetooth = "poweredOn"
    result = stats or STATS

    def sps_connect(_params):
        module.net.sps[0] = b""
        module._send([f"+UEBTC:0,{PEER}", "+UESPSC:0"])
        return {"ok": True}

    def start(params):
        if on_start:
            on_start()
        if stats is not None:
            threading.Timer(after, phone.notify, ["perf.onComplete", result]).start()
        return {"ok": True} if started is None else started

    phone.handlers.update({
        "ble.startScan": lambda p: {"ok": True},
        "ble.getDevices": lambda p: {"devices": [{"name": "someone else", "id": "AA"}, {"name": "vucx", "id": "ID-1"}]},
        "ble.stopScan": lambda p: {"ok": True},
        "ble.connect": lambda p: {"mtu": 247},
        "sps.connect": sps_connect,
        "perf.startThroughput": start,
        "perf.stop": lambda p: result,
        "sps.disconnect": lambda p: {"ok": True},
        "ble.disconnect": lambda p: {"ok": True},
    })
    return phone


# ---- perf.phone: the three directions ----------------------------------------------------------------------


def test_phone_receives_what_the_module_sends(app_exe, tmp_path):
    module = _module({"AT+USPSWB=*": ["+USPSWB:0,100", "OK"]})
    phone = _phone(module, STATS)
    phone.handlers["sps.connect"] = lambda p: (module._send([f"+UEBTC:0,{PEER}", "+UESPSC:0"]), {"ok": True})[1]
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.phone tx 1 100", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode == 0, text
    assert 'phone scans for "vucx"' in text, text
    assert "SPS with the phone (MTU 247): module sends, phone rx, 1 s, 100-byte packets" in text, text
    assert "phone:  sent 0 bytes, received 5000 bytes, 40 kbit/s, pattern 0x00-0xFF: 0 breaks" in text, text
    assert "AT+USPS=1" in module.log and any(c.startswith("AT+USPSWB=0") for c in module.log), module.log[-20:]
    assert phone.called("ble.connect") == [{"deviceId": "ID-1"}], phone.calls
    assert phone.called("sps.connect") == [{"deviceId": "ID-1", "force": True}], phone.calls
    assert phone.called("perf.startThroughput") == [{"deviceId": "ID-1", "mode": "rx", "durationMs": 1000,
                                                     "packetSize": 100, "sps": True}], phone.calls
    assert phone.called("sps.disconnect") and phone.called("ble.disconnect") == [{"deviceId": "ID-1"}], phone.calls
    # the module's own byte count, from the writes it made
    sent = 100 * sum(1 for c in module.log if c.startswith("AT+USPSWB=0"))
    assert f"module: sent {sent} bytes" in text, text


def test_module_receives_and_checks_what_the_phone_sends(app_exe, tmp_path):
    module = _module()
    pattern = bytes(range(256)) * 3

    def phone_sends():
        module.net.sps[0] = pattern
        module._send([f"+UESPSDA:0,{len(pattern)}"])

    phone = _phone(module, {"txBytes": len(pattern), "rxBytes": 0, "throughputKbps": 6, "patternBreaks": 0},
                   on_start=phone_sends, started={"packetSize": 200})
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.phone rx 1", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode == 0, text
    assert "module receives, phone tx, 1 s, 200-byte packets" in text, text
    assert f"received {len(pattern)} bytes" in text and "pattern 0x00-0xFF: 0 breaks" in text, text
    assert f"phone:  sent {len(pattern)} bytes, received 0 bytes" in text, text
    assert phone.called("perf.startThroughput")[0]["mode"] == "tx", phone.calls


def test_a_broken_pattern_from_the_phone_fails_the_run(app_exe, tmp_path):
    module = _module()
    data = bytes(range(100)) + bytes(range(50, 150))   # one jump in the 0x00-0xFF counter

    def phone_sends():
        module.net.sps[0] = data
        module._send([f"+UESPSDA:0,{len(data)}"])

    phone = _phone(module, {"txBytes": len(data), "rxBytes": 0, "throughputKbps": 1, "patternBreaks": 0},
                   on_start=phone_sends)
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.phone rx 1", tmp_path)
    finally:
        module.stop()
        phone.close()
    assert out.returncode != 0 and "pattern 0x00-0xFF: 1 break\n" in out.stdout, out.stdout


def test_module_echoes_what_the_phone_sends(app_exe, tmp_path):
    module = _module()

    def phone_sends():
        module.net.sps[0] = b"ping" * 16
        module._send(["+UESPSDA:0,64"])

    stats = {"txBytes": 640, "rxBytes": 640, "throughputKbps": 5, "patternBreaks": 0,
             "rttMs": {"count": 10, "min": 3.1, "avg": 4.5, "max": 9.0, "jitter": 1.2}}
    phone = _phone(module, stats, on_start=phone_sends)
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.phone echo 1 64", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode == 0, text
    assert "module echoes, phone echo, 1 s, 64-byte packets" in text, text
    assert "phone:  rtt min 3.1 ms, avg 4.5 ms, max 9.0 ms, jitter 1.2 ms (10 echoes)" in text, text
    echoed = [data for cmd, data in module.payloads if cmd.startswith("AT+USPSWB=0")]
    assert echoed and echoed[0] == b"ping" * 16, module.log[-20:]


def test_the_link_closing_ends_the_run_and_asks_the_phone_for_its_result(app_exe, tmp_path):
    module = _module({"AT+USPSWB=*": ["+USPSWB:0,50", "OK"]})
    phone = _phone(module, stats=None, on_start=lambda: threading.Timer(0.3, module._send_quiet,
                                                                         [["+UESPSDC:0"]]).start())
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.phone tx 2 50", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode != 0 and "ERROR: the SPS link closed during the run" in text, text
    assert phone.called("perf.stop"), phone.calls
    assert "phone:  sent 0 bytes, received 5000 bytes" in text, text


# ---- perf.phone: what can go wrong before the run ----------------------------------------------------------


def _fails(app_exe, tmp_path, phone, module, message, command="perf.phone tx 1"):
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, command, tmp_path)
    finally:
        module.stop()
        phone.close()
    assert out.returncode != 0 and message in out.stdout, out.stdout
    return out


def test_phone_bluetooth_is_off(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.bluetooth = "poweredOff"
    _fails(app_exe, tmp_path, phone, module, "ERROR: the phone's Bluetooth is poweredOff; turn it on")
    assert not phone.called("ble.startScan") and "AT+USPS=1" not in module.log, phone.calls


def test_phone_app_without_sps_perf(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.features = {"socketEcho": True}
    _fails(app_exe, tmp_path, phone, module, "cannot run perf over SPS (needs perfSps); update the app")
    assert not phone.called("ble.startScan"), phone.calls


def test_module_without_the_sps_service(app_exe, tmp_path):
    module = _module({"AT+USPS=*": ["ERROR"]})
    phone = _phone(module)
    _fails(app_exe, tmp_path, phone, module, "ERROR: cannot enable the SPS service")
    assert not phone.called("ble.startScan"), phone.calls


def test_module_name_cannot_be_read(app_exe, tmp_path):
    module = _module({"AT+UBTLN?": ["ERROR"]})
    phone = _phone(module)
    _fails(app_exe, tmp_path, phone, module, "ERROR: cannot read the module's Bluetooth name (AT+UBTLN)")
    assert len(phone.called("app.authenticate")) == 1, "only xplorer.connect may have reached the phone"


def test_phone_does_not_see_the_module(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.handlers["ble.getDevices"] = lambda p: {"scanning": True}   # no list at all: the scan gave up
    _fails(app_exe, tmp_path, phone, module, 'ERROR: the phone did not see "vucx" advertising within 15 s')
    assert phone.called("ble.stopScan") and not phone.called("ble.connect"), phone.calls


def test_phone_cannot_scan(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.handlers["ble.startScan"] = lambda p: {"error": {"code": -32010, "message": "Bluetooth unauthorized"}}
    _fails(app_exe, tmp_path, phone, module, "ERROR: ble.startScan: Bluetooth unauthorized (-32010)")
    assert not phone.called("ble.getDevices"), phone.calls


def test_phone_cannot_connect(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.handlers["ble.connect"] = lambda p: {"error": {"code": -32011, "message": "Connection timed out"}}
    _fails(app_exe, tmp_path, phone, module, "ERROR: ble.connect: Connection timed out (-32011)")
    assert not phone.called("sps.connect"), phone.calls


def test_link_drops_before_sps_connects(app_exe, tmp_path):
    module = _module()
    phone = _phone(module)
    phone.handlers["sps.connect"] = lambda p: {"error": {"code": -32012, "message": "Device disconnected"}}
    _fails(app_exe, tmp_path, phone, module, "the Bluetooth link dropped right after connecting")
    assert phone.called("ble.disconnect") and not phone.called("perf.startThroughput"), phone.calls


def test_phone_refuses_the_throughput_session(app_exe, tmp_path):
    module = _module()
    phone = _phone(module, started={"error": {"code": -32013, "message": "A session is already running"}})
    _fails(app_exe, tmp_path, phone, module, "ERROR: perf.startThroughput: A session is already running (-32013)")
    assert phone.called("sps.disconnect") and phone.called("ble.disconnect"), phone.calls
    assert not any(c.startswith("AT+USPSWB") for c in module.log), module.log[-20:]


# ---- perf.latency / perf.throughput <tcp|udp> xplorer <port>: the phone is the echo server ---------------------


def test_latency_against_the_phones_udp_echo(app_exe, tmp_path):
    module = _module()
    phone = FakePhone()
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.latency udp xplorer 7000 2 32", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode == 0, text
    assert "phone 127.0.0.1 echoes UDP on port 7000" in text, text
    assert "RTT over UDP, 2 packets of 32 bytes" in text and "2 sent, 2 received, 0 lost" in text, text
    assert "phone: udp listening port 7000 echo, received 2048 bytes (4 packets)" in text, text
    assert phone.called("socket.startServer") == [{"protocol": "udp", "port": 7000, "echo": True, "force": True}]
    assert phone.echo is None, "the echo server must be stopped after the run"
    assert any(c.startswith("AT+USOSTB=") and ",127.0.0.1,7000" in c for c in module.log), module.log[-20:]


def test_tcp_stream_that_reached_the_phone_with_gaps(app_exe, tmp_path):
    module = _module()
    phone = FakePhone()
    phone.breaks = 2
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.throughput tcp xplorer 7001 1 100", tmp_path)
    finally:
        module.stop()
        phone.close()
    text = out.stdout
    assert out.returncode != 0, text
    assert "phone 127.0.0.1 echoes TCP on port 7001" in text and "throughput over TCP for 1 s" in text, text
    assert "pattern 0x00-0xFF: 2 breaks" in text, text
    assert "ERROR: the TCP stream reached the phone with gaps" in text, text


def test_phone_app_that_cannot_echo_starts_nothing_on_the_module(app_exe, tmp_path):
    module = _module()
    phone = FakePhone(features={})
    try:
        _save_phone(app_exe, tmp_path, phone)
        out = _exec(app_exe, module, "perf.latency tcp xplorer 7001", tmp_path)
    finally:
        module.stop()
        phone.close()
    assert out.returncode != 0 and "cannot echo" in out.stdout, out.stdout
    assert not any(c.startswith("AT+USOCR") for c in module.log), module.log[-20:]
