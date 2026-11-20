"""DTLS 1.2 on the host (dtls.*): offline in-process round trip, no module needed."""

import subprocess

import pytest

pytestmark = pytest.mark.app


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


@pytest.fixture
def dtls_dir(app_exe, tmp_path):
    """A store with the local CA and a server certificate for localhost."""
    out = _run_in(app_exe, tmp_path, "utils.tls.ca.create", "utils.tls.server.create server localhost 127.0.0.1")
    assert out.returncode == 0, out.stdout
    return tmp_path


def test_dtls_selftest_handshake_and_echo(app_exe, dtls_dir):
    out = _run_in(app_exe, dtls_dir, "dtls.selftest server")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "dtls selftest:" in out.stdout
    assert "echo ok" in out.stdout


def test_dtls_selftest_with_a_pre_shared_key_needs_no_certificate(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "dtls.selftest psk")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "PSK handshake ok" in out.stdout and "echo ok" in out.stdout, out.stdout


def test_dtls_selftest_needs_the_server_certificate(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "utils.tls.ca.create", "dtls.selftest missing")
    assert out.returncode != 0
    assert "ERROR" in out.stdout


def test_dtls_socket_commands_need_their_device(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "dtls.client 127.0.0.1 5684", "dtls.server 5684")
    assert "not connected to a module" in out.stdout
