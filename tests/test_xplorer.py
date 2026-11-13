"""xplorer.* and perf ... xplorer against a fake u-connectXplorer remote control (JSON-RPC 2.0 over a WebSocket),
so the client, its JSON reading and the error paths run without a phone. tests/hardware drives the real app."""

import json
import socket
import subprocess
import threading

import pytest
from harness.xplorer import TOKEN, V2, FakePhone

pytestmark = pytest.mark.app


@pytest.fixture
def phone():
    fake = FakePhone()
    yield fake
    fake.close()


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def _connect(app_exe, tmp_path, phone):
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{phone.port} {TOKEN}")
    assert out.returncode == 0, out.stdout
    return out


def test_connect_shows_the_phone_and_saves_it(app_exe, tmp_path, phone):
    out = _connect(app_exe, tmp_path, phone)
    assert "u-connectXplorer 9.9.9+99 on ios 18.1 at 127.0.0.1" in out.stdout
    assert "phone Bluetooth: poweredOff" in out.stdout
    assert "saved" in out.stdout and "note:" not in out.stdout
    saved = (tmp_path / "storage" / "xplorer.cfg").read_text()
    assert "127.0.0.1" in saved and TOKEN not in saved, "the token must be stored sealed"
    out = _run(app_exe, tmp_path, "xplorer.info")
    assert out.returncode == 0 and "9.9.9+99" in out.stdout, out.stdout


def test_connect_reads_the_documented_version_field(app_exe, tmp_path, phone):
    """REMOTE_CONTROL.md documents only "version"; fullVersion is an extra some app builds send."""
    phone.handlers["app.version"] = lambda params: {"version": "2.1.0", "app": "u-connectXplorer"}
    out = _connect(app_exe, tmp_path, phone)
    assert "u-connectXplorer 2.1.0 on ios" in out.stdout, out.stdout


def test_connect_again_with_an_existing_key(app_exe, tmp_path, phone):
    """A second run loads ucxapp.key from disk instead of creating it; sealing must still work."""
    _connect(app_exe, tmp_path, phone)
    assert (tmp_path / "storage" / "ucxapp.key").exists()
    out = _connect(app_exe, tmp_path, phone)
    assert "cannot save" not in out.stdout and "saved" in out.stdout, out.stdout


def test_wrong_token(app_exe, tmp_path, phone):
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{phone.port} WRONG")
    assert out.returncode != 0, out.stdout
    assert "Invalid token" in out.stdout and "check the token" in out.stdout
    assert not (tmp_path / "storage" / "xplorer.cfg").exists()


def test_a_challenge_is_answered_so_the_token_is_never_sent(app_exe, tmp_path):
    """A phone with authChallenge gets the HMAC-SHA256 of its nonce under the token; the token itself does not
    cross the wire, on xplorer.connect or on any later command, and a wrong token is still refused."""
    fake = FakePhone(features=V2, protocol=2)
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert "saved" in out.stdout and "note:" not in out.stdout, out.stdout
        assert _run(app_exe, tmp_path, "xplorer.info").returncode == 0
        answers = fake.called("app.authenticate")
        assert len(answers) == 2 and all(set(params) == {"response"} for params in answers), answers
        assert len(fake.called("app.getChallenge")) == 2 and TOKEN not in json.dumps(fake.calls), fake.calls
        out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{fake.port} WRONG")
        assert out.returncode != 0 and "Invalid token" in out.stdout, out.stdout
        assert "check the token on the Remote Control screen" in out.stdout, out.stdout
    finally:
        fake.close()


def test_a_phone_with_a_newer_protocol_gets_a_note(app_exe, tmp_path):
    fake = FakePhone(protocol=3)
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert "note: this u-connectXplorer speaks remote control version 3, this app knows version 2" in out.stdout
        assert "saved" in out.stdout, out.stdout
    finally:
        fake.close()


