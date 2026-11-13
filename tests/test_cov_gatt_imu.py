"""Offline coverage of the GATT client/server commands and the IMU stream, against vucx and a TCP feeder."""

import os
import struct
import subprocess
import time

import pytest

pytestmark = pytest.mark.app

yaml = pytest.importorskip("yaml")

WPS_SVC = "14387800130C49E7B8772881C89CB258"
WPS_INFO = "14387801130C49E7B8772881C89CB258"
WPS_CTRL = "14387802130C49E7B8772881C89CB258"
WPS_DATA = "14387803130C49E7B8772881C89CB258"
SPS_SVC = "2456E1B926E28F83E744F34F01E9D701"
SPS_FIFO = "2456E1B926E28F83E744F34F01E9D703"
SPS_CRED = "2456E1B926E28F83E744F34F01E9D704"
NUS_SVC = "6E400001B5A3F393E0A9E50E24DCCA9E"
NUS_RX = "6E400002B5A3F393E0A9E50E24DCCA9E"
NUS_TX = "6E400003B5A3F393E0A9E50E24DCCA9E"
WIT20 = "5561" + "0040" + "00C0" + "0008" + "0000" + "0100" + "FFFF" + "0040" + "0080" + "0000"


@pytest.fixture(scope="module")
def cwd(tmp_path_factory):
    return tmp_path_factory.mktemp("cov-gatt-imu")


def _profile(path, **extra):
    path.write_text(yaml.safe_dump({"include": ["nora-w36.yaml"], **extra}), encoding="utf-8")
    return str(path)


def _start(profile):
    from harness.vucx import Vucx

    module = Vucx(profile)
    module.start()
    return module


def _shell(exe, port, commands, cwd, timeout=60, env=None):
    out = subprocess.run([str(exe), port, "--shell"], input="\n".join(commands) + "\n", capture_output=True,
                         text=True, timeout=timeout, errors="replace", cwd=cwd, env=env)
    assert out.returncode in (0, 1), out.stdout[-3000:] + out.stderr[-2000:]
    return out.stdout


def _hex(text):
    return text.encode().hex().upper()


def _write_urcs(lines):
    return ["+UEBTGCW:0,{h:14387802}," + data + ",0" for data in lines]


# ---- protobuf helpers (nRF Wi-Fi provisioning) ----

def _varint(v):
    out = b""
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


def _pb_int(field, v):
    return _varint(field << 3) + _varint(v)


def _pb_bytes(field, data):
    return _varint((field << 3) | 2) + _varint(len(data)) + data


def _pb_config(ssid, password, auth=None):
    info = _pb_bytes(1, ssid.encode()) + (_pb_int(5, auth) if auth is not None else b"")
    cfg = _pb_bytes(1, info) + (_pb_bytes(2, password.encode()) if password else b"")
    return cfg


def _pb_req(op, cfg=None, extra=b""):
    return (_pb_int(1, op) + extra + (_pb_bytes(11, cfg) if cfg is not None else b"")).hex().upper()


# ---- GATT client ----

