"""A Linux board on a serial console (the i.MX91 hub, UCX_HID_PEER) used as a Bluetooth LE test peer.

Logs in as root, copies tests/hardware/peer/ucxhid_peer.py to the board and runs it, so the module's HID
device (hid.setup) and HID host (hid.client) roles are tested against BlueZ instead of a Windows PC.
"""

import base64
import os
import re
import time
from pathlib import Path

PEER_SCRIPT = Path(__file__).resolve().parent / "peer" / "ucxhid_peer.py"
REMOTE_SCRIPT = "/tmp/ucxhid_peer.py"
RC_RE = re.compile(r"__UCX_RC=(\d+)__")


class LinuxPeer:
    def __init__(self, port, baud=None, user=None):
        import serial

        self.serial = serial.Serial(port, int(baud or os.environ.get("UCX_HID_PEER_BAUD", 115200)), timeout=0.2)
        self.user = user or os.environ.get("UCX_HID_PEER_USER", "root")
        self.port = port
        self.log = ""
        self._login()

    def _read(self):
        data = self.serial.read(4096).decode(errors="replace")
        self.log += data
        return data

    def _expect(self, pattern, timeout, send=None):
        """Reads until pattern shows up in the new output; returns the text read (or None on timeout)."""
        text = ""
        if send is not None:
            self.serial.write(send.encode())
        deadline = time.time() + timeout
        while time.time() < deadline:
            text += self._read()
            if re.search(pattern, text):
                return text
        return None

    def _login(self):
        self.serial.reset_input_buffer()
        text = self._expect(r"login:\s*$|[#$] $|=> $", 5, send="\n")
        if text is not None and re.search(r"=> $", text):
            # Stopped in U-Boot (a key pressed during autoboot): boot Linux and wait for it, without sending
            # anything more that would stop it again.
            self.serial.write(b"boot\n")
            text = self._expect(r"login:\s*$", 180)
            if text is None:
                raise RuntimeError(f"{self.port}: Linux did not boot from U-Boot:\n{self.log[-800:]}")
        if text is None or "login:" in text:
            # The console logs out when the port closes; answer the login prompt once it is there.
            if text is None and self._expect(r"login:\s*$", 10, send="\n") is None:
                raise RuntimeError(f"no login prompt on {self.port}:\n{self.log[-500:]}")
            if self._expect(r"[#$] $", 15, send=self.user + "\n") is None:
                raise RuntimeError(f"login as {self.user} on {self.port} failed:\n{self.log[-500:]}")
        # Echo off, kernel messages off the console, a plain prompt; every command ends with its exit code.
        self.run("stty -echo; dmesg -n 1 2>/dev/null; export PS1='# '")

    def run(self, command, timeout=30):
        """(exit code, output) of one shell command on the board."""
        text = self._expect(RC_RE.pattern, timeout, send=f"{command}; echo __UCX_RC=$?__\n")
        if text is None:
            self.serial.write(b"\x03")
            raise TimeoutError(f"'{command}' on {self.port} did not finish in {timeout} s:\n{self.log[-800:]}")
        out = text[:RC_RE.search(text).start()]
        out = out.replace(command + "; echo __UCX_RC=$?__", "")   # the echo of the first command
        return int(RC_RE.search(text).group(1)), out.replace("\r", "").strip()

    def upload(self, local=PEER_SCRIPT, remote=REMOTE_SCRIPT):
        data = base64.b64encode(Path(local).read_bytes()).decode()
        self.run(f"rm -f {remote}.b64")
        for i in range(0, len(data), 400):
            self.run(f"echo {data[i:i + 400]} >> {remote}.b64")
        rc, out = self.run(f"base64 -d {remote}.b64 > {remote} && rm {remote}.b64")
        if rc != 0:
            raise RuntimeError(f"upload to {self.port} failed: {out}")

    def start(self, args):
        """Starts the peer script in the foreground of the console; read its lines with expect/finish."""
        self.output = ""
        self.serial.write(f"python3 -u {REMOTE_SCRIPT} {args}; echo __UCX_RC=$?__\n".encode())

    def expect(self, pattern, timeout):
        """Waits for a line of the running peer script; returns the match or None (output kept in .output)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.output += self._read().replace("\r", "")
            match = re.search(pattern, self.output, re.M)
            if match:
                return match
            if RC_RE.search(self.output):
                return None
        return None

    def finish(self, timeout):
        """Waits for the peer script to end; returns (exit code or None, everything it printed)."""
        match = self.expect(RC_RE.pattern, timeout)
        if match is None:
            self.serial.write(b"\x03")
            self.expect(RC_RE.pattern, 5)
        rc = RC_RE.search(self.output)
        return (int(rc.group(1)) if rc else None), self.output.replace("\r", "")

    def bluetooth_reset(self, forget=()):
        """Removes the given devices (names or addresses) and power-cycles the controller for a clean start."""
        for target in forget:
            self.run(f"bluetoothctl remove {target} >/dev/null 2>&1")
        # a bluetoothd restart drops GATT applications left by earlier runs, so handles start low again
        self.run("systemctl restart bluetooth; sleep 2; bluetoothctl power on >/dev/null", timeout=30)

    def address(self):
        rc, out = self.run("btmgmt info | sed -n 's/.*addr \\([0-9A-F:]*\\).*/\\1/p' | head -1")
        return out.strip().splitlines()[-1] if rc == 0 and out.strip() else ""

    def close(self):
        try:
            self.serial.write(b"\x03")
            time.sleep(0.2)
            self.serial.write(b"exit\n")
        finally:
            self.serial.close()
