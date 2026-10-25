"""YMODEM file transfer: the app's own encoder/decoder (file.selftest) and the launcher's --put/--get side."""

import importlib.util
import queue
import subprocess
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def _launcher():
    spec = importlib.util.spec_from_file_location("launcher", ROOT / "ucxapp.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


@pytest.mark.app
def test_ymodem_selftest(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "file.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "ymodem selftest: crc16, 1K and 128-byte blocks, retry ok" in out.stdout


@pytest.mark.app
def test_ymodem_needs_a_serial_console(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "file.recv x.bin")
    assert out.returncode != 0
    assert "no raw serial access" in out.stdout


class _Pipe:
    """One end of an in-memory serial link (read with a timeout, write to the peer)."""

    def __init__(self, rx, tx, lose_first=False):
        self.rx, self.tx, self.lose_first = rx, tx, lose_first

    def read(self, n=1):
        out = b""
        try:
            while len(out) < n:
                out += bytes([self.rx.get(timeout=0.2)])
        except queue.Empty:
            pass
        return out

    def write(self, data):
        if self.lose_first and len(data) > 1000:
            self.lose_first = False
            data = data[:-1] + bytes([data[-1] ^ 1])   # bad CRC: the receiver must NAK and get it again
        for b in data:
            self.tx.put(b)


@pytest.mark.parametrize("size", [0, 1, 128, 129, 1024, 3000])
def test_launcher_ymodem_roundtrip(size):
    ym = _launcher()
    a, b = queue.Queue(), queue.Queue()
    sender, receiver = _Pipe(a, b, lose_first=True), _Pipe(b, a)
    data = bytes((i * 7 + 3) & 0xFF for i in range(size))
    result = {}
    t = threading.Thread(target=lambda: result.update(got=ym.ymodem_recv(receiver)))
    t.start()
    ym.ymodem_send(sender, "fw.bin", data)
    t.join(timeout=30)
    assert result["got"] == ("fw.bin", data)


def test_launcher_ymodem_packet_format():
    ym = _launcher()
    hdr = ym.ymodem_header("a.bin", 42)
    assert len(hdr) == 133 and hdr[:3] == b"\x01\x00\xff" and hdr[3:12] == b"a.bin\x0042\x00"
    pkt = ym.ymodem_packet(1, b"123456789", 1024)
    assert len(pkt) == 1029 and pkt[0] == 0x02 and pkt[12] == 0x1A
    assert ym.binascii.crc_hqx(b"123456789", 0) == 0x31C3
