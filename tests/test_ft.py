"""BLE file transfer (ft.*) and the app clock (app.time): offline checks, no module needed."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.app


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_ft_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "ft.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ft selftest: CRC-32, names and request encoding ok" in out.stdout


@pytest.mark.parametrize("command", ["ft.on rw", "ft.on rw tcp", "ft.on tcp:7000 30", "ft.status", "ft.off",
                                     "ft.get 0 a.bin", "ft.put 0 a.bin", "ft.list 0", "ft.remove 0 a.bin",
                                     "ft.get 192.168.1.10 a.bin", "ft.list 192.168.1.10:6970",
                                     "ft.get 192.168.1.10 a.bin a.bin 1455 resume", "ft.put 0 a.bin a.bin 240 resume"])
def test_ft_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert "not connected to a module" in out.stdout


def test_ft_has_examples(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "ft.example")
    assert "ft.on rw" in out.stdout, out.stdout


def test_ancs_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "ancs.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ancs selftest: event, attribute reply and request encoding ok" in out.stdout


@pytest.mark.parametrize("command", ["ancs.on", "ancs.sub", "ancs.list", "ancs.show 1", "ancs.action 1 pos"])
def test_ancs_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert "not connected to a module" in out.stdout


def test_notify_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "notify.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "notify selftest: parsing ok" in out.stdout


@pytest.mark.parametrize("command", ["notify.on", "notify.action 17 dismiss"])
def test_notify_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert "not connected to a module" in out.stdout


def test_time_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.time selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "app.time selftest: format, parse, HTTP date, SNTP, geo and zone ok" in out.stdout


def test_time_shows_utc(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.time")
    assert re.search(r"time\s+\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", out.stdout), out.stdout
    assert "host clock" in out.stdout


def test_time_set_keeps_an_offset(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.time set 2030-01-02T03:04:05", "app.time")
    assert "time 2030-01-02T03:04:05Z from manual" in out.stdout, out.stdout
    assert re.search(r"time\s+2030-01-02T03:04:0\dZ", out.stdout), out.stdout
    assert "offset" in out.stdout


@pytest.mark.parametrize("command", ["app.time sync", "app.time sync ntp", "app.time push", "app.time geo"])
def test_time_sync_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert "not connected to a module" in out.stdout


def test_time_place_is_unknown_without_a_source(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.time", "app.time zone")
    assert re.search(r"place\s+unknown", out.stdout), out.stdout
    assert "zone none" in out.stdout
    assert "local " not in out.stdout


@pytest.mark.parametrize("zone, shown", [("+01:00", "UTC+01:00"), ("-5:30", "UTC-05:30"), ("+5:45", "UTC+05:45"),
                                         ("0", "UTC+00:00")])
def test_time_zone_by_hand(app_exe, tmp_path, zone, shown):
    out = _run_in(app_exe, tmp_path, f"app.time zone {zone}", "app.time", "app.time zone off", "app.time")
    assert out.returncode == 0, out.stdout + out.stderr
    assert f"zone    manual {shown}, fixed (no daylight saving)" in out.stdout, out.stdout
    assert re.search(r"local\s+\d{4}-\d\d-\d\d \d\d:\d\d:\d\d " + re.escape(shown) + " manual", out.stdout), out.stdout
    assert "zone off: the clock shows UTC only" in out.stdout
    assert out.stdout.count("local ") == 2, out.stdout


def test_time_zone_shifts_the_clock(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.time set 2030-01-02T03:04:05", "app.time zone -05:00", "app.time")
    assert re.search(r"local\s+2030-01-01 22:04:0\d UTC-05:00", out.stdout), out.stdout


@pytest.mark.parametrize("zone", ["2x", "+15", "+01:07", "+1:5", "-", "abc"])
def test_time_zone_rejects_bad_values(app_exe, tmp_path, zone):
    out = _run_in(app_exe, tmp_path, f"app.time zone {zone}")
    assert out.returncode != 0
    assert f"ERROR: zone {zone}: use +HH:MM" in out.stdout, out.stdout


def test_time_has_geo_examples(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "app.example")
    assert "app.time geo" in out.stdout and "app.time zone +01:00" in out.stdout, out.stdout
