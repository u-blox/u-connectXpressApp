"""ppk.* on the simulated Nordic Power Profiler Kit II (virt:ppk): 2 mA with 60 mA bursts on range 3."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.app


def _run(exe, *commands, timeout=60):
    args = [str(exe)]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL)


def test_detect_finds_the_virtual_ppk2(app_exe):
    out = _run(app_exe, "ppk.detect virt:ppk").stdout
    assert "virt:ppk (Nordic Power Profiler Kit II (virtual)): Power Profiler Kit II, HW 9173" in out, out
    assert "1 PPK2(s) found" in out, out


def test_open_starts_safe_and_reports_the_calibration(app_exe):
    out = _run(app_exe, "ppk.open virt:ppk", "ppk.status", "ppk.meta", "ppk.close").stdout
    assert "ampere meter, DUT power off" in out, out
    assert "VDD:         not set" in out and "DUT power:   off" in out, out
    assert "Range 4:     R 0.050 ohm" in out, out
    assert "IA: 56" in out and out.count("END") == 1, out
    assert "closed" in out, out


def test_commands_need_an_open_ppk2(app_exe):
    for command in ("ppk.status", "ppk.vdd 3300", "ppk.power on", "ppk.measure 100", "ppk.meta"):
        out = _run(app_exe, command)
        assert out.returncode != 0 and "no PPK2 open" in out.stdout, (command, out.stdout)


def test_vdd_out_of_range_is_refused(app_exe):
    for mv in ("799", "5001", "abc"):
        out = _run(app_exe, "ppk.open virt:ppk", f"ppk.vdd {mv}")
        assert out.returncode != 0 and ("ERROR" in out.stdout or "usage:" in out.stdout), (mv, out.stdout)


def test_source_power_needs_vdd_first(app_exe):
    out = _run(app_exe, "ppk.open virt:ppk", "ppk.mode source", "ppk.power on")
    assert out.returncode != 0 and "set the output voltage first" in out.stdout, out.stdout
    out = _run(app_exe, "ppk.open virt:ppk", "ppk.mode source", "ppk.vdd 3300", "ppk.power on", "ppk.status",
               "ppk.power off")
    assert out.returncode == 0, out.stdout
    assert "source meter" in out.stdout, out.stdout


def test_measure_needs_vdd(app_exe):
    out = _run(app_exe, "ppk.open virt:ppk", "ppk.measure 100")
    assert out.returncode != 0 and "ppk.vdd" in out.stdout, out.stdout


def test_measure_reports_current_and_writes_csv(app_exe, tmp_path):
    csv = tmp_path / "p.csv"
    out = _run(app_exe, "ppk.open virt:ppk", "ppk.vdd 3300", f"ppk.measure 200 {csv}", "ppk.close")
    text = out.stdout
    assert out.returncode == 0, text
    samples = int(re.search(r"Samples:\s+(\d+)", text).group(1))
    assert samples >= 15000, text
    avg = float(re.search(r"Average:\s+([\d.]+) mA", text).group(1))
    lo, hi = map(float, re.search(r"Min/max:\s+([\d.]+) mA / ([\d.]+) mA", text).groups())
    assert 2.0 <= avg <= 60.0, text
    assert lo == pytest.approx(2.0, abs=0.01) and hi == pytest.approx(60.0, abs=0.1), text
    assert re.search(r"Ranges:\s+0 0 \d+ \d+ 0 samples", text), text
    lines = csv.read_text().splitlines()
    assert lines[0] == "t_us,current_uA,range,logic"
    assert len(lines) == samples + 1
    t, ua, rng, logic = lines[1].split(",")
    assert int(rng) in (2, 3) and int(logic) in (0, 1) and float(ua) > 0
    chart = re.search(r"^Chart:\s+([\d.]+) s uA avg ([\d.]+)((?: [\d.]+,[\d.]+,[\d.]+)+)\s*$", text, re.MULTILINE)
    assert chart, text
    slots = [tuple(map(float, p.split(","))) for p in chart.group(3).split()]
    assert float(chart.group(2)) == pytest.approx(avg * 1000, rel=1e-3)
    assert 150 <= len(slots) <= 200, len(slots)
    assert all(lo <= mid <= hi for lo, mid, hi in slots)
    assert min(s[0] for s in slots) == pytest.approx(lo * 1000, abs=10)
    assert max(s[2] for s in slots) == pytest.approx(hi * 1000, abs=10)


def test_usage_errors(app_exe):
    for command in ("ppk.mode foo", "ppk.power maybe", "ppk.measure -5"):
        out = _run(app_exe, "ppk.open virt:ppk", "ppk.vdd 3300", command)
        assert out.returncode != 0, (command, out.stdout)