def test_gatt_client_decoders_and_raw_commands(app_exe, cwd, tmp_path):
    from harness.vucx import peer_table

    services = [
        {"uuid": "180D", "chars": [["2A37", "R", "01"], ["2A37", "R", "01E803"], ["2A37", "R N", "0048"]]},
        {"uuid": "1805", "chars": [["2A2B", "R", "0102"]]},
        {"uuid": "181A", "chars": [["2A6E", "R", "01"], ["2A6F", "R", "01"]]},
        {"uuid": "1819", "chars": [["2A67", "R", "00"], ["2A67", "R", "0000"], ["2A67", "R", "0400"],
                                   ["2A67", "R", "0700AABBCCDDEE0011223344556677"]]},
        {"uuid": "1815", "chars": [["2A56", "R", "A5"], ["2A58", "R", "01"], ["2A58", "R", "18FC"]]},
        {"uuid": "180F", "chars": [["2A19", "R N", "FF"], ["2A19", "R", "32"]]},
        {"uuid": "180A", "chars": [["2A25", "R", "41004243"], ["2A27", "R", "text:B1"], ["2A28", "R", "text:3.5"],
                                   ["2A23", "R", "0102030405060708"]]},
        {"uuid": "185B", "chars": [["2C14", "R", "01"], ["2C18", "R", "01"], ["2C19", "R", "0500"],
                                   ["2C19", "R", "05"]]},
        {"uuid": WPS_SVC, "chars": [[WPS_INFO, "R", "text:v1"], [WPS_CTRL, "W w", "00"],
                                    [WPS_DATA, "R N", "text:STATUS:DISCONNECTED"]]},
        {"uuid": SPS_SVC, "chars": [[SPS_FIFO, "R W w N", "text:hey"], [SPS_CRED, "R W w N", "05"]]},
        {"uuid": NUS_SVC, "chars": [[NUS_RX, "W w", "00"], [NUS_TX, "R N", "4100"]]},
        {"uuid": "FFE5", "chars": [["FFE4", "R N", "0102"], ["FFE9", "W w", "00"]]},
        {"uuid": "ABCD", "chars": [["ABCE", "R W w N I", "68690A"]]},
    ]
    peer = {"start": 100, "services": services}
    _, _, handles = peer_table(peer)
    gen, cred, wit = handles["ABCE"], handles[SPS_CRED], handles["FFE4"]
    urcs = {"AT+VUCXN": [f"+UEBTGCN:0,{cred},FF", f"+UEBTGCN:0,{wit},{WIT20}", f"+UEBTGCN:0,{gen},0102",
                         "+UEBTGCN:0,4000,AA", f"+UEBTGCN:0,{handles[NUS_TX]},6869",
                         f"+UEBTGCN:0,{handles[WPS_DATA]},{_hex('STATUS:CONNECTED:x:1.2.3.4')}",
                         f"+UEBTGCN:0,{handles['2A19']},32"]}
    module = _start(_profile(tmp_path / "client.yaml", peer=peer, urcs=urcs))
    try:
        out = _shell(app_exe, module.port, [
            "bt.connect C0:FF:EE:00:11:22", "gatt.services 0", "gatt.chars 0", "gatt.chars 0 1", "gatt.chars 0 x",
            "gatt.client.hrs 0", "gatt.client.cts 0", "gatt.client.ess 0", "gatt.client.lns 0", "gatt.client.aio 0",
            "gatt.client.bas 0", "gatt.client.dis 0", "gatt.client.ras 0", "gatt.client.wps 0", "gatt.client.nus 0",
            "gatt.client.witmotion 0", "gatt.client.hrs", "gatt.client.hrs 0 flow", "gatt.client.sps 0 x",
            "gatt.client.sps 0 flow", "gatt.send 0 sps hello", f"gatt.read 0 {cred}", "gatt.send 0 sps hello",
            "gatt.client.sps 0", "gatt.send 0 sps hi", "gatt.send 0 nus hello world", "gatt.send 0 bad x",
            "gatt.send 0 nus " + " ".join(["y" * 60] * 5), "gatt.sps.credits 0 5", "gatt.sps.credits 0 900",
            f"gatt.read 0 {gen}", f"gatt.read 0 {handles[WPS_INFO]}", f"gatt.read 0 {handles[WPS_DATA]}",
            f"gatt.read 0 {handles[SPS_FIFO]}", f"gatt.read 0 {handles[NUS_TX]}", f"gatt.read 0 {handles['2A23']}",
            f"gatt.read 0 {handles['2A25']}", f"gatt.read 0 {handles['2A27']}", f"gatt.read 0 {handles['2A28']}",
            "gatt.read 0 x", f"gatt.write 0 {gen} abCD", f"gatt.write 0 {gen} text:hi norsp",
            f"gatt.write 0 {gen} zz", f"gatt.write 0 {gen} abc", f"gatt.write 0 {gen} 01 bad",
            f"gatt.subscribe 0 {gen} indicate", f"gatt.subscribe 0 {gen} off", f"gatt.subscribe 0 {gen} maybe",
            f"gatt.subscribe 0 {gen} notify", "at +VUCXN", "gatt.listen 1", "gatt.listen", "gatt.listen x",
            "gatt.listen off", "gatt.listen", f"gatt.writelong 0 {gen} 0102 4 reliable more",
            f"gatt.writelong 0 {gen} 0102 x", f"gatt.writelong 0 {gen} 01 0 bogus", "gatt.wps x status",
            "gatt.wps 0 config a b BAD", "gatt.wps 0 bogus"], cwd)
    finally:
        module.stop()
    assert "[HRS] BPM: 72" in out and "[DIS] System ID" in out and "[WPS] Info: v1" in out, out[-3000:]
    assert any(c.startswith("AT+UBTGWL") or c.startswith("AT+UBTGW") for c in module.log), module.log[-20:]


