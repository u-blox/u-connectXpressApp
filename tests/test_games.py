"""app.hunt: signal hunt over Wi-Fi scans. Errors run anywhere; the hunt runs against vucx (needs a pty: Linux/WSL)."""

import sys

import pytest
from test_gnss_replay import _run as _run_offline
from test_vucx import _run

pytestmark = pytest.mark.app


@pytest.mark.parametrize(("command", "message"), [
    ("app.hunt", "not connected"),
    ("app.hunt home 5", "not connected"),
    ("app.hunt 0", "usage: app.hunt"),
    ("app.hunt a b c", "usage: app.hunt"),
    ("app.hunt a b", "usage: app.hunt"),
])
def test_hunt_errors(app_exe, tmp_path, command, message):
    out = _run_offline(app_exe, tmp_path, command)
    assert out.returncode == 1 and message in out.stdout.lower(), out.stdout


vucx_only = pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")


def _module(scan):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    p = load("wifi.yaml")
    p["commands"]["AT+UWSSC*"] = scan + ["OK"]
    module = Vucx(p)
    module.start()
    return module


@vucx_only
def test_hunt_picks_the_strongest_and_wins_close_by(app_exe, tmp_path):
    module = _module(['+UWSSC:AABBCCDDEEF1,"far",36,-80,0,0,0', '+UWSSC:AABBCCDDEEF0,"near",6,-38,8,8,8'])
    try:
        out = _run(app_exe, module.port, "app.hunt", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert 'Signal hunt: walk the module to "near" (AA:BB:CC:DD:EE:F0, channel 6)' in out.stdout, out.stdout
    assert "Found it in 1 rounds: -38 dBm" in out.stdout, out.stdout


@vucx_only
def test_hunt_by_name_reports_the_best(app_exe, tmp_path):
    module = _module(['+UWSSC:AABBCCDDEEF1,"far",36,-80,0,0,0', '+UWSSC:AABBCCDDEEF0,"near",6,-38,8,8,8'])
    try:
        out = _run(app_exe, module.port, "app.hunt far 2", "app.hunt nowhere", cwd=tmp_path)
    finally:
        module.stop()
    assert "  1  far" in out.stdout and "start" in out.stdout, out.stdout
    assert "  2  far" in out.stdout and "same" in out.stdout, out.stdout
    assert "Best -80 dBm (start -80 dBm, +0 dB); -40 dBm wins" in out.stdout, out.stdout
    assert 'No access point named "nowhere" in range' in out.stdout, out.stdout
