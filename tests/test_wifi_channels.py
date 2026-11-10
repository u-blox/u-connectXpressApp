"""wifi.channels: mixed scans use the channels of the last scan, every 10th scan is full, 'full' clears the list.

Runs against vucx (needs a pty: Linux/WSL)."""


import pytest
from test_vucx import _run
from test_wifi_locate import _module

pytestmark = pytest.mark.app

vucx_only = pytest.mark.app   # vucx runs on a pty (Linux/WSL) or TCP (Windows)


@vucx_only
def test_mixed_scans_use_last_channels_and_full_every_tenth(app_exe, tmp_path):
    module = _module()
    try:
        out = _run(app_exe, module.port, "wifi.regdomain fcc", "wifi.channels mixed",
                   *["wifi.scan"] * 11, cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert out.stdout.count("Scanning the 2 channel(s) of the last scan") == 9, out.stdout
    assert sum(c.startswith("AT+UWCL=") for c in module.log) == 9, module.log
    assert "AT+UWCL=[6,36]" in module.log, module.log
    assert sum(c.startswith("AT+UWCLR") for c in module.log) == 1, module.log


@vucx_only
def test_full_clears_the_channel_list(app_exe, tmp_path):
    module = _module()
    try:
        out = _run(app_exe, module.port, "wifi.regdomain fcc", "wifi.channels mixed", "wifi.scan", "wifi.scan",
                   "wifi.channels full", "wifi.scan", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "channels: full (channel list cleared)" in out.stdout, out.stdout
    assert sum(c.startswith("AT+UWCL=") for c in module.log) == 1, module.log
    assert sum(c.startswith("AT+UWCLR") for c in module.log) == 1, module.log
    assert out.stdout.count("Scanning (can take up to 40 s)...") == 2, out.stdout