def test_gatt_client_error_paths(app_exe, cwd, tmp_path):
    commands = {"AT+UBTGPSD=*": ["ERROR"], "AT+UBTGSCD=*": ["ERROR"], "AT+UBTGR=*": ["ERROR"],
                "AT+UBTGCDD=*": ["ERROR"], "AT+UBTG*": ["ERROR"]}
    module = _start(_profile(tmp_path / "cerr.yaml", peer={"start": 100, "services": []}, commands=commands))
    try:
        out1 = _shell(app_exe, module.port, [
            "bt.connect C0:FF:EE:00:11:22", "gatt.services 0", "gatt.read 0 21", "gatt.write 0 21 01",
            "gatt.writelong 0 21 0102", "gatt.subscribe 0 21 notify", "gatt.server.hrs",
            "gatt.server.hostchar 180F 2A19 12", "gatt.server.set 120 01", "gatt.server.readresp 0 01",
            "gatt.server.writeresp 0", "gatt.server.error 0 read 2", "gatt.server.error 0 write 3",
            "gatt.server.error 0 maybe 2", "gatt.server.error 0 write 300", "gatt.server.servicechanged 0 1 100",
            "gatt.server.servicechanged 0 5 1", "gatt.server.notify 0 50 01", "gatt.server.indicate 0 50 01",
            "gatt.server.hostchar zz 2A19 12", "gatt.server.hostchar 180F 2A19 12 3"], cwd)
        module.commands["AT+UBTGPSD=*"] = ["OK"]
        out2 = _shell(app_exe, module.port, ["bt.connect C0:FF:EE:00:11:22", "gatt.services 0",
                                             "gatt.client.wps 0", "gatt.wps 0 status", "gatt.send 0 nus x",
                                             "gatt.sps.credits 0 5"], cwd)
        module.commands["AT+UBTGPSD=*"] = ["+UBTGPSD:0,16,32,180D", "OK"]
        out3 = _shell(app_exe, module.port, ["bt.connect C0:FF:EE:00:11:22", "gatt.services 0",
                                             "gatt.client.hrs 0"], cwd)
        module.commands["AT+UBTGSCD=*"] = ["+UBTGSCD:0,17,12,18,2A37", "+UBTGSCD:0,20,02,21,2A38", "OK"]
        out4 = _shell(app_exe, module.port, ["bt.connect C0:FF:EE:00:11:22", "gatt.client.hrs 0"], cwd)
    finally:
        module.stop()
    assert "ERROR" in out1 and "ERROR" in out2, out1[-2000:] + out2[-2000:]
    assert out3 and out4


# ---- GATT server ----

def test_gatt_server_partial_failures(app_exe, cwd, tmp_path):
    commands = {"AT+UBTA?": ["+UBTA:0,0,[]", "OK"], "AT+UBTAL": [["ERROR"], ["OK"]],
                "AT+UBTGNS=*": ["ERROR"], "AT+UBTGIS=*": ["ERROR"],
                "AT+UBTGSA": [["ERROR"], ["OK"], ["OK"], ["OK"], ["OK"], ["OK"], ["OK"], ["OK"]]}
    urcs = {"AT+VUCXW": _write_urcs([_hex("GET_STATUS"), _pb_req(1)])}
    module = _start(_profile(tmp_path / "srv.yaml", commands=commands, urcs=urcs))
    try:
        out = _shell(app_exe, module.port, [
            "gatt.server.hostchar 180F 2A19 12 1 2", "gatt.server.hrs", "gatt.server.demo 0 hrs 3",
            "gatt.server.ras", "gatt.server.demo 0 ras 1", "gatt.server.wps", "at +VUCXW", "gatt.listen 1",
            "gatt.server.notify 0 hrs.measurement 01", "gatt.server.indicate 0 hrs.measurement 01",
            "gatt.server.set hrs.measurement zz", "gatt.server.set nosuch 01", "gatt.server.notify 0 nosuch 01",
            "gatt.server.demo 0 bas 1", "gatt.server.demo 0 nus", "gatt.server.demo 0 zzz",
            "gatt.server.demo 0 hrs 0", "gatt.server.hrs 0", "gatt.server.hrs 1", "gatt.server.wps 1"], cwd)
    finally:
        module.stop()
    assert "response not sent" in out, out[-3000:]


