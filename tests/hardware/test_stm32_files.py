"""STM32 board: copy a file onto it with YMODEM (file.recv), read it back (file.send) and compare SHA-256.

Needs UCX_STM32_PORT (the ST-Link console port) and a board running the full (not lite) firmware.
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PORT = os.environ.get("UCX_STM32_PORT", "")

pytestmark = [pytest.mark.hardware,
              pytest.mark.skipif(not PORT, reason="set UCX_STM32_PORT to the STM32 console port")]


def _launcher(cwd, *args):
    return subprocess.run([sys.executable, str(ROOT / "ucxapp.py"), "--stm32", "--port", PORT, *args],
                          cwd=cwd, capture_output=True, text=True, errors="replace", timeout=300)


def test_stm32_put_get_sha256(tmp_path):
    data = os.urandom(20000) + b"tail"
    up, down = tmp_path / "up", tmp_path / "down"
    up.mkdir()
    down.mkdir()
    (up / "ymtest.bin").write_bytes(data)
    out = _launcher(up, "--put", "ymtest.bin")
    assert out.returncode == 0, out.stdout + out.stderr
    assert f"file.recv: ymtest.bin, {len(data)} bytes" in out.stdout
    out = _launcher(down, "--get", "ymtest.bin")
    assert out.returncode == 0, out.stdout + out.stderr
    assert hashlib.sha256((down / "ymtest.bin").read_bytes()).digest() == hashlib.sha256(data).digest()
