"""utils.qr.*: Wi-Fi payloads (open networks too) and the small/large rendering (no module needed)."""

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
pytestmark = pytest.mark.app


def _run(app_exe, command):
    out = subprocess.run([str(app_exe), "--offline", "--exec", command], capture_output=True, encoding="utf-8",
                         errors="replace", timeout=60, stdin=subprocess.DEVNULL, cwd=ROOT)
    return out.returncode, out.stdout


@pytest.mark.parametrize(
    "command, payload",
    [
        ("utils.qr.wifi Home", "WIFI:T:nopass;S:Home;;"),
        ("utils.qr.wifi Home nopass", "WIFI:T:nopass;S:Home;;"),
        ("utils.qr.wifi Home nopass hidden", "WIFI:T:nopass;S:Home;H:true;;"),
        ('utils.qr.wifi Home "" nopass hidden', "WIFI:T:nopass;S:Home;H:true;;"),
        ("utils.qr.wifi Home secret", "WIFI:T:WPA;S:Home;P:secret;;"),
        ("utils.qr.wifi Home secret WPA hidden", "WIFI:T:WPA;S:Home;P:secret;H:true;;"),
        ("utils.qr.wifi Home secret nopass", "WIFI:T:nopass;S:Home;;"),
        ("utils.qr.wifi a;b x:y", "WIFI:T:WPA;S:a\\;b;P:x\\:y;;"),
    ],
)
def test_wifi_payload(app_exe, command, payload):
    code, out = _run(app_exe, command)
    assert code == 0, out
    assert payload in out.splitlines()


def test_wifi_security_without_password_is_refused(app_exe):
    code, out = _run(app_exe, "utils.qr.wifi Home WPA")
    assert code != 0 and "WPA needs a password" in out


def test_small_is_default_and_half_the_size_of_large(app_exe):
    _, small = _run(app_exe, "utils.qr.text hello")
    _, large = _run(app_exe, "utils.qr.text hello large")
    small_rows = [line for line in small.splitlines() if line.strip()]
    large_rows = [line for line in large.splitlines() if line.strip()]
    assert "\u2580" in small or "\u2584" in small
    assert len(small_rows) * 2 >= len(large_rows) > len(small_rows)
    assert len(large_rows[0].rstrip()) > len(small_rows[0].rstrip())
