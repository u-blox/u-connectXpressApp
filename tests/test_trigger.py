"""trigger.*: a command runs when a distance, a signal level, a network, a device, the button or an event says so.
Offline with the emulated board pins, and against vucx for what the module reports."""

import re
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.app

BOARD = (Path(__file__).resolve().parent.parent / "app" / "boards" / "h753.json").as_posix()   # button PC13, LED PB0
LED_ON, LED_OFF = "hw.led 0 on", "hw.led 0 off"
ROW = "(?:^|> )"   # a row of trigger.list: the first one follows the prompt of the piped shell


def _shell(exe, cwd, commands, port=None, timeout=60):
    args = [str(exe), port, "--shell"] if port else [str(exe), "--offline", "--shell"]
    return subprocess.run(args, input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=timeout,
                          errors="replace", cwd=cwd)


def _fired(out, name):
    """How often trigger.list says the trigger ran."""
    row = re.search(rf"{ROW}{name} .*fired (\d+)", out.stdout, re.M)
    assert row, out.stdout
    return int(row.group(1))


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    yield module
    module.stop()


def _first(table, entries):
    """Puts entries before the profile's own patterns, which are matched in order."""
    rest = {k: v for k, v in table.items() if k not in entries}
    table.clear()
    table.update(entries)
    table.update(rest)


def test_button_runs_the_command_on_each_press_only(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, [
        f"hw.profile.load {BOARD}", "trigger.add press button do hw.led on",
        "trigger.add release button released do hw.led off", "app.wait 200",
        "hw.gpio.write PC13 1", "app.wait 300", "hw.gpio.read PB0",       # pressed, and held
        "hw.gpio.write PC13 0", "app.wait 300", "hw.gpio.read PB0",
        "hw.gpio.write PC13 1", "app.wait 300", "trigger.list"])
    assert out.returncode == 0, out.stdout
    assert "trigger press added: button pressed -> hw.led on" in out.stdout, out.stdout
    assert out.stdout.count("[trigger] press: pressed -> hw.led on") == 2, out.stdout
    assert out.stdout.count("[trigger] release: released -> hw.led off") == 1, out.stdout   # not when it was added
    assert re.findall(r"^(?:ucxapp> )?pin 0 = (\d)", out.stdout, re.M) == ["1", "0"], out.stdout
    assert _fired(out, "press") == 2 and _fired(out, "release") == 1, out.stdout


def test_off_on_fire_and_remove(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, [
        f"hw.profile.load {BOARD}", "trigger.add press button do hw.led on", "trigger.off press",
        "hw.gpio.write PC13 1", "app.wait 200", "trigger.list",
        "trigger.fire press", "trigger.on all", "app.wait 200",           # switched on while pressed: no run
        "trigger.add press button released do hw.led off",                # the same name replaces the rule
        "trigger.remove press", "trigger.list", "trigger.remove all"])
    assert out.returncode == 0, out.stdout
    assert re.search(ROW + r"press +off +button pressed -> hw\.led on", out.stdout, re.M), out.stdout
    assert out.stdout.count("[trigger] press:") == 1, out.stdout
    assert "[trigger] press: fired by hand -> hw.led on" in out.stdout, out.stdout
    assert "trigger press replaced: button released -> hw.led off" in out.stdout, out.stdout
    assert "1 trigger(s) removed" in out.stdout and "0 trigger(s) removed" in out.stdout, out.stdout
    assert "0 trigger(s); add one with trigger.add" in out.stdout, out.stdout


def test_the_virtual_button_runs_a_trigger_without_a_profile(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, ["trigger.add press button do hw.led on", "app.wait 200", "hw.button press",
                                    "app.wait 300", "hw.button release", "hw.gpio.read 0"])
    assert "[trigger] press: pressed -> hw.led on" in out.stdout, out.stdout
    assert re.search(r"^(?:ucxapp> )?pin 0 = 1", out.stdout, re.M), out.stdout


def test_a_board_without_a_button_refuses_a_button_trigger(app_exe, tmp_path):
    (tmp_path / "bare.json").write_text('{"board":"bare","gpio":["PA0"]}', encoding="utf-8")
    out = _shell(app_exe, tmp_path, ["hw.profile.load bare.json", "trigger.add x button do hw.led on", "trigger.list"])
    assert "no user button on this board" in out.stdout and "0 trigger(s)" in out.stdout, out.stdout


def test_a_failing_command_is_reported_and_counted(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, [f"hw.profile.load {BOARD}", "trigger.add bad button do hw.gpio.read 99",
                                    "hw.gpio.write PC13 1", "app.wait 300", "trigger.list"])
    assert "[trigger] bad: hw.gpio.read 99 failed (" in out.stdout, out.stdout
    assert re.search(ROW + r"bad .*fired 1, 1 failed\)", out.stdout, re.M), out.stdout


