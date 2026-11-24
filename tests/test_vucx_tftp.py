"""tftp.* against the virtual module: the client against its TFTP server (port 69), and the app's own server fed
by scripted UDP clients (Network requests)."""

import re
import subprocess
import time

import pytest

pytestmark = pytest.mark.app

HOST = "192.168.1.10"


def _module(requests=None):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    if requests:
        profile["sockets"]["requests"] = requests
    module = Vucx(profile)
    module.start()
    return module


def _run(exe, port, cwd, *commands, timeout=60):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd)


def test_tftp_get_and_put_against_the_virtual_server(app_exe, tmp_path):
    module = _module()
    try:
        (tmp_path / "up.txt").write_text("upload me\n")
        out = _run(app_exe, module.port, tmp_path, f"tftp.get {HOST} boot.cfg", f"tftp.get {HOST} boot.cfg copy.cfg 69",
                   f"tftp.put {HOST} up.txt", f"tftp.put {HOST} up.txt remote.txt 69")
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert (tmp_path / "boot.cfg").read_bytes() == b"vucx demo file boot.cfg\n", out.stdout
    assert (tmp_path / "copy.cfg").exists(), out.stdout
    # durations for people: "123 ms", "1.2 s" or "2 min 5 s", with the rate
    assert re.search(r"^got boot\.cfg -> .*: 24 bytes in (\d+ ms|\d+\.\d s|\d+ min \d+ s) \(\d+ B/s\)$", out.stdout,
                     re.M), out.stdout
    assert any(c.startswith("AT+USOSTB=") for c in module.log), module.log


def test_tftp_client_errors(app_exe, tmp_path):
    module = _module()
    try:
        out = _run(app_exe, module.port, tmp_path, f"tftp.put {HOST} missing.txt")
        out2 = _run(app_exe, module.port, tmp_path, f"tftp.get {HOST} a.bin a.bin 0")
    finally:
        module.stop()
    assert out.returncode != 0 and out2.returncode != 0, out.stdout + out2.stdout


def _rrq(name):
    return b"\0\1" + name.encode() + b"\0octet\0"


