"""The app against vucx over TCP (tcp:HOST:PORT): the simulated module on any OS, and `ucxapp --virtual`."""

import os
import socket
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.app

ROOT = Path(__file__).resolve().parent.parent


@pytest.fixture
def b26(tmp_path, monkeypatch):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    monkeypatch.chdir(tmp_path)   # the app's storage/ stays out of the repo
    module = Vucx("nora-b26-ble.yaml")
    module.start_tcp()
    yield module
    module.stop()


def _run(exe, port, *commands, cwd=None, timeout=60):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd)


def test_b26_over_tcp_ranges_with_channel_sounding(app_exe, b26, tmp_path):
    out = _run(app_exe, b26.port, "dev.info", "bt.defaults secure", "bt.cs initiator", "bt.connect reflector",
               "bt.bond 0", "bt.cs.range 0 2", "conn.status", cwd=tmp_path)
    assert out.returncode == 0, out.stdout + out.stderr
    assert f"connected to {b26.port}" in out.stdout, out.stdout
    assert "NORA-B26" in out.stdout and "bonding succeeded" in out.stdout, out.stdout
    assert "5.0" in out.stdout and "mtu 247" in out.stdout, out.stdout


def test_tcp_module_can_be_reopened(app_exe, b26, tmp_path):
    for _ in range(2):   # the simulator takes a new client after the last one left
        out = _run(app_exe, b26.port, "dev.info", cwd=tmp_path)
        assert "NORA-B26" in out.stdout, out.stdout


@pytest.mark.parametrize("port", ["tcp:127.0.0.1:{free}", "tcp:nohost.invalid:1", "tcp:127.0.0.1", "tcp::5"])
def test_tcp_port_that_does_not_answer_fails_cleanly(app_exe, tmp_path, port):
    with socket.socket() as s:   # a port nobody listens on
        s.bind(("127.0.0.1", 0))
        free = s.getsockname()[1]
    out = _run(app_exe, port.format(free=free), "dev.info", cwd=tmp_path, timeout=30)
    assert out.returncode != 0, out.stdout
    assert "ERROR: cannot open tcp:" in out.stdout, out.stdout


def test_launcher_virtual_runs_the_app_on_the_simulator(app_exe, tmp_path):
    pytest.importorskip("yaml")
    env = {**os.environ, "PYTHONIOENCODING": "utf-8", "UCX_APP_EXE": str(app_exe)}
    out = subprocess.run([sys.executable, str(ROOT / "ucxapp.py"), "--virtual", "nora-b26", "--exec", "dev.info"],
                         capture_output=True, text=True, timeout=120, errors="replace", stdin=subprocess.DEVNULL,
                         cwd=ROOT, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "Simulated nora-b26 on tcp:127.0.0.1:" in out.stdout and "NORA-B26" in out.stdout, out.stdout


def test_launcher_virtual_names_match_the_browser_modules():
    sys.path.insert(0, str(ROOT))
    try:
        import ucxapp
    finally:
        sys.path.pop(0)
    sites = {p.stem[1:] for p in (ROOT / "app" / "wasm").glob("v*.json")}
    assert set(ucxapp.VIRTUAL) == sites
    for profile in ucxapp.VIRTUAL.values():
        assert (ROOT / "tests" / "harness" / "vucx" / profile).exists(), profile
