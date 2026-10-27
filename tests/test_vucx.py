"""The app against vucx, the virtual u-connectXpress module (needs a pty: Linux/WSL, run by ucxapp --coverage)."""

import subprocess
import sys

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]


@pytest.fixture
def vucx():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("basic.yaml")
    module.start()
    yield module
    module.stop()


def _run(exe, port, *commands, timeout=60):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL)


def test_empty_module_answers_only_setup(app_exe):
    from harness.vucx import Vucx

    module = Vucx()
    try:
        out = _run(app_exe, module.start(), "at +GMM")
    finally:
        module.stop()
    assert "ATE0" in module.log and "AT+GMM" in module.log, module.log
    assert "ERROR" in out.stdout, out.stdout


def test_dev_info_shows_profile_identity(app_exe, vucx):
    out = _run(app_exe, vucx.port, "dev.info")
    assert out.returncode == 0, out.stdout
    assert "NORA-W36" in out.stdout and "3.5.0-0" in out.stdout, out.stdout


def test_reboot_waits_for_startup_event(app_exe, vucx):
    out = _run(app_exe, vucx.port, "dev.reboot")
    assert "AT+CPWROFF" in vucx.log, vucx.log
    assert "module rebooted" in out.stdout, out.stdout


def test_unknown_command_reports_error(app_exe, vucx):
    out = _run(app_exe, vucx.port, "at +UNKNOWN")
    assert "ERROR" in out.stdout, out.stdout


@pytest.fixture
def wifi_module():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("wifi.yaml")
    module.start()
    yield module
    module.stop()


def test_wifi_scan_lists_networks_strongest_first(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "wifi.scan")
    assert out.returncode == 0, out.stdout + str(wifi_module.log)
    assert out.stdout.index("vucx-net") < out.stdout.index("vucx-far"), out.stdout


def test_wifi_connect_status_disconnect(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "wifi.connect vucx-net secret123", "wifi.status", "wifi.disconnect")
    assert any(c.startswith("AT+UWSC=") for c in wifi_module.log), wifi_module.log
    assert "AT+UWSDC" in wifi_module.log, wifi_module.log
    assert "192.168.1.50" in out.stdout, out.stdout + str(wifi_module.log)


def test_socket_open_connect_list_close(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "sock.open tcp", "sock.connect 0 example.com 80", "sock.list",
               "sock.close 0")
    log = wifi_module.log
    assert any(c.startswith("AT+USOCR=") for c in log), log
    assert any(c.startswith("AT+USOC=") for c in log), log
    assert any(c.startswith("AT+USOCL=") for c in log), log
    assert out.returncode == 0, out.stdout + str(log)


def test_net_dns_resolves(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "net.dns example.com")
    assert "93.184.216.34" in out.stdout, out.stdout + str(wifi_module.log)