def _serve(app_exe, tmp_path, datagrams, *commands, wait_s=10, on="tftp.on rw"):
    module = _module({69: datagrams})
    proc = None
    try:
        proc = subprocess.Popen([str(app_exe), module.port, "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path)
        proc.stdin.write(on + "\n")
        proc.stdin.flush()
        end = time.monotonic() + wait_s
        while time.monotonic() < end and not module.net.replies:
            time.sleep(0.05)
        replies = module.net.replies[0] if module.net.replies else []
        last = -1
        while time.monotonic() < end and len(replies) != last:   # until the server goes quiet
            last = len(replies)
            time.sleep(0.4)
        proc.stdin.write("\n".join(("tftp.status",) + commands + ("tftp.off", "tftp.status", "tftp.off")) + "\n")
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=20)
        return out, list(replies)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        module.stop()


def test_tftp_server_sends_a_file_to_a_client(app_exe, tmp_path):
    (tmp_path / "hello.txt").write_bytes(b"hello over tftp\n")   # bytes: write_text would add \r on Windows
    out, replies = _serve(app_exe, tmp_path, [_rrq("hello.txt"), b"\0\4\0\1"])
    assert "TFTP server" in out, out
    assert replies and replies[0] == b"\0\3\0\1hello over tftp\n", (replies, out)
    assert "1 file(s)" in out or "stopped" in out, out


@pytest.mark.parametrize("request_", [_rrq("missing.txt"), _rrq("../etc/passwd"), b"\x00\x09junk"],
                         ids=["missing", "traversal", "bad-opcode"])
def test_tftp_server_refuses_bad_requests(app_exe, tmp_path, request_):
    out, replies = _serve(app_exe, tmp_path, [request_])
    assert replies and replies[0][:2] == b"\0\5", (replies, out)


def test_tftp_server_takes_an_upload_with_options(app_exe, tmp_path):
    out, replies = _serve(app_exe, tmp_path, [b"\0\2up.txt\0octet\0blksize\0" + b"512\0tsize\0" + b"6\0",
                                              b"\0\3\0\1hello\n"])
    assert replies and replies[0][:2] in (b"\0\4", b"\0\6"), (replies, out)
    assert (tmp_path / "up.txt").read_bytes() == b"hello\n", (replies, out)


@pytest.mark.parametrize("request_, on, error, last", [
    (b"\0\1hello.txt\0mail\0", "tftp.on rw", b"Only octet mode", None),
    (b"\0\2up.txt\0octet\0", "tftp.on", b"Read only (tftp.on rw allows uploads)", "refused, read only"),
    (b"\0\2up.txt\0octet\0tsize\0" + b"999999999\0", "tftp.on rw", b"File too large", "too large"),
], ids=["mode", "read-only", "too-large"])
def test_tftp_server_refuses_what_it_cannot_take(app_exe, tmp_path, request_, on, error, last):
    out, replies = _serve(app_exe, tmp_path, [request_], on=on)
    assert replies and replies[0][:2] == b"\0\5" and error in replies[0], (replies, out)
    if last:
        assert f"last: put up.txt from 192.168.1.10: {last}" in out, out
    assert not (tmp_path / "up.txt").exists()


def test_tftp_server_upload_ends_on_a_peer_error(app_exe, tmp_path):
    out, replies = _serve(app_exe, tmp_path, [b"\0\2up.txt\0octet\0", b"\0\5\0\1disk on fire\0"])
    assert replies and replies[0] == b"\0\4\0\0", (replies, out)
    assert "last: put up.txt from 192.168.1.10: peer error 1: disk on fire" in out, out
    assert not (tmp_path / "up.txt").exists() and not (tmp_path / "up.txt.part").exists()


def test_tftp_on_checks_its_arguments_and_stops_after_the_time_limit(app_exe, tmp_path):
    module = _module()
    try:
        out = subprocess.run([str(app_exe), module.port, "--shell"], input="tftp.on rw 6969 1 2\ntftp.on x\n"
                             "tftp.on 6969 1\ntftp.on rw\ntftp.on\ntftp.off x\ntftp.status x\ntftp.off\ntftp.off\n",
                             capture_output=True, text=True, timeout=60, errors="replace", cwd=tmp_path).stdout
    finally:
        module.stop()
    assert out.count("usage: tftp.on") == 2 and "usage: tftp.off" in out and "usage: tftp.status" in out, out
    assert "TFTP server stopped after 1 s: 0 file(s), 0 bytes" in out, out
    assert "already running; tftp.off first" in out, out
    assert "TFTP server stopped\n" in out and out.rstrip().endswith("not running\nucxapp>"), out


def test_tftp_get_takes_the_option_ack_and_skips_strangers(app_exe, tmp_path):
    """A server that answers the RRQ with an OACK (tsize, blksize) gets ACK 0 and then sends the data; a datagram
    from another host gets an "Unknown transfer ID" error and a datagram too short for TFTP is passed over."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    def server(data):
        if data[:2] == b"\0\1":
            return [("192.168.1.99", 69, b"\0\3\0\1stranger"), (HOST, 69, b"\0"),
                    (HOST, 69, b"\0\6blksize\0" + b"512\0tsize\0" + b"5\0")]
        if data == b"\0\4\0\0":
            return [(HOST, 69, b"\0\3\0\1hello")]
        return []

    profile = load("nora-w36.yaml")
    profile["sockets"]["servers"] = {**(profile["sockets"].get("servers") or {}), 69: server}
    module = Vucx(profile)
    module.start()
    try:
        out = _run(app_exe, module.port, tmp_path, f"tftp.get {HOST} small.bin")
        sent = [(c, d) for c, d in module.payloads if c.startswith("AT+USOSTB=")]
    finally:
        module.stop()
    assert out.returncode == 0 and "size 5 bytes" in out.stdout, out.stdout
    assert (tmp_path / "small.bin").read_bytes() == b"hello", out.stdout
    assert any(c.startswith("AT+USOSTB=0,192.168.1.99,69") and d.startswith(b"\0\5\0\5Unknown transfer ID")
               for c, d in sent), sent
    assert any(d == b"\0\4\0\0" for c, d in sent) and any(d == b"\0\4\0\1" for c, d in sent), sent
