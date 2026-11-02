"""cfg.*, at.trace/decode/replay and perf.*: offline checks, no module needed. Module runs are in tests/test_vucx.py
and tests/hardware."""

import subprocess

import pytest

pytestmark = pytest.mark.app

CFG_A = """# u-connectXpress settings, NORA-W36X 3.5.0
AT+UBTLN="ucx-a"
AT+UWSCP=0,"home"
AT+UMQCP=0,"broker.emqx.io",1883
AT+USYEC=1
"""

CFG_B = """# u-connectXpress settings, NORA-W36X 3.5.0
AT+UBTLN="ucx-b"
AT+UWSCP=0,"home"
AT+USYEC=1
AT+UNTSC=0,"pool.ntp.org"
"""

TRACE = """# ucxapp AT trace
0.000 TX AT+UWSCP=0,"home"
0.020 RX OK
0.030 TX AT+UWSC=0
0.250 RX OK
1.100 RX +UEWLU:0,0123456789AB,6
1.500 RX +UEWSNU
2.000 TX AT+UWSIPS=0
2.030 RX ERROR
"""


def _run_in(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def _write(tmp_path, name, text):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8")
    return path


@pytest.mark.parametrize("command", ["cfg.show", "cfg.save a.cfg", "cfg.load a.cfg",
                                     "at.replay a.trace", "perf.rtt udp 127.0.0.1 7", "perf.tput tcp 127.0.0.1 7",
                                     "perf.rtt sps", "perf.echo udp 7", "perf.echo sps 5", "perf.rtt",
                                     "perf.echo"])
def test_needs_a_device(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert out.returncode != 0 and "not connected to a module" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["cfg.diff", "cfg.diff a b c", "at.trace.show", "at.decode"])
def test_usage_errors(app_exe, tmp_path, command):
    out = _run_in(app_exe, tmp_path, command)
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout


def test_cfg_diff_two_files(app_exe, tmp_path):
    a = _write(tmp_path, "a.cfg", CFG_A)
    b = _write(tmp_path, "b.cfg", CFG_B)
    out = _run_in(app_exe, tmp_path, f"cfg.diff {a} {b}")
    assert out.returncode != 0, out.stdout
    assert 'AT+UBTLN="ucx-a"' in out.stdout and 'AT+UBTLN="ucx-b"' in out.stdout
    assert '- AT+UMQCP=0,"broker.emqx.io",1883' in out.stdout
    assert '+ AT+UNTSC=0,"pool.ntp.org"' in out.stdout
    assert "UWSCP" not in out.stdout and "USYEC" not in out.stdout


def test_cfg_diff_same_file_passes(app_exe, tmp_path):
    a = _write(tmp_path, "a.cfg", CFG_A)
    b = _write(tmp_path, "b.cfg", CFG_A.replace("# u-connectXpress settings", "# another header"))
    out = _run_in(app_exe, tmp_path, f"cfg.diff {a} {b}")
    assert out.returncode == 0, out.stdout


def test_cfg_diff_missing_file(app_exe, tmp_path):
    a = _write(tmp_path, "a.cfg", CFG_A)
    out = _run_in(app_exe, tmp_path, f"cfg.diff {a} {tmp_path / 'nope.cfg'}")
    assert out.returncode != 0, out.stdout


def test_decode_urcs(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "at.decode +UEWLD:0,3", "at.decode +UESODA:1,64", "at.decode +UEXYZ:1")
    assert "Wi-Fi link down" in out.stdout and "reason" in out.stdout, out.stdout
    assert "socket data available" in out.stdout and "number_bytes" in out.stdout


def test_trace_show_decodes_and_times(app_exe, tmp_path):
    trace = _write(tmp_path, "a.trace", TRACE)
    out = _run_in(app_exe, tmp_path, f"at.trace.show {trace}")
    assert out.returncode == 0, out.stdout
    assert "AT+UWSC=0" in out.stdout and "220 ms" in out.stdout, out.stdout
    assert "Wi-Fi link up" in out.stdout and "Wi-Fi network up" in out.stdout
    assert "ERROR" in out.stdout


def test_trace_show_errors_only(app_exe, tmp_path):
    trace = _write(tmp_path, "a.trace", TRACE)
    out = _run_in(app_exe, tmp_path, f"at.trace.show {trace} errors")
    assert out.returncode == 0, out.stdout
    assert "AT+UWSIPS=0" in out.stdout and "AT+UWSC=0 " not in out.stdout, out.stdout


def test_trace_stop_without_trace(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, "at.trace.stop")
    assert out.returncode != 0, out.stdout


@pytest.mark.parametrize("group", ["cfg", "at", "perf"])
def test_examples(app_exe, tmp_path, group):
    out = _run_in(app_exe, tmp_path, f"{group}.ex")
    assert out.returncode == 0 and f"{group}." in out.stdout and " 1. " in out.stdout, out.stdout
