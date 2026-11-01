"""usecase.* commands run offline and the web page has both views."""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
PAGE = (ROOT / "app" / "web" / "index.html").read_text(encoding="ascii")


def _exec(app_exe, line):
    return subprocess.run([str(app_exe), "COM255", "--exec", line], capture_output=True, text=True, timeout=30,
                          errors="replace")


def _row(out, name):
    m = re.search(rf"^\s*\[(ok|limit|check|gap)\]\s+{re.escape(name)}\s{{2,}}(.+)$", out, re.M)
    assert m, f"no {name} row in:\n{out}"
    return m.group(1), m.group(2)


@pytest.mark.app
def test_fit_check_without_a_rate_shows_every_row(app_exe):
    out = _exec(app_exe, "usecase.tool")
    assert out.returncode == 0, out.stdout + out.stderr
    for name in ("Host", "Station + AP", "Enterprise", "Protocols", "HTTPS, 5 PCs", "8 trays", "BLE"):
        _row(out.stdout, name)
    verdict, text = _row(out.stdout, "900 MB/30 min")
    assert verdict == "gap" and "(48 / 120 min)" in text
    assert "Coexistence:" in out.stdout and "usecase.tool <Mbit/s>" in out.stdout


@pytest.mark.app
@pytest.mark.parametrize("rate,verdict,minutes", [("2.5", "gap", 48), ("4", "ok", 30), ("8.0", "ok", 15)])
def test_fit_check_with_a_measured_rate(app_exe, rate, verdict, minutes):
    out = _exec(app_exe, f"usecase.tool {rate}")
    assert out.returncode == 0, out.stdout + out.stderr
    got, text = _row(out.stdout, "900 MB/30 min")
    assert got == verdict and f"in {minutes} min" in text
    assert "usecase.tool <Mbit/s>" not in out.stdout


@pytest.mark.app
@pytest.mark.parametrize("rate", ["abc", "0", "1.25", "-3", "1 2"])
def test_fit_check_rejects_a_bad_rate(app_exe, rate):
    out = _exec(app_exe, f"usecase.tool {rate}")
    assert out.returncode != 0
    assert "Connected tool" not in out.stdout


@pytest.mark.app
def test_use_case_list_and_examples(app_exe):
    out = _exec(app_exe, "usecase.list")
    assert out.returncode == 0 and "\ntool  Connected tool" in out.stdout
    ex = _exec(app_exe, "usecase.ex")
    assert ex.returncode == 0
    for word in ("usecase.tool", "wifi.peap", "wifi.ap.enable", "net.iperf", "gatt.server.wps"):
        assert word in ex.stdout


def test_page_has_toolbox_and_use_case_views():
    for marker in ('id="vw"', 'data-v="0"', 'data-v="1"', 'id="pu"', 'id="dg"', 'id="fit"', 'id="steps"', 'id="rc"'):
        assert marker in PAGE, marker
    assert "/api/examples?g=usecase" in PAGE and "'usecase.tool'" in PAGE


def test_page_does_not_remember_secrets():
    m = re.search(r"const SECRET=/([^/]+)/i", PAGE)
    assert m
    for field in ("pass", "password", "token", "key"):
        assert re.search(m.group(1), field, re.I), field
