"""usecase.* commands run offline and the web page has both views."""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
PAGE = (ROOT / "app" / "web" / "index.html").read_text(encoding="ascii")


def _exec(app_exe, line):
    return subprocess.run([str(app_exe), "COM255", "--exec", line], capture_output=True, text=True, timeout=30,
                          errors="replace", stdin=subprocess.DEVNULL)


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
    assert out.returncode == 0 and "\ntool      Connected tool" in out.stdout
    for uc in USE_CASES:
        assert re.search(rf"^{uc} +\S", out.stdout, re.M), uc
    ex = _exec(app_exe, "usecase.example")
    assert ex.returncode == 0
    for word in ("usecase.tool", "wifi.peap", "wifi.ap.enable", "net.iperf", "gatt.server.wps"):
        assert word in ex.stdout


USE_CASES = ("iiot", "medical", "site", "implement", "condition", "agv", "eol", "blegw", "edge", "mesh",
             "border", "building", "gateway",
             "swarm", "recovery", "handover", "livestock", "boat", "rtk", "mower", "farm", "tracker",
             "home", "provision", "hotel", "notify", "remote")
SECTIONS = {"Industry": ("tool", "iiot", "medical", "site", "implement", "condition", "agv", "eol", "blegw"),
            "Edge AI": ("edge",),
            "Thread": ("mesh", "border", "building", "gateway"),
            "Location": ("swarm", "recovery", "handover", "livestock", "boat", "rtk", "mower", "farm", "tracker"),
            "Everyday": ("home", "provision", "hotel", "notify", "remote")}


@pytest.mark.app
def test_use_case_list_has_sections(app_exe):
    out = _exec(app_exe, "usecase.list").stdout
    blocks = re.findall(r"^(\w[\w ]*):\n((?:\w+ +\S.*\n)+)", out, re.M)
    assert [b[0] for b in blocks] == list(SECTIONS), out
    for name, body in blocks:
        assert tuple(re.findall(r"^(\w+) ", body, re.M)) == SECTIONS[name], (name, body)
    assert set(USE_CASES) | {"tool"} == {uc for ids in SECTIONS.values() for uc in ids}


def test_page_groups_use_cases_by_section():
    assert "createElement('optgroup')" in PAGE and "UC[k].s||'Everyday'" in PAGE
    for name, ids in SECTIONS.items():
        for uc in ids:
            m = re.search(rf"\n {uc}:\{{t:'[^']*'(?:,s:'([^']*)')?", PAGE)
            assert m and (m.group(1) or "Everyday") == name, uc


@pytest.mark.app
@pytest.mark.parametrize("uc", USE_CASES)
def test_use_case_check_and_steps(app_exe, uc):
    out = _exec(app_exe, f"usecase.check {uc}")
    assert out.returncode == 0, out.stdout + out.stderr
    rows = re.findall(r"^\s*\[(ok|limit|check|gap)\]\s+(.+?)\s{2,}(.+)$", out.stdout, re.M)
    assert len(rows) >= 4, out.stdout
    # every command a row relies on is in the Windows build
    assert not [r for r in rows if "not in this build" in r[2]], rows
    assert "Now: no module connected" in out.stdout and f"examples uc-{uc}" in out.stdout
    steps = _exec(app_exe, f"examples uc-{uc}")
    assert steps.returncode == 0 and " 1. " in steps.stdout and f"examples uc-{uc} <number>" in steps.stdout
    # every step line is a command of this build
    names = {line.split()[1] for line in steps.stdout.splitlines() if line.startswith("    > ")}
    help_out = _exec(app_exe, "help").stdout
    assert not [n for n in names if not re.search(rf"\b{re.escape(n)}\b", help_out)], names


@pytest.mark.app
def test_use_case_check_tool_and_bad_names(app_exe):
    assert "900 MB/30 min" in _exec(app_exe, "usecase.check tool").stdout
    bad = _exec(app_exe, "usecase.check bogus")
    assert bad.returncode != 0 and "no use case bogus" in bad.stdout
    assert _exec(app_exe, "usecase.check").returncode != 0
    assert _exec(app_exe, "usecase.list extra").returncode != 0
    missing = _exec(app_exe, "examples uc-boat 99")
    assert "no example 99 for uc-boat (see examples uc-boat)" in missing.stdout


@pytest.mark.app
def test_use_case_steps_stop_at_the_first_error(app_exe):
    out = _exec(app_exe, "examples uc-home 2")
    assert "Running uc-home example 2" in out.stdout and "> mqtt.connect" in out.stdout
    assert out.returncode != 0 and "stopped:" in out.stdout and "> mqtt.publish" not in out.stdout


def test_page_has_toolbox_and_use_case_views():
    for marker in ('id="vw"', 'data-v="0"', 'data-v="1"', 'id="pu"', 'id="dg"', 'id="fit"', 'id="steps"', 'id="rc"',
                   'id="ucs"'):
        assert marker in PAGE, marker
    assert "/api/examples?g=" in PAGE and "'usecase.tool'" in PAGE and "'usecase.check'" in PAGE
    for uc in USE_CASES:
        assert f"\n {uc}:{{t:" in PAGE, uc


def test_page_does_not_remember_secrets():
    m = re.search(r"const SECRET=/([^/]+)/i", PAGE)
    assert m
    for field in ("pass", "password", "token", "key"):
        assert re.search(m.group(1), field, re.I), field
