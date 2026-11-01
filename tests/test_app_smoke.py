"""Smoke tests that need the built exe but no hardware."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.app


def test_version_flag(app_exe):
    out = subprocess.run([str(app_exe), "--version"], capture_output=True, text=True, timeout=10, check=True)
    assert re.match(r"u-connectXpressApp 3\.4\.0\.\d{6} ", out.stdout)


def test_list_ports_exits_cleanly(app_exe):
    out = subprocess.run([str(app_exe), "--list-ports"], capture_output=True, text=True, timeout=30, errors="replace")
    assert out.returncode == 0


def test_shell_starts_without_device_and_quits(app_exe):
    out = subprocess.run([str(app_exe), "COM255", "--shell"], input="help\nquit\n", capture_output=True, text=True,
                         timeout=30, errors="replace")
    assert out.returncode == 0
    assert "Command shell" in out.stdout
    assert "hid.setup" in out.stdout


def test_web_mode_needs_a_module(app_exe):
    out = subprocess.run([str(app_exe), "--web", "--offline"], capture_output=True, text=True, timeout=30,
                         errors="replace", stdin=subprocess.DEVNULL)
    assert out.returncode == 1, out.stdout
    assert "ERROR: --web needs a module" in out.stdout
    assert "Command shell" not in out.stdout


def test_usage_lists_web_mode(app_exe):
    out = subprocess.run([str(app_exe), "--bogus"], capture_output=True, text=True, timeout=10, errors="replace")
    assert out.returncode == 2
    assert "[--web]" in out.stdout and "--web only serves the web UI" in out.stdout