# Whole command groups against a module that answers OK to what it does not know: no crash, no hang.
SCENARIOS = {
    "dev": ["dev.info", "dev.caps", "dev.status", "dev.uptime", "dev.stack", "dev.crash", "dev.diagnose example.com"],
    "wifi": ["wifi.scan", "wifi.connect 1 secret123", "wifi.status", "wifi.rssi 2 0", "wifi.ip",
             "wifi.ip 192.168.1.50 255.255.255.0 192.168.1.1 8.8.8.8", "wifi.ip dhcp", "wifi.roaming",
             "wifi.roaming on", "wifi.regdomain", "wifi.ap.enable vucxap secret123 6", "wifi.ap.status",
             "wifi.ap.disable", "wifi.eap net ca cert key me", "wifi.peap net user pw ca", "wifi.disconnect"],
    "net": ["net.ping example.com 2", "net.ntp status", "net.ntp server 0 pool.ntp.org", "net.ntp on",
            "net.ntp off", "net.iperf server tcp", "net.iperf stop", "net.iperf client 192.168.1.10 udp 1",
            "http.get http://example.com 50", "http.post http://example.com/p a=1", "wifi.portal check"],
    "sock": ["sock.open tcp", "sock.connect 0 example.com 80", "sock.send 0 hello", "sock.read 0 64 1",
             "sock.list", "sock.close 0", "sock.open udp", "sock.bind 0 5000",
             "sock.sendto 0 192.168.1.10 5000 hi", "sock.recvfrom 0 64 1", "sock.listen 0 8000",
             "sock.accept 1", "sock.tls 0 ca", "sock.close all"],
    "mqtt": ["mqtt.connect broker.local 1883", "mqtt.subscribe a/# 1", "mqtt.publish a/b hello",
             "mqtt.read 1", "mqtt.unsubscribe a/#", "mqtt.disconnect"],
    "cert": ["cert.list", "cert.info myca"],
    "bt": ["bt.scan", "bt.scan full", "bt.connect 1", "bt.list", "bt.pairing on", "bt.iocap yesno",
           "bt.security sc", "bt.defaults secure", "bt.defaults", "bt.autoconfirm off", "bt.confirm yes",
           "bt.passkey 123456", "bt.bonds", "bt.unbond all", "bt.profile.add vucx C0:FF:EE:00:11:22",
           "bt.profile.list", "bt.profile.connect vucx", "bt.profile.auto", "bt.disconnect 0"],
    "gatt-client": ["bt.connect C0:FF:EE:00:11:22", "gatt.services 0", "gatt.chars 0", "gatt.read 0 18",
                    "gatt.write 0 18 0102", "gatt.write 0 18 text:hi norsp", "gatt.writelong 0 18 0102030405",
                    "gatt.subscribe 0 18 notify", "gatt.listen", "gatt.listen 1", "gatt.listen off",
                    "gatt.client.hrs 0", "gatt.client.cts 0", "gatt.client.ess 0", "gatt.client.lns 0",
                    "gatt.client.aio 0", "gatt.client.bas 0", "gatt.client.dis 0", "gatt.client.ras 0",
                    "gatt.client.nus 0", "gatt.client.sps 0", "gatt.client.wit 0", "gatt.client.wps 0",
                    "gatt.wps 0 status", "gatt.send 0 nus hello", "gatt.credits 0 10"],
    "gatt-server": ["gatt.server.hrs", "gatt.server.cts", "gatt.server.ess", "gatt.server.lns",
                    "gatt.server.aio", "gatt.server.bas", "gatt.server.ras", "gatt.server.nus",
                    "gatt.server.sps", "gatt.server.wps", "gatt.server.list", "gatt.server.set hrs 0048",
                    "gatt.server.notify 0 hrs 0048", "gatt.server.indicate 0 hrs 0048",
                    "gatt.server.demo 0 hrs 1", "gatt.server.hostchar 180F 2A19 12",
                    "gatt.server.readresp 0 01", "gatt.server.writeresp 0", "gatt.server.error 0 read 2",
                    "gatt.server.servicechanged 0 1 100"],
    "hid": ["hid.setup", "hid.status", "hid.key a ctrl shift", "hid.text Hello", "hid.mouse 10 -5 1",
            "hid.click left double", "hid.media volup", "hid.client 0", "hid.client off"],
    "web": ["web.status", "web.start 8080 tok", "web.status", "web.stop"],
    "ancs": ["ancs.on", "ancs.sub 0", "ancs.list", "ancs.show 1", "ancs.action 1 pos", "ancs.off"],
    "notify": ["notify.on", "notify.list", "notify.action 1 dismiss", "notify.off"],
    "coap": ["coap.get 192.168.1.10 5683 temp"],
    "time": ["app.time", "app.time sync module", "app.time push", "app.time set 2026-10-27T12:00:00"],
    "cert-more": ["cert.remove myca", "cert.upload ca missing.pem"],
}


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    yield module
    module.stop()


@pytest.mark.parametrize("group", SCENARIOS)
def test_command_group_runs_against_module(app_exe, nora, group):
    # net: three HTTP requests each wait 15 s for a response event the profile does not send
    out = _shell(app_exe, nora.port, SCENARIOS[group], timeout=90 if group == "net" else 30, log=nora.log)
    assert out.returncode in (0, 1), (f"rc={out.returncode}\n{out.stdout[-3000:]}\n{out.stderr[-2000:]}\n"
                                      f"last AT: {nora.log[-10:]}")
    assert len(nora.log) > 3, nora.log


def test_cert_upload_list_and_info(app_exe, nora, tmp_path):
    import base64

    pem = tmp_path / "myca.pem"
    pem.write_text("-----BEGIN CERTIFICATE-----\n" + base64.b64encode(bytes(range(48))).decode() +
                   "\n-----END CERTIFICATE-----\n")
    out = _shell(app_exe, nora.port, [f"cert.upload ca {pem} myca", "cert.list", "cert.info myca",
                                      "cert.info nosuch", f"cert.upload key {pem} k pw"], log=nora.log)
    assert out.returncode in (0, 1), out.stdout + out.stderr
    assert "myca" in out.stdout and "sha256:" in out.stdout and "1024 bytes" in out.stdout


