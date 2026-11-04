"""Isolate the open HID findings (docs/HID_FINDINGS.md) with two modules and plain GATT, no HID host or Linux peer.

Module A serves Battery (bas.level), optionally behind many other services so its handles are high. Module B
connects by address, optionally bonds, subscribes, receives notifications from A and unsubscribes again:

- issue 2: notifications sent by the server must reach the client as URCs, also at high handles on an encrypted link
- issue 3: switching notifications off after bonding must not fail (ERROR:114 seen with hid.client off)
"""

import queue
import re
import subprocess
import threading
import time

import pytest
from hwutil import _args, _reboot, _run, _stop

pytestmark = [pytest.mark.hardware, pytest.mark.two_devices]

FILLER = ["hrs", "cts", "ess", "lns", "aio", "sps", "wps", "ras"]
NOTIFICATIONS = 3


def _address(exe, port):
    out = _run(exe, port, "bt.addr").stdout
    match = re.search(r"^([0-9A-F]{2}(?::[0-9A-F]{2}){5})", out, re.M | re.I)
    assert match, out
    return match.group(1).upper()


class _Reader:
    """Collects the output of a process in the background and waits for patterns in it."""

    def __init__(self, proc):
        self.text = ""
        self.lines = queue.Queue()
        threading.Thread(target=lambda: [self.lines.put(line) for line in proc.stdout], daemon=True).start()

    def wait(self, pattern, seconds):
        deadline = time.time() + seconds
        while not re.search(pattern, self.text) and time.time() < deadline:
            try:
                self.text += self.lines.get(timeout=1)
            except queue.Empty:
                pass
        return re.search(pattern, self.text)

    def drain(self):
        while not self.lines.empty():
            self.text += self.lines.get()
        return self.text


@pytest.mark.parametrize("bonded", [False, True], ids=["plain", "bonded"])
@pytest.mark.parametrize("handles", ["low", "high"])
def test_gatt_notify_and_unsubscribe_between_two_modules(app_exe, port_a, port_b, handles, bonded):
    _reboot(app_exe, port_a, port_b)
    for port in (port_a, port_b):
        _run(app_exe, port, "bt.unbond all")
    server_addr = _address(app_exe, port_a)
    services = (FILLER if handles == "high" else []) + ["bas"]
    server = subprocess.Popen(_args(app_exe, port_a, *[f"gatt.server.{s}" for s in services], "gatt.server.list")
                              + ["--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, errors="replace")
    srv = _Reader(server)
    client = None
    try:
        level = srv.wait(r"bas\.level\s+value handle (\d+)", 90)
        assert level, srv.text
        handle = int(level.group(1))
        steps = [f"bt.connect {server_addr}"] + (["bt.bond 0"] if bonded else []) + [
            f"gatt.subscribe 0 {handle} notify", "gatt.listen 15", f"gatt.subscribe 0 {handle} off", "bt.disconnect"]
        client = subprocess.Popen(_args(app_exe, port_b, *steps), stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, errors="replace")
        cli = _Reader(client)
        assert cli.wait(r"notifications enabled", 120), f"client:\n{cli.text}\nserver:\n{srv.drain()}"
        for value in range(NOTIFICATIONS):
            server.stdin.write(f"gatt.server.notify 0 bas.level {0x40 + value:02x}\n")
            server.stdin.flush()
            time.sleep(1)
        client.wait(timeout=120)
        out = cli.drain()
    finally:
        _stop(server)
        if client:
            client.kill()
    context = f"handle {handle}, bonded={bonded}\nclient:\n{out}\nserver:\n{srv.drain()}"
    if bonded:
        assert "bonding succeeded" in out, context
    got = re.findall(rf"\[notify conn=\d+ handle=0x{handle:04X}\]", out, re.I)
    assert len(got) == NOTIFICATIONS, context                       # issue 2
    assert re.search(rf"off on handle 0x{handle:04X}", out, re.I), context  # issue 3
    assert client.returncode == 0, context
