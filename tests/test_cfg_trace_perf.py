"""cfg.*, at.trace/decode/replay and perf.*: offline checks, no module needed. Module runs are in tests/test_vucx.py
and tests/hardware."""

import os
import shutil
import subprocess
from pathlib import Path

import pytest
from harness import pcapng

pytestmark = pytest.mark.app

LUA = Path(__file__).resolve().parents[1] / "scripts" / "wireshark" / "ucxapp_at.lua"
START_MS = 1793626919000

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


@pytest.mark.parametrize("command", ["cfg.diff", "cfg.diff a b c", "at.trace.show", "at.decode", "at.trace.pcap",
                                     "at.trace.pcap a b c"])
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


def _dated(text):
    return text.replace("# ucxapp AT trace\n", f"# ucxapp AT trace\n# start {START_MS} ms since 1970 UTC\n")


def test_trace_pcap_frames(app_exe, tmp_path):
    trace = _write(tmp_path, "a.trace", _dated(TRACE))
    out = _run_in(app_exe, tmp_path, f"at.trace.pcap {trace}")
    assert out.returncode == 0 and "8 frames written" in out.stdout and "1970" not in out.stdout, out.stdout
    interfaces, frames = pcapng.read(tmp_path / "a.pcapng")
    assert interfaces == [{"linktype": 252, "name": "u-connectXpress AT", "tsresol": 3}]
    assert [f["ts"] - START_MS for f in frames] == [0, 20, 30, 250, 1100, 1500, 2000, 2030]
    assert [f["out"] for f in frames] == [True, False, True, False, False, False, True, False]
    assert all(f["tags"][12] == b"at\0\0" for f in frames)
    assert [f["tags"][35] for f in frames[:2]] == [b"\0\0\0\0", b"\0\0\0\1"]
    assert frames[0]["payload"] == b'AT+UWSCP=0,"home"\r' and frames[1]["payload"] == b"OK\r\n"
    infos = [f["tags"].get(36, b"").decode() for f in frames]
    assert "220 ms AT+UWSC=0" in infos and "30 ms AT+UWSIPS=0" in infos, infos
    assert any(i.startswith("Wi-Fi link up: wlan_handle=0") for i in infos), infos
    assert "Wi-Fi network up (IP assigned)" in infos, infos


def test_trace_pcap_named_output_and_undated(app_exe, tmp_path):
    trace = _write(tmp_path, "a.trace", TRACE)
    out = _run_in(app_exe, tmp_path, f"at.trace.pcap {trace} {tmp_path / 'b.pcapng'}")
    assert out.returncode == 0 and "1970" in out.stdout, out.stdout
    _, frames = pcapng.read(tmp_path / "b.pcapng")
    assert frames[0]["ts"] == 0 and frames[-1]["ts"] == 2030


def test_trace_pcap_missing_file(app_exe, tmp_path):
    out = _run_in(app_exe, tmp_path, f"at.trace.pcap {tmp_path / 'nope.trace'}")
    assert out.returncode != 0 and "cannot read" in out.stdout, out.stdout


def _tshark():
    found = shutil.which("tshark")
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432")):
        if found is None and base and (Path(base) / "Wireshark" / "tshark.exe").exists():
            found = str(Path(base) / "Wireshark" / "tshark.exe")
    return found


def test_trace_pcap_in_wireshark(app_exe, tmp_path):
    tshark = _tshark()
    if tshark is None:
        pytest.skip("Wireshark (tshark) is not installed")
    trace = _write(tmp_path, "a.trace", _dated(TRACE) + "3.000 TX AT+GMM\n4.000 RX +STARTUP\n")
    assert _run_in(app_exe, tmp_path, f"at.trace.pcap {trace}").returncode == 0
    out = subprocess.run([tshark, "-X", f"lua_script:{LUA}", "-2", "-r", str(tmp_path / "a.pcapng"), "-T", "fields",
                          "-E", "separator=|", "-e", "at.role", "-e", "at.cmd", "-e", "ucx.kind", "-e", "ucx.cmd",
                          "-e", "ucx.result", "-e", "ucx.time", "-e", "ucx.request_in", "-e", "ucx.response_in",
                          "-e", "ucx.meaning", "-e", "_ws.expert.message"],
                         capture_output=True, text=True, timeout=120, errors="replace")
    assert out.returncode == 0, out.stderr
    rows = [line.split("|") for line in out.stdout.splitlines()]
    assert len(rows) == 10, out.stdout
    assert rows[2][:8] == ["2", "+UWSC", "command", "UWSC", "", "", "", "4"], rows[2]
    assert rows[3][:7] == ["1", "OK", "final", "", "OK", "220", "3"], rows[3]
    assert rows[4][2:4] == ["urc", "UEWLU"] and rows[4][8] == "Wi-Fi link up", rows[4]
    assert rows[7][4:6] == ["ERROR", "30"] and "Command failed" in rows[7][9], rows[7]
    assert "No OK/ERROR" in rows[8][9] and "Module started" in rows[9][9], rows[8:]


@pytest.mark.parametrize("group", ["cfg", "at", "perf"])
def test_examples(app_exe, tmp_path, group):
    out = _run_in(app_exe, tmp_path, f"{group}.ex")
    assert out.returncode == 0 and f"{group}." in out.stdout and " 1. " in out.stdout, out.stdout
