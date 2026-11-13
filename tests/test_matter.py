"""matter.*: the app's side of the Matter plug-in interface (app/src/ucxapp_matter.h), against the stand-in plug-in
(tests/matter_stub) and vucx, the virtual module. The real plug-in needs the Matter SDK and has its own tests."""

import os
import re
import subprocess
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.app


@pytest.fixture
def stub(app_exe):
    path = Path(app_exe).parent / ("ucxmatter_stub.dll" if os.name == "nt" else "ucxmatter_stub.so")
    assert path.exists(), f"{path} is missing: rebuild the app, the stand-in plug-in is built with it"
    return path


@pytest.fixture
def module():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    vucx = Vucx("wifi.yaml")
    vucx.start()
    yield vucx
    vucx.stop()


def _run(exe, module, plugin, cwd, *commands, env=None):
    args = [str(exe), module.port]
    for command in commands:
        args += ["--exec", command]
    environ = {k: v for k, v in os.environ.items() if k != "UCX_STUB_ABI"}
    environ.update(env or {}, UCX_MATTER_PLUGIN=str(plugin))
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd, env=environ)


def _other_library():
    """A library of the system: it loads, but it is no Matter plug-in."""
    if os.name == "nt":
        return Path(os.environ["SystemRoot"]) / "System32" / "version.dll"
    # the C library this process runs on; searching /usr/lib for one takes minutes under WSL
    with open("/proc/self/maps", encoding="utf-8") as maps:
        found = sorted({line.split()[-1] for line in maps if re.search(r"/libc[.-][^/]*$", line.strip())})
    assert found, "no C library in /proc/self/maps"
    return Path(found[0])


def test_start_without_the_plugin_says_how_to_build_it(app_exe, module, tmp_path):
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.start")
    assert out.returncode != 0, out.stdout
    assert "is not installed" in out.stdout and "ucxapp --matter --build" in out.stdout, out.stdout
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.status")
    assert out.returncode == 0, out.stdout
    assert "matter: stopped" in out.stdout and "(not installed)" in out.stdout, out.stdout


