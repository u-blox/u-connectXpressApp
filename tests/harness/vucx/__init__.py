"""vucx: a virtual u-connectXpress module on a pseudo terminal (Linux/WSL).

Empty by default: it only answers the AT setup commands. A YAML profile decides what else it can do:

    identity: {manufacturer: u-blox, model: NORA-W36, version: 3.5.0-0, serial: "0000"}
    commands:                       # exact command or fnmatch pattern -> response lines ("OK" is not added)
      "AT+UWSC?": ["+UWSC:0,1", "OK"]
    urcs:                           # lines sent after the response to a matching command
      "AT+CPWROFF": ["+STARTUP"]
    include: [wifi.yaml]            # profiles merged in first (this one wins)
    default: ["OK"]                 # answer to unmatched commands (default ERROR)

Run alone: python -m harness.vucx tests/harness/vucx/basic.yaml  (prints the port to open)
"""

import fnmatch
import os
import sys
import threading
import time
from pathlib import Path

PROFILES = Path(__file__).parent

SETUP = {"AT": ["OK"], "ATE0": ["OK"], "ATE1": ["OK"], "AT+USYEE=1": ["OK"]}


def load(profile):
    """Profile from a dict, or a .yaml/.json file path."""
    if isinstance(profile, dict):
        return profile
    path = Path(profile)
    if not path.is_absolute() and not path.exists():
        path = PROFILES / path
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
        import json

        return json.loads(text)
    import yaml

    p = yaml.safe_load(text) or {}
    merged = {}
    for name in p.get("include") or []:
        base = load(name)
        for key, value in base.items():
            merged[key] = {**merged.get(key, {}), **value} if isinstance(value, dict) else value
    for key, value in p.items():
        merged[key] = {**merged.get(key, {}), **value} if isinstance(value, dict) else value
    return merged


class Vucx:
    def __init__(self, profile=None):
        p = load(profile) if profile is not None else {}
        self.commands = dict(SETUP)
        ident = p.get("identity") or {}
        for cmd, key in (("AT+GMI", "manufacturer"), ("AT+GMM", "model"), ("AT+GMR", "version"),
                         ("AT+GSN", "serial")):
            if key in ident:
                self.commands[cmd] = [f'"{ident[key]}"', "OK"]
        self.commands.update(p.get("commands") or {})
        self.urcs = dict(p.get("urcs") or {})
        self.default = p.get("default") or ["ERROR"]
        self.log = []   # every command received, for the tests
        self._fd = None
        self._stop = threading.Event()

    def _lookup(self, table, cmd):
        if cmd in table:
            return table[cmd]
        for pattern, lines in table.items():
            if fnmatch.fnmatchcase(cmd, pattern):
                return lines
        return None

    def _send(self, lines):
        os.write(self._fd, "".join(f"\r\n{line}" for line in lines).encode() + b"\r\n")

    def handle(self, cmd):
        self.log.append(cmd)
        self._send(self._lookup(self.commands, cmd) or self.default)
        urcs = self._lookup(self.urcs, cmd)
        if urcs:
            time.sleep(0.05)
            self._send(urcs)

    def start(self):
        """Opens the pty and serves it in a thread; returns the port name for the app."""
        import tty

        self._fd, slave = os.openpty()
        tty.setraw(self._fd)
        self._slave = slave
        self.port = os.ttyname(slave)
        threading.Thread(target=self._serve, daemon=True).start()
        return self.port

    def _serve(self):
        import select

        buf = b""
        while not self._stop.is_set():
            if not select.select([self._fd], [], [], 0.1)[0]:
                continue
            try:
                buf += os.read(self._fd, 4096)
            except OSError:
                return
            while b"\r" in buf:
                line, buf = buf.split(b"\r", 1)
                line = line.strip(b"\n").decode(errors="replace").strip()
                if line:
                    self.handle(line)

    def stop(self):
        self._stop.set()
        for fd in (self._fd, self._slave):
            try:
                os.close(fd)
            except OSError:
                pass


class Feeder:
    """A pty that repeats a byte stream (a virtual GNSS receiver or IMU) and records what it receives."""

    def __init__(self, data, interval=0.1):
        self.data = data.encode() if isinstance(data, str) else data
        self.interval = interval
        self.received = b""
        self._stop = threading.Event()

    def start(self):
        import tty

        self._fd, self._slave = os.openpty()
        tty.setraw(self._fd)
        tty.setraw(self._slave)
        self.port = os.ttyname(self._slave)
        threading.Thread(target=self._serve, daemon=True).start()
        return self.port

    def _serve(self):
        import select

        while not self._stop.is_set():
            try:
                os.write(self._fd, self.data)
                if select.select([self._fd], [], [], self.interval)[0]:
                    self.received += os.read(self._fd, 4096)
            except OSError:
                return

    stop = Vucx.stop


if __name__ == "__main__":
    v = Vucx(sys.argv[1] if len(sys.argv) > 1 else None)
    print(v.start(), flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        v.stop()