@pytest.mark.parametrize("command, message", [
    ("trigger.add x button do nosuch.command", "unknown command 'nosuch.command' after do"),
    ("trigger.add x event boot do trigger.remove all", "a trigger cannot run trigger.remove"),
    pytest.param("trigger.add x event boot do app.wait " + "1" * 160, "too long", id="command-too-long"),
    ("trigger.fire nosuch", "no trigger 'nosuch'"),
    ("trigger.off nosuch", "no trigger 'nosuch'"),
    ("trigger.remove nosuch", "no trigger 'nosuch'"),
])
def test_errors(app_exe, tmp_path, command, message):
    out = _shell(app_exe, tmp_path, [command, "trigger.list"])
    assert message in out.stdout and "0 trigger(s)" in out.stdout, out.stdout


@pytest.mark.parametrize("command", [
    "trigger.add", "trigger.add x cs do app.wait 1", "trigger.add x cs below 0 do app.wait 1",
    "trigger.add x cs below two do app.wait 1", "trigger.add x cs near 2 do app.wait 1",
    "trigger.add x rssi below -200 do app.wait 1", "trigger.add x rssi below -70", "trigger.add x rssi below -70 do",
    "trigger.add x ssid do app.wait 1", "trigger.add x ssid net maybe do app.wait 1",
    "trigger.add x button every 5 do app.wait 1", "trigger.add x rssi below -70 every 0 do app.wait 1",
    "trigger.add x rssi below -70 every=0 do app.wait 1", "trigger.add x rssi below -70 every= do app.wait 1",
    "trigger.add x sun rises do app.wait 1", "trigger.add all event boot do app.wait 1",
    "trigger.add bad/name event boot do app.wait 1", "trigger.list now", "trigger.fire", "trigger.on", "trigger.remove",
])
def test_usage(app_exe, tmp_path, command):
    out = _shell(app_exe, tmp_path, [command])
    assert out.returncode != 0 and "usage: trigger." in out.stdout, out.stdout


def test_rules_are_listed_as_typed_and_wait_for_a_module(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, [
        "trigger.add far cs 1 above 2.5 do matter.set light on", "trigger.add weak rssi < -75 every 5 do hw.led 0 on",
        'trigger.add home ssid "My Net" above -70 do "hw.led 0 on"', "trigger.add tag bt C0:FF:EE:00:11:22 lost do "
        "hw.led 0 off", "trigger.add near bt.rssi > -50 every=10 do hw.led 0 on", "app.wait 200", "trigger.list"])
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("no module connected: it starts checking after dev.connect") == 5, out.stdout
    for row in ("far +on +cs 1 above 2.50 m -> matter.set light on", "weak +on +rssi below -75 dBm -> hw.led 0 on",
                "home +on +ssid My Net above -70 dBm -> hw.led 0 on",
                "tag +on +bt C0:FF:EE:00:11:22 lost -> hw.led 0 off", "near +on +bt.rssi above -50 dBm -> hw.led 0 on"):
        assert re.search(rf"{ROW}{row}  \(no reading yet, fired 0\)$", out.stdout, re.M), (row, out.stdout)
    assert "[trigger]" not in out.stdout, out.stdout   # nothing is lost or weak without a module


def test_at_most_eight(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, [f"trigger.add t{i} event boot do app.wait 1" for i in range(9)])
    assert out.stdout.count("added:") == 8 and "at most 8 triggers" in out.stdout, out.stdout


def test_distance_runs_once_when_it_gets_below_the_limit(app_exe, nora, tmp_path):
    nora.urcs["AT+UBTCSP=0,1"] = {"after": 0.3, "times": 1, "lines": [
        "+UEBTCSM:0,3000", "+UEBTCSM:1,500", "+UEBTCSM:0,1800", "+UEBTCSM:0,1700"]}
    out = _shell(app_exe, tmp_path, [
        f"trigger.add near cs 0 below 2 do {LED_ON}", f"trigger.add never cs 0 below 1.5 do {LED_OFF}",
        "bt.cs.range 0 2", "trigger.list"], port=nora.port)
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("[trigger] near: 1.80 m -> hw.led 0 on") == 1, out.stdout   # not for 1.70 m again
    assert "[trigger] never" not in out.stdout, out.stdout
    assert re.search(ROW + r"near .*\(1\.70 m, fired 1\)$", out.stdout, re.M), out.stdout
    assert re.search(ROW + r"never .*\(1\.70 m, fired 0\)$", out.stdout, re.M), out.stdout


def test_distance_of_any_connection_and_back_above(app_exe, nora, tmp_path):
    nora.urcs["AT+UBTCSP=0,1"] = {"after": 0.3, "times": 1, "lines": ["+UEBTCSM:1,900"]}
    nora.urcs["AT+UBTCSP=0,0"] = {"after": 0.3, "times": 1, "lines": ["+UEBTCSM:1,4200"]}
    out = _shell(app_exe, tmp_path, [
        f"trigger.add near cs below 2 do {LED_ON}", f"trigger.add far cs above 2.5 do {LED_OFF}",
        "bt.cs.range 0 1", "app.wait 1000", "trigger.list"], port=nora.port)
    assert out.stdout.index("[trigger] near: 0.90 m") < out.stdout.index("[trigger] far: 4.20 m"), out.stdout
    assert _fired(out, "near") == 1 and _fired(out, "far") == 1, out.stdout


