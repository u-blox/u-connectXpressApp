"""session.record / session.replay and prod.test: offline checks, no module needed."""

import subprocess

import pytest

pytestmark = pytest.mark.app


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_session_selftest(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "session.selftest")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "session selftest: secret masking and report names ok" in out.stdout


def test_record_then_replay(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "session.record s.ucx", "utils.hash.text sha256 abc")
    script = (tmp_path / "s.ucx").read_text(encoding="utf-8")
    assert "utils.hash.text sha256 abc\n" in script, script
    assert "# ba7816bf" in script, script
    assert "web.stop" not in script, script

    out = _run_in(app_exe, tmp_path, "session.replay s.ucx")
    assert "> utils.hash.text sha256 abc" in out.stdout, out.stdout
    assert out.stdout.count("ba7816bf") >= 1, out.stdout


def test_replay_skips_danger_commands(app_exe, tmp_path):
    (tmp_path / "d.ucx").write_text("# test\ndev.factory confirm\nutils.hash.text md5 a\n", encoding="utf-8")
    out = _run_in(app_exe, tmp_path, "session.replay d.ucx")
    assert "skipped: changes the module for good" in out.stdout, out.stdout
    assert "replayed 1 command(s), 0 failed" in out.stdout, out.stdout


def test_replay_missing_file(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "session.replay nope.ucx")
    assert "cannot read nope.ucx" in out.stdout


def test_prod_test_needs_a_device(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "prod.test")
    assert "not connected to a module" in out.stdout
