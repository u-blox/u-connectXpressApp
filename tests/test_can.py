"""CAN (can.*): record format, emulated loopback controller and bridge argument checks; no module needed."""

import subprocess
import sys
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.app


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_can_selftest_record_format(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "can selftest: record format ok" in out.stdout


def test_can_loopback_send_recv_status(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.cfg 250000", "can.send 0x123 DE AD BE EF", "can.recv", "can.recv",
                  "can.status", "can.bridge status")
    assert out.returncode == 0, out.stdout
    assert "can bitrate 250000" in out.stdout
    assert "0x123 [4] DE AD BE EF" in out.stdout
    assert "no frame" in out.stdout
    assert "250000 bit/s, tx 1, rx 1, queued 0" in out.stdout
    assert "bridge: off" in out.stdout


def test_hw_has_no_can_commands(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "hw.exec can.send 0x1")
    assert "unknown command" in out.stdout


def test_can_cfg_loopback_mode(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.cfg 125000 loopback", "can.cfg 500000 normal")
    assert out.returncode == 0, out.stdout
    assert "can bitrate 125000, loopback" in out.stdout
    assert "can bitrate 500000\n" in out.stdout


def test_can_listen_only_refuses_send(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.cfg 500000 listen", "can.status", "can.send 0x123 01")
    assert out.returncode == 1, out.stdout
    assert "can bitrate 500000, listen-only" in out.stdout
    assert "listen-only, 500000 bit/s, tx 0" in out.stdout
    assert "CAN is listen-only" in out.stdout


def test_can_listen_then_normal_sends_again(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.cfg 500000 listen", "can.cfg 500000 normal", "can.send 0x42 AA",
                  "can.recv")
    assert out.returncode == 0, out.stdout
    assert "0x42 [1] AA" in out.stdout


def test_can_open_shows_and_selects_emulated(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.open", "can.open emulated", "can.status")
    assert out.returncode == 0, out.stdout
    assert "can on emulated\n" in out.stdout
    assert "can on emulated loopback" in out.stdout
    assert "can emulated loopback, normal" in out.stdout


def test_can_open_missing_interface_fails(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.open", "can.open nosuchcan9")
    assert out.returncode == 1, out.stdout
    assert ("no CAN interface nosuchcan9" in out.stdout or "no SocketCAN" in out.stdout
            or "SocketCAN interfaces need Linux" in out.stdout)
    assert "can on emulated" in out.stdout


_VCAN = sys.platform.startswith("linux") and Path("/sys/class/net/vcan0").exists()


@pytest.mark.skipif(not _VCAN, reason="needs Linux with vcan0 (sudo ip link add dev vcan0 type vcan; "
                    "sudo ip link set up vcan0)")
def test_socketcan_vcan_two_nodes(app_exe, tmp_path):
    watcher = subprocess.Popen([str(app_exe), "--offline", "--exec", "can.open vcan0", "--exec", "can.watch 4"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True,
                               cwd=tmp_path)
    try:
        time.sleep(1.5)
        sender = _run_in(app_exe, tmp_path, "can.open vcan0", "can.send 0x18DAF110 01 02 03", "can.send 0x7FF AA",
                         "can.recv", "can.cfg 500000 loopback", "can.send 0x55", "can.recv", "can.status")
        out, _ = watcher.communicate(timeout=30)
    finally:
        watcher.kill()
    assert sender.returncode == 0, sender.stdout
    assert "no frame" in sender.stdout          # normal mode: no echo of its own frames
    assert "0x55 [0]" in sender.stdout           # loopback: its own frame comes back
    assert "can vcan0 (SocketCAN), loopback" in sender.stdout
    assert "0x18DAF110 [3] 01 02 03" in out
    assert "0x7FF [1] AA" in out


def test_can_watch_prints_frames_with_time(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "can.send 0x7FF 01 02", "can.send 0x18DAF110", "can.watch 1", "can.watch 1")
    assert out.returncode == 0, out.stdout
    assert "watching CAN for 1 s" in out.stdout
    lines = [line for line in out.stdout.splitlines() if not line.startswith("sent") and
             ("0x7FF [2] 01 02" in line or "0x18DAF110 [0]" in line)]
    assert len(lines) == 2, out.stdout
    assert all(line.split()[0].count(".") == 1 for line in lines)
    assert "2 frame(s)" in out.stdout
    assert "0 frame(s)" in out.stdout


@pytest.mark.parametrize("command", ["can.send 0x20000000", "can.send 0x1 112233445566778899", "can.send 0x1 zz",
                                     "can.cfg 5", "can.cfg 500000 silent", "can.cfg 500000 normal x", "can.recv x",
                                     "can.watch 0", "can.watch 1 2", "can.bridge", "can.open a b"])
def test_can_usage(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert out.returncode == 1
    assert "usage" in out.stdout.lower()


@pytest.mark.parametrize("kind", ["gatt", "slcan"])
def test_can_bridge_ble_needs_a_module(app_exe, tmp_path, kind):
    out = _run_in(app_exe, tmp_path, f"can.bridge ble {kind}", "can.bridge status")
    assert out.returncode == 1, out.stdout
    assert "can bridge:" not in out.stdout, out.stdout
