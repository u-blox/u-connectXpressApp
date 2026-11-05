"""gnss.replay (a virtual receiver playing an NMEA or UBX recording), gnss.log, gnss.fence, gnss.export and
gnss.virtual, offline on every OS."""

import struct
import subprocess
import xml.etree.ElementTree as ET

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
    ("gnss.replay empty.txt", "ERROR: no NMEA sentences or UBX frames in empty.txt"),
    ("gnss.replay track.nmea 0", "usage"),
    ("gnss.replay track.nmea fast", "usage"),
    ("gnss.log x.nmea raw", "usage"),
])
def test_replay_and_log_errors(app_exe, tmp_path, command, message):
    (tmp_path / "empty.txt").write_text("hello\n")
    (tmp_path / "track.nmea").write_text(_track(1), newline="")
    out = _run(app_exe, tmp_path, command)
    assert message.lower() in out.stdout.lower(), out.stdout


def test_open_without_a_receiver_points_to_the_virtual_one(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.open COM254")
    assert "ERROR: no GNSS receiver found (gnss.virtual starts a virtual one)" in out.stdout, out.stdout


def test_replay_refuses_a_second_source(app_exe, tmp_path):
    (tmp_path / "track.nmea").write_text(_track(2), newline="")
    out = _run(app_exe, tmp_path, "gnss.replay track.nmea", "gnss.send $PUBX,00", "gnss.rate 5",
               "gnss.restart cold", "gnss.replay track.nmea")
    assert "ERROR: virt:replay is already open (gnss.close first)" in out.stdout, out.stdout
    errors = [line for line in out.stdout.splitlines() if line.startswith("ERROR")]
    assert len(errors) == 2, out.stdout   # "cannot open COM255", then the second replay


def _ubx(cls, mid, payload):
    body = bytes([cls, mid]) + len(payload).to_bytes(2, "little") + payload
    a = b = 0
    for x in body:
        a = (a + x) & 0xFF
        b = (b + a) & 0xFF
    return b"\xb5\x62" + body + bytes([a, b])


def _navpvt(i, lat, lon, alt=45.0):
    """UBX-NAV-PVT of epoch i (one a second), 3D fix."""
    p = bytearray(92)
    struct.pack_into("<IHBBBBBB", p, 0, 300_000_000 + i * 1000, 2026, 11, 1, 12, 35, 19 + i, 0x03)
    p[20], p[21], p[23] = 3, 0x01, 9
    struct.pack_into("<ii", p, 24, round(lon * 1e7), round(lat * 1e7))
    struct.pack_into("<i", p, 36, round(alt * 1000))
    return _ubx(0x01, 0x07, bytes(p))


def _ubx_track(epochs):
    return b"".join(_navpvt(i, 55.7687 + i * 0.0002, 13.0261) for i in range(epochs))


def _frames(data):
    """Splits a UBX-only file into frames and checks every checksum."""
    frames = []
    while data:
        assert data[:2] == b"\xb5\x62", data[:8]
        n = 8 + int.from_bytes(data[4:6], "little")
        frame = data[:n]
        assert _ubx(frame[2], frame[3], frame[6:-2]) == frame
        frames.append(frame)
        data = data[n:]
    return frames


def test_replay_plays_a_ubx_recording(app_exe, tmp_path):
    (tmp_path / "track.ubx").write_bytes(b"\x00junk" + _ubx_track(3) + b"\xb5\x00")
    out = _run(app_exe, tmp_path, "gnss.replay track.ubx 10", "gnss.watch 2", "gnss.status", "gnss.close")
    assert "3 UBX frames, 0 sentences, 3 epochs at 10x" in out.stdout, out.stdout
    assert "Fix:       3D" in out.stdout and "Position:  55.76" in out.stdout, out.stdout
    assert "Time:      2026-11-01 12:35:" in out.stdout, out.stdout
    assert "Messages:  0 NMEA" in out.stdout, out.stdout


def test_log_ubx_keeps_only_ubx_frames(app_exe, tmp_path):
    (tmp_path / "mixed.raw").write_bytes(_track(3).encode() + _ubx_track(3))
    out = _run(app_exe, tmp_path, "gnss.replay mixed.raw 10", "gnss.log rec.ubx ubx", "gnss.status", "gnss.watch 2",
               "gnss.log off", "gnss.close", "gnss.replay rec.ubx 10", "gnss.close")
    assert "logging UBX frames to rec.ubx" in out.stdout and "rec.ubx (UBX)" in out.stdout, out.stdout
    frames = _frames((tmp_path / "rec.ubx").read_bytes())
    assert len(frames) >= 3 and all(f[2:4] == b"\x01\x07" for f in frames)
    assert "Replaying rec.ubx" in out.stdout, out.stdout


KML = """<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>My map</name>
<Placemark><name><![CDATA[ Garden ]]></name><Polygon><outerBoundaryIs><LinearRing><tessellate>1</tessellate>
<coordinates>
  13.02,55.768,0 13.03,55.768,0 13.03,55.769,0 13.02,55.769,0 13.02,55.768,0
</coordinates></LinearRing></outerBoundaryIs></Polygon></Placemark></Document></kml>
"""


def test_fence_reports_leaving_and_coming_back(app_exe, tmp_path):
    (tmp_path / "garden.kml").write_text(KML)
    (tmp_path / "walk.nmea").write_text(_track(5), newline="")   # epochs 0-1 inside, 2-4 north of the garden
    out = _run(app_exe, tmp_path, "gnss.fence status", "gnss.fence garden.kml", "gnss.replay walk.nmea 10",
               "gnss.watch 3", "gnss.status", "gnss.fence off", "gnss.fence status")
    assert "Fence:     off" in out.stdout, out.stdout
    assert "fence 'Garden': 4 points from garden.kml" in out.stdout, out.stdout
    assert "FENCE: outside Garden at 55.769" in out.stdout and "FENCE: back inside Garden" in out.stdout, out.stdout
    assert "Fence:     Garden, 4 points," in out.stdout and "left it 0 time(s)" not in out.stdout, out.stdout
    assert "fence off" in out.stdout


@pytest.mark.parametrize(("kml", "message"), [
    (None, "ERROR: cannot open area.kml"),
    ("<kml><Placemark><Point><name>x</name></Point></Placemark></kml>", "ERROR: no polygon in area.kml"),
    ("<kml><Polygon><coordinates>13,55 14,55</coordinates></Polygon></kml>", "needs at least 3 points"),
    ("", "ERROR: area.kml is empty or too big"),
])
def test_fence_errors(app_exe, tmp_path, kml, message):
    if kml is not None:
        (tmp_path / "area.kml").write_text(kml)
    out = _run(app_exe, tmp_path, "gnss.fence area.kml")
    assert message in out.stdout, out.stdout


def test_fence_from_a_line_string_without_a_name(app_exe, tmp_path):
    (tmp_path / "path.kml").write_text(
        "<kml><LineString><coordinates>13,55 14,55 14,56</coordinates></LineString></kml>")
    out = _run(app_exe, tmp_path, "gnss.fence path.kml", "gnss.fence")
    assert "fence 'fence': 3 points" in out.stdout and "usage" in out.stdout.lower(), out.stdout


@pytest.mark.parametrize("source", ["nmea", "ubx"])
def test_export_writes_gpx_and_kml(app_exe, tmp_path, source):
    if source == "nmea":
        (tmp_path / "in.rec").write_text(_track(4) + _nmea("GPGGA,123523.00,,,,,0,00,99.9,,M,,M,,"), newline="")
    else:
        (tmp_path / "in.rec").write_bytes(_ubx_track(4))
    out = _run(app_exe, tmp_path, "gnss.export in.rec t.gpx", "gnss.export in.rec t.kml")
    assert "4 points written to t.gpx" in out.stdout and "4 points written to t.kml" in out.stdout, out.stdout
    ns = {"g": "http://www.topografix.com/GPX/1/1", "k": "http://www.opengis.net/kml/2.2"}
    pts = ET.parse(tmp_path / "t.gpx").getroot().findall(".//g:trkpt", ns)
    assert len(pts) == 4 and pts[0].get("lat").startswith("55.768") and pts[0].get("lon").startswith("13.026")
    assert pts[0].find("g:time", ns).text.startswith("2026-11-01T12:35:")
    coords = ET.parse(tmp_path / "t.kml").getroot().find(".//k:coordinates", ns).text.split()
    assert len(coords) == 4 and coords[0].startswith("13.026") and ",55.768" in coords[0]


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.export in.nmea out.txt", "usage"),
    ("gnss.export missing.nmea out.gpx", "ERROR: cannot open missing.nmea"),
    ("gnss.export nofix.nmea out.gpx", "ERROR: no positions in nofix.nmea"),
    ("gnss.export in.nmea nodir/out.kml", "ERROR: cannot create nodir/out.kml"),
])
def test_export_errors(app_exe, tmp_path, command, message):
    (tmp_path / "in.nmea").write_text(_track(1), newline="")
    (tmp_path / "nofix.nmea").write_text(_nmea("GPGGA,123519.00,,,,,0,00,99.9,,M,,M,,"), newline="")
    out = _run(app_exe, tmp_path, command)
    assert message.lower() in out.stdout.lower(), out.stdout
    assert not (tmp_path / "out.gpx").exists()


