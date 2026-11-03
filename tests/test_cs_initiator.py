"""Channel Sounding initiator (NORA-B26 cs_initiator evaluation firmware) against vucx: the distance comes from
the module's distance events, PCT events carry binary data (needs a pty: Linux/WSL)."""

import struct
import subprocess
import sys

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    yield module
    module.stop()


def _override(table, entries):
    rest = {k: v for k, v in table.items() if k not in entries}
    table.clear()
    table.update(entries)
    table.update(rest)


def _shell(exe, port, commands, timeout=40):
    return subprocess.run([str(exe), port, "--shell"], input="\n".join(commands) + "\n",
                          capture_output=True, text=True, timeout=timeout, errors="replace")


def _pct(*entries):
    return b"".join(struct.pack("<Bii", ch, i, q) for ch, i, q in entries)


def test_initiator_mode_and_settings(app_exe, nora):
    _override(nora.commands, {"AT+UBTCSM?": ["+UBTCSM:2", "OK"], "AT+UBTCSRT?": ["+UBTCSRT:20", "OK"],
                              "AT+UBTCSKF?": ["+UBTCSKF:1,120,1", "OK"], "AT+UBTCSIQ?": ["+UBTCSIQ:0", "OK"]})
    out = _shell(app_exe, nora.port, ["bt.cs initiator", "bt.cs", "bt.cs.rate", "bt.cs.rate 40", "bt.cs.filter",
                                      "bt.cs.filter off", "bt.cs.filter 5 200", "bt.cs.filter default", "bt.cs.phase",
                                      "bt.cs.phase on", "bt.cs.phase ref 1000"])
    assert out.returncode == 0, out.stdout
    assert "Channel Sounding initiator" in out.stdout and "every 20 connection" in out.stdout, out.stdout
    assert "Kalman filter on: Q 0.001, R 0.120" in out.stdout and "Kalman filter off" in out.stdout, out.stdout
    for cmd in ("AT+UBTCSM=2", "AT+UBTCSRT=40", "AT+UBTCSKF=1,120,0", "AT+UBTCSKF=5,200,1", "AT+UBTCSKF=1,120,1",
                "AT+UBTCSIQ=1", "AT+UBTCSREF=2"):
        assert cmd in nora.log, (cmd, nora.log)


@pytest.mark.parametrize("args", ["bt.cs.rate 0", "bt.cs.rate 70000", "bt.cs.filter 2000 120", "bt.cs.filter 1 5",
                                  "bt.cs.filter 1", "bt.cs.phase ref 7", "bt.cs.range", "bt.cs.range x",
                                  "bt.cs.range 0 0", "bt.cs.range 0 maybe", "bt.cs both"])
def test_usage(app_exe, nora, args):
    out = _shell(app_exe, nora.port, [args])
    assert out.returncode != 0 and "usage" in out.stdout.lower(), out.stdout
    assert not any(c.startswith("AT+UBTCS") for c in nora.log), nora.log


def test_range_prints_the_module_distance(app_exe, nora):
    pct = _pct((2, 100, -100), (3, 7, 8))
    nora.urcs["AT+UBTCSP=0,1"] = {"after": 0.3, "times": 1, "lines": [
        "+UEBTCSS:0,2,0", "+UEBTCSM:0,1234", "+UEBTCSM:1,9999", "+UEBTCSM:0,1500",
        ("+UEBTCSIQ:0,0,2", pct),           # complete
        ("+UEBTCSIQ:0,1,9", pct[:13]),      # claims 9 channels, holds 1.4
        "+UEBTCSIQ:0,x", "+UEBTCSM:garbage", "+UEBTCSM:0,1600"]}
    _override(nora.commands, {"AT+UBTCSIQ?": ["+UBTCSIQ:1", "OK"]})
    out = _shell(app_exe, nora.port, ["bt.cs.range 0 2", "bt.cs.phase"])
    assert out.returncode == 0, out.stdout
    assert "1.23 m" in out.stdout and "1.50 m" in out.stdout and "1.60 m" in out.stdout, out.stdout
    assert "9.99" not in out.stdout, out.stdout   # another connection
    assert "3 readings, 1.23..1.60 m" in out.stdout, out.stdout
    assert "PCT reporting on, 2 reports so far" in out.stdout, out.stdout
    assert "AT+UBTCSP=0,1" in nora.log and "AT+UBTCSP=0,0" in nora.log, nora.log


def test_range_without_distance_or_bond(app_exe, nora):
    out = _shell(app_exe, nora.port, ["bt.cs.range 0 1"])
    assert out.returncode != 0 and "no distance in 1 s" in out.stdout, out.stdout
    _override(nora.commands, {"AT+UBTCSP=*": ["ERROR"]})
    out = _shell(app_exe, nora.port, ["bt.cs.range 0 on"])
    assert out.returncode != 0 and "cs_initiator evaluation firmware" in out.stdout, out.stdout
    out = _shell(app_exe, nora.port, ["bt.cs.range 5"])   # no such connection: nothing to bond with
    assert out.returncode != 0 and "connection 5 not found" in out.stdout, out.stdout
    assert "initiator mode (bt.cs initiator)" in out.stdout, out.stdout


def test_range_bonds_when_the_module_refuses(app_exe, nora):
    _override(nora.commands, {"AT+UBTCSP=0,1": [["ERROR"], ["OK"]]})
    nora.urcs["AT+UBTB=*"] = ["+UEBTB:C0FFEE001122p,0"]
    nora.urcs["AT+UBTCSP=0,1"] = {"after": 0.3, "times": 2, "lines": ["+UEBTCSM:0,800"]}
    out = _shell(app_exe, nora.port, ["bt.cs.range 0 2"])
    assert out.returncode == 0 and "bonding with the reflector" in out.stdout, out.stdout
    assert "bonding succeeded" in out.stdout and "0.80 m" in out.stdout, out.stdout
    assert any(c.startswith("AT+UBTB=C0FFEE001122") for c in nora.log), nora.log


def test_bond_by_connection_or_address(app_exe, nora):
    nora.urcs["AT+UBTB=C0FFEE001122*"] = ["+UEBTB:C0FFEE001122p,0"]
    out = _shell(app_exe, nora.port, ["bt.bond 0", "bt.bond C0:FF:EE:00:11:22"])
    assert out.returncode == 0 and out.stdout.count("bonding succeeded") == 2, out.stdout
    assert sum(c.startswith("AT+UBTB=C0FFEE001122") for c in nora.log) == 2, nora.log


def test_bond_failures(app_exe, nora):
    nora.urcs["AT+UBTB=*"] = []
    out = _shell(app_exe, nora.port, ["bt.bond 7"])
    assert out.returncode != 0 and "connection 7 not found" in out.stdout, out.stdout
    out = _shell(app_exe, nora.port, ["bt.bond 0 1"])
    assert out.returncode != 0 and "no bond result in 1 s" in out.stdout, out.stdout
    for args in ("bt.bond", "bt.bond 0 0", "bt.bond nonsense", "bt.bond 0 1 2"):
        out = _shell(app_exe, nora.port, [args])
        assert out.returncode != 0 and "usage" in out.stdout.lower(), (args, out.stdout)
