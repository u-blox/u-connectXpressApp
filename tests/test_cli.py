"""Command-layer CLI tests: ucxapp <port> --exec "<command>" ..."""

import json
import re
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.app


def _run(app_exe, tmp_path, port, *commands, timeout=60):
    args = [str(app_exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL)


def test_help_lists_command_packs(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "help")
    assert out.returncode == 0
    for name in ("device.info", "wifi.scan", "wifi.connect", "sock.open", "device.connect",
                 "net.ping", "net.dns", "http.get", "mqtt.connect", "mqtt.publish",
                 "bt.scan", "bt.connect", "at"):
        assert name in out.stdout


def test_unknown_command_fails(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "no.such.command")
    assert out.returncode == 1
    assert "unknown command" in out.stdout


def test_device_command_without_device_fails(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "wifi.status")
    assert out.returncode == 1
    assert "not connected" in out.stdout


def test_usage_is_printed_for_bad_arguments(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "device.connect a b")
    assert out.returncode == 1
    assert "usage: device.connect" in out.stdout


def test_long_usage_is_one_alternative_per_line(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "help gnss.bridge", "gnss.bridge")
    lines = [ln.strip() for ln in out.stdout.splitlines()]
    for alt in ("tcp [port]", "tls [port] [cert] [clientauth]", "tcpc <host> <port>", "udps <port>",
                "ble [nus|sps|custom <uuid> [rx tx]]", "uart <port> [baud]", "cell", "off", "status"):
        assert alt in lines, alt
        assert f"gnss.bridge {alt}" in lines, alt
    assert not any("<tcp" in ln for ln in lines)


def test_long_usage_without_alternatives_is_one_line(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "help gatt.writelong")
    lines = [ln.strip() for ln in out.stdout.splitlines()]
    assert "[conn] <handle> <hex|text:abc> [offset] [reliable] [more]" in lines


@pytest.mark.parametrize("command", ["gnss.ex", "wifi.ex", "sock.ex", "examples gatt"])
def test_examples(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 0
    assert "Examples for" in out.stdout and " 1. " in out.stdout and "    > " in out.stdout


def test_examples_lists_groups_and_rejects_extra_args(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "examples")
    assert out.returncode == 0
    for group in ("gnss", "wifi", "bt", "gatt", "sock", "mqtt", "hw"):
        assert f" {group}" in out.stdout
    assert _run(app_exe, tmp_path, "COM255", "gnss.ex a b").returncode == 1

@pytest.mark.parametrize("command", ["web.start", "hid.status", "hid.key a", "wifi.ap.status", "gatt.writelong 0 1 00"])
def test_new_device_commands_need_a_device(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "not connected" in out.stdout


@pytest.mark.parametrize("command", [
    "mqtt.connect", "mqtt.disconnect", "mqtt.subscribe t", "mqtt.unsubscribe t",
    "mqtt.publish t hello", "mqtt.read 0",
])
def test_mqtt_commands_need_a_device(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "not connected" in out.stdout


@pytest.mark.parametrize("command", ["http.get", "http.download http://x", "http.get " + "a" * 300])
def test_http_commands_reject_bad_arguments(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1


def test_web_stop_and_status_work_without_device(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "web.status", "web.stop")
    assert out.returncode == 0, out.stdout
    assert "web server: stopped" in out.stdout


def test_qr_text_needs_no_device(app_exe, tmp_path):
    out = _run(
        app_exe, tmp_path, "COM255",
        "utils.qr.text hello", "utils.qr.wifi stenmo secret", "utils.qr.geo 59.9 10.7",
    )
    assert out.returncode == 0, out.stdout


def test_file_hash_sha256(app_exe, tmp_path):
    f = tmp_path / "abc.txt"
    f.write_bytes(b"abc")
    out = _run(app_exe, tmp_path, "COM255", f"file.hash sha256 {f}")
    assert out.returncode == 0, out.stdout
    assert "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" in out.stdout.lower()


def test_file_hash_md5_and_missing_file(app_exe, tmp_path):
    f = tmp_path / "abc.txt"
    f.write_bytes(b"abc")
    out = _run(app_exe, tmp_path, "COM255", f"file.hash md5 {f}")
    assert out.returncode == 0, out.stdout
    assert "900150983cd24fb0d6963f7d28e17f72" in out.stdout.lower()
    out = _run(app_exe, tmp_path, "COM255", f"file.hash md5 {tmp_path / 'missing.bin'}")
    assert out.returncode == 1


def test_help_prefix_filters_commands(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "help wifi.profile")
    assert out.returncode == 0
    assert "wifi.profile.auto" in out.stdout
    assert "sock.open" not in out.stdout


def test_wifi_profiles_round_trip(app_exe, tmp_path):
    def run(*commands):
        args = [str(app_exe), "COM255"]
        for command in commands:
            args += ["--exec", command]
        return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path)

    out = run("wifi.profile.add home ubx s3cret!", "wifi.profile.add cafe CafeNet", "wifi.profile.list")
    assert out.returncode == 0, out.stdout
    assert "home: ssid=ubx WPA" in out.stdout
    assert "cafe: ssid=CafeNet open" in out.stdout
    assert "2 profile(s)" in out.stdout
    assert "s3cret!" not in out.stdout
    assert "s3cret!" not in (tmp_path / "storage" / "ucxapp.wifi").read_text()

    out = run("wifi.profile.add home ubx newpass", "wifi.profile.remove cafe", "wifi.profile.list")
    assert "1 profile(s)" in out.stdout

    out = run("wifi.profile.remove nosuch")
    assert out.returncode == 1

    out = run("wifi.profile.connect home")
    assert out.returncode == 1
    assert "not connected" in out.stdout


def test_wifi_profiles_move_from_the_old_file_name(app_exe, tmp_path):
    def run(*commands):
        args = [str(app_exe), "COM255"]
        for command in commands:
            args += ["--exec", command]
        return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path)

    run("wifi.profile.add home ubx s3cret!")
    storage = tmp_path / "storage"
    (storage / "ucxapp.wifi").rename(storage / "u-connectClientApp.wifi")
    out = run("wifi.profile.list")
    assert "home: ssid=ubx WPA" in out.stdout
    assert (storage / "ucxapp.wifi").exists()
    assert not (storage / "u-connectClientApp.wifi").exists()


def test_app_about_commands(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "app.info", "app.version", "app.uptime", "app.sbom", "app.license",
               "app.license qrcodegen")
    assert out.returncode == 0, out.stdout
    assert "u-blox AG" in out.stdout
    assert re.search(r"built\s+\w{3} +\d+ \d{4} \d\d:\d\d:\d\d", out.stdout)
    assert re.search(r"^0 days 00:00:\d\d$", out.stdout, re.M)
    assert "u-connectClient " in out.stdout and "Apache-2.0" in out.stdout
    assert "Permission is hereby granted" in out.stdout
    assert _run(app_exe, tmp_path, "COM255", "app.license nosuch").returncode != 0


def test_stats_show_and_clear(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "stats.show", "stats.show tcp rx", "stats.clear", "stats.clear sps")
    assert out.returncode == 0, out.stdout
    assert re.search(r"^mqtt\s+0\s+0$", out.stdout, re.M)
    assert re.search(r"^0$", out.stdout, re.M)
    for bad in ("stats.show foo", "stats.show tcp up", "stats.clear foo"):
        assert _run(app_exe, tmp_path, "COM255", bad).returncode != 0, bad


def test_file_list_and_remove(app_exe, tmp_path):
    def run(*commands):
        args = [str(app_exe), "COM255"]
        for command in commands:
            args += ["--exec", command]
        return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path)

    storage = tmp_path / "storage"
    storage.mkdir()
    (storage / "a.zip").write_bytes(b"x" * 1234)
    (storage / "b.bin").write_bytes(b"y")
    (storage / "sub").mkdir()
    out = run("file.list")
    assert out.returncode == 0, out.stdout
    assert re.search(r"\d+\s+1234\s+a\.zip", out.stdout)
    assert "sub" not in out.stdout

    number = re.search(r"(\d+)\s+1\s+b\.bin", out.stdout).group(1)
    out = run("file.list", f"file.remove {number}", "file.remove a.zip")
    assert out.returncode == 0, out.stdout
    assert not (storage / "a.zip").exists()
    assert not (storage / "b.bin").exists()

    for bad in ("file.remove 99", "file.remove ../x", "file.remove missing.bin"):
        assert run(bad).returncode == 1, bad


def test_hid_key_usage_error(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hid.key")
    assert out.returncode == 1


@pytest.mark.hardware
def test_device_info_and_wifi_status(app_exe, tmp_path, port_a):
    out = _run(app_exe, tmp_path, port_a, "device.info", "wifi.status", "sock.list")
    assert out.returncode == 0, out.stdout
    assert "Model:" in out.stdout
    assert "NORA-W36" in out.stdout
    assert "wifi:" in out.stdout
    assert "socket(s)" in out.stdout


@pytest.mark.parametrize("command", [
    "gnss.status", "gnss.close", "gnss.send 00", "gnss.log off", "gnss.bridge tcp", "gnss.bridge status",
])
def test_gnss_commands_need_an_open_receiver(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "no receiver open" in out.stdout


@pytest.mark.parametrize("command", [
    "imu.status", "imu.close", "imu.send aa", "imu.rate 10", "imu.bridge udp host 5005", "imu.bridge ble",
])
def test_imu_commands_need_an_open_sensor(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "no IMU open" in out.stdout


@pytest.mark.parametrize("command, usage", [
    ("gnss.restart bogus", "usage: gnss.restart"),
    ("gnss.rate x", "usage: gnss.rate"),
])
def test_gnss_commands_reject_bad_arguments(app_exe, tmp_path, command, usage):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert usage in out.stdout


def test_gnss_and_imu_detect_without_hardware_do_not_crash(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "gnss.detect COM254", "imu.detect COM254")
    assert out.returncode in (0, 1)
    assert "Traceback" not in out.stdout


@pytest.mark.parametrize("command", [
    "gatt.listen", "gatt.listen off", "gatt.listen 1", "gatt.client.wit", "gatt.client.hrs",
])
def test_gatt_listen_and_examples_need_a_device(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "not connected" in out.stdout


def test_gatt_listen_help_mentions_off(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "help gatt.listen", "help gatt.client.wit")
    assert "[seconds|off]" in out.stdout
    assert "WitMotion" in out.stdout


@pytest.mark.parametrize("command", [
    "cell.close", "cell.status", "cell.at AT", "cell.apn", "cell.apn internet", "cell.attach", "cell.ping 8.8.8.8",
    "cell.bridge tcp", "cell.bridge uart COM1",
])
def test_cell_commands_need_an_open_module(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "no cellular module open" in out.stdout


@pytest.mark.parametrize("command, expected", [
    ("gnss.bridge uart COM1", "no receiver open"),
    ("gnss.bridge tcpc 10.0.0.1 80", "no receiver open"),
    ("gnss.bridge udps 5000", "no receiver open"),
    ("gnss.bridge ble sps", "no receiver open"),
    ("imu.bridge uart COM1", "no IMU open"),
    ("imu.bridge udp 10.0.0.1 5000 6000", "no IMU open"),
    ("cell.bridge uart COM1 9600", "no cellular module open"),
    ("cell.bridge tcpc 10.0.0.1 80", "no cellular module open"),
])
def test_uart_bridge_needs_an_open_device(app_exe, tmp_path, command, expected):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert expected in out.stdout


def test_hw_gpio_roundtrip(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.gpio.mode 3 out", "hw.gpio.write 3 1", "hw.gpio.read 3",
               "hw.gpio.toggle 3", "hw.gpio.read 3")
    assert out.returncode == 0
    assert out.stdout.count("pin 3 = 1") >= 2
    assert "pin 3 = 0" in out.stdout


def test_hw_led_on_toggle_off(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.led 5 on", "hw.gpio.read 5", "hw.led 5 toggle",
               "hw.gpio.read 5", "hw.led 5 off", "hw.gpio.read 5")
    assert out.returncode == 0
    assert "led on pin 5 = 1" in out.stdout
    assert "led on pin 5 = 0" in out.stdout
    assert "pin 5 = 0" in out.stdout


def test_hw_i2c_scan_and_registers(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.i2c.scan", "hw.i2c.reg 0x68 0x75", "hw.i2c.write 0x48 10 AA BB",
               "hw.i2c.reg 0x48 0x10 2")
    assert out.returncode == 0
    assert "found 0x48 0x68 0x76" in out.stdout
    assert "\n68\n" in out.stdout
    assert "AA BB" in out.stdout


def test_hw_i2c_nack_fails(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.i2c.read 0x50 1")
    assert out.returncode == 1
    assert "no ACK from 0x50" in out.stdout


def test_hw_spi_loopback_adc_and_pwm(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.spi.xfer 9F 00 12", "hw.adc.read 2", "hw.pwm.set 1 2000 12.5",
               "hw.status")
    assert out.returncode == 0
    assert "9F 00 12" in out.stdout
    assert "adc 2 = " in out.stdout
    assert "pwm 1: 2000 Hz, 12.5 %" in out.stdout
    assert "pwm 1 of 4 channels running" in out.stdout


@pytest.mark.parametrize("command, usage", [
    ("hw.gpio.read 99", "usage: hw.gpio.read"),
    ("hw.gpio.write 1 2", "usage: hw.gpio.write"),
    ("hw.led 3", "usage: hw.led"),
    ("hw.led 3 blink", "usage: hw.led"),
    ("hw.i2c.write 0x48", "usage: hw.i2c.write"),
    ("hw.spi.xfer zz", "usage: hw.spi.xfer"),
    ("hw.adc.read 8", "usage: hw.adc.read"),
    ("hw.pwm.set 0 1000 101", "usage: hw.pwm.set"),
])
def test_hw_commands_reject_bad_arguments(app_exe, tmp_path, command, usage):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert usage in out.stdout


def test_hw_exec_speaks_the_bridge_protocol(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.exec gpio.write 4 1", "hw.exec gpio.read 4", "hw.exec i2c.scan",
               "hw.exec nope")
    assert "OK pin 4 = 1" in out.stdout
    assert "OK found 0x48" in out.stdout
    assert "ERR unknown command 'nope'" in out.stdout


BOARDS = Path(__file__).resolve().parents[1] / "app" / "boards"


@pytest.mark.parametrize("board", ["h753", "f767", "f439", "f407", "f401", "f446", "f103", "g071"])
def test_hw_load_board_profiles(app_exe, tmp_path, board):
    out = _run(app_exe, tmp_path, "COM255", f"hw.load {BOARDS / (board + '.json')}", "hw.board", "hw.gpio.mode PB0 out",
               "hw.gpio.write PB0 1", "hw.gpio.read pb0", "hw.i2c.scan", "hw.adc.read 3", "hw.adc.read 4")
    assert "loaded " + json.loads((BOARDS / (board + ".json")).read_text(encoding="utf-8"))["board"] in out.stdout
    assert "i2c scl PB8 sda PB9" in out.stdout
    assert "pin 0 = 1" in out.stdout
    assert "found 0x48 0x68 0x76" in out.stdout
    assert out.returncode == 1   # adc 4 does not exist on the profile
    assert "usage: hw.adc.read" in out.stdout


@pytest.mark.parametrize("text, message", [
    ('{"board":"x","gpio":["PB0","PB0"]}', "pin PB0 is assigned twice"),
    ('{"board":"x","gpio":["PB7"],"reserved":["PB7"]}', "pin PB7 is reserved"),
    ('{"board":"x","gpio":["B7"]}', "bad pin name 'B7'"),
    ('{"board":"x","gpoi":["PB1"]}', "unknown or invalid key 'gpoi[0]'"),
    ('{"board":"x",', "JSON syntax error"),
])
def test_hw_load_rejects_bad_profiles(app_exe, tmp_path, text, message):
    path = tmp_path / "bad.json"
    path.write_text(text, encoding="utf-8")
    out = _run(app_exe, tmp_path, "COM255", f"hw.load {path}")
    assert out.returncode == 1
    assert message in out.stdout


def test_hw_load_missing_file(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.load nothere.json")
    assert out.returncode == 1
    assert "cannot open nothere.json" in out.stdout


def test_hw_help_lists_every_documented_command(app_exe, tmp_path):
    doc = (Path(__file__).resolve().parents[1] / "COMMANDS.md").read_text(encoding="utf-8")
    out = _run(app_exe, tmp_path, "COM255", "help hw.")
    for name in sorted(set(re.findall(r"`(hw\.[a-z0-9.]+)`", doc))):
        assert name in out.stdout, name


def test_hw_bridge_without_module_or_port_fails_cleanly(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "hw.bridge status", "hw.bridge off", "hw.bridge udp 10.0.0.1 5000")
    assert out.stdout.count("bridge: off") == 2
    assert "no module connected" in out.stdout
    out = _run(app_exe, tmp_path, "COM255", "hw.bridge uart COM254")
    assert out.returncode == 1
    assert "cannot open COM254" in out.stdout


def test_cell_connect_needs_an_open_module(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "cell.connect 10.0.0.1 80")
    assert out.returncode == 1
    assert "no cellular module open" in out.stdout


@pytest.mark.parametrize("command", ["cell.connect", "cell.connect host", "cell.connect host 0", "cell.connect h 1 x"])
def test_cell_connect_rejects_bad_arguments(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert "usage: cell.connect" in out.stdout


def test_bridge_over_cell_needs_a_data_link(app_exe, tmp_path):
    for command in ("cell.disconnect", "hw.bridge cell"):
        out = _run(app_exe, tmp_path, "COM255", command)
        assert out.returncode == 1
        assert "no cellular data link" in out.stdout


@pytest.mark.parametrize("command, expected", [
    ("hw.bridge ble custom 6E400001-B5A3-F393-E0A9-E50E24DCCA9E", "no module connected"),
    ("hw.bridge ble custom 6E400001B5A3F393E0A9E50E24DCCA9E", "no module connected"),
    ("hw.bridge ble custom zz", "usage: hw.bridge"),
    ("hw.bridge ble custom 6E400001-B5A3-F393-E0A9-E50E24DCCA9E 6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
     "usage: hw.bridge"),
])
def test_custom_ble_bridge_arguments(app_exe, tmp_path, command, expected):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert expected in out.stdout


@pytest.mark.parametrize("command, usage", [
    ("cell.open", "usage: cell.open"),
    ("cell.open COM254 x", "usage: cell.open"),
    ("cell.at", "usage: cell.at"),
    ("cell.attach maybe", "usage: cell.attach"),
    ("cell.ping", "usage: cell.ping"),
])
def test_cell_commands_reject_bad_arguments(app_exe, tmp_path, command, usage):
    out = _run(app_exe, tmp_path, "COM255", command)
    assert out.returncode == 1
    assert usage in out.stdout


def test_cell_open_on_missing_port_fails_cleanly(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "COM255", "cell.open COM254")
    assert out.returncode == 1
    assert "no AT response" in out.stdout


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def _store(cwd):
    """The app keeps certificates in storage/ under its working directory."""
    return cwd / "storage"


def _handshake(ca, cert, key, hostname, client_cert=None):
    """TLS handshake over a socket pair with Python's ssl: proves the generated files form a valid chain."""
    errors = _try_handshake(ca, cert, key, hostname, client_cert)
    assert not errors, errors


def _try_handshake(ca, cert, key, hostname, client_cert=None, require_client=None):
    """Echo over TLS; returns the exceptions the client and the server saw (empty when it works)."""
    import ssl
    import sys

    truststore = None
    if ssl.SSLContext.__module__.endswith("truststore._api"):   # injected truststore checks peers even on servers
        truststore = sys.modules[ssl.SSLContext.__module__]
        truststore.extract_from_ssl()
    try:
        return _handshake_once(ssl, ca, cert, key, hostname, client_cert, require_client)
    finally:
        if truststore is not None:
            truststore.inject_into_ssl()


def _handshake_once(ssl, ca, cert, key, hostname, client_cert, require_client):
    import socket
    import threading

    context = ssl.SSLContext
    server_ctx = context(ssl.PROTOCOL_TLS_SERVER)
    server_ctx.load_cert_chain(cert, key)
    if require_client or (require_client is None and client_cert):
        server_ctx.verify_mode = ssl.CERT_REQUIRED
        server_ctx.load_verify_locations(require_client or ca)
    client_ctx = context(ssl.PROTOCOL_TLS_CLIENT)
    client_ctx.verify_flags |= ssl.VERIFY_X509_STRICT
    client_ctx.load_verify_locations(ca)
    if client_cert:
        client_ctx.load_cert_chain(*client_cert)
    a, b = socket.socketpair()
    a.settimeout(10)
    b.settimeout(10)
    errors = []

    def serve():
        try:
            with server_ctx.wrap_socket(a, server_side=True) as s:
                s.sendall(s.recv(5))
        except Exception as e:   # noqa: BLE001
            errors.append(e)
            a.close()

    t = threading.Thread(target=serve)
    t.start()
    try:
        with client_ctx.wrap_socket(b, server_hostname=hostname) as c:
            c.sendall(b"hello")
            if c.recv(5) != b"hello":
                errors.append(AssertionError("echo mismatch"))
    except Exception as e:   # noqa: BLE001
        errors.append(e)
        b.close()
    t.join(10)
    return errors


def test_tls_certificates_form_a_valid_chain(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "utils.tls.ca.create", "utils.tls.server.create server localhost 127.0.0.1",
                  "utils.tls.client.create alice", "utils.tls.list")
    assert out.returncode == 0, out.stdout
    store = _store(tmp_path)
    for name in ("ca", "server", "alice"):
        assert (store / f"tls-{name}.crt").read_text().startswith("-----BEGIN CERTIFICATE-----")
        assert "PRIVATE KEY-----" in (store / f"tls-{name}.key").read_text()
    assert re.search(r"^server\s+CN=localhost, O=u-blox ucxapp \| issuer CN=ucxapp-local-ca", out.stdout, re.M)
    assert "| SAN" in out.stdout

    _handshake(store / "tls-ca.crt", store / "tls-server.crt", store / "tls-server.key", "localhost")
    _handshake(store / "tls-ca.crt", store / "tls-server.crt", store / "tls-server.key", "localhost",
               client_cert=(store / "tls-alice.crt", store / "tls-alice.key"))


def test_tls_commands_protect_the_ca_and_reject_bad_names(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "utils.tls.server.create web 10.0.0.1")
    assert out.returncode == 1
    assert "run utils.tls.ca.create first" in out.stdout

    assert _run_in(app_exe, tmp_path, "utils.tls.ca.create").returncode == 0
    before = (_store(tmp_path) / "tls-ca.crt").read_text()
    out = _run_in(app_exe, tmp_path, "utils.tls.ca.create")
    assert out.returncode == 1
    assert "already exists" in out.stdout
    assert (_store(tmp_path) / "tls-ca.crt").read_text() == before

    for bad in ("utils.tls.server.create ../x host", "utils.tls.server.create ca host",
                "utils.tls.server.create web a,b", "utils.tls.client.create bob extra"):
        assert _run_in(app_exe, tmp_path, bad).returncode == 1, bad

    out = _run_in(app_exe, tmp_path, "utils.tls.pem ca", "utils.tls.remove ca", "utils.tls.pem ca")
    assert "-----BEGIN CERTIFICATE-----" in out.stdout
    assert "tls-ca.crt not found" in out.stdout


def test_https_needs_a_device(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "web.start https")
    assert out.returncode == 1
    assert "not connected" in out.stdout
    out = _run_in(app_exe, tmp_path, "help gnss.bridge")
    assert "tls [port] [cert] [clientauth]" in out.stdout


@pytest.fixture
def tls_dir(app_exe, tmp_path):
    """A store with the local CA, a server certificate for gnss.local + 192.168.1.42 and a client 'rover'."""
    out = _run_in(app_exe, tmp_path, "utils.tls.ca.create", "utils.tls.server.create server gnss.local 192.168.1.42",
                  "utils.tls.client.create rover")
    assert out.returncode == 0, out.stdout
    return tmp_path


def _files(d, name):
    return _store(d) / f"tls-{name}.crt", _store(d) / f"tls-{name}.key"


def test_tls_server_certificate_is_valid_for_its_names_only(tls_dir):
    import ssl

    ca = _store(tls_dir) / "tls-ca.crt"
    for host in ("gnss.local", "192.168.1.42"):
        assert not _try_handshake(ca, *_files(tls_dir, "server"), host), host
    for host in ("other.local", "192.168.1.43"):
        errors = _try_handshake(ca, *_files(tls_dir, "server"), host)
        assert any(isinstance(e, ssl.SSLCertVerificationError) for e in errors), (host, errors)


def test_tls_certificate_contents(tls_dir):
    import ssl

    decode = ssl._ssl._test_decode_cert   # noqa: SLF001 - stdlib X.509 decoder, avoids a cryptography dependency
    ca, server, rover = (decode(str(_store(tls_dir) / f"tls-{n}.crt")) for n in ("ca", "server", "rover"))
    assert ca["subject"] == ca["issuer"]
    assert server["issuer"] == ca["subject"] == rover["issuer"]
    assert set(server["subjectAltName"]) == {("DNS", "gnss.local"), ("IP Address", "192.168.1.42")}
    assert ("commonName", "gnss.local") in [rdn[0] for rdn in server["subject"]]
    assert ("commonName", "rover") in [rdn[0] for rdn in rover["subject"]]
    year = lambda c, k: int(c[k].split()[3])   # noqa: E731
    assert year(ca, "notAfter") - year(ca, "notBefore") == 20
    assert year(server, "notAfter") - year(server, "notBefore") == 10
    assert server["serialNumber"] != rover["serialNumber"]


def test_tls_client_auth_accepts_only_certificates_from_the_ca(app_exe, tls_dir, tmp_path_factory):
    ca = _store(tls_dir) / "tls-ca.crt"
    server = _files(tls_dir, "server")
    assert not _try_handshake(ca, *server, "gnss.local", client_cert=_files(tls_dir, "rover"))
    assert _try_handshake(ca, *server, "gnss.local", require_client=ca)   # no client certificate

    other = tmp_path_factory.mktemp("other")
    assert _run_in(app_exe, other, "utils.tls.ca.create", "utils.tls.client.create mallory").returncode == 0
    assert _try_handshake(ca, *server, "gnss.local", client_cert=_files(other, "mallory"), require_client=ca)


def test_tls_ca_force_invalidates_old_certificates(app_exe, tls_dir):
    old = (_store(tls_dir) / "tls-ca.crt").read_text()
    out = _run_in(app_exe, tls_dir, "utils.tls.ca.create my-ca force", "utils.tls.list")
    assert out.returncode == 0, out.stdout
    assert (_store(tls_dir) / "tls-ca.crt").read_text() != old
    assert re.search(r"^ca\s+CN=my-ca", out.stdout, re.M)
    assert _try_handshake(_store(tls_dir) / "tls-ca.crt", *_files(tls_dir, "server"), "gnss.local")
    assert _run_in(app_exe, tls_dir, "utils.tls.server.create server gnss.local").returncode == 0
    assert not _try_handshake(_store(tls_dir) / "tls-ca.crt", *_files(tls_dir, "server"), "gnss.local")


def test_tls_pem_prints_the_key_only_when_asked(app_exe, tls_dir):
    out = _run_in(app_exe, tls_dir, "utils.tls.pem rover")
    assert "BEGIN CERTIFICATE" in out.stdout and "PRIVATE KEY" not in out.stdout
    out = _run_in(app_exe, tls_dir, "utils.tls.pem rover key")
    assert "PRIVATE KEY" in out.stdout and "BEGIN CERTIFICATE" not in out.stdout
    assert _run_in(app_exe, tls_dir, "utils.tls.pem rover cert").returncode == 1


def test_tls_remove_and_list(app_exe, tls_dir):
    out = _run_in(app_exe, tls_dir, "utils.tls.remove rover", "utils.tls.list")
    assert out.returncode == 0, out.stdout
    assert not (_store(tls_dir) / "tls-rover.crt").exists() and not (_store(tls_dir) / "tls-rover.key").exists()
    assert "rover" not in out.stdout.split("removed rover", 1)[1]
    assert _run_in(app_exe, tls_dir, "utils.tls.remove rover").returncode == 1


def test_tls_list_on_an_empty_store(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "utils.tls.list")
    assert out.returncode == 0
    assert "no certificates, start with utils.tls.ca.create" in out.stdout


@pytest.mark.parametrize("command, expected", [
    ("gnss.bridge tls", "no receiver open"),
    ("gnss.bridge tls 2947 server clientauth", "no receiver open"),
    ("imu.bridge tls 5000", "no IMU open"),
    ("cell.bridge tls", "no cellular module open"),
    ("sock.tls ca", "not connected"),
    ("sock.tls 0 ca client key", "not connected"),
    ("web.start 8443 tok https server", "not connected"),
])
def test_tls_bridges_and_sockets_need_their_device(app_exe, tmp_path, command, expected):
    out = _run_in(app_exe, tmp_path, command)
    assert out.returncode == 1
    assert expected in out.stdout
