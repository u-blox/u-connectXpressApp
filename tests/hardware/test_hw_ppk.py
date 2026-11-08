"""Nordic Power Profiler Kit II (UCX_PPK_PORT or ppk.detect) in ampere meter mode; the DUT power stays off."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.ppk


def _run(exe, *commands, timeout=60):
    args = [str(exe)]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL)


def test_ppk2_opens_and_reports_its_calibration(app_exe, ppk):
    out = _run(app_exe, f"ppk.open {ppk[0]}", "ppk.status", "ppk.close")
    assert out.returncode == 0, out.stdout
    assert "DUT power off" in out.stdout and "Range 4:" in out.stdout, out.stdout


def test_ppk2_measures_at_full_rate(app_exe, ppk):
    out = _run(app_exe, f"ppk.open {ppk[0]}", "ppk.vdd 3300", "ppk.measure 500", "ppk.close")
    assert out.returncode == 0, out.stdout
    samples = int(re.search(r"Samples:\s+(\d+)", out.stdout).group(1))
    assert samples >= 40000, out.stdout  # 100 kS/s over 0.5 s
    assert re.search(r"Average:\s+-?[\d.]+ [mun]?A", out.stdout), out.stdout