def test_gatt_server_demo_every_profile(app_exe, cwd, tmp_path):
    module = _start("nora-w36.yaml")
    try:
        out = _shell(app_exe, module.port, [
            "gatt.server.bas", "gatt.server.ess", "gatt.server.aio", "gatt.server.lns", "gatt.server.cts",
            "gatt.server.ras", "gatt.server.list", "gatt.server.demo 0 bas 1", "gatt.server.demo 0 ess 1",
            "gatt.server.demo 0 aio 1", "gatt.server.demo 0 lns 1", "gatt.server.demo 0 cts 1",
            "gatt.server.demo 0 ras 1", "gatt.server.bas 1"], cwd)
    finally:
        module.stop()
    assert any(c.startswith("AT+UBTGNS=") for c in module.log), module.log[-20:]
    assert out


def test_gatt_server_wps_text_and_protobuf(app_exe, cwd, tmp_path):
    cfg_wpa2 = _pb_config("vucx-net", "secret123", 4)
    urcs = {
        "AT+VUCXA": _write_urcs([_hex("SET_CONFIG:vucx-net:secret123:WPA2"), _hex("SET_CONFIG:bad"),
                                 _hex("CONNECT"), _hex("START_SCAN"), _hex("BOGUS")]),
        "AT+VUCXP": _write_urcs([_pb_req(2), _pb_req(4, cfg_wpa2)]),
        "AT+VUCXB": _write_urcs([_hex("GET_STATUS"), _hex("START_SCAN"), _hex("CONNECT"), _hex("FORGET"),
                                 _hex("SET_CONFIG:vucx-net:secret123:WPA3")]),
        "AT+VUCXC": _write_urcs([_hex("SET_CONFIG:open:-:OPEN"), _hex("SET_CONFIG:vucx-net:secret123:WPA2"),
                                 _hex("GET_STATUS")]),
        "AT+VUCXD": _write_urcs([_pb_req(1), _pb_req(2), _pb_req(3), _pb_req(5), _pb_req(9)]),
        "AT+VUCXE": _write_urcs([_pb_req(4), _pb_req(4, cfg_wpa2), _pb_req(4, _pb_config("other", "secret123", 6)),
                                 _pb_req(4, _pb_config("open", "", 0)), _pb_req(4, _pb_config("x", ""))]),
        "AT+VUCXF": _write_urcs([_pb_req(4, cfg_wpa2, extra=b"\x09" + bytes(8) + b"\x0d" + bytes(4)),
                                 "08041B", "08045A7F", "08FF", _pb_req(1)]),
        "AT+VUCXQ": _write_urcs([_hex("FORGET")] * 6),
        "AT+VUCXR": ["+UEBTGCW:0,{h:6E400002},414243,0", "+UEBTGCW:0,999,414243,0"],
    }
    module = _start(_profile(tmp_path / "wps.yaml", urcs=urcs))
    env = {k: v for k, v in os.environ.items() if k != "UCX_REGDOMAIN"}
    try:
        out = _shell(app_exe, module.port, [
            "gatt.server.nus", "gatt.server.wps", "at +VUCXA", "gatt.listen 1", "at +VUCXP", "gatt.listen 1",
            "wifi.regdomain etsi", "at +VUCXB", "gatt.listen 1", "at +VUCXC", "gatt.listen 1",
            "at +VUCXD", "gatt.listen 1", "at +VUCXE", "gatt.listen 1", "at +VUCXF", "gatt.listen 1",
            "at +VUCXQ", "gatt.listen 1", "gatt.listen", "at +VUCXR", "gatt.listen 1", "gatt.server.wps 1"],
            tmp_path, timeout=80, env=env)
    finally:
        module.stop()
    assert "[WPS] queue full, command dropped" in out, out[-4000:]
    assert "SET_CONFIG:vucx-net:***" in out and "op 9" in out, out[-4000:]


def test_gatt_wps_client_commands(app_exe, cwd):
    module = _start("nora-w36.yaml")
    try:
        out = _shell(app_exe, module.port, [
            "bt.connect C0:FF:EE:00:11:22", "gatt.client.wps 0", "gatt.wps 0 status", "gatt.wps 0 connect",
            "gatt.wps 0 forget", "gatt.wps 0 config vucx-net secret123", "gatt.wps 0 config a b WPA3",
            "gatt.wps 0 config a b OPEN", "gatt.wps 0 raw HELLO"], cwd)
    finally:
        module.stop()
    assert "sent: GET_STATUS" in out and "sent: SET_CONFIG:..." in out, out[-3000:]


