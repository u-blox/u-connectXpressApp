"""Reboot a module, then unbond all in a new process, many times (docs/HID_FINDINGS.md issue 5).

The HID client cleanup does exactly this with separate app processes: `dev.reboot` (AT+CPWROFF, +STARTUP), the
port closes, a new process opens it again and runs AT+UBTUB and AT+UBTBDL. A NORA-B26 stopped answering the first
ATE0 of that second process. Each module takes the DUT role in turn; the other module is the bonded peer.
"""

import os
import re

import pytest
from hwutil import _LAST_REBOOT, _reboot, _run

pytestmark = [pytest.mark.hardware, pytest.mark.two_devices]

CYCLES = int(os.environ.get("UCX_UNBOND_CYCLES", "10"))


def _address(exe, port):
    out = _run(exe, port, "bt.addr").stdout
    match = re.search(r"^([0-9A-F]{2}(?::[0-9A-F]{2}){5})", out, re.M | re.I)
    assert match, out
    return match.group(1).upper()


@pytest.mark.parametrize("bonded", [False, True], ids=["plain", "bonded"])
@pytest.mark.parametrize("dut", ["a", "b"])
def test_module_answers_unbond_all_in_a_new_process_after_reboot(app_exe, port_a, port_b, dut, bonded):
    dut_port, peer_port = (port_a, port_b) if dut == "a" else (port_b, port_a)
    _reboot(app_exe, dut_port, peer_port)
    for port in (dut_port, peer_port):
        _run(app_exe, port, "bt.unbond all")
    peer_addr = _address(app_exe, peer_port) if bonded else None
    for cycle in range(1, CYCLES + 1):
        if bonded:
            peer_out = _run(app_exe, peer_port, "bt.pairing on", "bt.adv on", "bt.adv").stdout
            out = _run(app_exe, dut_port, f"bt.connect {peer_addr}", "bt.bond 0", "bt.disconnect").stdout
            assert "bonding succeeded" in out, f"cycle {cycle}: bond failed\n{out}\n--- peer ---\n{peer_out}"
            _run(app_exe, peer_port, "bt.unbond all")
        _reboot(app_exe, dut_port)
        reboot_out = _LAST_REBOOT.get(dut_port, "")
        assert "module rebooted" in reboot_out, f"cycle {cycle}: reboot failed\n{reboot_out[-1500:]}"
        out = _run(app_exe, dut_port, "bt.unbond all", "bt.bonds").stdout
        assert "bond(s) removed" in out, f"cycle {cycle}:\n{out}\n--- reboot ---\n{reboot_out[-1500:]}"
        assert not re.search(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}", out, re.M | re.I), \
            f"cycle {cycle}: bond still listed\n{out}"
