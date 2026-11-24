"""session.record / session.replay and prod.test: offline checks, no module needed."""

import hashlib
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


@pytest.mark.parametrize("text", ["\u00e5\u00e4\u00f6", "\u20ac", "\U0001f600 caf\u00e9"])
def test_non_ascii_arguments_reach_the_app_as_utf8(app_exe, tmp_path, text):
    """Windows passes the command line as UTF-16; the app must hand each argument over as the exact UTF-8 bytes."""
    out = _run_in(app_exe, tmp_path, f"utils.hash.text sha256 {text}")
    assert out.returncode == 0, out.stdout + out.stderr
    assert hashlib.sha256(text.encode("utf-8")).hexdigest() in out.stdout, out.stdout


def test_record_then_replay(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "session.record s.ucx", "utils.hash.text sha256 abc")
    script = (tmp_path / "s.ucx").read_text(encoding="utf-8")
    assert "utils.hash.text sha256 abc\n" in script, script
    assert "# ba7816bf" in script, script
    assert "web.stop" not in script, script

    out = _run_in(app_exe, tmp_path, "session.replay s.ucx")
    assert "> utils.hash.text sha256 abc" in out.stdout, out.stdout
    assert out.stdout.count("ba7816bf") >= 1, out.stdout


def test_record_keeps_no_passphrase(app_exe, tmp_path):
    (tmp_path / "plain.txt").write_text("secret data", encoding="utf-8")
    _run_in(app_exe, tmp_path, "session.record s.ucx", "utils.encrypt plain.txt sealed.bin my-passphrase-42")
    script = (tmp_path / "s.ucx").read_text(encoding="utf-8")
    assert "utils.encrypt plain.txt sealed.bin ${UCX_SECRET}" in script, script
    assert "my-passphrase-42" not in script, script


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


def test_record_marks_failures_and_pauses(app_exe, tmp_path):
    """The recording marks a failed command with #! and a pause of a second or more with #+; stop without a recording
    and a file that cannot be made say so."""
    out = subprocess.run([str(app_exe), "--offline", "--shell"], input="session.stop\nsession.record nodir/x.ucx\n"
                         "session.record r.ucx\nsession.record again.ucx\nutils.hash.text\napp.wait 1100\n"
                         "app.version\nsession.stop\n", capture_output=True, text=True, timeout=60, errors="replace",
                         cwd=tmp_path).stdout
    assert "not recording" in out and "ERROR: cannot create nodir/x.ucx" in out, out
    assert "already recording; session.stop first" in out, out
    script = (tmp_path / "r.ucx").read_text(encoding="utf-8")
    assert "utils.hash.text\n" in script and "\n#! " in script, script
    assert "app.wait 1100\n" in script and "\n#+ 1." in script.split("app.wait 1100", 1)[1], script


def test_replay_expect_escapes_classes_and_groups(app_exe, tmp_path):
    """\\D \\w \\W \\s \\S \\t, escapes inside classes, starred and optional groups match; malformed patterns
    (an open class, an open group, a trailing backslash) match nothing."""
    (tmp_path / "e.ucx").write_text(
        "utils.hash.text md5 a\n"
        "expect ^\\w+\\W?\\S*$\n"
        "expect ^[\\d\\s]*0cc\\d+\\D\n"
        "expect ^(0c|c1|75)*b9\n"
        "expect ^(x)?0cc(17)?5\n"
        "expect [\\t\\n\\r]?b9\n"
        "expect ^0cc[a\n"
        "expect ^(0cc\n"
        "expect 0cc\\\n", encoding="utf-8")
    out = _run_in(app_exe, tmp_path, "session.replay e.ucx")
    assert "replayed 1 command(s), 1 failed" in out.stdout and out.stdout.count("ERROR: no match") == 3, out.stdout
    for pattern in ("^0cc[a", "^(0cc", "0cc\\"):
        assert f"ERROR: no match for '{pattern}'" in out.stdout, (pattern, out.stdout)


def _prod_on_vucx(app_exe, cwd, *commands, answers=None):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["commands"].update(answers or {})
    module = Vucx(profile)
    try:
        args = [str(app_exe), module.start()]
        for command in commands:
            args += ["--exec", command]
        return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                              stdin=subprocess.DEVNULL)
    finally:
        module.stop()


def test_prod_test_writes_a_report_per_module_and_one_csv(app_exe, tmp_path):
    """prod.test runs the built-in checks or a script, writes <serial>_<time>.txt with the module's identity and
    the result, and appends one line per run to report.csv (header once)."""
    (tmp_path / "steps.txt").write_text("dev.info\nnope.cmd\n")
    # the report name has the time to the second: a second run in the same second would replace the first
    out = _prod_on_vucx(app_exe, tmp_path, "prod.test - lot1", "app.wait 1100", "prod.test steps.txt lot1")
    assert "PASS: serial VUCX0123456789, 0 of 3 step(s) failed; report in lot1/VUCX0123456789_" in out.stdout
    assert "FAIL: serial VUCX0123456789, 1 of 2 step(s) failed" in out.stdout, out.stdout[-2000:]
    reports = sorted((tmp_path / "lot1").glob("VUCX0123456789_*.txt"))
    texts = [r.read_text() for r in reports]
    assert len(texts) == 2, reports
    assert any("Script:   (built-in checks)" in t and "Result:   PASS (0 of 3 step(s) failed)" in t for t in texts)
    assert any("Script:   steps.txt" in t and "Result:   FAIL (1 of 2 step(s) failed)" in t
               and "unknown command 'nope.cmd'" in t for t in texts), texts
    assert all("Model:    NORA-W36" in t and "Firmware: 3.5.0-0" in t for t in texts), texts
    rows = (tmp_path / "lot1" / "report.csv").read_text().splitlines()
    assert rows[0] == "date,serial,model,firmware,script,steps,failed,result", rows
    assert rows[1].endswith(",VUCX0123456789,NORA-W36,3.5.0-0,built-in,3,0,PASS"), rows
    assert rows[2].endswith(",VUCX0123456789,NORA-W36,3.5.0-0,steps.txt,2,1,FAIL") and len(rows) == 3, rows


@pytest.mark.parametrize("command, answers, says", [
    ("prod.test missing.txt", {}, "ERROR: cannot read missing.txt"),
    ("prod.test", {"AT+GSN": ["ERROR"]}, "ERROR: the module did not report a serial number"),
    ("prod.test a b c", {}, "usage: prod.test"),
])
def test_prod_test_failures_leave_no_report(app_exe, tmp_path, command, answers, says):
    out = _prod_on_vucx(app_exe, tmp_path, command, answers=answers)
    assert says in out.stdout and out.returncode != 0, out.stdout[-2000:]
    assert not list(tmp_path.glob("reports/*.txt")), list(tmp_path.glob("reports/*"))