def test_wifi_signal_below_a_level(app_exe, nora, tmp_path):
    gone = ["+UWSST:4,-32768", "OK"]   # not connected, for the checks 3 to 6 s after the start
    _first(nora.commands, {"AT+UWSST=4": [["+UWSST:4,-60", "OK"], ["+UWSST:4,-80", "OK"], ["+UWSST:4,-82", "OK"],
                                          gone, gone, gone]})
    out = _shell(app_exe, tmp_path, [f"trigger.add weak rssi below -75 every 1 do {LED_ON}", "app.wait 4500",
                                    "trigger.list"], port=nora.port)
    assert out.stdout.count("[trigger] weak: -80 dBm -> hw.led 0 on") == 1, out.stdout
    assert re.search(ROW + r"weak .*\(no reading, fired 1\)$", out.stdout, re.M), out.stdout   # -32768: not connected


def test_network_and_device_found_and_lost(app_exe, nora, tmp_path):
    out = _shell(app_exe, tmp_path, [
        f"trigger.add home ssid vucx-net do {LED_ON}", f"trigger.add strong ssid vucx-far above -70 do {LED_ON}",
        f"trigger.add away ssid nosuch-net lost do {LED_OFF}", f"trigger.add tag bt REFLECTOR do {LED_ON}",
        f"trigger.add peer bt C0:FF:EE:00:11:55 above -60 do {LED_ON}", f"trigger.add gone bt nosuch lost do {LED_OFF}",
        "app.wait 1500", "trigger.list"], port=nora.port)
    assert out.returncode == 0, out.stdout
    for line in ("home: found, -45 dBm -> hw.led 0 on", "away: not found -> hw.led 0 off",
                 "tag: found, -64 dBm -> hw.led 0 on", "peer: found, -58 dBm -> hw.led 0 on",
                 "gone: not found -> hw.led 0 off"):
        assert out.stdout.count(f"[trigger] {line}") == 1, (line, out.stdout)
    assert "[trigger] strong" not in out.stdout, out.stdout   # there, but at -80 dBm
    assert 'AT+UWSSC=0,"vucx-net"' in nora.log and "AT+UBTD=0,0,3000" in nora.log, nora.log


def test_scan_without_the_ssid_filter_when_the_module_refuses_it(app_exe, nora, tmp_path):
    _first(nora.commands, {"AT+UWSSC=*": ["ERROR"]})
    out = _shell(app_exe, tmp_path, [f"trigger.add home ssid vucx-net do {LED_ON}", "app.wait 1000"], port=nora.port)
    assert "[trigger] home: found, -45 dBm -> hw.led 0 on" in out.stdout, out.stdout
    assert "AT+UWSSC" in nora.log, nora.log


def test_event_line_runs_the_command_every_time(app_exe, nora, tmp_path):
    out = _shell(app_exe, tmp_path, [
        f'trigger.add up event "bt: connected" do {LED_ON}', f'trigger.add down event "bt: disconnected" do {LED_OFF}',
        "bt.disconnect 0", "app.wait 300", "bt.connect C0:FF:EE:00:11:22", "app.wait 300",   # it starts connected
        "bt.disconnect 0", "app.wait 300", "bt.connect C0:FF:EE:00:11:22", "app.wait 300", "trigger.list"],
        port=nora.port)
    assert out.stdout.count("[trigger] up: bt: connected, conn 0 -> hw.led 0 on") == 2, out.stdout
    assert out.stdout.count("[trigger] down: bt: disconnected, conn 0 -> hw.led 0 off") == 2, out.stdout
    assert re.search(ROW + r'up +on +event "bt: connected" -> hw\.led 0 on  \(fired 2\)$', out.stdout, re.M), out.stdout


def test_disconnecting_the_module_waits_for_a_running_check(app_exe, nora, tmp_path):
    nora.delays = {"AT+UBTD*": 1}
    out = _shell(app_exe, tmp_path, [f"trigger.add gone bt nosuch lost do {LED_OFF}", "app.wait 300", "dev.disconnect",
                                    "trigger.list"], port=nora.port)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "[trigger] gone: not found -> hw.led 0 off" in out.stdout, out.stdout


def test_examples_and_help(app_exe, tmp_path):
    out = _shell(app_exe, tmp_path, ["trigger.example", "help trigger"])
    assert out.returncode == 0, out.stdout
    assert "trigger.add near cs below 2 do matter.set light off" in out.stdout, out.stdout
    for name in ("trigger.add", "trigger.list", "trigger.remove", "trigger.on", "trigger.off", "trigger.fire"):
        assert name in out.stdout, (name, out.stdout)
