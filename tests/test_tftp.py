"""TFTP (tftp.*): offline checks, no module needed. The transfer itself is in tests/hardware/test_net.py."""

import subprocess

import pytest

pytestmark = pytest.mark.app


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_tftp_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "tftp.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "tftp selftest: names, options, request and OACK ok" in out.stdout


@pytest.mark.parametrize("command", ["tftp.on rw", "tftp.status", "tftp.off", "tftp.get 127.0.0.1 a.bin",
                                     "tftp.put 127.0.0.1 a.bin"])
def test_tftp_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert "not connected to a module" in out.stdout


def test_tftp_get_leaves_no_partial_file(app_exe, tmp_path):
    _run_in(app_exe, tmp_path, "tftp.get 127.0.0.1 a.bin")
    assert list(tmp_path.glob("*.part")) == []


def test_tftp_has_examples(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "tftp.ex")
    assert "tftp.on rw" in out.stdout, out.stdout
