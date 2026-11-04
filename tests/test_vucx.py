"""The app against vucx, the virtual u-connectXpress module (needs a pty: Linux/WSL, run by ucxapp --coverage)."""

import re
import subprocess
import sys
import time

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]

# The app keeps profiles and settings in storage/ under its working directory; run it in a scratch directory so the
# virtual networks never land in the repo's storage, where the hardware tests would try them.
_CWD = None


@pytest.fixture(autouse=True, scope="module")
def _scratch_cwd(tmp_path_factory):
    global _CWD
    _CWD = tmp_path_factory.mktemp("vucx-cwd")
    yield
    _CWD = None


@pytest.fixture
def vucx():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("basic.yaml")
    module.start()
    yield module
    module.stop()


def _run(exe, port, *commands, timeout=60, cwd=None):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd or _CWD)


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


def test_dev_crash_shows_the_stored_record(app_exe, vucx):
    out = _run(app_exe, vucx.port, "dev.crash")
    assert out.returncode == 0, out.stdout
    assert '+USYCI:"Assert","","u_buffer.c",267,"3.5.0-0"' in out.stdout, out.stdout
    assert "no crash information" not in out.stdout, out.stdout


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


def test_wifi_radio_refuses_until_regdomain_is_chosen(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    _run(app_exe, wifi_module.port, "wifi.profile.add home vucx-net secret123", cwd=tmp_path)
    for command in ("wifi.scan", "wifi.connect vucx-net secret123", "wifi.ap.enable vucxap secret123",
                    "wifi.profile.auto", "wifi.locate google k"):
        out = _run(app_exe, wifi_module.port, command, cwd=tmp_path)
        assert out.returncode != 0, command + "\n" + out.stdout
        assert out.stdout.count("ERROR: choose the Wi-Fi regulatory domain") == 1, command + "\n" + out.stdout
        assert "wifi.regdomain <world|etsi|" in out.stdout, out.stdout
        assert "Scanning" not in out.stdout and "usable access point" not in out.stdout, out.stdout
    radio = ("AT+UWRD=", "AT+UWSSC", "AT+UWSCP=", "AT+UWSC=", "AT+UWAPCP=", "AT+UWAPA")
    assert not [c for c in wifi_module.log if c.startswith(radio)], wifi_module.log


def test_wifi_regdomain_once_unlocks_the_radio(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, "wifi.regdomain fcc", "wifi.scan", "wifi.connect vucx-net secret123",
               "wifi.ap.enable vucxap secret123", cwd=tmp_path)
    assert "regulatory domain" not in out.stdout, out.stdout
    assert (tmp_path / "storage" / "ucxapp.regdomain").read_text() == "fcc"
    assert wifi_module.log.count("AT+UWRD=2") == 4, wifi_module.log
    assert "vucx-net" in out.stdout, out.stdout


def test_wifi_saved_regdomain_wins_over_environment(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.setenv("UCX_REGDOMAIN", "etsi")
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "ucxapp.regdomain").write_text("fcc")
    _run(app_exe, wifi_module.port, "wifi.scan", cwd=tmp_path)
    assert "AT+UWRD=2" in wifi_module.log and "AT+UWRD=1" not in wifi_module.log, wifi_module.log


def test_wifi_regdomain_from_environment(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.setenv("UCX_REGDOMAIN", "etsi")
    out = _run(app_exe, wifi_module.port, "wifi.scan", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    assert "AT+UWRD=1" in wifi_module.log, wifi_module.log


def test_wifi_regdomain_show_notes_when_unset(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, "wifi.regdomain", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    assert "regdomain=world" in out.stdout and "not chosen yet" in out.stdout, out.stdout
    assert "Where are you located?" in out.stdout and " 2  fcc    USA" in out.stdout, out.stdout


@pytest.mark.parametrize("choice, name, value", [("2", "fcc", 2), ("Europe", "etsi", 1), ("JAPAN", "mkk", 5),
                                                 ("new-zealand", "nz", 4), ("10", "br", 10), ("world", "world", 0)])
def test_wifi_regdomain_by_number_or_place(app_exe, wifi_module, tmp_path, monkeypatch, choice, name, value):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, f"wifi.regdomain {choice}", cwd=tmp_path)
    assert out.returncode == 0, out.stdout
    assert (tmp_path / "storage" / "ucxapp.regdomain").read_text() == name
    assert f"AT+UWRD={value}" in wifi_module.log, wifi_module.log


@pytest.mark.parametrize("choice", ["mars", "11", "-1", "2x"])
def test_wifi_regdomain_unknown_place_shows_the_list(app_exe, wifi_module, tmp_path, monkeypatch, choice):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, f"wifi.regdomain {choice}", cwd=tmp_path)
    assert out.returncode != 0, out.stdout
    assert f"'{choice}' is not a region" in out.stdout and "Where are you located?" in out.stdout, out.stdout
    assert not (tmp_path / "storage" / "ucxapp.regdomain").exists()


def test_wifi_radio_without_region_lists_the_choices(app_exe, wifi_module, tmp_path, monkeypatch):
    """--exec never waits for an answer: it prints the list and the error and stops."""
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, "wifi.scan", "dev.info", cwd=tmp_path)
    assert "Where are you located?" in out.stdout and "Choose 0-10" not in out.stdout, out.stdout
    assert out.returncode != 0 and "ERROR: choose the Wi-Fi regulatory domain" in out.stdout, out.stdout
    assert "NORA-W36" not in out.stdout.split("ERROR: choose the Wi-Fi regulatory domain")[1], out.stdout


def test_dev_caps_shows_region_and_wifi_features(app_exe, wifi_module, tmp_path, monkeypatch):
    monkeypatch.delenv("UCX_REGDOMAIN", raising=False)
    out = _run(app_exe, wifi_module.port, "dev.caps", "wifi.regdomain etsi", "dev.caps", cwd=tmp_path)
    assert out.returncode == 0 and out.stdout.count("regdomain=") == 1, out.stdout
    first, second = out.stdout.split("regdomain=")
    assert re.search(r"^Region:\s+unset$", first, re.M) and "wifi.regdomain (pick where you are)" in first, first
    assert re.search(r"^Region:\s+etsi$", second, re.M) and re.search(r"^Sockets:\s+yes$", second, re.M), second
    assert re.search(r"^Wi-Fi:\s+yes$", second, re.M), second


def test_dev_caps_ble_only_module_hides_wifi(app_exe):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-b26.yaml")
    try:
        out = _run(app_exe, module.start(), "dev.caps", "wifi.scan")
    finally:
        module.stop()
    assert re.search(r"^Wi-Fi:\s+no$", out.stdout, re.M) and re.search(r"^GATT:\s+yes$", out.stdout, re.M), out.stdout
    for hidden in ("Sockets:", "MQTT:", "Region:", "Wi-Fi features"):
        assert hidden not in out.stdout, out.stdout
    assert "wifi.scan needs Wi-Fi; this module has Bluetooth LE only" in out.stdout, out.stdout


def test_thread_module_caps_and_network(app_exe):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-b26-thread.yaml")
    try:
        out = _run(app_exe, module.start(), "dev.caps", "thread.network new", "thread.start 5",
                   "thread.status", "thread.network", "thread.type", "thread.addr", "thread.neighbors",
                   "thread.counters", "thread.scan", "thread.join j01nme 5", "thread.dataset")
    finally:
        module.stop()
    assert re.search(r"^Thread:\s+yes$", out.stdout, re.M), out.stdout
    assert "AT+UTHDSN" in module.log and "AT+UTHE=1" in module.log, module.log
    assert "Thread up" in out.stdout and re.search(r"^Role:\s+leader$", out.stdout, re.M), out.stdout
    assert "vucx-mesh" in out.stdout and "dead00beef00cafe" in out.stdout, out.stdout
    assert "sleepy (polls its parent every 2000 ms)" in out.stdout, out.stdout
    assert "fd00" in out.stdout and "1 neighbour(s)" in out.stdout, out.stdout
    assert re.search(r"attach attempts\s+3", out.stdout) and re.search(r"tx frames\s+120", out.stdout), out.stdout
    assert "counter 30" not in out.stdout and "1 network(s)" in out.stdout, out.stdout
    assert 'AT+UTHJS="J01NME"' in module.log and "Joined" in out.stdout, out.stdout + str(module.log)
    assert "0e080000000000010000000300000f" in out.stdout, out.stdout


def test_thread_refused_without_thread_firmware(app_exe):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-b26.yaml")
    try:
        out = _run(app_exe, module.start(), "dev.caps", "thread.status")
    finally:
        module.stop()
    assert re.search(r"^Thread:\s+no$", out.stdout, re.M), out.stdout
    assert "thread.status needs Thread; this module has no Thread firmware" in out.stdout, out.stdout


def _summary(exe, profile, cwd, *before):
    from harness.vucx import Vucx

    module = Vucx(profile)
    try:
        out = _run(exe, module.start(), *before, "dev.summary", cwd=cwd)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    return out.stdout.split("Module:")[-1]


def _store(tmp_path, **files):
    (tmp_path / "storage").mkdir(exist_ok=True)
    for name, text in files.items():
        (tmp_path / "storage" / f"ucxapp.{name}").write_text(text + "\n")


def test_dev_summary_of_a_connected_wifi_module(app_exe, tmp_path, monkeypatch):
    pytest.importorskip("yaml")
    monkeypatch.setenv("UCX_REGDOMAIN", "etsi")
    _store(tmp_path, wifilast="home", blelast="tag")
    out = _summary(app_exe, "wifi.yaml", tmp_path, "wifi.connect vucx-net secret123")
    assert "NORA-W36, firmware 3.5.0" in out, out
    assert re.search(r"^Radios:\s+Wi-Fi, Bluetooth LE$", out, re.M), out
    assert re.search(r"^Wi-Fi:\s+.*IP 192\.168\.1\.50$", out, re.M), out
    assert "Update:" not in out and "Thread:" not in out and "Wi-Fi 'home'" not in out, out
    assert re.search(r"^Last used:\s+BLE 'tag' \(bt\.profile\.auto\)$", out, re.M), out


def test_dev_summary_notes_old_firmware_and_last_used(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("basic.yaml")
    profile["identity"] = {**profile.get("identity", {}), "version": "3.2.0-0"}
    _store(tmp_path, wifilast="home", blelast="tag", fwlatest="NORA-W36-3.4.0.zip")
    out = _summary(app_exe, profile, tmp_path)
    assert re.search(r"^Update:\s+firmware 3\.5\.0 is out, update with fw\.update$", out, re.M), out
    assert "Last used:        Wi-Fi 'home' (wifi.profile.auto), BLE 'tag' (bt.profile.auto)" in out, out
    assert "Open:" not in out, out


def test_dev_summary_uses_the_release_fw_update_found(app_exe, tmp_path):
    pytest.importorskip("yaml")
    _store(tmp_path, fwlatest="NORA-B26-9.9.1.zip")
    out = _summary(app_exe, "nora-b26.yaml", tmp_path)
    assert re.search(r"^Radios:\s+Bluetooth LE$", out, re.M), out
    assert "firmware 9.9.1 is out" in out and "Wi-Fi" not in out and "Last used" not in out, out


def test_dev_summary_of_a_thread_leader(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import load

    profile = load("nora-b26-thread.yaml")
    profile["commands"]["AT+UTHE?"] = ["+UTHE:1", "OK"]
    out = _summary(app_exe, profile, tmp_path)
    assert re.search(r"^Radios:\s+Bluetooth LE, Thread$", out, re.M), out
    assert re.search(r"^Thread:\s+leader$", out, re.M), out


def test_thread_usage_errors(app_exe):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-b26-thread.yaml")
    port = module.start()
    try:
        for bad in ("thread.network x 99 1 00 00", "thread.dataset 0g", "thread.join ABC", "thread.type fast",
                    "thread.srp add _x._udp 0 i"):
            out = _run(app_exe, port, bad)
            assert "AT+UTHNC=" not in module.log and "AT+UTHJS=" not in module.log, (bad, module.log)
            assert out.returncode != 0 or "ERROR" in out.stdout or "Usage" in out.stdout, (bad, out.stdout)
    finally:
        module.stop()


def test_wifi_connect_status_disconnect(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "wifi.connect vucx-net secret123", "wifi.status", "wifi.disconnect")
    assert any(c.startswith("AT+UWSC=") for c in wifi_module.log), wifi_module.log
    assert "AT+UWSDC" in wifi_module.log, wifi_module.log
    assert "192.168.1.50" in out.stdout, out.stdout + str(wifi_module.log)


def test_at_trace_hides_wifi_password(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "app.log.at.on", "wifi.connect vucx-net secret123", "app.log.at.off")
    assert "AT+UWSSW=" in out.stdout, out.stdout
    assert "secret123" not in out.stdout and '"****"' in out.stdout, out.stdout


def test_wifi_survey_gives_a_verdict(app_exe, wifi_module):
    out = _run(app_exe, wifi_module.port, "wifi.connect vucx-net secret123", "wifi.survey 2 example.com")
    assert "Signal:" in out.stdout and "Stability:" in out.stdout, out.stdout + str(wifi_module.log)
    assert re.search(r"^Verdict:\s+(EXCELLENT|GOOD|FAIR|POOR) - ", out.stdout, re.MULTILINE), out.stdout


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
                    "gatt.client.nus 0", "gatt.client.sps 0", "gatt.client.witmotion 0", "gatt.client.wps 0",
                    "gatt.wps 0 status", "gatt.send 0 nus hello", "gatt.sps.credits 0 10"],
    "gatt-server": ["gatt.server.hrs", "gatt.server.cts", "gatt.server.ess", "gatt.server.lns",
                    "gatt.server.aio", "gatt.server.bas", "gatt.server.ras", "gatt.server.nus",
                    "gatt.server.sps", "gatt.server.wps", "gatt.server.list", "gatt.server.set hrs.measurement 0048",
                    "gatt.server.notify 0 hrs.measurement 0048", "gatt.server.indicate 0 hrs.measurement 0048",
                    "gatt.server.demo 0 hrs 1", "gatt.server.hostchar 180F 2A19 12",
                    "gatt.server.readresp 0 01", "gatt.server.writeresp 0", "gatt.server.error 0 read 2",
                    "gatt.server.servicechanged 0 1 100"],
    "hid": ["hid.setup", "hid.status", "hid.key a ctrl shift", "hid.text Hello", "hid.mouse 10 -5 1",
            "hid.click left double", "hid.media volup", "hid.client 0", "hid.client off"],
    "web": ["web.status", "web.start 8080 tok", "web.status", "web.stop"],
    "ancs": ["ancs.on", "ancs.sub 0", "ancs.list", "ancs.show 42", "ancs.action 42 pos", "ancs.off"],
    "notify": ["notify.on", "notify.list", "notify.action 1 dismiss", "notify.off"],
    "coap": ["coap.get 192.168.1.10 5683 temp"],
    "time": ["app.time", "app.time sync module", "app.time push", "app.time set 2026-10-27T12:00:00"],
    "cert-more": ["cert.remove myca"],
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
    errors = [line for line in out.stdout.splitlines() if re.search(r"\bERROR\b|(?<!\b0 )\b[Ff]ailed\b", line)]
    assert not errors, "\n".join(errors) + f"\nlast AT: {nora.log[-10:]}"


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
                              capture_output=True, text=True, timeout=timeout, errors="replace", cwd=_CWD)
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


def test_gnss_runs_several_bridges_at_once(app_exe, nora):
    """Up to 4 bridges side by side on one receiver, numbered, stopped one by one; a fifth is refused."""
    from harness.vucx import Feeder

    gnss = Feeder(NMEA, 0.2)
    port = gnss.start()
    try:
        out = _shell(app_exe, nora.port, [
            f"gnss.open {port} 9600", "gnss.bridge tcp 2947", "gnss.bridge udp 192.168.1.10 10110",
            "gnss.bridge ble nus", "gnss.bridge status", "gnss.bridge off 2", "gnss.bridge status",
            "gnss.bridge udps 9000", "gnss.bridge tcp 2948", "bridge.list", "gnss.status",
            "gnss.bridge udps 9001"])
    finally:
        gnss.stop()
    text = out.stdout
    assert out.returncode == 1, text[-3000:]
    for n, kind in ((1, "TCP server port 2947"), (2, "UDP port 10110"), (3, "BLE Nordic UART")):
        assert f"bridge {n}: {kind}" in text, text[-3000:]
    assert "bridge 2 stopped" in text, text[-3000:]
    after_off = text.split("bridge 2 stopped", 1)[1]
    first_status = after_off.split("bridge 3: BLE", 1)[0]
    assert "bridge 2:" not in first_status, text[-3000:]
    assert "bridge 2: UDP port 0" in after_off and "bridge 4: TCP server port 2948" in after_off, text[-3000:]
    assert "4 bridges are already running on the gnss (gnss.bridge off <n> first)" in text, text[-3000:]
    assert text.count("bridge 1: TCP server port 2947") >= 4, text[-3000:]   # status twice, bridge.list, gnss.status


def test_ntrip_bridge_checks_every_correction_frame(app_exe, nora):
    """The profile's caster sends RTCM, SPARTN (CRC-8/16/24/32, encrypted, 32-bit time tag) and UBX frames, junk
    and one RTCM frame with a broken CRC; gnss.bridge status counts good and bad frames of each kind."""
    from harness.vucx import Feeder

    gnss = Feeder(NMEA, 0.2)
    port = gnss.start()
    try:
        out = _shell(app_exe, nora.port, [
            f"gnss.open {port} 9600", "gnss.bridge ntrip caster.local 2101 MP", "gnss.watch 3",
            "gnss.bridge status", "gnss.bridge off"], log=nora.log)
    finally:
        gnss.stop()
    text = out.stdout
    assert re.search(r"\(RTCM 2 \(1 bad\), SPARTN 4, UBX 1, \d+ bytes outside frames\)", text), text[-3000:]


def test_hw_bridges_share_the_stream_until_the_last_one_stops(app_exe, nora):
    """hw.bridge keeps its stream while one bridge is left; only one BLE bridge per stream."""
    out = _shell(app_exe, nora.port, [
        "hw.bridge udps 7000", "hw.bridge ble nus", "hw.bridge status", "hw.bridge off 1", "hw.bridge status",
        "hw.bridge off", "hw.bridge status", "hw.bridge tcp", "hw.bridge ble nus", "hw.bridge ble sps"])
    text = out.stdout
    assert out.returncode == 1, text[-3000:]
    assert "bridge 1: UDP port 0" in text and "bridge 2: BLE Nordic UART" in text, text[-3000:]
    assert "bridge 1 stopped" in text and "bridge 2 stopped" not in text, text[-3000:]
    assert text.count("bridge: off") == 1, text[-3000:]
    assert "ERROR: a BLE bridge is already running on the hw" in text, text[-3000:]


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


USE_CASES = ["boat", "rtk", "mower", "farm", "tracker", "home", "provision", "hotel", "notify", "remote"]


@pytest.mark.parametrize("case", USE_CASES)
def test_use_case_steps_run_on_the_virtual_module(app_exe, nora, case, tmp_path):
    """Every step of every use case (examples uc-<name> N) runs on the virtual NORA-W36 without stopping."""
    from harness.vucx import Feeder, Vucx

    listing = subprocess.run([str(app_exe), "COM255", "--exec", f"examples uc-{case}"], capture_output=True,
                             text=True, timeout=30, errors="replace", stdin=subprocess.DEVNULL).stdout
    count = len(re.findall(r"^ \d+\. ", listing, re.MULTILINE))
    assert count > 0, listing
    gnss = Feeder(NMEA, 0.2)
    modem = Vucx("cell.yaml")
    args = [str(app_exe), nora.port, "--exec", f"gnss.open {gnss.start()} 9600", "--exec",
            f"cell.open {modem.start()}"]
    for n in range(1, count + 1):
        args += ["--exec", f"examples uc-{case} {n}"]
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=150, errors="replace",
                             stdin=subprocess.DEVNULL, cwd=tmp_path)
    finally:
        gnss.stop()
        modem.stop()
    ran = out.stdout.count(f"Running uc-{case} example")
    bad = [line for line in out.stdout.splitlines() if line.startswith(("stopped", "ERROR"))]
    assert ran == count and not bad, (f"{ran}/{count} ran\n" + "\n".join(bad) + f"\n{out.stdout[-3000:]}"
                                      f"\nlast AT: {nora.log[-15:]}")
    if case == "provision":   # the profile's virtual phone sends SET_CONFIG to gatt.server.wps
        assert "the phone sent the Wi-Fi settings" in out.stdout, out.stdout[-3000:]
    if case in ("mower", "farm"):   # PointPerfect over the modem's data link: request up, corrections down
        assert any(c.startswith("GET /NEAR-SPARTN ") for c in modem.log), modem.log[-20:]
        assert "NTRIP: streaming corrections from /NEAR-SPARTN" in out.stdout, out.stdout[-3000:]