def test_old_app_gets_update_notes(app_exe, tmp_path):
    fake = FakePhone(features={})
    try:
        out = _connect(app_exe, tmp_path, fake)
        assert out.stdout.count("note: update u-connectXplorer") == 2, out.stdout
        out = _run(app_exe, tmp_path, "xplorer.echo udp 7000")
        assert out.returncode != 0 and "cannot echo" in out.stdout, out.stdout
        assert not any(m == "socket.startServer" for m, _ in fake.calls)
    finally:
        fake.close()


def test_call_builds_params(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    out = _run(app_exe, tmp_path, "xplorer.call echo.params n=5 f=-1.5 b=true z=null s=hello o={} id=AB:CD")
    assert out.returncode == 0, out.stdout
    assert phone.calls[-1] == ("echo.params", {"n": 5, "f": -1.5, "b": True, "z": None, "s": "hello", "o": {},
                                               "id": "AB:CD"})
    out = _run(app_exe, tmp_path, "xplorer.call ble.getState")
    assert out.returncode == 0, out.stdout
    assert json.loads(out.stdout)["note"] == 'a "quoted" text'
    out = _run(app_exe, tmp_path, "xplorer.call no.such")
    assert out.returncode != 0 and "ERROR: no.such: Method not found: no.such (-32601)" in out.stdout, out.stdout


def test_echo_start_state_stop(app_exe, tmp_path, phone):
    _connect(app_exe, tmp_path, phone)
    out = _run(app_exe, tmp_path, "xplorer.echo tcp 7001")
    assert out.returncode == 0 and "the phone echoes tcp on 127.0.0.1:7001" in out.stdout, out.stdout
    assert phone.echo == {"protocol": "tcp", "port": 7001, "echo": True, "force": True}
    out = _run(app_exe, tmp_path, "xplorer.echo state")
    assert out.returncode == 0, out.stdout
    assert "phone: tcp listening port 7001 echo, received 2048 bytes (4 packets)" in out.stdout
    assert "pattern 0x00-0xFF: 0 breaks" in out.stdout
    out = _run(app_exe, tmp_path, "xplorer.echo stop")
    assert out.returncode == 0 and phone.echo is None, out.stdout
    out = _run(app_exe, tmp_path, "xplorer.echo state")
    assert out.returncode == 0 and "phone: no socket session" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["xplorer.info", "xplorer.echo state", "xplorer.call app.version"])
def test_no_phone_saved(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "no phone saved" in out.stdout, out.stdout


def test_unreachable_phone(app_exe, tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{port} {TOKEN}")
    assert out.returncode != 0 and "cannot reach u-connectXplorer" in out.stdout, out.stdout


def test_not_a_websocket(app_exe, tmp_path):
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def answer():
        conn, _ = srv.accept()
        conn.recv(1024)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
        conn.close()

    threading.Thread(target=answer, daemon=True).start()
    try:
        out = _run(app_exe, tmp_path, f"xplorer.connect 127.0.0.1:{srv.getsockname()[1]} {TOKEN}")
        assert out.returncode != 0 and "WebSocket upgrade failed" in out.stdout, out.stdout
    finally:
        srv.close()


@pytest.mark.parametrize("command", ["xplorer.connect", "xplorer.connect 1.2.3.4", "xplorer.connect 1.2.3.4:0 t",
                                     "xplorer.connect :8765 t", "xplorer.info x", "xplorer.call",
                                     "xplorer.call nodot", "xplorer.call a.b novalue", "xplorer.echo",
                                     "xplorer.echo tcp", "xplorer.echo sps 7", "xplorer.echo udp 70000"])
def test_usage(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


@pytest.mark.parametrize("command", ["perf.latency udp xplorer 7000", "perf.throughput tcp xplorer 7001",
                                     "perf.phone rx",
                                     "perf.phone", "perf.phone up", "perf.phone tx 0"])
def test_perf_with_the_phone_needs_a_device(app_exe, tmp_path, phone, command):
    _connect(app_exe, tmp_path, phone)
    calls = len(phone.calls)
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0 and "not connected to a module" in out.stdout, out.stdout
    assert len(phone.calls) == calls, "nothing may start on the phone without a module"
