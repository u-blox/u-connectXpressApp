"""RTK state, jamming/spoofing alarms, gnss.cfg, gnss.backup and the PointPerfect bridge, offline on every OS
(a UBX replay and the virtual receiver, which answers UBX-CFG with UBX-ACK and UBX-UPD-SOS like a receiver)."""

import re
import struct

import pytest
from test_gnss_replay import _run, _ubx

pytestmark = pytest.mark.app


def _navpvt_rtk(i, carr=2, h_acc_mm=14, v_acc_mm=21, age_index=2):
    """UBX-NAV-PVT with a 3D fix, corrections applied and the given carrier solution."""
    p = bytearray(92)
    struct.pack_into("<IHBBBBBB", p, 0, 300_000_000 + i * 1000, 2026, 11, 1, 12, 35, 19 + i, 0x03)
    p[20], p[21], p[23] = 3, 0x03 | (carr << 6), 12
    struct.pack_into("<ii", p, 24, round(13.0261 * 1e7), round((55.7687 + i * 0.0002) * 1e7))
    struct.pack_into("<iII", p, 36, 45_000, h_acc_mm, v_acc_mm)
    struct.pack_into("<H", p, 78, age_index << 1)
    return _ubx(0x01, 0x07, bytes(p))


def _hpposllh(invalid=False):
    """UBX-NAV-HPPOSLLH: 55.768700012, 13.026099966, 45.0123 m above MSL, 14.2 mm / 21.1 mm accuracy."""
    p = struct.pack("<BBBBIiiiibbbbII", 0, 0, 0, int(invalid), 300_000_000, 130_261_000, 557_687_000, 85_000,
                    45_012, -34, 12, 0, 3, 142, 211)
    return _ubx(0x01, 0x14, p)


def _secsig(jam, spoof):
    """UBX-SEC-SIG version 1: detection enabled, jammingState and spoofingState."""
    return _ubx(0x27, 0x09, bytes([1, 0, 0, 0, 1 | (jam << 1), 0, 0, 0, 1 | (spoof << 1), 0, 0, 0]))


def _monrf(ant_status, jam_ind, jam_state=1):
    """UBX-MON-RF with one RF block."""
    block = bytearray(24)
    block[1], block[2], block[3], block[16] = jam_state, ant_status, 1, jam_ind
    return _ubx(0x0A, 0x38, bytes([0, 1, 0, 0]) + bytes(block))


def test_replay_shows_rtk_jamming_spoofing_and_antenna(app_exe, tmp_path):
    data = b"".join(_navpvt_rtk(i) for i in range(3)) + _secsig(2, 2) + _monrf(4, 42)
    (tmp_path / "rtk.ubx").write_bytes(data)
    out = _run(app_exe, tmp_path, "gnss.replay rtk.ubx 10", "gnss.watch 2", "gnss.status", "gnss.close")
    assert "RTK:       RTK fixed, corrections 1-2 s old; accuracy 0.014 m horizontal, 0.021 m vertical" in out.stdout, \
        out.stdout
    assert ("Security:  jamming interference detected, spoofing indicated, CW indicator 42/255, "
            "antenna open (no antenna) (powered)") in out.stdout, out.stdout
    assert "JAM: interference detected (CW indicator" in out.stdout, out.stdout
    assert "SPOOF: spoofing indicated" in out.stdout and "ANT: antenna open (no antenna)" in out.stdout, out.stdout
    assert "RTK   RTK fixed" in out.stdout and "ALERT jamming interference detected" in out.stdout, out.stdout


def test_replay_shows_high_precision_position(app_exe, tmp_path):
    data = b"".join(_navpvt_rtk(i) + _hpposllh() for i in range(3))
    (tmp_path / "hp.ubx").write_bytes(data)
    out = _run(app_exe, tmp_path, "gnss.replay hp.ubx 10", "gnss.watch 2", "gnss.status", "gnss.close")
    assert "Position:  55.768700012, 13.026099966  alt 45.0123 m (high precision)" in out.stdout, out.stdout
    assert "RTK:       RTK fixed, corrections 1-2 s old; accuracy 0.0142 m horizontal, 0.0211 m vertical" \
        in out.stdout, out.stdout
    assert re.search(r"RTK   RTK fixed\s+\S+0\.0142 m", out.stdout), out.stdout


def test_invalid_high_precision_position_is_ignored(app_exe, tmp_path):
    (tmp_path / "hp.ubx").write_bytes(b"".join(_navpvt_rtk(i) + _hpposllh(invalid=True) for i in range(3)))
    out = _run(app_exe, tmp_path, "gnss.replay hp.ubx 10", "gnss.watch 2", "gnss.status")
    assert "(high precision)" not in out.stdout, out.stdout
    assert "accuracy 0.014 m horizontal, 0.021 m vertical" in out.stdout, out.stdout


