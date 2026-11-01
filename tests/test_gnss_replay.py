"""gnss.replay (a virtual receiver playing an NMEA file) and gnss.log nmea, offline on every OS."""

import subprocess

import pytest

pytestmark = pytest.mark.app


def _nmea(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"${body}*{cs:02X}\r\n"


def _track(epochs, start=123519):
    """One RMC, GGA and GSA per second, moving north; like a file from nmeagen.org."""
    out = []
    for i in range(epochs):
        t = f"{start + i:06d}.00"
        lat = f"55{46.1234 + i * 0.01:07.4f}"
        out += [_nmea(f"GPRMC,{t},A,{lat},N,01301.5678,E,5.0,0.0,011126,,,A"),
                _nmea(f"GPGGA,{t},{lat},N,01301.5678,E,1,08,0.9,45.0,M,40.0,M,,"),
                _nmea("GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.5,0.9,1.2")]
    return "".join(out)


def _run(app_exe, cwd, *commands, timeout=60):
    args = [str(app_exe), "COM255"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_replay_feeds_status_and_loops(app_exe, tmp_path):
    (tmp_path / "track.nmea").write_text(_track(3) + "$GPGGA,bad*00\r\n\r\nnot nmea\r\n", newline="")
    out = _run(app_exe, tmp_path, "gnss.replay track.nmea 10", "gnss.watch 3", "gnss.status", "gnss.close")
    assert "10 sentences, 3 epochs at 10x" in out.stdout, out.stdout
    assert "Replay:    track.nmea at 10x" in out.stdout, out.stdout
    lap = int(out.stdout.split(", lap ")[1].split(",")[0])
    assert lap >= 2, out.stdout   # 3 epochs at 10x take 0.3 s, the watch 2 s
    assert "Fix:       3D" in out.stdout and "Position:  55.76" in out.stdout, out.stdout
    assert "closed" in out.stdout


def test_replay_keeps_the_recorded_pace(app_exe, tmp_path):
    (tmp_path / "slow.nmea").write_text(_track(5), newline="")
    out = _run(app_exe, tmp_path, "gnss.replay slow.nmea", "gnss.watch 2", "gnss.status")
    assert "lap 1," in out.stdout, out.stdout   # 5 one-second epochs at 1x are not done after 1 s
    assert "Time:      2026-11-01 12:35:" in out.stdout, out.stdout


def test_log_nmea_records_a_replayable_track(app_exe, tmp_path):
    (tmp_path / "in.nmea").write_text(_track(3) + "$GPGGA,bad*00\r\n", newline="")
    out = _run(app_exe, tmp_path, "gnss.replay in.nmea 10", "gnss.log rec.nmea nmea", "gnss.status",
               "gnss.watch 2", "gnss.log off", "gnss.close", "gnss.replay rec.nmea 10", "gnss.close")
    assert "logging NMEA sentences to rec.nmea" in out.stdout and "rec.nmea (NMEA)" in out.stdout, out.stdout
    lines = (tmp_path / "rec.nmea").read_bytes().decode().split("\r\n")
    assert lines[-1] == "" and len(lines) > 9, lines
    for line in lines[:-1]:
        body, cs = line[1:].split("*")
        assert line[0] == "$" and int(cs, 16) == _xor(body), line
    assert "Replaying rec.nmea" in out.stdout and "logging stopped" in out.stdout, out.stdout


def _xor(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return cs


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.replay missing.nmea", "ERROR: cannot open missing.nmea"),
    ("gnss.replay empty.txt", "ERROR: no NMEA sentences in empty.txt"),
    ("gnss.replay track.nmea 0", "usage"),
    ("gnss.replay track.nmea fast", "usage"),
    ("gnss.log x.nmea raw", "usage"),
])
def test_replay_and_log_errors(app_exe, tmp_path, command, message):
    (tmp_path / "empty.txt").write_text("hello\n")
    (tmp_path / "track.nmea").write_text(_track(1), newline="")
    out = _run(app_exe, tmp_path, command)
    assert message.lower() in out.stdout.lower(), out.stdout


def test_replay_refuses_a_second_source(app_exe, tmp_path):
    (tmp_path / "track.nmea").write_text(_track(2), newline="")
    out = _run(app_exe, tmp_path, "gnss.replay track.nmea", "gnss.send $PUBX,00", "gnss.rate 5",
               "gnss.restart cold", "gnss.replay track.nmea")
    assert "ERROR: virt:replay is already open (gnss.close first)" in out.stdout, out.stdout
    assert out.stdout.count("\nERROR") == 2, out.stdout   # "cannot open COM255", then the second replay
