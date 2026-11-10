"""ft.* file transfer client against the ft server behind the virtual module (TCP port 6970, see FtServer)."""

import subprocess

import pytest

pytestmark = pytest.mark.app

HOST = "192.168.1.10"


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    yield module
    module.stop()


def _run(exe, port, cwd, *commands, timeout=60):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd)


def test_ft_list_get_and_remove_over_tcp(app_exe, nora, tmp_path):
    out = _run(app_exe, nora.port, tmp_path, f"ft.list {HOST}", f"ft.get {HOST}:6970 notes.txt",
               f"ft.get {HOST} big.bin copy.bin 1455", f"ft.remove {HOST} notes.txt", f"ft.list {HOST}")
    assert out.returncode == 0, out.stdout
    assert "notes.txt" in out.stdout and "big.bin" in out.stdout, out.stdout
    assert (tmp_path / "notes.txt").read_bytes().startswith(b"notes.txt: a demo file"), out.stdout
    want = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(20000))
    assert (tmp_path / "copy.bin").read_bytes() == want, out.stdout
    assert "deleted notes.txt" in out.stdout, out.stdout


def test_ft_put_uploads_a_file_and_lists_it(app_exe, nora, tmp_path):
    (tmp_path / "up.bin").write_bytes(bytes(range(256)) * 20)
    out = _run(app_exe, nora.port, tmp_path, f"ft.put {HOST} up.bin", f"ft.put {HOST} up.bin again.bin 64 resume",
               f"ft.list {HOST}")
    assert out.returncode == 0, out.stdout
    assert "up.bin" in out.stdout and "again.bin" in out.stdout, out.stdout


def test_ft_errors_against_the_server(app_exe, nora, tmp_path):
    out = _run(app_exe, nora.port, tmp_path, f"ft.get {HOST} missing.txt", f"ft.remove {HOST} missing.txt",
               f"ft.put {HOST} nofile.bin", f"ft.get {HOST}:0 a.bin", "ft.get 0 ../x")
    assert "ERROR" in out.stdout, out.stdout
    assert not (tmp_path / "missing.txt").exists(), out.stdout


def test_ft_resume_continues_a_partial_download(app_exe, nora, tmp_path):
    want = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(20000))
    (tmp_path / "big.bin.part").write_bytes(want[:5000])
    out = _run(app_exe, nora.port, tmp_path, f"ft.get {HOST} big.bin big.bin 1000 resume")
    assert out.returncode == 0, out.stdout
    assert (tmp_path / "big.bin").read_bytes() == want, out.stdout


def test_ft_over_ble_against_the_peer(app_exe, nora, tmp_path):
    (tmp_path / "up.txt").write_text("over ble\n" * 50)
    out = _run(app_exe, nora.port, tmp_path, "bt.connect C0:FF:EE:00:11:22", "ft.list 0", "ft.get 0 notes.txt",
               "ft.get 0 big.bin big.bin 242", "ft.put 0 up.txt", "ft.remove 0 up.txt")
    assert out.returncode == 0, out.stdout
    assert "big.bin" in out.stdout and "deleted up.txt" in out.stdout, out.stdout
    want = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(20000))
    assert (tmp_path / "big.bin").read_bytes() == want, out.stdout


def test_ft_ble_server_defines_the_service(app_exe, nora, tmp_path):
    out = _run(app_exe, nora.port, tmp_path, "ft.on rw", "ft.status", "ft.off")
    assert out.returncode == 0, out.stdout
    assert "BLE file transfer server on (read/write)" in out.stdout, out.stdout
    assert "ft: BLE, read/write, CTRL handle" in out.stdout, out.stdout


def test_ft_tcp_server_answers_a_client(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    (tmp_path / "served.txt").write_bytes(b"hello\n")   # bytes: write_text would make it hello\r\n on Windows

    def frame(pkt):
        return len(pkt).to_bytes(2, "little") + b"\0" + pkt

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {7003: [frame(b"\x03") + frame(b"\x01\xf0\x00served.txt")]}
    module = Vucx(profile)
    try:
        out = _run(app_exe, module.start(), tmp_path, "ft.on tcp:7003 2")
    finally:
        module.stop()
    reply = bytes(module.net.replies[0]) if module.net.replies else b""
    assert b"served.txt" in reply and b"\x83\xff" in reply, (reply, out.stdout)
    assert b"hello\n" in reply, (reply, out.stdout)


def test_ft_tcp_server_on_status_off(app_exe, nora, tmp_path):
    (tmp_path / "served.txt").write_text("hello\n")
    out = _run(app_exe, nora.port, tmp_path, "ft.on rw tcp:8000", "ft.status", "ft.off", "ft.off", "ft.status",
               "ft.on tcp:8000 1")
    assert "TCP file transfer server on" in out.stdout, out.stdout
    assert "ft: TCP port 8000" in out.stdout, out.stdout
    assert "not running" in out.stdout, out.stdout
    assert "stopped after 1 s" in out.stdout, out.stdout
    out = _run(app_exe, nora.port, tmp_path, "ft.on tcp", "ft.on tcp")
    assert "already running" in out.stdout, out.stdout
