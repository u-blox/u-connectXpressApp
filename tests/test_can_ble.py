"""can.bridge ble gatt / ble slcan against vucx: a virtual phone writes to the CAN GATT service or SLCAN text to NUS
(needs a pty: Linux/WSL)."""

import subprocess
import sys
import time

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]

CONNECT = "+UEBTC:0,C0FFEE001122p"
# ts 0, id 0x123, dlc 2, data 11 22
RECORD = "00000000" "23010000" "02" "1122000000000000"


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    yield module
    module.stop()


def _override(table, entries):
    """Puts entries in front of the profile's own wildcards (vucx takes the first matching pattern)."""
    rest = {k: v for k, v in table.items() if k not in entries}
    table.clear()
    table.update(entries)
    table.update(rest)


def _drive(app_exe, module, first, until, timeout=20):
    """Starts the app's shell with `first`, waits until until(log) is true, then stops the bridge."""
    proc = subprocess.Popen([str(app_exe), module.port, "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace")
    try:
        proc.stdin.write(first)
        proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline and not until(module.log):
            time.sleep(0.2)
        time.sleep(0.5)
        out, _ = proc.communicate("can.bridge status\ncan.status\ncan.bridge tcp\ncan.bridge off\ncan.bridge status\n",
                                  timeout=30)
    finally:
        proc.kill()
    return out


def _notified(log, handle):
    prefix = f"AT+UBTGNS=0,{handle},"
    return [bytes.fromhex(c[len(prefix):]) for c in log if c.startswith(prefix)]


def test_gatt_bridge_records_config_and_status(app_exe, nora):
    entries = {f"AT+UBTGC=81?A000{n}*": [f"+UBTGC:{rsp}", "OK"]
               for n, rsp in (("2", "60,0"), ("3", "62,63"), ("4", "65,0"), ("5", "67,68"))}
    entries["AT+UBTCST=*"] = ["+UBTCST:3,247", "OK"]
    _override(nora.commands, entries)
    # the phone connects, subscribes to TX and Status, writes a record; after the echo it sets 250 kbit/s listen-only
    nora.urcs["AT+UBTGSA"] = {"after": 1, "times": 1, "lines": [
        CONNECT, "+UEBTGCW:0,63,0100,0", "+UEBTGCW:0,68,0100,0", f"+UEBTGCW:0,60,{RECORD},0"]}
    nora.urcs["AT+UBTGNS=0,62,*"] = {"after": 0.5, "times": 1, "lines": ["+UEBTGCW:0,65,90D0030002,0"]}
    nora.start()
    out = _drive(app_exe, nora, "can.bridge ble gatt\n",
                 lambda log: "AT+UBTGAV=65,90D0030002" in log and any(c.startswith("AT+UBTGAV=67,") for c in log))
    assert "can bridge: BLE CAN service 81ea0001" in out, out[-3000:]
    tx = _notified(nora.log, 62)
    assert len(tx) == 1 and len(tx[0]) == 17 and tx[0][4:] == bytes.fromhex(RECORD)[4:], nora.log
    status = [c for c in nora.log if c.startswith("AT+UBTGAV=67,")]
    assert any(s.startswith("AT+UBTGAV=67,0100000001000000") for s in status), status   # bus rx 1, tx 1
    assert any(c.startswith("AT+UBTGNS=0,67,") for c in nora.log), nora.log
    assert "bridge: ble gatt, connected (conn 0, 244 B per notification)" in out, out[-3000:]
    assert "listen-only, 250000 bit/s" in out and "bridge ble gatt" in out, out[-3000:]
    assert "ERROR: the ble gatt bridge is on (can.bridge off first)" in out, out[-3000:]
    assert "bridge stopped" in out, out[-3000:]


def test_gatt_bridge_without_client_counts_lost_frames(app_exe, nora):
    _override(nora.commands, {"AT+UBTGC=81?A000*": ["+UBTGC:70,71", "OK"], "AT+UBTCL": ["OK"]})
    nora.urcs["AT+UBTGSA"] = []
    nora.start()
    out = _drive(app_exe, nora, "can.bridge ble gatt\ncan.send 0x10 01\n", lambda log: False, timeout=2)
    assert "bridge: ble gatt, no client" in out and "not delivered 17 B" in out, out[-3000:]
    assert not _notified(nora.log, 70), nora.log


def _hex(text):
    return text.encode().hex().upper()


def test_slcan_bridge_over_nus(app_exe, nora):
    _override(nora.commands, {"AT+UBTCST=*": ["+UBTCST:3,23", "OK"]})
    open_and_send = _hex("V\rN\rF\rS5\rZ1\rZ0\rO\rt1232AABB\r")
    close_and_try = _hex("C\rt1230\rS9\rQ\rT1FFFFFFF0\r")
    # NUS rx and tx are both handle 42 in this profile; the phone opens at 250 kbit/s, sends a frame, later closes
    nora.urcs["AT+UBTGSA"] = {"after": 1, "times": 1, "lines": [CONNECT, f"+UEBTGCW:0,42,{open_and_send},0"]}
    nora.urcs["AT+UBTGNS=*"] = {"after": 1, "times": 1, "lines": [f"+UEBTGCW:0,42,{close_and_try},0"]}
    nora.start()
    out = _drive(app_exe, nora, "gatt.server.setup nus\ncan.bridge ble slcan\n",
                 lambda log: sum(c.startswith("AT+UBTGNS=0,42,") for c in log) >= 3)
    assert "SLCAN over the Nordic UART Service" in out, out[-3000:]
    text = b"".join(_notified(nora.log, 42)).decode()
    assert all(len(p) <= 20 for p in _notified(nora.log, 42)), nora.log   # MTU 23
    assert text.startswith("V1013\rNUCX1\rF00\r\r\r\r\rz\rt1232AABB\r"), repr(text)
    assert text.endswith("\r\a\a\a\a"), repr(text)   # closed: no frames, no bit rate 9, Q unknown
    assert "250000 bit/s" in out, out[-3000:]


def test_slcan_frame_codec_rejects_bad_frames(app_exe, nora):
    _override(nora.commands, {"AT+UBTCST=*": ["+UBTCST:3,247", "OK"]})
    bad = "t12\rt1239\rtXYZ0\rt8000\rt1232001\rt12320G11\rL\rt1230\r" + "x" * 50 + "\r"
    lines = _hex("O\r" + bad)
    nora.urcs["AT+UBTGSA"] = {"after": 1, "times": 1, "lines": [CONNECT, f"+UEBTGCW:0,42,{lines},0"]}
    nora.start()
    out = _drive(app_exe, nora, "gatt.server.setup nus\ncan.bridge ble slcan\n",
                 lambda log: any(c.startswith("AT+UBTGNS=0,42,") for c in log))
    text = b"".join(_notified(nora.log, 42)).decode()
    # O opens; six malformed frames; L while open; a valid frame; an overlong line
    assert text == "\r" + "\a" * 6 + "\a" + "z\r" + "\a" + "t1230\r", repr(text)
    assert "bus open" in out, out[-3000:]
