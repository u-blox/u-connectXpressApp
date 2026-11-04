"""HID in both roles against a Linux BLE peer (BlueZ on the i.MX91 hub, UCX_HID_PEER), not the Windows PC.

Device role: module A runs hid.setup and the peer is the HID host; every report the module sends is checked
byte for byte on the peer. Host role: the peer is a BLE keyboard and module A reads it with hid.client.
Every test starts with no bonds on either side, so a PC that once paired with the module cannot reconnect
and take the link; the tests also check that the peer is the only connection.
"""

import queue
import re
import subprocess
import threading
import time

import pytest
from hwutil import _args, _reboot, _run

pytestmark = [pytest.mark.hardware, pytest.mark.hid_peer]

KBD_NAME = "ucx-kbd"


class Shell:
    """Module A in an interactive app shell: send commands, wait for their output."""

    def __init__(self, exe, port, *commands):
        self.proc = subprocess.Popen(_args(exe, port, *commands) + ["--shell"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        self.lines = queue.Queue()
        self.text = ""
        threading.Thread(target=lambda: [self.lines.put(line) for line in self.proc.stdout], daemon=True).start()

    def wait_for(self, pattern, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            match = re.search(pattern, self.text, re.M)
            if match:
                return match
            try:
                self.text += self.lines.get(timeout=0.5)
            except queue.Empty:
                if self.proc.poll() is not None:
                    break
        return re.search(pattern, self.text, re.M)

    def command(self, command, done, timeout=90):
        """Runs one shell command and returns its output once `done` shows up after it."""
        start = len(self.text)
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        deadline = time.time() + timeout
        while time.time() < deadline and not re.search(done, self.text[start:], re.M):
            try:
                self.text += self.lines.get(timeout=0.5)
            except queue.Empty:
                pass
        return self.text[start:]

    def close(self):
        try:
            self.proc.stdin.write("exit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            self.proc.kill()


def _module_address(exe, port):
    out = _run(exe, port, "bt.addr").stdout
    match = re.search(r"^([0-9A-F]{2}(?::[0-9A-F]{2}){5})", out, re.M | re.I)
    assert match, out
    return match.group(1).upper()


def _clean(exe, port, peer, module_addr):
    """No bonds on either side and a freshly rebooted module."""
    peer.bluetooth_reset(forget=[module_addr])
    _reboot(exe, port)
    out = _run(exe, port, "bt.unbond all", "bt.bonds")
    assert out.returncode == 0, out.stdout


def _reports(peer_output):
    """[(uuid, report reference, bytes)] of every REPORT line the peer printed."""
    return [(u, ref, bytes.fromhex(h)) for u, ref, h in re.findall(r"^REPORT (\w+) (\S+) ([0-9a-f]*)$",
                                                                     peer_output, re.M)]


def _start_host(exe, port, peer, module_addr, keep=False):
    """Module A runs hid.setup in a shell and the peer pairs as its host; returns the shell once subscribed."""
    shell = Shell(exe, port, "hid.setup")
    if not shell.wait_for(r"HID service active", 60):
        shell.close()
        pytest.fail(f"hid.setup failed:\n{shell.text[-1500:]}")
    peer.start(f"host {module_addr} 120{' keep' if keep else ''}")
    ready = peer.expect(r"^READY (\S+) (\d+)", 90)
    if not ready:
        shell.close()
        pytest.fail(f"the peer did not connect as HID host:\n{peer.output[-1500:]}\nmodule:\n{shell.text[-1500:]}")
    assert int(ready.group(2)) >= 3, peer.output   # keyboard, mouse and media input reports at least
    assert shell.wait_for(r"\[HID\] keyboard input report notifications enabled", 30), shell.text[-1500:]
    return shell


def _assert_only_peer(shell, peer_addr):
    """The peer is the one and only link (no PC or phone reconnected to the module); returns its handle."""
    out = shell.command("bt.list", r"\d+ connection\(s\)")
    assert "1 connection(s)" in out and peer_addr in out.upper(), out
    return int(re.search(r"connection (\d+):", out).group(1))


def _disconnect(shell, handle):
    out = shell.command(f"bt.disconnect {handle}", r"connection \d+ closed|ERROR")
    assert f"connection {handle} closed" in out, out


def test_hid_device_reports_reach_a_linux_host(app_exe, port_a, hid_peer):
    module_addr = _module_address(app_exe, port_a)
    peer_addr = hid_peer.address()
    _clean(app_exe, port_a, hid_peer, module_addr)
    shell = _start_host(app_exe, port_a, hid_peer, module_addr)
    try:
        handle = _assert_only_peer(shell, peer_addr)
        status = shell.command("hid.status", r"media\s+handle")
        assert re.search(r"keyboard\s+handle \d+, CCCD \d+, notifications on", status), status
        assert "protocol report" in status, status
        for command, done in (("hid.text Hello1", r"typed 6 character"), ("hid.key a ctrl shift", r"key sent"),
                              ("hid.mouse 10 -5 1", r"mouse moved"), ("hid.click right", r"clicked"),
                              ("hid.media volup", r"volup sent")):
            out = shell.command(command, done + "|ERROR")
            assert re.search(done, out), f"{command}:\n{out}"
        # The media release goes out 50 ms after the press; disconnecting before it arrives would drop it.
        assert hid_peer.expect(r"^REPORT 2a4d 0301 0000$", 10), hid_peer.output
        _disconnect(shell, handle)
        rc, peer_out = hid_peer.finish(60)
    finally:
        shell.close()
    assert rc == 0 and "INFO disconnected" in peer_out, peer_out
    reports = _reports(peer_out)
    keyboard = [d for u, ref, d in reports if ref == "0101"]
    mouse = [d for u, ref, d in reports if ref == "0201"]
    media = [d for u, ref, d in reports if ref == "0301"]
    pressed = [(d[0], d[2]) for d in keyboard if any(d)]
    # H (shift), e, l, l, o, 1, then ctrl+shift+a; every press is followed by an all-zero release.
    assert pressed == [(2, 0x0B), (0, 0x08), (0, 0x0F), (0, 0x0F), (0, 0x12), (0, 0x1E), (3, 0x04)], peer_out
    assert len(keyboard) == 2 * len(pressed) and all(len(d) == 8 for d in keyboard), peer_out
    assert mouse == [bytes([0, 10, 0xFB, 1]), bytes([2, 0, 0, 0]), bytes(4)], peer_out
    assert media == [bytes([0xE9, 0x00]), bytes(2)], peer_out


def test_hid_device_bonded_host_reconnects_after_reboot(app_exe, port_a, hid_peer):
    """The host bonds once; after a module reboot it reconnects on the stored bond and input still arrives."""
    module_addr = _module_address(app_exe, port_a)
    peer_addr = hid_peer.address()
    _clean(app_exe, port_a, hid_peer, module_addr)
    shell = _start_host(app_exe, port_a, hid_peer, module_addr, keep=True)
    try:
        bonds = shell.command("bt.bonds", r"\d+ bonded device\(s\)")
        assert "1 bonded device(s)" in bonds and peer_addr in bonds.upper(), bonds
        hid_peer.serial.write(b"\x03")   # stop the peer script; the link stays up until the module reboots
        hid_peer.finish(10)
    finally:
        shell.close()

    _reboot(app_exe, port_a)
    bonds = _run(app_exe, port_a, "bt.bonds").stdout
    assert "1 bonded device(s)" in bonds and peer_addr in bonds.upper(), f"bond lost in reboot:\n{bonds}"
    shell = _start_host(app_exe, port_a, hid_peer, module_addr, keep=True)
    try:
        assert "ERROR" not in hid_peer.output, hid_peer.output
        handle = _assert_only_peer(shell, peer_addr)
        out = shell.command("hid.text ok", r"typed 2 character|ERROR")
        assert "typed 2 character" in out, out
        assert hid_peer.expect(r"^REPORT 2a4d 0101 0{16}\n(?:.*\n)*?REPORT 2a4d 0101 0{16}$", 10), hid_peer.output
        _disconnect(shell, handle)
        rc, peer_out = hid_peer.finish(60)
    finally:
        shell.close()
        _clean(app_exe, port_a, hid_peer, module_addr)
    pressed = [d[2] for u, ref, d in _reports(peer_out) if ref == "0101" and any(d)]
    assert pressed == [0x12, 0x0E], peer_out   # o, k


@pytest.mark.parametrize("protocol", ["report", "boot"])
def test_hid_client_reads_a_linux_keyboard(app_exe, port_a, hid_peer, protocol):
    """The peer is a BLE keyboard; the module connects, bonds, subscribes and decodes the typed text."""
    module_addr = _module_address(app_exe, port_a)
    peer_addr = hid_peer.address()
    _clean(app_exe, port_a, hid_peer, module_addr)
    hid_peer.start(f"kbd {KBD_NAME} 120 hello 42")
    if not hid_peer.expect(r"^READY ", 30):
        pytest.fail(f"the peer keyboard did not start:\n{hid_peer.output}")
    client = f"hid.client 0{' boot' if protocol == 'boot' else ''}"
    try:
        out = _run(app_exe, port_a, f"bt.connect {peer_addr}", "bt.list", client, "gatt.listen 10",
                   "hid.client off", "bt.disconnect 0", timeout=180)
    finally:
        rc, peer_out = hid_peer.finish(60)
        _clean(app_exe, port_a, hid_peer, module_addr)
    assert out.returncode == 0, f"{out.stdout}\npeer:\n{peer_out}"
    assert "1 connection(s)" in out.stdout, out.stdout
    assert f"INFO subscribed {protocol}" in peer_out and f"SENT {protocol}" in peer_out, peer_out
    expected = "keyboard input (boot)" if protocol == "boot" else "keyboard input, report ID 1"
    assert expected in out.stdout, out.stdout
    keys = re.findall(r"^\[HID\] keys: (.+)$", out.stdout, re.M)
    assert keys == ["h", "e", "l", "l", "o", "space", "4", "2"], out.stdout
    assert "HID host stopped" in out.stdout, out.stdout
