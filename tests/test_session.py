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
    assert "session selftest: secret masking (scripts and AT trace) and report names ok" in out.stdout


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


def test_replay_expect_lines(app_exe, tmp_path):
    (tmp_path / "e.ucx").write_text(
        "app.version\n"
        "expect u-connectXpressApp \\d+\\.\\d+\n"
        "expect ^u-connect(Xpress|Foo)App\n"
        "expect [A-Za-z]+ x?\\d*\n"
        "no.such.command\n"
        "expect-fail\n"
        "utils.hash.text md5 a\n"
        "expect ^0cc175b9c0f1b6a831c399e269772661$\n", encoding="utf-8")
    out = _run_in(app_exe, tmp_path, "session.replay e.ucx")
    assert out.returncode == 0, out.stdout
    assert "replayed 3 command(s), 0 failed" in out.stdout, out.stdout


def test_replay_expect_misses_fail(app_exe, tmp_path):
    (tmp_path / "m.ucx").write_text(
        "expect early\n"
        "app.version\n"
        "expect (Foo|Bar)+\n"
        "utils.hash.text md5 a\n"
        "expect-fail\n", encoding="utf-8")
    out = _run_in(app_exe, tmp_path, "session.replay m.ucx")
    assert out.returncode != 0, out.stdout
    assert "ERROR: 'expect early' before the first command" in out.stdout, out.stdout
    assert "ERROR: no match for '(Foo|Bar)+' in the output of 'app.version'" in out.stdout, out.stdout
    assert "ERROR: 'utils.hash.text md5 a' should have failed (expect-fail)" in out.stdout, out.stdout
    assert "replayed 2 command(s), 3 failed" in out.stdout, out.stdout


def test_prod_test_needs_a_device(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "prod.test")
    assert "not connected to a module" in out.stdout