def test_start_sends_at_commands_through_the_apps_client(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.status")
    assert out.returncode == 0, out.stdout
    assert "matter: node started (stub)" in out.stdout, out.stdout
    # the model came back through cmdBeginF / cmdGetRspParamsF, the variadic calls of the generated API
    assert re.search(r"node: stub on NORA-\S+, started 1 time\(s\)", out.stdout), out.stdout
    assert "matter: running" in out.stdout and "radios: BLE, Wi-Fi" in out.stdout, out.stdout
    assert "AT" in module.log and "AT+GMM" in module.log, module.log
    assert "[matter] stub node stopped" in out.stdout, out.stdout   # leaving the app stops the node


def test_urcs_reach_the_plugin_and_still_the_app(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "wifi.connect vucx-net secret123", "matter.status",
               "wifi.status")
    assert out.returncode == 0, out.stdout
    assert re.search(r"URCs passed on: [1-9]", out.stdout), out.stdout
    assert re.search(r"stub URCs: [1-9]\d*, last \+UE\w+", out.stdout), out.stdout
    assert "192.168.1.50" in out.stdout, out.stdout   # the stub cut its copy at the ':', the app's line was whole


def test_a_urc_the_node_keeps_does_not_reach_the_app(app_exe, module, stub, tmp_path):
    commands = ("app.log.events.on", "matter.start", "wifi.connect vucx-net secret123", "wifi.disconnect",
                "matter.status")
    out = _run(app_exe, module, stub, tmp_path, *commands)
    assert "wifi: link down (0)" in out.stdout and "kept by the node: 0" in out.stdout, out.stdout
    # as the node does for writes to its own GATT characteristics: the app's handlers do not see them
    out = _run(app_exe, module, stub, tmp_path, *commands, env={"UCX_STUB_KEEP": "+UEWLD"})
    assert "wifi: link down" not in out.stdout and "kept by the node: 1" in out.stdout, out.stdout
    assert "last +UEWLD" in out.stdout, out.stdout


def test_default_and_chosen_endpoints(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.endpoints")
    assert re.search(r"1  light  off\n2  temp  21.5\n3  contact  closed\nkinds: light temp contact", out.stdout), \
        out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.start contact light", "matter.endpoints")
    assert "1  contact  closed\n2  light  off\nkinds:" in out.stdout, out.stdout
    # all (the web form's default) is the default set, not an endpoint kind
    out = _run(app_exe, module, stub, tmp_path, "matter.start all", "matter.endpoints")
    assert re.search(r"1  light  off\n2  temp  21.5\n3  contact  closed\nkinds:", out.stdout), out.stdout


def test_unknown_endpoint_does_not_start(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start toaster")
    assert out.returncode != 0, out.stdout
    assert "unknown endpoint toaster" in out.stdout and "did not start (USAGE)" in out.stdout, out.stdout
    assert "node started" not in out.stdout and "stub node stopped" not in out.stdout, out.stdout


def test_set_changes_an_endpoint_and_tells_the_events(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "app.log.events.on", "matter.start", "matter.set light on",
               "matter.endpoints")
    assert out.returncode == 0, out.stdout
    assert "[event] matter: light = on" in out.stdout and "1  light  on" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.set oven on")
    assert out.returncode != 0 and "no endpoint oven" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.set light")
    assert out.returncode != 0 and "matter.set <endpoint> <value...>" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["matter.stop", "matter.endpoints", "matter.set light on", "matter.qr",
                                     "matter.open"])
def test_commands_need_a_running_node(app_exe, module, stub, tmp_path, command):
    out = _run(app_exe, module, stub, tmp_path, command)
    assert out.returncode != 0, out.stdout
    assert "no Matter node is running" in out.stdout, out.stdout


def test_qr_code_and_manual_code(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.qr")
    assert out.returncode == 0, out.stdout
    assert "manual code: 0000-000-0000" in out.stdout, out.stdout
    drawn = out.stdout.split("node started (stub)\n")[1].split("manual code")[0].splitlines()
    assert len(drawn) >= 10 and all(row.startswith("  ") for row in drawn), out.stdout   # the code with its quiet zone


def test_command_the_plugin_does_not_have(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.open 60")
    assert out.returncode != 0, out.stdout
    assert "matter.open is not in this plug-in (stub)" in out.stdout, out.stdout


def test_second_start_is_refused_and_stop_ends_the_node(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.start")
    assert out.returncode != 0 and "already running" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.stop", "matter.status", "wifi.status")
    assert out.returncode == 0, out.stdout
    assert "matter: node stopped" in out.stdout and "matter: stopped" in out.stdout, out.stdout


def test_start_closes_a_udp_socket_an_earlier_session_left_bound(app_exe, stub, tmp_path):
    """A reloaded web page never runs matter.stop: its node's socket keeps 5540, and the next node cannot bind it."""
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")   # sockets with state, kept from one run of the app to the next
    module.start()
    try:
        out = _run(app_exe, module, stub, tmp_path, "sock.open udp", "sock.bind 0 5540")
        assert "socket 0 bound to port 5540" in out.stdout, out.stdout
        out = _run(app_exe, module, stub, tmp_path, "matter.start", "sock.list")
        assert out.returncode == 0, out.stdout
        assert "closed UDP socket 0, left bound on the module by an earlier session" in out.stdout, out.stdout
        assert "0 socket(s)" in out.stdout, out.stdout
        # a socket of this session is the user's: it stays
        out = _run(app_exe, module, stub, tmp_path, "sock.open udp", "sock.bind 5541", "matter.start", "sock.list")
        assert out.returncode == 0, out.stdout
        assert "closed UDP socket" not in out.stdout and "1 socket(s)" in out.stdout, out.stdout
    finally:
        module.stop()

def test_state_is_sealed_on_disk_and_survives_a_restart(app_exe, module, stub, tmp_path):
    _run(app_exe, module, stub, tmp_path, "matter.start")
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.status")
    assert "started 2 time(s)" in out.stdout, out.stdout
    files = list(tmp_path.rglob("matter.stub.key"))
    assert len(files) == 1, list(tmp_path.rglob("*"))
    assert files[0].read_bytes().startswith(b"UCXS"), files[0].read_bytes()[:16]


def test_reset_removes_what_the_node_stored_and_reboots_the_module(app_exe, stub, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")   # answers the factory reset (AT+USYFR, AT&W)
    module.start()
    try:
        _check_reset(app_exe, module, stub, tmp_path)
    finally:
        module.stop()


def test_reset_goes_on_when_the_module_restarts_while_leaving_wifi(app_exe, stub, tmp_path):
    """Seen on an EVK: the module restarted on the Wi-Fi status query, so the reset stopped with an error."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    try:
        _run(app_exe, module, stub, tmp_path, "matter.start")
        module.commands["AT+UWSNST=0"] = [""]   # no answer: it restarts instead
        module.urcs["AT+UWSNST=0"] = ["+STARTUP"]
        start = len(module.log)
        out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.reset")
        assert out.returncode == 0, out.stdout
        assert "left Wi-Fi network vucx-net (the module restarted)\nmatter: module rebooted\n" in out.stdout, out.stdout
        log = module.log[module.log.index("AT+UWSNST=0", start):]
        assert "ATE0" in log and "AT+UWSDC" not in log and "AT+CPWROFF" not in log, log
        assert "matter: node started (stub)" in out.stdout.split("module rebooted")[1], out.stdout
    finally:
        module.stop()


def _check_reset(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.set light on", "matter.reset", "matter.status",
               "matter.endpoints")
    assert out.returncode == 0 and "1 stored file(s) removed" in out.stdout, out.stdout
    # off the Wi-Fi network for real, then a reboot: no factory reset
    sent = "\n".join(module.log)
    assert "AT+UWSDC" in module.log and "AT+CPWROFF" in module.log[module.log.index("AT+UWSDC"):], sent
    assert "AT+USYFR" not in sent, sent
    assert "matter: left Wi-Fi network vucx-net\nmatter: module rebooted\n" in out.stdout, out.stdout
    # the node that ran starts again with the same endpoints, from an empty store
    after = out.stdout.split("matter: module rebooted")[1]
    assert "matter: node started (stub)" in after and "commission the node again\n" in after, out.stdout
    assert "started 1 time(s)" in after and "1  light  off" in after, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.start contact", "matter.reset all", "matter.endpoints")
    assert out.returncode == 0 and "with its new setup codes" in out.stdout, out.stdout
    assert "matter: module reset to factory settings and rebooted" in out.stdout, out.stdout
    assert "1  contact  closed\nkinds:" in out.stdout, out.stdout
    log = module.log[module.log.index("AT+USYFR"):]
    assert log.index("AT&W") < log.index("AT+CPWROFF"), log
    out = _run(app_exe, module, stub, tmp_path, "matter.reset", "matter.status")
    assert "removed\n" in out.stdout and "matter: stopped" in out.stdout, out.stdout
    assert "matter.start, then commission the node again" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.reset now")
    assert out.returncode != 0, out.stdout


def test_selftest_checks_the_plugin_module_store_and_node(app_exe, stub, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    nora = Vucx("nora-w36.yaml")
    nora.start()
    try:
        out = _run(app_exe, nora, stub, tmp_path, "matter.selftest", "matter.status")
        running = _run(app_exe, nora, stub, tmp_path, "matter.start", "matter.selftest", "matter.status")
        failing = _run(app_exe, nora, stub, tmp_path, "matter.selftest", env={"UCX_STUB_SELFTEST_FAIL": "1"})
    finally:
        nora.stop()
    assert out.returncode == 0 and "matter selftest: ok" in out.stdout, out.stdout
    for name in ("plug-in", "module", "store", "stub"):
        assert re.search(rf"^  {name} +ok ", out.stdout, re.M), (name, out.stdout)
    # a node started for the test only is stopped again; the store keeps no probe
    assert "matter: node started (stub)" in out.stdout and "matter: stopped" in out.stdout, out.stdout
    assert not list(tmp_path.rglob("matter.selftest.key")), list(tmp_path.rglob("*"))
    after = running.stdout.split("matter selftest: ok")[1]
    assert running.returncode == 0 and "matter: running" in after, running.stdout
    assert failing.returncode != 0 and "ERROR: matter selftest: 1 check(s) failed" in failing.stdout, failing.stdout
    # a module without the peripheral role: no node is started for it
    profile = load("nora-w36.yaml")
    profile["commands"]["AT+UBTM?"] = ["+UBTM:1", "OK"]
    central = Vucx(profile)
    central.start()
    try:
        out = _run(app_exe, central, stub, tmp_path, "matter.selftest")
    finally:
        central.stop()
    assert out.returncode != 0, out.stdout
    assert re.search(r"^  module +FAILED  Bluetooth LE mode 1, needs 2 or 3", out.stdout, re.M), out.stdout
    assert "node started" not in out.stdout, out.stdout


def test_log_level_filters_the_plugins_log(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.log", "matter.start")
    assert "matter log: info" in out.stdout and "[matter] stub node on" in out.stdout, out.stdout
    assert "stub detail line" not in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log detail", "matter.start", "matter.status")
    assert "[matter] stub detail line" in out.stdout and "log: detail" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log error", "matter.start")
    assert "[matter]" not in out.stdout.split("matter: node started")[0], out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log loud")
    assert out.returncode != 0 and "<info|detail|error=info>" in out.stdout, out.stdout
    # back from detail to info in the same session
    out = _run(app_exe, module, stub, tmp_path, "matter.log detail", "matter.log info", "matter.start")
    assert "matter: node started" in out.stdout and "stub detail line" not in out.stdout, out.stdout


def test_card_shows_the_node_at_a_glance(app_exe, module, stub, tmp_path):
    # stopped, with the plug-in installed: a card to start the node from
    out = _run(app_exe, module, stub, tmp_path, "matter.card")
    assert out.returncode == 0 and "NODE  stopped\nLOG   info\n" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log detail", "matter.start", "matter.set light on",
               "matter.card", "matter.stop", "matter.card")
    assert out.returncode == 0, out.stdout
    running, stopped = out.stdout.split("matter: node stopped")
    # the plug-in's lines, each endpoint with what matter.set takes, then the app's own
    assert re.search(r"NODE  stub on NORA-\S+, 3 endpoint\(s\)\nSTATE not commissioned\nSTEP  2/6 fabric installed\n"
                     r"TIME  5\.0 s so far \(steps at 1\.2 5\.0 s\)\nCODE  0000-000-0000\nQR    MT:STUB0000000000000\n"
                     r"EP    1 light = on  \[on \| off \| 0\.\.100 \(percent\)\]\n"
                     r"EP    2 temp = 21\.5  \[-40\.\.125 \(degrees Celsius\)\]\n"
                     r"EP    3 contact = closed  \[open \| closed\]\nLOG   detail\n", running), out.stdout
    assert "NODE  stopped\nLOG   detail\n" in stopped, out.stdout
    # every line is "TAG   value", which is what the web page draws
    card = running[running.index("NODE  "):running.index("LOG   detail") + 12]
    assert all(re.fullmatch(r"[A-Z]{2,5} +\S.*", line) for line in card.splitlines()), card


def test_web_page_gets_the_card_and_every_change_of_it(app_exe, stub, tmp_path):
    """The card reaches a page as the "matter" event of the event stream: when the page connects, and again when
    an endpoint changes or the node stops."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    token = "0123456789abcdef0123456789abcdef"
    profile = load("nora-w36.yaml")
    # a page: it connects to the event stream as soon as the web server listens
    profile["sockets"]["requests"] = {8080: [f"GET /events HTTP/1.1\r\nHost: m\r\nX-Token: {token}\r\n\r\n"]}
    module = Vucx(profile)
    proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path,
                            env=dict(os.environ, UCX_MATTER_PLUGIN=str(stub)))

    def stream_with(text):
        end = time.monotonic() + 20
        seen = ""
        while time.monotonic() < end and text not in seen:
            time.sleep(0.05)
            seen = bytes(module.net.replies[0]).decode("latin-1") if module.net.replies else ""
        return seen

    try:
        for command, expected in (
                (f"matter.start\nweb.start 8080 {token} http\n",
                 'event: matter\ndata: "NODE  stub on NORA-W36, 3 endpoint(s)\\nSTATE not commissioned\\n'),
                ("matter.set light on\n", "EP    1 light = on  [on | off | 0..100 (percent)]\\n"),
                ("matter.log detail\n", '\\nLOG   detail"'),
                ("matter.stop\n", 'event: matter\ndata: "NODE  stopped\\nLOG   detail"')):
            proc.stdin.write(command)
            proc.stdin.flush()
            seen = stream_with(expected)
            assert expected in seen, (command, seen[-1500:])
        proc.stdin.write("web.stop\n")
        proc.stdin.close()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()


def _shell(app_exe, module, stub, cwd):
    return subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=cwd,
                            env=dict(os.environ, UCX_MATTER_PLUGIN=str(stub)))


def _until(check, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end and not check():
        time.sleep(0.05)
    return check()


def test_bridge_sends_the_node_and_takes_commands_over_tcp(app_exe, stub, tmp_path):
    """matter.bridge tcps: a client gets every change of the card and the node's events as lines, and sets and gets
    the endpoints with lines of its own."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    # a client: it connects as soon as the bridge listens, and sends its lines ending in \n, \r\n, \r, and the last
    # one without an ending, as phone terminals send it
    profile["sockets"]["requests"] = {7540: ["get\r\nset light on\rset oven on\nset light\r\nhello\nset temp 25"]}
    module = Vucx(profile)
    proc = _shell(app_exe, module, stub, tmp_path)

    def got():
        return bytes(module.net.replies[0]).decode("latin-1") if module.net.replies else ""

    try:
        proc.stdin.write("matter.start\nmatter.bridge tcps\n")
        proc.stdin.flush()
        assert _until(lambda: "ERR unknown command 'hello'" in got() and "EP light = on\n" in got()), got()
        text = got()
        assert ("NODE stub on NORA-W36, 3 endpoint(s)\nSTATE not commissioned\nSTEP 2/6 fabric installed\n"
                "TIME 5.0 s so far (steps at 1.2 5.0 s)\nEP light = off\nEP temp = 21.5\nEP contact = closed\nOK\n"
                ) in text, text
        # the setup codes are on the card, for who holds the node: a bridge's far side never gets them
        assert "CODE" not in text and "QR" not in text and "MT:" not in text, text
        assert "ERR no endpoint oven\n" in text and "ERR usage: set <endpoint> <value...>\n" in text, text
        assert "ERR unknown command 'hello' (set <endpoint> <value...> | get)\n" in text, text
        assert "LOG" not in text, text   # the app's own line
        assert text.count("ERR ") == 3, text   # the empty line between \r and \n is no command
        assert _until(lambda: "EP temp = 25\n" in got()), got()
        # a change made in the app reaches the client as well, and so do the node's events
        proc.stdin.write("matter.set temp 30\n")
        proc.stdin.flush()
        assert _until(lambda: "EP temp = 30\n" in got()), got()
        proc.stdin.write("matter.stop\nmatter.start\n")
        proc.stdin.flush()
        assert _until(lambda: "EVT matter: stub node started\n" in got()), got()
        proc.stdin.write("matter.bridge status\nmatter.bridge off\nmatter.bridge status\n")
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert "7540" in out and "bridge: off" in out, out


def test_bridge_mqtt_publishes_the_endpoints_and_takes_sets(app_exe, stub, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    # one message on <prefix>/light/set, then none
    profile["commands"]["AT+UMQRB=*"] = [["+UMQRB:0,\"ucxapp/matter/light/set\"\x01on", "OK"], ["ERROR"]]
    module = Vucx(profile)
    proc = _shell(app_exe, module, stub, tmp_path)

    def published(topic, data):
        return any(f'"{topic}"' in cmd and bytes(payload) == data for cmd, payload in list(module.payloads))

    try:
        proc.stdin.write("matter.start\nmatter.bridge mqtt\nmqtt.connect broker.local\nmatter.bridge mqtt\n")
        proc.stdin.flush()
        assert _until(lambda: published("ucxapp/matter/light", b"on")), (module.log, module.payloads)
        assert published("ucxapp/matter/light", b"off"), module.payloads   # the whole card first
        assert published("ucxapp/matter/temp", b"21.5"), module.payloads
        assert published("ucxapp/matter/node/state", b"not commissioned"), module.payloads
        assert published("ucxapp/matter/node/time", b"5.0 s so far (steps at 1.2 5.0 s)"), module.payloads
        # the setup codes never go to a broker; what an older version left there retained is removed
        assert not any(b"MT:" in data or b"0000-000-0000" in data for _, data in module.payloads), module.payloads
        assert any(cmd.startswith('AT+UMQPB=0,0,1,"ucxapp/matter/node/code"') for cmd in module.log), module.log
        assert any(cmd.startswith('AT+UMQPB=0,0,1,"ucxapp/matter/node/qr"') for cmd in module.log), module.log
        assert any(cmd.startswith("AT+UMQS=") and "ucxapp/matter/+/set" in cmd for cmd in module.log), module.log
        proc.stdin.write("matter.endpoints\nmatter.bridge status\nmatter.bridge mqtt off\nmatter.bridge status\n"
                         "matter.bridge mqtt bad#\nmatter.bridge off 3\n")
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert "ERROR: not connected to an MQTT broker (mqtt.connect first)" in out, out
    assert "matter.bridge mqtt: endpoints on ucxapp/matter/" in out and "1  light  on" in out, out
    assert "mqtt: ucxapp/matter/#" in out and "matter.bridge mqtt stopped" in out, out
    assert "bridge: off" in out and "without + # or a closing /" in out, out
    assert sum(cmd.startswith("AT+UMQS=") for cmd in module.log) == 2, module.log   # subscribe, unsubscribe


def test_demo_skips_matter_without_the_plugin(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "usecase.demo matter")
    assert out.returncode != 0, out.stdout
    assert "=== matter: skipped, the Matter plug-in is not installed (ucxapp --matter --build) ===" in out.stdout, \
        out.stdout
    assert "Running uc-matter" not in out.stdout and "no use case" not in out.stdout, out.stdout
    # with a plug-in the steps run: the stand-in has no matter.open, so the second window is where it stops
    out = _run(app_exe, module, stub, tmp_path, "usecase.demo matter")
    assert "demo: 1 use case, 3 of 4 examples ran to the end\nstopped early: matter/3" in out.stdout, out.stdout
    assert "manual code: 0000-000-0000" in out.stdout and "STATE not commissioned" in out.stdout, out.stdout


def test_card_without_the_plugin(app_exe, module, tmp_path):
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.card")
    assert out.returncode != 0 and "the plug-in is not installed" in out.stdout, out.stdout
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.card now")
    assert out.returncode != 0 and "NODE" not in out.stdout, out.stdout


def test_plugin_for_another_app_version_is_refused(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", env={"UCX_STUB_ABI": "99"})
    assert out.returncode != 0, out.stdout
    assert "another version of the app (interface 99" in out.stdout, out.stdout


def test_sbom_lists_the_components_of_an_installed_plugin(app_exe, module, stub, tmp_path):
    # asked of the plug-in itself, running or not; in the form of the app's own components
    block = "Matter stub 1.2.3  [MIT]\n  stand-in for the Matter SDK in the offline tests\n  https://github.com/"
    out = _run(app_exe, module, stub, tmp_path, "app.sbom", "matter.start", "app.sbom", "matter.status")
    assert out.returncode == 0 and out.stdout.count(block) == 2, out.stdout
    assert "matter: running" in out.stdout, out.stdout   # asking did not unload the running node
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "app.sbom")
    assert out.returncode == 0 and "u-connectClient" in out.stdout and "Matter" not in out.stdout, out.stdout


def test_files_that_are_no_plugin_are_refused(app_exe, module, tmp_path):
    text = tmp_path / "notes.dll"
    text.write_text("not a library")
    out = _run(app_exe, module, text, tmp_path, "matter.start")
    assert out.returncode != 0 and "does not load" in out.stdout, out.stdout
    out = _run(app_exe, module, _other_library(), tmp_path, "matter.start")
    assert out.returncode != 0 and "is not a Matter plug-in" in out.stdout, out.stdout


def test_a_module_restart_under_the_node_is_told_until_the_node_starts_again(app_exe, stub, tmp_path):
    """The module restarting (a brownout, dev.reboot) takes the node's GATT service, sockets and network with it;
    the node itself does not notice. The app says so once, on the card and in matter.status."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    try:
        out = _run(app_exe, module, stub, tmp_path, "app.log.events.on", "matter.start", "matter.card", "dev.reboot",
                   "matter.status", "matter.card", "matter.stop", "matter.start", "matter.status", "matter.card")
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    before, after = out.stdout.split("matter: node stopped")
    told = "the module restarted: the node lost its Bluetooth LE service, sockets and network; matter.stop, then " \
           "matter.start brings it back"
    assert before.count(f"[matter] ERROR: {told}") == 1, out.stdout
    assert f"\nERROR: {told}\n" in before, out.stdout   # matter.status
    assert "[event] matter: the module restarted, the node must be started again" in before, out.stdout
    first, second = before.split("[matter] ERROR:")
    assert "ALERT" not in first, out.stdout
    assert "ALERT the module restarted: stop the node and start it again\nLOG   info\n" in second, out.stdout
    assert "matter: node started (stub)" in after and "restarted" not in after and "ALERT" not in after, out.stdout


def test_a_second_start_is_refused_on_a_module_that_does_not_reboot(app_exe, stub, tmp_path):
    """The module keeps the Matter GATT service of the first start until it reboots: with a second one beside it a
    commissioner would talk to the stale one, so no reboot means no start."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["commands"]["AT+CPWROFF"] = ["ERROR"]
    module = Vucx(profile)
    module.start()
    try:
        out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.stop", "matter.start")
    finally:
        module.stop()
    assert out.returncode != 0, out.stdout
    assert out.stdout.count("matter: node started (stub)") == 1, out.stdout
    assert "matter: rebooting the module, its GATT server has the Matter service of the last start" in out.stdout
    assert "ERROR: the node is not started on a module that did not reboot; dev.reboot, then matter.start" in out.stdout


def test_mqtt_bridge_ends_when_the_module_is_disconnected(app_exe, stub, tmp_path):
    """The bridge polls every 100 ms on its own thread and publishes through the module: after dev.disconnect there
    is no module to publish through."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    proc = _shell(app_exe, module, stub, tmp_path)
    try:
        proc.stdin.write("matter.start\nmqtt.connect broker.local\nmatter.bridge mqtt\n")
        proc.stdin.flush()
        assert _until(lambda: any('"ucxapp/matter/light"' in cmd for cmd in module.log)), module.log
        proc.stdin.write("dev.disconnect\n")
        proc.stdin.flush()
        time.sleep(0.6)   # several polls of the bridge without a module
        proc.stdin.write("matter.bridge status\nmatter.status\napp.version\n")
        proc.stdin.close()
        out = proc.stdout.read()
        code = proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert code == 0, out
    after = out[out.index("matter.bridge mqtt stopped"):]
    assert "bridge: off" in after and "matter: stopped" in after and "u-connectXpressApp" in after, out
    assert "[matter] stub node stopped" in out, out


def test_a_command_does_not_end_the_at_command_of_another_thread(app_exe, stub, tmp_path):
    """After each command the shell closes an AT command that was left open. One that a bridge's thread (or the
    Matter node's) is waiting on, a scan or a write that takes seconds, is not left open: it is in use."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {7540: ["get\n"]}
    profile["delays"] = {"AT+USOWB=*": 2.6}   # the module is slow to take the bridge's answer
    module = Vucx(profile)
    proc = _shell(app_exe, module, stub, tmp_path)

    def got():
        return bytes(module.net.replies[0]).decode("latin-1") if module.net.replies else ""

    try:
        proc.stdin.write("matter.start\nmatter.bridge tcps\n")
        proc.stdin.flush()
        assert _until(lambda: any(cmd.startswith("AT+USOWB=") for cmd in module.log)), module.log
        proc.stdin.write("app.version\n")   # returns at once, while the bridge's write is on its way
        proc.stdin.flush()
        assert _until(lambda: "EP light = off\n" in got()), (got(), module.log[-10:])
        proc.stdin.write("matter.bridge off\n")
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert "left an AT command open" not in out, out
    assert "u-connectXpressApp" in out, out


def test_a_bridge_listening_beside_the_web_page_keeps_its_own_clients(app_exe, stub, tmp_path):
    """Every incoming connection is told to the web server too. One to a bridge's port is the bridge's: the web
    server reading it would take the client's lines away and answer them as HTTP."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    token = "0123456789abcdef0123456789abcdef"
    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8080: [f"GET /api/version HTTP/1.1\r\nHost: m\r\nX-Token: {token}\r\n\r\n"],
                                      7540: ["get\n"]}
    module = Vucx(profile)
    proc = _shell(app_exe, module, stub, tmp_path)

    def got(i):
        return bytes(module.net.replies[i]).decode("latin-1") if len(module.net.replies) > i else ""

    try:
        proc.stdin.write(f"matter.start\nweb.start 8080 {token} http\n")
        proc.stdin.flush()
        assert _until(lambda: '{"v":"u-connectXpressApp ' in got(0)), (got(0), module.log[-20:])
        proc.stdin.write("matter.bridge tcps\n")
        proc.stdin.flush()
        assert _until(lambda: "EP light = off\n" in got(1) and got(1).endswith("OK\n")), (got(1), module.log[-20:])
        assert "HTTP/1.1" not in got(1), got(1)
        proc.stdin.write("matter.bridge off\nweb.stop\n")
        proc.stdin.close()
        proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