# ---- IMU ----

def _wit(kind, *values):
    frame = bytes([0x55, kind]) + struct.pack("<4h", *values)
    return frame + bytes([sum(frame) & 0xFF])


def _wit_raw(kind, payload):
    frame = bytes([0x55, kind]) + payload
    return frame + bytes([sum(frame) & 0xFF])


def _interactive(exe, port, steps, cwd, timeout=60):
    log = cwd / f"imu-{time.monotonic_ns()}.txt"
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.Popen([str(exe), port, "--shell"], stdin=subprocess.PIPE, stdout=f,
                                stderr=subprocess.STDOUT, text=True, cwd=cwd)
        try:
            proc.stdin.write("\n".join(steps) + "\n")
            proc.stdin.close()
            rc = proc.wait(timeout=timeout)
        finally:
            if proc.poll() is None:
                proc.kill()
    out = log.read_text(encoding="utf-8", errors="replace")
    assert rc in (0, 1), out[-3000:]
    return out


def test_imu_witmotion_frames_and_errors(app_exe, cwd, tmp_path):
    from harness.vucx import Feeder

    data = (b"\x00\x12" + _wit(0x51, 100, 200, 2048, 0) + _wit(0x52, 1, 2, 3, 0) + _wit(0x53, 100, -100, 50, 0) +
            _wit(0x54, 10, 20, 30, 0) + _wit(0x59, 32767, 0, 0, 0) + _wit_raw(0x56, struct.pack("<ii", 101325, 120)) +
            bytes([0x55, 0x61]) + struct.pack("<9h", 1, 2, 3, 4, 5, 6, 7, 8, 9) +
            bytes([0x55, 0x51, 0x01, 0x55, 0x53, 0, 0, 0, 0, 0, 0x77]))
    module = _start("nora-w36.yaml")
    feeder = Feeder(data, 0.05)
    fport = feeder.start()
    try:
        out = _interactive(app_exe, module.port, [
            "imu.status", "imu.log x", "imu.send 01", "imu.rate 10", "imu.close",
            f"imu.open {fport} 100", f"imu.open {fport} 115200 bogus", f"imu.open {fport} 115200 wit",
            "gatt.listen 1", "imu.status", "dash", f"imu.open {fport}", "imu.open tcp:127.0.0.1:1",
            f"imu.detect {fport}", "imu.send zz", "imu.send 123", "imu.send " + "00" * 70, "imu.send FF AA 69 00 00",
            f"imu.log {tmp_path / 'no' / 'dir' / 'x.log'}", f"imu.log {tmp_path / 'imu.log'}", "gatt.listen 1",
            "imu.log off", "imu.rate 7", "imu.rate 10",
            "imu.bridge ble custom 12345678123412341234123456789ABC", "imu.bridge off",
            "imu.bridge ble custom 12345678-1234-1234-1234-123456789ABC", "imu.bridge off", "imu.close",
            "imu.status"], tmp_path)
    finally:
        feeder.stop()
        module.stop()
    assert "Quat" in out and "Baro" in out, out[-4000:]


def test_imu_bno055_stream(app_exe, cwd, tmp_path):
    from harness.vucx import Feeder

    data = (b"\x12\xbb\x01\xa0" + b"\xbb\x20" + bytes(range(1, 33)) + b"\xee\x01\xee\x07" + b"\xbb\x00" +
            b"\xbb\x40" + b"\xbb\x05" + bytes(5))
    module = _start("nora-w36.yaml")
    feeder = Feeder(data, 0.05)
    fport = feeder.start()
    try:
        out = _interactive(app_exe, module.port, [
            f"imu.open {fport} 115200 bno055", "gatt.listen 1", "imu.status", "dash", "imu.rate 10",
            "imu.bridge tcps 9123", "imu.bridge off", "imu.close"], tmp_path)
    finally:
        feeder.stop()
        module.stop()
    assert "BNO055" in out or "bno055" in out, out[-3000:]
    assert feeder.received, "the app should poll the BNO055"


def test_imu_detect_on_virtual_imu(app_exe, cwd, tmp_path):
    module = _start("nora-w36.yaml")
    try:
        out = _interactive(app_exe, module.port, [
            "imu.detect virt:imu", "imu.open virt:imu", "gatt.listen 1", "imu.status", "imu.detect",
            "imu.close"], tmp_path)
    finally:
        module.stop()
    assert "virt:imu" in out, out[-3000:]
