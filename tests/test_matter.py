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


def test_state_is_sealed_on_disk_and_survives_a_restart(app_exe, module, stub, tmp_path):
    _run(app_exe, module, stub, tmp_path, "matter.start")
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.status")
    assert "started 2 time(s)" in out.stdout, out.stdout
    files = list(tmp_path.rglob("matter.stub.key"))
    assert len(files) == 1, list(tmp_path.rglob("*"))
    assert files[0].read_bytes().startswith(b"UCXS"), files[0].read_bytes()[:16]


def test_reset_removes_what_the_node_stored(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", "matter.reset")
    assert out.returncode == 0 and "1 stored file(s) removed" in out.stdout, out.stdout
    assert not list(tmp_path.rglob("matter.stub.key")), list(tmp_path.rglob("*"))
    out = _run(app_exe, module, stub, tmp_path, "matter.reset", "matter.start", "matter.status")
    assert "0 stored file(s) removed" in out.stdout and "started 1 time(s)" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.reset now")
    assert out.returncode != 0, out.stdout


def test_log_level_filters_the_plugins_log(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.log", "matter.start")
    assert "matter log: info" in out.stdout and "[matter] stub node on" in out.stdout, out.stdout
    assert "stub detail line" not in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log detail", "matter.start", "matter.status")
    assert "[matter] stub detail line" in out.stdout and "log: detail" in out.stdout, out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log error", "matter.start")
    assert "[matter]" not in out.stdout.split("matter: node started")[0], out.stdout
    out = _run(app_exe, module, stub, tmp_path, "matter.log loud")
    assert out.returncode != 0 and "[error|info|detail]" in out.stdout, out.stdout


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


def test_card_without_the_plugin(app_exe, module, tmp_path):
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.card")
    assert out.returncode != 0 and "the plug-in is not installed" in out.stdout, out.stdout
    out = _run(app_exe, module, tmp_path / "missing.dll", tmp_path, "matter.card now")
    assert out.returncode != 0 and "NODE" not in out.stdout, out.stdout


def test_plugin_for_another_app_version_is_refused(app_exe, module, stub, tmp_path):
    out = _run(app_exe, module, stub, tmp_path, "matter.start", env={"UCX_STUB_ABI": "99"})
    assert out.returncode != 0, out.stdout
    assert "another version of the app (interface 99" in out.stdout, out.stdout


def test_files_that_are_no_plugin_are_refused(app_exe, module, tmp_path):
    text = tmp_path / "notes.dll"
    text.write_text("not a library")
    out = _run(app_exe, module, text, tmp_path, "matter.start")
    assert out.returncode != 0 and "does not load" in out.stdout, out.stdout
    out = _run(app_exe, module, _other_library(), tmp_path, "matter.start")
    assert out.returncode != 0 and "is not a Matter plug-in" in out.stdout, out.stdout
