"""gnss.assist (u-blox AssistNow): UBX-MGA injection, chipcode storage and errors, offline on a virtual receiver."""

import pytest
from test_gnss_replay import _run, _ubx

pytestmark = pytest.mark.app


def _mga_file():
    """An AssistNow answer: MGA-INI-TIME_UTC and two MGA-GPS-EPH, with junk, a NAV frame and a broken frame."""
    good = [_ubx(0x13, 0x40, bytes(24)), _ubx(0x13, 0x00, bytes(range(68))), _ubx(0x13, 0x00, bytes(68))]
    broken = bytearray(_ubx(0x13, 0x02, bytes(8)))
    broken[-1] ^= 0xFF
    data = b"\x00junk\xb5" + good[0] + _ubx(0x01, 0x07, bytes(92)) + good[1] + bytes(broken) + good[2] + b"\xb5\x62"
    return data, sum(len(f) for f in good)


def test_assist_sends_only_valid_mga_frames(app_exe, tmp_path):
    data, size = _mga_file()
    (tmp_path / "mga.ubx").write_bytes(data)
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", "gnss.assist mga.ubx", "gnss.close")
    assert f"3 UBX-MGA messages ({size} bytes) sent to the receiver" in out.stdout, out.stdout


def test_assist_chipcode_is_saved_and_used_for_online(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.assist chipcode Ab+c/d==", "gnss.virtual malmo", "gnss.assist online gps,gal")
    assert "chipcode saved to ucxapp.assistnow" in out.stdout, out.stdout
    assert (tmp_path / "storage" / "ucxapp.assistnow").read_text() == "Ab+c/d==\n\n"
    assert "AssistNow ulorb_l1,ukion,usvht for gps,gal" in out.stdout, out.stdout   # then no module to fetch with


def test_assist_predictive_orbits_when_the_plan_has_no_live_orbits(app_exe, tmp_path):
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "ucxapp.assistnow").write_text("XYZ\r\nualm,uporb_1,uporb_3\r\n")
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", "gnss.assist online")
    assert "AssistNow uporb_1,ualm for gps,gal,bds,glo" in out.stdout, out.stdout


def test_assist_id_needs_an_answering_receiver(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", "gnss.assist id")
    assert "ERROR: the receiver did not answer UBX-SEC-UNIQID" in out.stdout, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.assist missing.ubx", "ERROR: cannot read missing.ubx"),
    ("gnss.assist err.txt", "ERROR: no UBX-MGA messages in err.txt: Invalid chipcode"),
    ("gnss.assist online", "ERROR: no chipcode"),
    ("gnss.assist", "usage"),
    ("gnss.assist register", "usage"),
    ("gnss.assist chipcode a;b", "usage"),
    ("gnss.assist online gps;x", "usage"),
    ("gnss.assist legacy", "usage"),
    ("gnss.assist a.ubx b.ubx", "usage"),
])
def test_assist_errors(app_exe, tmp_path, command, message):
    (tmp_path / "err.txt").write_text("Invalid chipcode\r\n")
    out = _run(app_exe, tmp_path, "gnss.virtual malmo", command)
    assert message.lower() in out.stdout.lower(), out.stdout


def test_assist_needs_a_receiver(app_exe, tmp_path):
    (tmp_path / "mga.ubx").write_bytes(_mga_file()[0])
    out = _run(app_exe, tmp_path, "gnss.assist mga.ubx")
    assert "ERROR: no receiver open (use gnss.open)" in out.stdout, out.stdout