def _nmea(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"${body}*{cs:02X}\r\n"


NMEA = "".join(_nmea(s) for s in (
    "GNRMC,123519.00,A,5546.1234,N,01301.5678,E,0.10,45.0,271026,,,A",
    "GNGGA,123519.00,5546.1234,N,01301.5678,E,1,08,0.9,45.0,M,40.0,M,,",
    "GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.5,0.9,1.2",
    "GPGSV,2,1,08,01,45,090,40,02,30,180,35,03,60,270,42,04,15,045,20",
    "GPGSV,2,2,08,05,50,120,38,06,25,200,30,07,70,300,45,08,10,010,15",
    "GNVTG,45.0,T,,M,0.10,N,0.19,K,A",
    "GNGLL,5546.1234,N,01301.5678,E,123519.00,A,A",
))


def _wit(kind, *values):
    import struct

    frame = bytes([0x55, kind]) + struct.pack("<4h", *values)
    return frame + bytes([sum(frame) & 0xFF])


def _shell(exe, port, commands, timeout=30, log=None):
    try:
        return subprocess.run([str(exe), port, "--shell"], input="\n".join(commands) + "\n",
                              capture_output=True, text=True, timeout=timeout, errors="replace")
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"hung after {timeout} s:\n{text[-3000:]}\nlast AT: {(log or [])[-15:]}")


def test_gnss_commands_on_a_virtual_receiver(app_exe, nora, tmp_path):
    from harness.vucx import Feeder

    gnss = Feeder(NMEA, 0.2)
    port = gnss.start()
    try:
        out = _shell(app_exe, nora.port, [
            f"gnss.open {port} 9600", "gnss.status", "gnss.sats", "gnss.watch 2",
            f"gnss.log {tmp_path / 'g.nmea'}", "gnss.status", "gnss.log off", "gnss.send $PUBX,00",
            "gnss.send b5 62 06 04", "gnss.send ubx 06 08 e8030100", "gnss.restart cold", "gnss.rate 5",
            "gnss.bridge status", "gnss.bridge tcp 2947", "gnss.bridge status", "gnss.bridge off",
            "gnss.bridge udp 192.168.1.10 10110", "gnss.bridge off", "gnss.bridge ble nus", "gnss.bridge off",
            "gnss.close", "gnss.status"])
    finally:
        gnss.stop()
    assert out.returncode in (0, 1), out.stdout[-3000:]
    assert "5546" in out.stdout or "55.7" in out.stdout, out.stdout[-3000:]


def test_imu_commands_on_a_virtual_sensor(app_exe, nora, tmp_path):
    from harness.vucx import Feeder

    data = (_wit(0x51, 100, -200, 2048, 2500) + _wit(0x52, 10, 20, 30, 2500) + _wit(0x53, 1000, -1000, 9000, 0)
            + _wit(0x54, 300, 200, 100, 0)) * 5
    imu = Feeder(data, 0.05)
    port = imu.start()
    try:
        out = _shell(app_exe, nora.port, [
            f"imu.open {port} 9600 wit", "imu.status", f"imu.log {tmp_path / 'i.bin'}", "imu.status",
            "imu.log off", "imu.rate 10", "imu.send ff aa 69 88 b5", "imu.bridge status", "imu.bridge tcp 9000",
            "imu.bridge status", "imu.bridge off", "imu.bridge udp 192.168.1.10 9001", "imu.bridge off",
            "imu.close", "imu.status"])
    finally:
        imu.stop()
    assert out.returncode in (0, 1), out.stdout[-3000:]


def test_cell_commands_on_a_virtual_modem(app_exe, nora):
    from harness.vucx import Vucx

    modem = Vucx("cell.yaml")
    port = modem.start()
    try:
        out = _shell(app_exe, nora.port, [
            f"cell.open {port}", "cell.status", "cell.at AT+CSQ", "cell.apn", "cell.apn internet ip",
            "cell.attach", "cell.attach on", "cell.ping example.com", "cell.bridge status", "cell.attach off",
            "cell.close", "cell.status"])
    finally:
        modem.stop()
    assert out.returncode in (0, 1), out.stdout[-3000:]
    assert any(c.startswith("AT+CSQ") for c in modem.log), modem.log