def test_virtual_receiver_at_a_given_position_feeds_log_and_export(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual 55.5 13.25 20", "gnss.log v.nmea nmea", "gnss.watch 3",
               "gnss.status", "gnss.sats", "gnss.virtual off", "gnss.export v.nmea v.gpx", "gnss.virtual malmo",
               "gnss.virtual 1 2")
    assert "Virtual receiver at 55.5000000, 13.2500000 (fixed position)" in out.stdout, out.stdout
    assert "Virtual:   fixed position" in out.stdout and "Fix:       3D" in out.stdout, out.stdout
    assert "Position:  55.5000000, 13.2500000  alt 20.0 m" in out.stdout, out.stdout
    assert "10 satellite(s)" in out.stdout, out.stdout
    assert "closed" in out.stdout and "ERROR: virt:gnss is already open" in out.stdout, out.stdout
    assert "points written to v.gpx" in out.stdout, out.stdout
    pts = ET.parse(tmp_path / "v.gpx").getroot().findall(".//{http://www.topografix.com/GPX/1/1}trkpt")
    assert pts and pts[0].get("lat") == "55.5000000" and pts[0].get("lon") == "13.2500000"


def test_virtual_receiver_in_malmo_and_from_the_host(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", "gnss.watch 2", "gnss.status", "gnss.close",
               "gnss.virtual", "gnss.close", timeout=90)
    assert "Virtual receiver at 55.6125000, 12.9925000 (u-blox Malmo office)" in out.stdout, out.stdout
    assert "Position:  55.6125000, 12.9925000" in out.stdout, out.stdout
    assert "Asking the host for its location" in out.stdout, out.stdout
    assert out.stdout.count("Virtual receiver at") == 2, out.stdout


def test_virtual_receiver_reports_jamming_and_spoofing(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", "gnss.cfg msg mon-rf 1", "gnss.cfg msg sec-sig 1",
               "gnss.status", "gnss.watch 30", "gnss.status", "gnss.close", timeout=90)
    assert "Security:  jamming none, spoofing none" in out.stdout, out.stdout
    assert "JAM: interference detected (CW indicator 170/255)" in out.stdout, out.stdout
    assert "ALERT jamming interference detected, spoofing none, antenna OK" in out.stdout, out.stdout
    assert "JAM: cleared" in out.stdout and "SPOOF: spoofing indicated" in out.stdout, out.stdout
    assert "SPOOF: cleared" in out.stdout, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.virtual off", "ERROR: no virtual receiver is running"),
    ("gnss.virtual 91 0", "usage"),
    ("gnss.virtual 55 13 x", "usage"),
    ("gnss.virtual paris", "usage"),
])
def test_virtual_receiver_errors(app_exe, tmp_path, command, message):
    out = _run(app_exe, tmp_path, command)
    assert message.lower() in out.stdout.lower(), out.stdout
