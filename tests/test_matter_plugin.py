"""matter.* with the real plug-in (ucxmatter, built from matter/ by "ucxapp --matter --build") against vucx, the
virtual module. The plug-in needs the Matter SDK, so these tests run only where it has been built."""

import os
import re
import subprocess
import time
from pathlib import Path

import pytest
from harness.matter import (
    PAIRING_EXCHANGE,
    PAIRING_NODE,
    PBKDF_PARAM_RESPONSE,
    at_burst,
    codes_allowed,
    manual_code,
    pairing_request,
    setup_codes,
    unsecured_message,
)

pytestmark = pytest.mark.app


@pytest.fixture
def plugin(app_exe):
    path = Path(app_exe).parent / ("ucxmatter.dll" if os.name == "nt" else "libucxmatter.so")
    if not path.exists():
        pytest.skip("the Matter plug-in is not built (ucxapp --matter --build)")
    return path


@pytest.fixture
def module():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    # Bluetooth address C0FFEE001144
    vucx = Vucx("nora-w36.yaml")
    vucx.start()
    yield vucx
    vucx.stop()


def _run(exe, module, plugin, cwd, *commands):
    args = [str(exe), module.port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd, env=dict(os.environ, UCX_MATTER_PLUGIN=str(plugin)))


def test_start_runs_the_matter_service_on_the_module_and_shows_the_setup_codes(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status")
    assert out.returncode == 0, out.stdout
    discriminator, passcode = setup_codes(out.stdout)
    assert codes_allowed(discriminator, passcode), out.stdout
    assert "matter: node started (Matter SDK v" in out.stdout, out.stdout
    assert re.search(r"node: NORA-\S+ over Wi-Fi, 3 endpoint\(s\)", out.stdout), out.stdout
    # the virtual module connects a host as soon as a GATT service is activated
    assert "state: Bluetooth LE link up, not commissioned" in out.stdout, out.stdout
    assert f"discriminator {discriminator}, vendor 0xFFF1, product 0x8001, passcode {passcode:08d}" in out.stdout
    # the Matter stack defines its GATT service: C1 for the commissioner's writes, C2 for the node's indications
    sent = "\n".join(module.log)
    assert "AT+UBTGS=FFF6" in module.log, module.log
    assert "AT+UBTGHCC=18EE2EF5263D4559959F4F9C429F9D11,08" in sent, sent
    assert "AT+UBTGHCC=18EE2EF5263D4559959F4F9C429F9D12,22" in sent, sent
    assert "AT+UBTGSA" in module.log, module.log
    # service UUID FFF6, then its service data: commissionable, discriminator, vendor, product (as the QR code has)
    data = f"00{discriminator & 0xFF:02X}{discriminator >> 8:02X}F1FF018000"
    assert f"AT+UBTADL=0303F6FF0B16F6FF{data}" in module.log, sent
    assert "AT+UBTAL" in module.log, sent
    # from the SDK's own generators
    assert re.search(rf"manual code: {manual_code(discriminator, passcode)}\d\n", out.stdout), out.stdout
    assert re.search(r"QR payload: MT:[0-9A-Z.\-]{19}\n", out.stdout), out.stdout
    # the SDK's own log lines are detail: the default log shows the node's milestones only
    assert "[matter] [" not in out.stdout, out.stdout
    # leaving the app stops the advertising
    assert "AT+UBTALD" in module.log[module.log.index("AT+UBTAL"):], module.log


def test_the_modules_bonds_mode_and_name_stay_as_the_user_set_them(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start")
    assert out.returncode == 0, out.stdout
    sent = "\n".join(module.log)
    assert "AT+UBTGS=FFF6" in sent, sent   # the node did come up
    for command in ("AT+UBTUB", "AT+UBTM=", "AT+UBTLN=", "AT+USYFR", "AT&W"):
        assert command not in sent, sent


def test_setup_codes_are_random_for_each_node_and_kept_sealed_in_its_store(app_exe, module, plugin, tmp_path):
    first = tmp_path / "first"
    first.mkdir()
    codes = setup_codes(_run(app_exe, module, plugin, first, "matter.start", "matter.status").stdout)
    # the same node again: the codes on its label
    assert setup_codes(_run(app_exe, module, plugin, first, "matter.start", "matter.status").stdout) == codes
    stored = list(first.rglob("matter.factory.key"))
    assert len(stored) == 1, list(first.rglob("*"))
    sealed = stored[0].read_bytes()
    assert sealed.startswith(b"UCXS") and codes[1].to_bytes(4, "little") not in sealed, sealed[:16]
    # another node on the same module (its address is the same): other codes, nothing comes from the address
    second = tmp_path / "second"
    second.mkdir()
    other = setup_codes(_run(app_exe, module, plugin, second, "matter.start", "matter.status").stdout)
    assert other != codes and codes_allowed(*other), (codes, other)


def test_reset_keeps_the_setup_codes_and_reset_all_draws_new_ones(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status", "matter.reset", "matter.status")
    assert out.returncode == 0 and "commission the node again\n" in out.stdout, out.stdout
    before, after = out.stdout.split("matter: reset")
    codes = setup_codes(before)
    assert setup_codes(after) == codes and "commissioning window: open" in after, out.stdout
    # new codes: the node advertises them at once, and has them at its next start
    sent = len(module.log)
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.reset all", "matter.status")
    assert out.returncode == 0 and "again with its new setup codes" in out.stdout, out.stdout
    new = setup_codes(out.stdout)
    assert new != codes and codes_allowed(*new), (codes, new)
    assert re.search(rf"manual code: {manual_code(*new)}\d\n", out.stdout), out.stdout
    data = [f"AT+UBTADL=0303F6FF0B16F6FF00{d & 0xFF:02X}{d >> 8:02X}F1FF018000" for d in (codes[0], new[0])]
    log = module.log[sent:]
    assert data[0] in log and data[1] in log[log.index(data[0]):], log
    assert setup_codes(_run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status").stdout) == new
    # without a running node the codes go with the rest of the store
    out = _run(app_exe, module, plugin, tmp_path, "matter.reset all", "matter.start", "matter.status")
    assert out.returncode == 0 and setup_codes(out.stdout) not in (codes, new), out.stdout
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.reset codes")
    assert out.returncode != 0, out.stdout


def test_node_says_which_product_it_is(app_exe, module, plugin, tmp_path):
    from harness.vucx import Vucx, load

    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status")
    assert out.returncode == 0, out.stdout
    # what the Basic Information cluster gives a controller: the module is the hardware, its firmware the software
    assert "product: u-blox u-connectXpressApp, hardware NORA-W36, serial VUCX0123456789, software 3.5.0-0\n" \
        in out.stdout, out.stdout
    # a commissioner that looks on the network sees the name before it pairs
    assert 'AT+UMDTRA=0,"DN","u-connectXpressApp-001144"' in module.log, module.log
    # a module that gives its type code: that is the hardware version and the part number
    profile = load("nora-w36.yaml")
    profile["commands"]["ATI0"] = ['"NORA-W361-00B"', "OK"]
    typed = Vucx(profile)
    typed.start()
    try:
        out = _run(app_exe, typed, plugin, tmp_path, "matter.start", "matter.status")
    finally:
        typed.stop()
    assert "hardware NORA-W361-00B, serial VUCX0123456789" in out.stdout, out.stdout


def test_detail_log_shows_the_matter_stack(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.log detail", "matter.start")
    assert out.returncode == 0, out.stdout
    assert "[matter] [DL] [BLE] CHIPoBLE service activated" in out.stdout, out.stdout
    assert "Select failed" not in out.stdout, out.stdout


def test_a_commissioner_opens_the_matter_transport_over_bluetooth(app_exe, module, plugin, tmp_path):
    # the virtual module's host writes a BTP handshake request to C1 when the Matter service is activated
    out = _run(app_exe, module, plugin, tmp_path, "app.log.events.on", "matter.start", "gatt.listen 2",
               "matter.status")
    assert out.returncode == 0, out.stdout
    sent = "\n".join(module.log)
    # the Matter server answers on C2: handshake response, version 4, then fragment size and window
    assert re.search(r"AT\+UBTGIS=0,\d+,656C04[0-9A-F]{6}\n", sent), sent
    assert "matter: commissioning window open" in out.stdout, out.stdout
    assert "matter: Bluetooth LE link 0 up" in out.stdout, out.stdout
    assert "commissioning window: open" in out.stdout, out.stdout
    # the write was for the node: the app's own GATT server did not report it
    assert re.search(r"kept by the node: [1-9]", out.stdout) and "[write conn=" not in out.stdout, out.stdout


def test_open_shows_the_codes_again_and_keeps_the_window_open(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.open", "matter.status")
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("QR payload: MT:") == 2 and "commissioning window: open" in out.stdout, out.stdout
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.open 20")
    assert out.returncode != 0 and "180 to 900 seconds" in out.stdout, out.stdout


def test_at_bursts_of_the_node_have_no_gaps(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "at.trace t.trace", "matter.start", "gatt.listen 1",
               "at.trace.stop")
    assert out.returncode == 0, out.stdout
    trace = (tmp_path / "storage" / "t.trace").read_text()
    # the GATT set-up on the thread of matter.start, the advertisement on the Matter thread
    for first, last, count in (("AT+UBTPM=", "AT+UBTGSA", 7), ("AT+UBTADL=", "AT+UBTAL", 5)):
        seconds, gap, sent = at_burst(trace, first, last)
        assert len(sent) == count, sent
        assert seconds < 1.5 and gap < 0.5, (seconds, gap, trace)
    assert " RX ERROR" not in trace, trace


def test_app_commands_run_beside_the_matter_thread(app_exe, module, plugin, tmp_path):
    # the app's thread sends its own commands while the Matter thread answers the commissioner
    out = _run(app_exe, module, plugin, tmp_path, "at.trace t.trace", "matter.start", *["at AT"] * 40,
               "matter.status", "at.trace.stop")
    assert out.returncode == 0, out.stdout
    trace = (tmp_path / "storage" / "t.trace").read_text()
    # the app's 40, and the node's own before it names the module for mDNS
    assert len(re.findall(r" TX AT$", trace, re.M)) >= 40 and " RX ERROR" not in trace, trace
    assert re.search(r" TX AT\+UBTGIS=0,\d+,656C04", trace), trace   # the handshake answer went out among them
    assert "timeout" not in out.stdout.lower() and "matter: running" in out.stdout, out.stdout


def test_an_idle_node_leaves_the_processor_and_the_module_alone(app_exe, module, plugin, tmp_path):
    psutil = pytest.importorskip("psutil")
    args = [str(app_exe), module.port]
    for command in ("matter.start", "gatt.listen 9"):
        args += ["--exec", command]
    app = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                           stdin=subprocess.DEVNULL, cwd=tmp_path,
                           env=dict(os.environ, UCX_MATTER_PLUGIN=str(plugin)))
    try:
        deadline = time.monotonic() + 30
        while "AT+UBTAL" not in module.log and time.monotonic() < deadline:
            time.sleep(0.05)
        time.sleep(3)   # the virtual host's handshake is answered, the node waits for a commissioner
        process = psutil.Process(app.pid)
        before, sent = process.cpu_times(), len(module.log)
        time.sleep(4)
        after, commands = process.cpu_times(), module.log[sent:]
    finally:
        out = app.communicate(timeout=60)[0]
    assert app.returncode == 0 and "matter: node started" in out, out
    used = (after.user + after.system) - (before.user + before.system)
    print(f"idle node: {used:.2f} s of processor in 4 s, AT commands: {commands}")
    # a node that polls would keep the app's own commands and the TUI waiting behind it
    assert used < 0.2, (used, out)
    assert len(commands) <= 2, commands


def test_a_node_on_wifi_is_announced_by_the_modules_own_mdns(app_exe, module, plugin, tmp_path):
    # the virtual module is on Wi-Fi already, as after wifi.connect
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status")
    assert out.returncode == 0, out.stdout
    assert "network: Wi-Fi up, 192.168.1.50, channel 6" in out.stdout, out.stdout
    sent = "\n".join(module.log)
    # the Matter stack's UDP socket is on the module, read when the module says it has data
    assert "AT+USOCR=17,1" in module.log and "AT+USOB=0,5540" in module.log, sent
    # commissionable over the network as well: the module answers the queries, the host has no multicast socket
    assert re.search(r'AT\+UMDSA="_matterc",1,5540,"[0-9A-F]{16}"', sent), sent
    discriminator = setup_codes(out.stdout)[0]
    assert f'AT+UMDTRA=0,"D","{discriminator}"' in sent and 'AT+UMDTRA=0,"VP","65521+32769"' in sent, sent
    # stopping: the node's service goes; the responder and its host name are again as the user had them
    assert sent.index("AT+UMDSR=0") > sent.index("AT+UMDSA="), sent
    assert sent.rindex('AT+UMDHN="vucx"') > sent.index('AT+UMDHN="C0FFEE001144"'), sent
    assert sent.rindex("AT+UMDE=1") > sent.rindex("AT+UMDE=0"), sent


def test_a_pairing_request_over_udp_reaches_the_matter_server_and_is_answered(app_exe, plugin, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    # a client on the network sends the request to the Matter port as soon as the node has bound its socket
    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {5540: [pairing_request()]}
    module = Vucx(profile)
    module.start()
    try:
        out = _run(app_exe, module, plugin, tmp_path, "app.log.events.on", "matter.start", "gatt.listen 3",
                   "matter.status")
        replies = [bytes(r) for r in (module.net.replies[0] if module.net.replies else [])]
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert any(c.startswith("AT+USORFB=0,") for c in module.log), module.log   # the datagram was read ...
    # ... and answered through the module: PBKDFParamResponse to the same node on the same exchange
    answer = (PAIRING_NODE, PBKDF_PARAM_RESPONSE, PAIRING_EXCHANGE)
    assert answer in [unsecured_message(r) for r in replies], (replies, out.stdout)
    # the data on the node's socket was the node's alone: the app's socket handler did not see it
    assert re.search(r"kept by the node: [2-9]", out.stdout), out.stdout
    assert "matter: pairing with the passcode" in out.stdout, out.stdout


def test_card_shows_the_node_its_codes_and_endpoints(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status", "gatt.listen 1",
               "matter.set light 40", "matter.card")
    assert out.returncode == 0, out.stdout
    discriminator, passcode = setup_codes(out.stdout)
    card = out.stdout[out.stdout.rindex("NODE  "):]
    assert re.match(r"NODE  NORA-\S+ over Wi-Fi, 3 endpoint\(s\)\nSTATE not commissioned\n", card), card
    # the virtual module's host has connected and the window is open; the module is on Wi-Fi
    assert "BLE   link up\nWIN   open\nNET   Wi-Fi up, 192.168.1.50, channel 6\n" in card, card
    assert re.search(rf"CODE  {manual_code(discriminator, passcode)}\d\nQR    MT:[0-9A-Z.\-]{{19}}\n", card), card
    assert "EP    1 light = on 40%  [on | off | 0..100 (percent)]\n" in card, card
    assert "EP    2 temp = 21.5 C  [-40..125 (degrees Celsius)]\n" in card, card
    assert "EP    3 contact = closed  [open | closed]\nLOG   info\n" in card, card
    assert "STEP" not in card, card   # no commissioning under way


def test_node_stops_and_starts_again_in_one_session(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.stop", "matter.status", "matter.start",
               "matter.status")
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("matter: node started (Matter SDK v") == 2, out.stdout
    assert "matter: stopped" in out.stdout and "matter: running" in out.stdout, out.stdout
    # the Matter thread is gone with the node: each stop took the advertisement down
    assert module.log.count("AT+UBTAL") == 2 and module.log.count("AT+UBTALD") >= 3, module.log


def test_endpoints_are_chosen_at_start_and_set_from_the_app(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start light light contact", "matter.set light2 40",
               "matter.set contact open", "matter.endpoints")
    assert out.returncode == 0, out.stdout
    assert re.search(r"1 +light +off +Dimmable Light", out.stdout), out.stdout
    assert re.search(r"2 +light2 +on 40% +Dimmable Light", out.stdout), out.stdout
    assert re.search(r"3 +contact +open +Contact Sensor", out.stdout), out.stdout
    assert "kinds: light temp contact" in out.stdout, out.stdout


def test_unknown_endpoint_does_not_start_or_advertise(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start fridge")
    assert out.returncode != 0, out.stdout
    assert "unknown endpoint fridge; this plug-in builds: light temp contact" in out.stdout, out.stdout
    assert "AT+UBTAL" not in module.log, module.log