THREAD_CASES = {"mesh": "nora-b26-thread.yaml", "border": "nora-b26-thread.yaml",
                "building": "nora-b26-thread.yaml", "gateway": "alma-w1-thread.yaml"}


@pytest.mark.parametrize("case", THREAD_CASES)
def test_thread_use_case_fits_and_runs_on_a_virtual_module(app_exe, case, tmp_path):
    """The Thread use cases fit a virtual Thread module (gateway: ALMA-W1 with Wi-Fi) and every step runs."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    listing = subprocess.run([str(app_exe), "COM255", "--exec", f"examples uc-{case}"], capture_output=True,
                             text=True, timeout=30, errors="replace", stdin=subprocess.DEVNULL).stdout
    count = len(re.findall(r"^ \d+\. ", listing, re.MULTILINE))
    assert count > 0, listing
    module = Vucx(THREAD_CASES[case])
    try:
        out = _run(app_exe, module.start(), f"usecase.check {case}",
                   *[f"examples uc-{case} {n}" for n in range(1, count + 1)], timeout=150, cwd=tmp_path)
    finally:
        module.stop()
    assert "[gap]" not in out.stdout, out.stdout
    ran = out.stdout.count(f"Running uc-{case} example")
    bad = [line for line in out.stdout.splitlines() if line.startswith(("stopped", "ERROR"))]
    assert ran == count and not bad, f"{ran}/{count} ran\n" + "\n".join(bad) + f"\n{out.stdout[-3000:]}"
    assert any(c.startswith("AT+UTH") for c in module.log), module.log[-15:]


@pytest.mark.parametrize("profile, gaps", [("nora-b26.yaml", "no Thread firmware"),
                                          ("nora-b26-thread.yaml", "has no Wi-Fi")])
def test_thread_gateway_shows_what_the_module_lacks(app_exe, profile, gaps):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx(profile)
    try:
        out = _run(app_exe, module.start(), "usecase.check gateway")
    finally:
        module.stop()
    assert re.search(rf"^\s*\[gap\].*{gaps}", out.stdout, re.M), out.stdout


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


def test_can_bridge_cell_reconnects_a_dropped_link(app_exe, nora):
    from harness.vucx import Vucx

    modem = Vucx("cell.yaml")
    modem.urcs["AT+USODL=*"] = {"after": 2, "lines": ["NO CARRIER"], "times": 1}
    port = modem.start()
    proc = subprocess.Popen([str(app_exe), nora.port, "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    try:
        proc.stdin.write(f"cell.open {port}\ncell.connect caster.local 2101\ncan.bridge cell\n")
        proc.stdin.flush()
        deadline = time.time() + 20
        while time.time() < deadline and sum(c.startswith("AT+USODL=") for c in modem.log) < 2:
            time.sleep(0.2)
        time.sleep(0.5)
        out, _ = proc.communicate("can.bridge status\ncan.bridge off\n", timeout=30)
    finally:
        proc.kill()
        modem.stop()
    assert "can bridge: cellular link on" in out and "reconnected" in out, out[-3000:]
    assert "cellular, 1 reconnect(s)" in out, out[-3000:]
    assert sum(c.startswith("AT+USODL=") for c in modem.log) == 2, modem.log
    assert sum(c.startswith('AT+USOCO=0,"caster.local",2101') for c in modem.log) == 2, modem.log
    assert "AT+USOCL=0" in modem.log, modem.log


def test_can_bridge_cell_needs_a_data_link(app_exe, nora):
    out = _shell(app_exe, nora.port, ["can.bridge cell", "can.bridge status"]).stdout
    assert "no cellular data link (use cell.connect first)" in out, out[-2000:]


def test_cfg_save_diff_load_round_trip(app_exe, tmp_path):
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    port = module.start()
    try:
        out = _run(app_exe, port, "cfg.show", "cfg.save a.cfg", "cfg.diff a.cfg", "cfg.load a.cfg nostore",
                   cwd=tmp_path)
    finally:
        module.stop()
    saved = (tmp_path / "storage" / "a.cfg").read_text()
    assert out.returncode == 0, out.stdout
    assert 'AT+UBTLN="vucx"' in out.stdout and 'AT+UBTLN="vucx"' in saved, (out.stdout, saved)
    assert "NORA-W36" in saved and "no differences" in out.stdout.lower(), out.stdout
    assert "0 applied" in out.stdout and "AT&W" not in module.log, out.stdout


def test_cfg_load_writes_only_changed_known_settings(app_exe, tmp_path):
    from harness.vucx import Vucx

    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "b.cfg").write_text('AT+UBTLN="other"\nAT+UBTM=3\nAT+CPWROFF\nAT+GMM\n')
    module = Vucx("nora-w36.yaml")
    port = module.start()
    try:
        out = _run(app_exe, port, "cfg.load b.cfg", "cfg.diff b.cfg", cwd=tmp_path)
    finally:
        module.stop()
    assert 'AT+UBTLN="other"' in module.log and "AT&W" in module.log, module.log
    assert "AT+UBTM=3" not in module.log and "AT+CPWROFF" not in module.log, module.log
    assert "1 applied" in out.stdout and "1 unchanged" in out.stdout and "2 skipped" in out.stdout, out.stdout


def test_at_trace_show_and_replay(app_exe, wifi_module, tmp_path):
    out = _run(app_exe, wifi_module.port, "at.trace t.trace", "wifi.connect vucx-net secret123", "dev.info",
               "at.trace.stop", "at.trace.show t.trace", "at.replay t.trace", cwd=tmp_path)
    trace = (tmp_path / "storage" / "t.trace").read_text()
    assert out.returncode == 0, out.stdout
    assert "\n# start " in trace and " TX AT+GMM" in trace and " RX OK" in trace, trace
    assert "secret123" not in trace and "****" in trace, trace
    assert "Wi-Fi network up" in out.stdout and "slowest" in out.stdout.lower(), out.stdout
    assert "0 different" in out.stdout or "differ" not in out.stdout, out.stdout


def test_at_trace_to_pcapng(app_exe, wifi_module, tmp_path):
    from harness import pcapng

    out = _run(app_exe, wifi_module.port, "at.trace t.pcapng", "wifi.connect vucx-net secret123", "at.trace.stop",
               cwd=tmp_path)
    assert out.returncode == 0 and "Wireshark pcapng" in out.stdout, out.stdout
    _, frames = pcapng.read(tmp_path / "storage" / "t.pcapng")
    payloads = b"".join(f["payload"] for f in frames)
    assert f"{len(frames)} lines written" in out.stdout, out.stdout
    assert b"secret123" not in payloads and b"****" in payloads, payloads
    assert any(f["out"] and f["payload"].startswith(b"AT+UWS") for f in frames), payloads
    assert frames[0]["ts"] > 1_600_000_000_000, frames[0]


@pytest.mark.parametrize("proto,host", [("udp", "192.168.1.20"), ("tcp", "example.com")])
def test_perf_rtt_and_tput_over_the_echo(app_exe, proto, host):
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")   # the profile with the echo network behind AT+USO*
    port = module.start()
    try:
        out = _run(app_exe, port, f"perf.latency {proto} {host} 7 5 64", f"perf.throughput {proto} {host} 7 2 128",
                   "sock.list")
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout + str(module.log)
    assert "5 sent, 5 received, 0 lost" in out.stdout and "rtt min" in out.stdout, out.stdout
    assert "0.0% not echoed" in out.stdout and "pattern 0x00-0xFF: 0 breaks" in out.stdout, out.stdout
    assert "0 socket(s)" in out.stdout, out.stdout
    # The s-center data pump pattern: one 0x00-0xFF counter running on across every packet sent.
    sent = [data for cmd, data in module.payloads if cmd.startswith(("AT+USOWB=", "AT+USOSTB="))]
    stream = b"".join(sent[5:])   # the tput packets, after the 5 rtt ones
    assert sent[0] == bytes(range(64)) and len(stream) >= 256, sent[:2]
    assert stream == bytes(i & 0xFF for i in range(len(stream))), stream[:300]


def test_perf_sps_needs_a_connection(app_exe, vucx):
    out = _run(app_exe, vucx.port, "perf.latency sps 2")
    assert out.returncode != 0 and "no Bluetooth connection" in out.stdout, out.stdout

def test_bt_addr_name_and_filter(app_exe, nora):
    out = _shell(app_exe, nora.port, ["bt.addr", "bt.name", "bt.name MySensor", "bt.addr 54:F8:2A:14:C9:EF",
                                      "bt.addr a:b:c:d:e:f,public",
                                      "bt.filter", "bt.filter 0 9 4E:4F:52:41", "bt.filter 1 0xFF 0102",
                                      "bt.filter 2 9 NORA-W36", "bt.filter 0 clear", "bt.filter clear"], log=nora.log)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "C0:FF:EE:00:11:44" in out.stdout and "vucx" in out.stdout, out.stdout
    assert '"NORA"' in out.stdout and "2 data filter(s)" in out.stdout, out.stdout
    assert '"NORA-W36"' in out.stdout and "all data filters cleared" in out.stdout, out.stdout
    sent = nora.log
    assert 'AT+UBTLN="MySensor"' in sent and "AT+USYLA=0,54F82A14C9EF" in sent and "AT&W" in sent, sent
    assert "AT+USYLA=0,0A0B0C0D0E0F" in sent, sent
    assert "AT+UBTDFD=0,9,4E4F5241" in sent and "AT+UBTDFD=1,255,0102" in sent, sent
    assert "AT+UBTDFD=2,9,4E4F52412D573336" in sent and '"NORA-W36"' in out.stdout, sent
    assert sum(cmd.startswith("AT+UBTDFD=") and cmd.endswith(",0,") for cmd in sent) == 11, sent


@pytest.mark.parametrize("args", ["bt.filter 10 9 41", "bt.filter 0 9 0x4G", "bt.filter 0 9 ABCDEFGHIJK",
                                  "bt.filter 0 300 41", "bt.filter 0 9 0102030405060708090A0B",
                                  "bt.filter 0 9", "bt.addr 11:22:33", "bt.name a b", "bt.addr 11:22:33:44:55:66x",
                                  "bt.addr 11:22:33:44:55:66,other", "bt.addr 11:22:33:44:5566",
                                  "bt.addr 111:22:33:44:55:66", "bt.addr :11:22:33:44:55"])
def test_bt_filter_addr_name_usage(app_exe, nora, args):
    out = _shell(app_exe, nora.port, [args], log=nora.log)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout
    assert not any(cmd.startswith(("AT+UBTDFD=", "AT+USYLA=0,", "AT+UBTLN=")) for cmd in nora.log), nora.log


def test_bt_cs_sets_reflector_and_explains_a_module_without_it(app_exe, nora):
    out = _shell(app_exe, nora.port, ["bt.cs on", "bt.cs off"], log=nora.log)
    assert out.returncode == 0 and "Channel Sounding on: reflector" in out.stdout, out.stdout
    assert "Channel Sounding off" in out.stdout, out.stdout
    assert "AT+UBTCSM=1" in nora.log and "AT+UBTCSM=0" in nora.log and "AT&W" in nora.log, nora.log
    # the W36 profile has no mode to read back, as a real NORA-W36
    out = _shell(app_exe, nora.port, ["bt.cs"], log=nora.log)
    assert out.returncode != 0 and "needs a NORA-B26 or NORA-B27" in out.stdout, out.stdout
    out = _shell(app_exe, nora.port, ["bt.cs maybe"], log=nora.log)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


def test_mqtt_connect_with_login_and_tls(app_exe, nora):
    out = _shell(app_exe, nora.port, ["mqtt.connect broker.local user=me pass=pw ca=myca cert=dev key=devkey",
                                      "mqtt.connect broker.local 1884 id=plant7 tls", "mqtt.connect"], log=nora.log)
    assert out.returncode == 0, out.stdout + out.stderr
    sent = nora.log
    login = [cmd for cmd in sent if cmd.startswith('AT+UMQCP=0,"broker.local",8883,"ucxapp-')]
    assert login and login[0].endswith(',"me","pw"'), sent
    assert 'AT+UMQTLS=0,3,"myca","dev","devkey"' in sent, sent
    assert 'AT+UMQCP=0,"broker.local",1884,"plant7"' in sent and "AT+UMQTLS=0,3" in sent, sent
    assert "AT+UMQTLS=0,0" in sent, sent
    assert "user me, TLS with client certificate" in out.stdout, out.stdout
    assert "TLS (server not verified)" in out.stdout, out.stdout


def test_mqtt_publish_qos_and_retain(app_exe, nora):
    out = _shell(app_exe, nora.port, ["mqtt.publish home/door qos=1 retain open", "mqtt.publish a/b qos is fine",
                                      "mqtt.publish a/b qos=3 x"], log=nora.log)
    pubs = [cmd for cmd in nora.log if cmd.startswith("AT+UMQPB=")]
    assert pubs[0].startswith('AT+UMQPB=0,1,1,"home/door"'), pubs
    assert pubs[1].startswith('AT+UMQPB=0,0,0,"a/b"') and len(pubs) == 2, pubs
    assert "published 4 bytes to home/door (qos 1, retained)" in out.stdout, out.stdout
    assert "published 11 bytes to a/b (qos 0)" in out.stdout, out.stdout


@pytest.mark.parametrize("args", ["mqtt.connect h cert=c key=k", "mqtt.connect h ca=a cert=c",
                                  "mqtt.connect h pass=pw", "mqtt.connect h id=", "mqtt.connect h 1883 x",
                                  "mqtt.connect h 70000", "mqtt.connect h color=red"])
def test_mqtt_connect_usage(app_exe, nora, args):
    out = _shell(app_exe, nora.port, [args], log=nora.log)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout
    assert not any(cmd.startswith(("AT+UMQCP=", "AT+UMQTLS=")) for cmd in nora.log), nora.log


def test_dev_power_levels_sleep_and_deep_sleep(app_exe, nora):
    out = _shell(app_exe, nora.port, ["dev.power", "dev.power save 1 2000", "dev.power save 0", "dev.power sleep",
                                      "dev.power wake", "dev.power deep", "dev.power deep uart"], log=nora.log)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "power save level 0 (off" in out.stdout and "idle timeout 2000 ms" in out.stdout, out.stdout
    assert "light sleep" in out.stdout and "awake" in out.stdout and "UART RX pin" in out.stdout, out.stdout
    sent = nora.log
    for cmd in ("AT+UPMPSL=1", "AT+UPMPSTO=2000", "AT+UPMPSL=0", "AT&W", "AT+UPMS", "AT", "AT+UPMDS", "AT+UPMDS=2"):
        assert cmd in sent, (cmd, sent)


@pytest.mark.parametrize("args", ["dev.power save 2", "dev.power save 1 0", "dev.power save", "dev.power deep gpio",
                                  "dev.power sleep now", "dev.power nap"])
def test_dev_power_usage(app_exe, nora, args):
    out = _shell(app_exe, nora.port, [args], log=nora.log)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout
    assert not any(cmd.startswith("AT+UPM") for cmd in nora.log), nora.log


def test_bt_connect_by_name_scans_with_a_filter(app_exe, nora):
    # a name not in range: first 10 bytes on AD type 9 in slot 9, scan, slot 9 restored, then an error
    out = _shell(app_exe, nora.port, ["bt.connect vucx-hrs-sensor"], log=nora.log)
    assert out.returncode != 0 and 'scanning for "vucx-hrs-s"' in out.stdout, out.stdout
    assert "name of a device in range" in out.stdout, out.stdout
    sent = nora.log
    name = sent.index("AT+UBTDFD=9,9,767563782D6872732D73")
    scan = next(i for i, cmd in enumerate(sent) if cmd.startswith("AT+UBTD=") and i > name)
    restore = sent.index("AT+UBTDFD=9,0,")
    assert name < scan < restore and not any(cmd.startswith("AT+UBTC=") for cmd in sent), sent
    # the device's own name: filtered scan, slot 9 restored, then connect to the device found
    # (the virtual module always lists a link to it, so the app answers "already connected")
    first = len(nora.log)
    out = _shell(app_exe, nora.port, ["bt.connect vucx-hrs"], log=nora.log)
    sent = nora.log[first:]
    assert out.returncode == 0 and "connecting to vucx-hrs (C0:FF:EE:00:11:22)" in out.stdout, out.stdout
    assert sent.index("AT+UBTDFD=9,9,767563782D687273") < sent.index("AT+UBTDFD=9,0,"), sent


LINK = ["+UBTCL:0,C0FFEE001122p", "OK"]


@pytest.mark.parametrize("after_connect, retried, ok", [
    ([LINK, ["OK"], LINK, LINK], True, True),                         # dropped once, then holds
    ([LINK, ["OK"]], True, False),                                    # every attempt drops
])
def test_bt_connect_retries_a_link_that_drops_right_away(app_exe, nora, after_connect, retried, ok):
    """A connect request the peer never heard is reported up and then dropped; bt.connect tries again."""
    nora.setters["AT+UBTC=*"] = {"link": "asked"}
    entries = {"AT+UBTCL [link=asked]": after_connect, "AT+UBTCL": ["OK"]}
    rest = {k: v for k, v in nora.commands.items() if k not in entries}
    nora.commands.clear()
    nora.commands.update({**entries, **rest})
    out = _shell(app_exe, nora.port, ["bt.connect C0:FF:EE:00:11:22", "gatt.services"], log=nora.log)
    attempts = sum(cmd.startswith("AT+UBTC=") for cmd in nora.log)
    assert ("retrying (2/3)" in out.stdout) == retried, out.stdout
    if ok:
        assert attempts == 2 and "connection 0 (current)" in out.stdout and "service(s)" in out.stdout, out.stdout
    else:
        assert attempts == 3 and "dropped right after connecting 3 times" in out.stdout, out.stdout
        assert "connection 0 (current)" not in out.stdout and "no BLE connection open" in out.stdout, out.stdout


def test_gatt_server_reboots_the_module_once_before_its_first_service(app_exe, nora):
    """The module keeps old services until a restart; a second run must not stack duplicates."""
    out = _shell(app_exe, nora.port, ["gatt.server.bas", "gatt.server.nus"], log=nora.log)
    assert "AT+CPWROFF" in nora.log, (nora.log, out.stdout)
    reboot = nora.log.index("AT+CPWROFF")
    first = next(i for i, cmd in enumerate(nora.log) if cmd.startswith("AT+UBTGS="))
    assert nora.log.count("AT+CPWROFF") == 1 and reboot < first, nora.log
    assert out.stdout.count("GATT server starts clean") == 1, out.stdout
