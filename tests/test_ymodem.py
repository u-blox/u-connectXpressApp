"""YMODEM file transfer: the app's own encoder/decoder (file.selftest) and the launcher's --put/--get side."""

import importlib.util
import queue
import subprocess
import threading
import time
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
    assert "ymodem selftest: crc16, 1K and 128-byte blocks, retry, short file, header size ok" in out.stdout


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

class _Console:
    """Fake serial console: the app runs with --raw-console and its stdin/stdout are the line."""

    def __init__(self, app_exe, cwd, command):
        self.proc = subprocess.Popen([str(app_exe), "--offline", "--raw-console", "--exec", command], cwd=cwd,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.rx, self.seen, self.lose_first = queue.Queue(), bytearray(), False
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while b := self.proc.stdout.read1(4096):
            self.seen += b
            for x in b:
                self.rx.put(x)

    read = _Pipe.read

    def write(self, data):
        if self.lose_first and len(data) > 1000:
            self.lose_first = False
            data = data[:-1] + bytes([data[-1] ^ 1])
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def finish(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=60)
        time.sleep(0.2)
        return self.proc.returncode, self.seen.decode(errors="replace")


@pytest.mark.app
@pytest.mark.parametrize("size,forced", [(3000, None), (100, "other.bin")])
def test_file_recv_stores_an_upload_from_the_console(app_exe, tmp_path, size, forced):
    data = bytes((i * 5 + 1) & 0xFF for i in range(size))
    con = _Console(app_exe, tmp_path, "file.recv" + (f" {forced}" if forced else ""))
    con.lose_first = True
    _launcher().ymodem_send(con, "dir/up.bin", data)
    rc, out = con.finish()
    name = forced or "up.bin"
    assert rc == 0 and f"file.recv: {name}, {size} bytes" in out, out
    assert (tmp_path / "storage" / name).read_bytes() == data


@pytest.mark.app
def test_file_send_downloads_a_stored_file_to_the_console(app_exe, tmp_path):
    data = bytes((i * 3) & 0xFF for i in range(2500))
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "down.bin").write_bytes(data)
    con = _Console(app_exe, tmp_path, "file.send down.bin")
    got = _launcher().ymodem_recv(con)
    rc, out = con.finish()
    assert got == ("down.bin", data)
    assert rc == 0 and "file.send: down.bin, 2500 bytes" in out, out


@pytest.mark.app
@pytest.mark.parametrize("command,peer,expect", [
    ("file.recv", b"\x18\x18", "ERROR: file.recv cancelled"),
    ("file.recv", b"", "ERROR: file.recv timeout"),
    ("file.send up.bin", b"\x18\x18", "ERROR: file.send cancelled"),
])
def test_file_transfer_ends_cleanly_when_the_peer_cancels_or_leaves(app_exe, tmp_path, command, peer, expect):
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "up.bin").write_bytes(b"x" * 10)
    con = _Console(app_exe, tmp_path, command)
    time.sleep(0.5)
    if peer:
        con.write(peer)
    rc, out = con.finish()
    assert rc != 0 and expect in out, out


@pytest.mark.app
def test_file_recv_refuses_a_header_without_a_file_name(app_exe, tmp_path):
    ym = _launcher()
    con = _Console(app_exe, tmp_path, "file.recv")
    assert ym._ym_wait(con, ym.YM_C, 10) == ym.YM_C
    try:
        ym._ym_send_packet(con, ym.ymodem_header("..", 5))
    except RuntimeError:
        pass   # the app cancels the transfer
    rc, out = con.finish()
    assert rc != 0 and "ERROR: file.recv" in out, out
    assert not (tmp_path / "storage" / "..").is_file()


@pytest.mark.app
def test_file_commands_check_their_arguments(app_exe, tmp_path):
    out = "".join(_run(app_exe, tmp_path, c).stdout for c in ("file.recv a b", "file.recv dir/..", "file.send",
                                                              "file.send ..", "file.send missing.bin",
                                                              "file.selftest x"))
    assert out.count("usage: file.") == 5, out
    assert "ERROR: cannot open missing.bin" in out, out
