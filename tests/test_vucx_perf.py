"""perf.echo against scripted TCP and UDP clients of the virtual module (Network requests)."""

import subprocess
import sys

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]


def _echo(app_exe, tmp_path, port, requests, command):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {port: requests}
    module = Vucx(profile)
    try:
        out = subprocess.run([str(app_exe), module.start(), "--exec", command], capture_output=True, text=True,
                             timeout=60, errors="replace", stdin=subprocess.DEVNULL, cwd=tmp_path)
    finally:
        module.stop()
    return out, [bytes(r) if not isinstance(r, list) else r for r in module.net.replies]


def test_perf_echo_tcp_returns_what_a_client_sends(app_exe, tmp_path):
    out, replies = _echo(app_exe, tmp_path, 7000, ["ping over tcp\n"], "perf.echo tcp 7000 2")
    assert out.returncode == 0, out.stdout
    assert replies and replies[0] == b"ping over tcp\n", (replies, out.stdout)


def test_perf_echo_udp_returns_every_datagram(app_exe, tmp_path):
    out, replies = _echo(app_exe, tmp_path, 7001, [b"one", b"two", b"three"], "perf.echo udp 7001 2")
    assert out.returncode == 0, out.stdout
    assert replies and replies[0] == [b"one", b"two", b"three"], (replies, out.stdout)


@pytest.mark.parametrize("command", ["perf.echo", "perf.echo tcp", "perf.echo tcp 0", "perf.echo ftp 7000",
                                     "perf.echo udp 7000 x", "perf.latency tcp", "perf.throughput udp h 0",
                                     "perf.latency tcp h 7 0", "perf.throughput tcp h 7 1 0",
                                     "perf.phone", "perf.phone sideways"])
def test_perf_usage_errors(app_exe, tmp_path, command):
    out, _ = _echo(app_exe, tmp_path, 7002, [], command)
    assert out.returncode != 0, out.stdout

def test_dtls_client_on_the_virtual_module_sends_in_clear(app_exe, tmp_path):
    out, _ = _echo(app_exe, tmp_path, 7004, [], "dtls.client 192.168.1.50 5684 hello-dtls")
    assert out.returncode == 0, out.stdout
    assert "handshake skipped" in out.stdout and "reply: hello-dtls" in out.stdout, out.stdout


def test_dtls_server_on_the_virtual_module_echoes_one_client(app_exe, tmp_path):
    out, replies = _echo(app_exe, tmp_path, 5684, [b"from client"], "dtls.server 5684")
    assert out.returncode == 0, out.stdout
    assert "echoed 11 bytes" in out.stdout, out.stdout
    assert replies and replies[0] == [b"from client"], (replies, out.stdout)


@pytest.mark.parametrize("command", ["dtls.client h", "dtls.client h 0", "dtls.client h x", "dtls.client h 1 a b",
                                     "dtls.server", "dtls.server 0", "dtls.server 1 a b", "dtls.selftest a b"])
def test_dtls_usage_errors(app_exe, tmp_path, command):
    out, _ = _echo(app_exe, tmp_path, 7005, [], command)
    assert out.returncode != 0, out.stdout