def test_replay_reports_clearing_and_float(app_exe, tmp_path):
    data = (_navpvt_rtk(0, carr=1, age_index=0) + _secsig(2, 1) + _monrf(2, 3) + _navpvt_rtk(1, carr=1, age_index=0)
            + _secsig(1, 1) + _navpvt_rtk(2, carr=1, age_index=0))
    (tmp_path / "float.ubx").write_bytes(data)
    out = _run(app_exe, tmp_path, "gnss.replay float.ubx 1", "gnss.watch 3", "gnss.status")
    assert "JAM: interference detected" in out.stdout and "JAM: cleared" in out.stdout, out.stdout
    assert "RTK:       RTK float; accuracy" in out.stdout, out.stdout
    assert "SPOOF:" not in out.stdout and "ANT:" not in out.stdout, out.stdout


def test_status_without_corrections_or_security_data(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual 55.6 13.0", "gnss.watch 2", "gnss.status")
    assert "RTK:       no corrections (gnss.bridge ntrip or pointperfect)" in out.stdout, out.stdout
    assert "Security:  unknown (gnss.cfg msg sec-sig 1" in out.stdout, out.stdout
    assert "Backup:" not in out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.cfg model automotive", "dynamic model set to automotive"),
    ("gnss.cfg gnss gps,gal,BDS", "GNSS systems set to gps,gal,BDS"),
    ("gnss.cfg msg gsv 5", "gsv on UART1 and USB every 5 epoch(s)"),
    ("gnss.cfg msg sec-sig 0", "sec-sig off on UART1 and USB"),
    ("gnss.cfg msg nav-hpposllh 1", "nav-hpposllh on UART1 and USB every 1 epoch(s)"),
    ("gnss.cfg msg nav-svin 1", "nav-svin on UART1 and USB every 1 epoch(s)"),
    ("gnss.cfg msg nav-relposned 1", "nav-relposned on UART1 and USB every 1 epoch(s)"),
    ("gnss.cfg save", "configuration saved"),
    ("gnss.cfg reset", "configuration reset to the factory defaults"),
])
def test_cfg_is_acknowledged_by_the_virtual_receiver(app_exe, tmp_path, command, message):
    out = _run(app_exe, tmp_path, "gnss.virtual", command)
    assert message in out.stdout, out.stdout
    errors = [line for line in out.stdout.splitlines() if line.startswith("ERROR:") and "COM255" not in line]
    assert not errors, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.cfg", "usage"),
    ("gnss.cfg gnss gps,moon", "usage"),
    ("gnss.cfg gnss ,", "usage"),
    ("gnss.cfg model rocket", "usage"),
    ("gnss.cfg msg xyz 1", "usage"),
    ("gnss.cfg msg gga 256", "usage"),
    ("gnss.cfg baud 12345", "usage"),
    ("gnss.cfg baud fast", "usage"),
    ("gnss.cfg save now", "usage"),
    ("gnss.cfg baud 115200", "ERROR: a virtual receiver has no baud rate"),
    ("gnss.backup", "usage"),
    ("gnss.backup later", "usage"),
])
def test_cfg_and_backup_errors(app_exe, tmp_path, command, message):
    out = _run(app_exe, tmp_path, "gnss.virtual", command)
    assert message.lower() in out.stdout.lower(), out.stdout


@pytest.mark.parametrize("command", ["gnss.cfg save", "gnss.backup status", "gnss.cfg msg gga 1"])
def test_cfg_and_backup_need_a_receiver(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert "ERROR: no receiver open (use gnss.open)" in out.stdout, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.cfg model sea", "ERROR: no answer from the receiver (no UBX-ACK"),
    ("gnss.backup status", "ERROR: no UBX-UPD-SOS answer"),
])
def test_cfg_without_answer_on_a_replay(app_exe, tmp_path, command, message):
    (tmp_path / "rtk.ubx").write_bytes(b"".join(_navpvt_rtk(i) for i in range(2)))
    out = _run(app_exe, tmp_path, "gnss.replay rtk.ubx", command)
    assert message in out.stdout, out.stdout


def test_backup_save_status_and_clear(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual", "gnss.backup save", "gnss.backup status", "gnss.status",
               "gnss.backup clear")
    assert "backup saved and navigation stopped" in out.stdout, out.stdout
    assert "last start: started without a backup" in out.stdout, out.stdout
    assert "Backup:    started without a backup" in out.stdout, out.stdout
    assert "backup cleared" in out.stdout, out.stdout


