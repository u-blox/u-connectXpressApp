"""Tab completion (app.complete uses the same code as Tab in the TUI), offline."""

import subprocess

import pytest

pytestmark = pytest.mark.app


def _complete(app_exe, tmp_path, prefix):
    out = subprocess.run([str(app_exe), "--offline", "--exec", f"app.complete {prefix}"], capture_output=True,
                         text=True, timeout=60, errors="replace", cwd=tmp_path, stdin=subprocess.DEVNULL)
    return out.stdout


@pytest.mark.parametrize("prefix, expected", [
    ("app.compl", "1 match: app.complete"),
    ("gatt.client.hr", "1 match: gatt.client.hrs"),
    ("gatt.server.servicech", "1 match: gatt.server.servicechanged"),
    ("gatt.server.wp", "1 match: gatt.server.wps"),
    ("utils.unz", "1 match: utils.unzip"),
])
def test_unique_prefix_completes(app_exe, tmp_path, prefix, expected):
    assert expected in _complete(app_exe, tmp_path, prefix)


def test_common_prefix(app_exe, tmp_path):
    out = _complete(app_exe, tmp_path, "gatt.cl")
    assert "matches: gatt.client." in out


def test_no_example_names_left(app_exe, tmp_path):
    assert "0 matches" in _complete(app_exe, tmp_path, "gatt.client.example")
    assert "0 matches" in _complete(app_exe, tmp_path, "gatt.server.example")


def test_unknown_prefix(app_exe, tmp_path):
    assert "0 matches: nosuch" in _complete(app_exe, tmp_path, "nosuch")


@pytest.mark.parametrize("line, expected", [
    ("WIFI.sca", "1 match: wifi.scan "),                       # any case
    ("hel", "1 match: help "),                                 # built in, not in a command table
    ("help wifi.sc", "1 match: help wifi.scan "),              # help completes command names
    ('"  wifi.sc"', "1 match: wifi.scan "),                    # leading spaces
    ('"gnss.bridge nt"', "1 match: gnss.bridge ntrip "),       # first keyword of a <a|b ...> group
    ('"gnss.bridge ntrip h 2101 M ble c"', "1 match: gnss.bridge ntrip h 2101 M ble cell "),   # optional flags
    ('"bt.pairing of"', "1 match: bt.pairing off "),           # <on|off>
    ('"sock.close a"', "1 match: sock.close all "),            # top-level "[handle] | all"
    ('"app.time sync n"', "1 match: app.time sync ntp "),      # nested [sync [ntp|http|module] ...]
    ('"gatt.subscribe 0 12 ind"', "1 match: gatt.subscribe 0 12 indicate "),   # after optional [conn]
    ('"wifi.ip d"', "1 match: wifi.ip dhcp "),
    ('"gnss.restart C"', "1 match: gnss.restart cold "),
    ('"bt.pairing o"', "2 matches: bt.pairing o\n"),
])
def test_line_completes(app_exe, tmp_path, line, expected):
    assert expected in _complete(app_exe, tmp_path, line)


@pytest.mark.parametrize("line", [
    '"wifi.connect "',            # [ssid|n]: values, not keywords
    '"gnss.bridge ntrip "',       # <host>
    '"sock.tls 0 ca "',           # [client key] are values
    '"net.ntp on x "',            # [args...]
    '"nosuch x"',
])
def test_values_are_not_offered(app_exe, tmp_path, line):
    assert "0 matches" in _complete(app_exe, tmp_path, line)


def test_second_line_lists_the_keywords(app_exe, tmp_path):
    out = _complete(app_exe, tmp_path, '"gnss.bridge "')
    listed = out.split("\n")[1].split()
    assert {"tcp", "tls", "tcpc", "ntrip", "udp", "udps", "ble", "uart", "cell",
            "pointperfect", "off", "status"} == set(listed)


def test_long_list_is_cut_with_dots(app_exe, tmp_path):
    out = _complete(app_exe, tmp_path, "g")
    assert "matches: g" in out and out.rstrip().endswith("...")