@pytest.mark.parametrize(("command", "message"), [
    ("gnss.bridge pointperfect", "usage"),
    ("gnss.bridge pointperfect my-client mars", "usage"),
    ("gnss.bridge pointperfect bad/id", "usage"),
    ("gnss.bridge pointperfect my-client eu cert/x", "usage"),
    ("gnss.bridge pointperfect my-client us pp extra", "usage"),
    ("gnss.bridge pointperfect my-client", "ERROR: no module connected"),
    ("gnss.bridge pointperfect my-client jp mycert", "ERROR: no module connected"),
])
def test_pointperfect_bridge_offline(app_exe, tmp_path, command, message):
    out = _run(app_exe, tmp_path, "gnss.virtual", command, "gnss.bridge status")
    assert message.lower() in out.stdout.lower(), out.stdout
    assert "MQTT" not in out.stdout, out.stdout


def test_pointperfect_needs_a_receiver(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.bridge pointperfect my-client")
    assert "ERROR: no receiver open (use gnss.open)" in out.stdout, out.stdout


# ---- gnss.setup: standard receiver, RTK rover and RTK base station ----

def _crc24q(data):
    crc = 0
    for x in data:
        crc ^= x << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


def _rtcm(msg_type, length=12, bad=False):
    """An RTCM3 frame of the given message number (the rest of the message zero) with its CRC-24Q."""
    frame = bytes([0xD3, length >> 8, length & 0xFF, msg_type >> 4, (msg_type & 0xF) << 4]) + bytes(length - 2)
    return frame + (_crc24q(frame) ^ int(bad)).to_bytes(3, "big")


def _navsvin(dur, acc_01mm, obs, valid, active):
    return _ubx(0x01, 0x3B, struct.pack("<B3xIIiiibbbxIIBB2x", 0, 300_000_000, dur, 350_000_000, 80_000_000,
                                        520_000_000, 12, -5, 0, acc_01mm, obs, valid, active))


def _monver(protver):
    ext = f"PROTVER={protver}".encode().ljust(30, b"\0")
    return _ubx(0x0A, 0x04, b"ROM CORE 3.01 (107888)".ljust(30, b"\0") + b"00080000".ljust(10, b"\0") + ext)


def _no_errors(out):
    return [line for line in out.stdout.splitlines() if line.startswith("ERROR:") and "COM255" not in line]


@pytest.mark.parametrize(("command", "steps"), [
    ("gnss.setup standard", ["NMEA GGA GSA GSV RMC VTG every epoch, GLL off", "UBX NAV-PVT every epoch",
                             "base mode, high-precision NMEA and NAV-HPPOSLLH off", "RTCM3 output off"]),
    ("gnss.setup rover", ["RTCM3 corrections in on UART1 and USB", "SPARTN corrections in (PointPerfect)",
                          "UBX NAV-PVT, NAV-HPPOSLLH and NMEA GGA every epoch",
                          "high-precision NMEA, RTK fixed mode, base mode off",
                          "next: corrections with gnss.bridge ntrip"]),
    ("gnss.setup base survey", ["survey-in for at least 120 s and 2.00 m",
                                "RTCM 1005 1074 1084 1094 1124 every epoch, 1230 every 5",
                                "RTCM3 out on UART1 and USB, NAV-SVIN, stationary model", "serve the corrections"]),
    ("gnss.setup base fixed 55.612760001 -12.98625 52.1234 0.02",
     ["fixed at 55.612760001, -12.986250000, 52.1234 m (0.0200 m)", "RTCM3 out on UART1 and USB"]),
    ("gnss.setup base off", ["base mode and NAV-SVIN off", "RTCM3 output off"]),
])
def test_setup_is_acknowledged_step_by_step(app_exe, tmp_path, command, steps):
    out = _run(app_exe, tmp_path, "gnss.virtual", command)
    for step in steps:
        assert step in out.stdout, out.stdout
    assert "refused" not in out.stdout and "no answer" not in out.stdout, out.stdout
    assert not _no_errors(out), out.stdout


def test_base_survey_on_the_virtual_receiver_converges_and_sends_rtcm(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual 55.6 13.0 10", "gnss.setup base survey 3 5", "gnss.watch 6",
               "gnss.status")
    assert "Base:      survey-in done after 3 s, mean accuracy 5.000 m; RTCM3 out: 1005 (" in out.stdout, out.stdout
    assert re.search(r"Messages:  \d+ NMEA, \d+ UBX, \d+ RTCM3, 0 bad", out.stdout), out.stdout
    keep = re.search(r"keep it: gnss.setup base fixed (\S+) (\S+) (\S+) 5.000", out.stdout)
    assert keep, out.stdout
    lat, lon, h = (float(x) for x in keep.groups())
    assert abs(lat - 55.6) < 1e-8 and abs(lon - 13.0) < 1e-8 and abs(h - 10.0) < 1e-3, keep.group(0)


def test_base_survey_in_progress_and_fixed_base(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.virtual 55.6 13.0 10", "gnss.setup base survey 600 0.01", "gnss.watch 2",
               "gnss.status", "gnss.setup base fixed 55.6 13.0 10 0.02", "gnss.watch 2", "gnss.status",
               "gnss.setup base off", "gnss.setup standard", "gnss.status")
    assert re.search(r"Base:      survey-in running \d+ s, \d+ observations, mean accuracy [\d.]+ m; "
                     r"no RTCM3 output yet", out.stdout), out.stdout
    assert re.search(r"Base:      fixed position; RTCM3 out: 1005 \(\d+ frames\)", out.stdout), out.stdout
    assert not _no_errors(out), out.stdout


def test_replay_of_a_base_shows_survey_in_and_rtcm_messages(app_exe, tmp_path):
    data = (_navpvt_rtk(0) + _navsvin(42, 18_500, 840, 0, 1) + _rtcm(1074) + _rtcm(1230, 8) + _rtcm(1074, bad=True)
            + _navpvt_rtk(1))
    (tmp_path / "base.ubx").write_bytes(data)
    out = _run(app_exe, tmp_path, "gnss.replay base.ubx 10", "gnss.watch 2", "gnss.status")
    assert "Base:      survey-in running 42 s, 840 observations, mean accuracy 1.850 m; RTCM3 out: 1074 1230 (" \
        in out.stdout, out.stdout
    assert re.search(r"\d+ RTCM3, [1-9]\d* bad", out.stdout), out.stdout   # the frame with the wrong CRC-24Q


def test_replay_of_a_finished_survey_offers_the_fixed_position(app_exe, tmp_path):
    (tmp_path / "done.ubx").write_bytes(_navpvt_rtk(0) + _navsvin(300, 9_000, 3000, 1, 0) + _navpvt_rtk(1))
    out = _run(app_exe, tmp_path, "gnss.replay done.ubx 10", "gnss.watch 2", "gnss.status")
    assert "Base:      survey-in done after 300 s, mean accuracy 0.900 m; no RTCM3 output yet" in out.stdout, out.stdout
    assert "keep it: gnss.setup base fixed " in out.stdout, out.stdout


def test_setup_on_a_legacy_receiver(app_exe, tmp_path):
    (tmp_path / "m8.ubx").write_bytes(_monver("18.00") + _navpvt_rtk(0) + _navpvt_rtk(1))
    out = _run(app_exe, tmp_path, "gnss.replay m8.ubx 10", "gnss.watch 2", "gnss.setup rover")
    assert "ERROR: RTK needs a high-precision receiver (ZED-F9P, ZED-X20P: PROTVER 27 or later); this one has " \
           "PROTVER 18.00" in out.stdout, out.stdout
    out = _run(app_exe, tmp_path, "gnss.replay m8.ubx 10", "gnss.watch 2", "gnss.setup standard", timeout=90)
    assert "standard GNSS on a PROTVER 18.00 receiver (UBX-CFG-MSG" in out.stdout, out.stdout
    assert re.search(r"NMEA GGA\s+no answer", out.stdout), out.stdout   # a replay answers nothing


def test_setup_without_answer_fails(app_exe, tmp_path):
    (tmp_path / "rtk.ubx").write_bytes(b"".join(_navpvt_rtk(i) for i in range(2)))
    out = _run(app_exe, tmp_path, "gnss.replay rtk.ubx", "gnss.setup base survey", timeout=90)
    assert re.search(r"survey-in for at least 120 s and 2.00 m\s+no answer", out.stdout), out.stdout
    assert "ERROR: the receiver refused part of the setup" in out.stdout, out.stdout


@pytest.mark.parametrize("command", [
    "gnss.setup", "gnss.setup rover x", "gnss.setup base", "gnss.setup base later", "gnss.setup base survey 0",
    "gnss.setup base survey 60 200", "gnss.setup base survey 60 2 x", "gnss.setup base fixed 91 0 0",
    "gnss.setup base fixed 1 2", "gnss.setup base fixed 1 200 0", "gnss.setup base fixed 1 2 3 0",
    "gnss.setup base off now", "gnss.setup moon",
])
def test_setup_usage(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, "gnss.virtual", command)
    assert "usage" in out.stdout.lower(), out.stdout


def test_setup_needs_a_receiver(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "gnss.setup standard")
    assert "ERROR: no receiver open (use gnss.open)" in out.stdout, out.stdout
