"""vucx: a virtual u-connectXpress module on a pseudo terminal (Linux/WSL).

Empty by default: it only answers the AT setup commands. A YAML profile decides what else it can do:

    identity: {manufacturer: u-blox, model: NORA-W36, version: 3.5.0-0, serial: "0000"}
    commands:                       # exact command or fnmatch pattern -> response lines ("OK" is not added)
      "AT+UWSC?": ["+UWSC:0,1", "OK"]
    urcs:                           # lines sent after the response to a matching command
      "AT+CPWROFF": ["+STARTUP"]
      "AT+USODL=*": {after: 2, lines: ["NO CARRIER"], times: 1}   # later, and only the first n times
    include: [wifi.yaml]            # profiles merged in first (this one wins)
    default: ["OK"]                 # answer to unmatched commands (default ERROR)
    state:                          # commands that set state variables, for keys like "AT+X=* [page=login]"
      'AT+UHTCRP=0,"/login*"': {page: login}

A list of lists is answered in turn (then from the start again). A line with \x01 sends the rest as binary data
(SOH, 2-byte length, bytes), as the module does; commands with binary data (AT+X=... SOH len data) are matched
on the text before the SOH. State setters apply after the answer, so a command can answer from the old state.

    peer:                           # the connected BLE peer's GATT database, from handle `start` up
      start: 100
      services:
        - uuid: "180F"
          chars:                    # [uuid, properties R w W N I, value hex or text:..., {descriptor uuid: value}]
            - ["2A19", R N, "5A"]

becomes AT+UBTGPSD lines, one AT+UBTGSCD per service, AT+UBTGCDD per characteristic and AT+UBTGR per readable
value (a CCCD follows every notifiable value). Static, so the browser's virtual module has it too.

    sockets:                        # the network behind the module, run in Python (not in the browser)
      servers: {2101: ntrip, 5683: coap}   # remote port -> behaviour (echo for the rest; sink drops)
      accept: [8000]                # listening ports a client connects to (it echoes)

answers AT+USOCR/USOC/USOWB/USOSTB/USORB/USORFB/USOCL/USOL/USOST? with real per-socket state.
Only lines starting with AT are commands; anything else (data mode, a bridged stream) is logged and dropped, unless

    data:                           # fnmatch pattern on a data-mode line -> raw bytes sent back as they are
      "GET /*": "ICY 200 OK\\r\\n\\r\\n"   # an NTRIP caster behind a modem in transparent mode

Run alone: python -m harness.vucx tests/harness/vucx/basic.yaml  (prints the port to open)
As JSON for the browser's virtual module: python -m harness.vucx --json nora-w36.yaml > app/wasm/vnora.json
"""

import fnmatch
import json
import os
import re
import sys
import threading
import time
from pathlib import Path

from .. import corrections

PROFILES = Path(__file__).parent

SETUP = {"AT": ["OK"], "ATE0": ["OK"], "ATE1": ["OK"], "AT+USYEE=1": ["OK"]}

PROPS = {"R": 0x02, "w": 0x04, "W": 0x08, "N": 0x10, "I": 0x20}


def _hex(value):
    value = str(value)
    return value[5:].encode().hex().upper() if value.startswith("text:") else value.upper()


def peer_table(peer):
    """(service discovery lines, commands) for a peer GATT database; see the module docstring."""
    commands = {}
    services = []
    h = int(peer.get("start", 100))
    for svc in peer.get("services") or []:
        start = h
        h += 1
        decls = []     # (value handle, [(descriptor handle, uuid)])
        lines = []
        for ch in svc.get("chars") or []:
            uuid, props = str(ch[0]), sum(PROPS[c] for c in str(ch[1]) if c in PROPS)
            value = ch[2] if len(ch) > 2 else ""
            descs = []
            lines.append(f"+UBTGSCD:0,{h},{props:02X},{h + 1},{uuid}")
            val = h + 1
            h += 2
            if props & PROPS["R"]:
                commands[f"AT+UBTGR=0,{val}"] = [f"+UBTGR:0,{val},{_hex(value)}", "OK"]
            if props & (PROPS["N"] | PROPS["I"]):
                descs.append((h, "2902"))
                h += 1
            for duuid, dvalue in (ch[3] if len(ch) > 3 else {}).items():
                descs.append((h, str(duuid)))
                commands[f"AT+UBTGR=0,{h}"] = [f"+UBTGR:0,{h},{_hex(dvalue)}", "OK"]
                h += 1
            decls.append((val, descs))
        end = h - 1
        services.append(f"+UBTGPSD:0,{start},{end},{svc['uuid']}")
        commands[f"AT+UBTGSCD=0,{start},{end}"] = lines + ["OK"]
        for i, (val, descs) in enumerate(decls):
            last = decls[i + 1][0] - 2 if i + 1 < len(decls) else end
            commands[f"AT+UBTGCDD=0,{val},{last}"] = [f"+UBTGCDD:0,{val},{d},{u}" for d, u in descs] + ["OK"]
    return services, commands


def load(profile):
    """Profile from a dict, or a .yaml/.json file path."""
    if isinstance(profile, dict):
        return profile
    path = Path(profile)
    if not path.is_absolute() and not path.exists():
        path = PROFILES / path
    text = path.read_text(encoding="utf-8")
    if path.suffix == ".json":
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


def table(profile=None):
    """What the module answers: {commands, urcs, default}, with the setup and identity commands filled in."""
    p = load(profile) if profile is not None else {}
    commands = dict(SETUP)
    ident = p.get("identity") or {}
    for cmd, key in (("AT+GMI", "manufacturer"), ("AT+GMM", "model"), ("AT+GMR", "version"),
                     ("AT+GSN", "serial")):
        if key in ident:
            commands[cmd] = [f'"{ident[key]}"', "OK"]
    commands.update(p.get("commands") or {})
    if p.get("peer"):
        services, generated = peer_table(p["peer"])
        base = [line for line in commands.get("AT+UBTGPSD=*", []) if line != "OK"]
        commands["AT+UBTGPSD=*"] = base + services + ["OK"]
        commands.update(generated)
    return {"commands": commands, "urcs": dict(p.get("urcs") or {}), "state": dict(p.get("state") or {}),
            "data": dict(p.get("data") or {}), "default": p.get("default") or ["ERROR"]}


class Network:
    """The sockets behind the module (AT+USO*): TCP and UDP with an echo, NTRIP caster or CoAP server far end."""

    # The caster's stream: RTCM 1005, SPARTN (plain CRC-16, encrypted CRC-24, 32-bit time tag CRC-32, CRC-8),
    # a UBX-RXM-SPARTNKEY, then junk and an RTCM frame with a broken CRC, then a good one: RTCM 2 (1 bad)
    RTCM = (corrections.rtcm(bytes.fromhex("3ED00003") + bytes(15))
            + corrections.spartn(1, bytes(range(40)))
            + corrections.spartn(0, bytes(30), crc_type=2, auth_len=1)
            + corrections.spartn(2, b"\x55" * 12, crc_type=3, time_tag=0x12345678)
            + corrections.spartn(1, bytes(5), crc_type=0)
            + corrections.ubx(0x02, 0x36, bytes(16))
            + b"junk" + corrections.corrupt(corrections.rtcm(bytes(8)))
            + corrections.rtcm(bytes.fromhex("3ED00003") + bytes(15)))

    def __init__(self, cfg):
        self.servers = {int(k): v for k, v in (cfg.get("servers") or {}).items()}
        self.accept = {int(p) for p in cfg.get("accept") or []}
        self.socks = {}

    def _new(self, proto, port=0):
        n = next(i for i in range(64) if i not in self.socks)
        self.socks[n] = {"proto": proto, "port": port, "rx": [], "state": 0, "status": 1}
        return n

    def _serve(self, s, data, ip="192.168.1.10", port=None):
        kind = self.servers.get(s["port"], "echo")
        port = port or s["port"]
        if kind == "ntrip" and s["state"] == 0:
            s["state"] = 1
            s["rx"].append((ip, port, b"ICY 200 OK\r\n\r\n" + self.RTCM))
        elif kind == "coap" and len(data) >= 4:
            tkl, kind_ = data[0] & 0x0F, (data[0] >> 4) & 3
            head = bytes([0x40 | ((2 if kind_ == 0 else 1) << 4) | tkl, 0x45])
            s["rx"].append((ip, port, head + data[2:4 + tkl] + b"\xff21.5"))
        elif kind == "echo":
            s["rx"].append((ip, port, data))

    def handle(self, cmd, data):
        """(response, urcs) as lists of str or (text, bytes), or None when it is not a socket command."""
        m = re.fullmatch(r"AT\+USOCR=(\d+)(?:,\d+)?", cmd)
        if m:
            return [f"+USOCR:{self._new(int(m[1]))}", "OK"], []
        m = re.fullmatch(r'AT\+USOC=(\d+),"?([^",]*)"?,(\d+)', cmd)
        if m and int(m[1]) in self.socks:
            self.socks[int(m[1])].update(port=int(m[3]), status=2)
            return ["OK"], [f"+UESOC:{m[1]}"]
        m = re.fullmatch(r"AT\+USOWB=(\d+),?", cmd)
        if m and int(m[1]) in self.socks:
            s = self.socks[int(m[1])]
            self._serve(s, data)
            return [f"+USOWB:{m[1]},{len(data)}", "OK"], self._avail(m[1])
        m = re.fullmatch(r"AT\+USOSTB=(\d+),([\d.]+),(\d+),?", cmd)
        if m and int(m[1]) in self.socks:
            s = self.socks[int(m[1])]
            self._serve({**s, "port": int(m[3])}, data, m[2], int(m[3]))
            return [f"+USOSTB:{m[1]},{len(data)}", "OK"], self._avail(m[1])
        m = re.fullmatch(r"AT\+USOR(F?)B=(\d+),(\d+)", cmd)
        if m and int(m[2]) in self.socks:
            rx, size = self.socks[int(m[2])]["rx"], int(m[3])
            ip, port, chunk = rx.pop(0) if rx else ("0.0.0.0", 0, b"")
            if len(chunk) > size and not m[1]:
                rx.insert(0, (ip, port, chunk[size:]))
            chunk = chunk[:size]
            text = f"+USORFB:{m[2]},{ip},{port}" if m[1] else f"+USORB:{m[2]}"
            return [(text, chunk), "OK"], []
        m = re.fullmatch(r"AT\+USOCL=(\d+)", cmd)
        if m:
            return (["OK"], []) if self.socks.pop(int(m[1]), None) else (["ERROR"], [])
        m = re.fullmatch(r"AT\+USO[BL]=(\d+)(?:,(\d+))?", cmd)
        if m and int(m[1]) in self.socks:
            port = int(m[2] or 0)
            self.socks[int(m[1])].update(port=port, status=3 if cmd.startswith("AT+USOL") else 1)
            if cmd.startswith("AT+USOL") and port in self.accept:
                return ["OK"], [f"+UESOIC:{self._new(6, port)},192.168.1.10,{m[1]}"]
            return ["OK"], []
        if cmd == "AT+USOST?":
            return [f"+USOST:{n},{s['proto']},{s['status']}" for n, s in self.socks.items()] + ["OK"], []
        return None

    def _avail(self, n):
        rx = self.socks[int(n)]["rx"]
        return [f"+UESODA:{n},{sum(len(d) for _, _, d in rx)}"] if rx else []


class Vucx:
    def __init__(self, profile=None):
        t = table(profile)
        p = load(profile) if profile is not None else {}
        self.net = Network(p["sockets"]) if p.get("sockets") else None
        self.commands = t["commands"]
        self.urcs = t["urcs"]
        self.setters = t["state"]
        self.data = t["data"]
        self.default = t["default"]
        self.state = {}
        self.log = []   # every command received, for the tests
        self.payloads = []   # (command, binary data) for commands that carried data (AT+X=... SOH len data)
        self._turn = {}
        self._urc_sent = {}
        self._fd = None
        self._stop = threading.Event()

    def _lookup(self, table, cmd):
        for exact in (True, False):
            for key, lines in table.items():
                pattern, _, cond = key.partition(" [")
                if not (pattern == cmd if exact else fnmatch.fnmatchcase(cmd, pattern)):
                    continue
                if all(self.state.get(k, "") == v for k, _, v in (c.partition("=") for c in cond[:-1].split())):
                    if lines and all(isinstance(x, list) for x in lines):
                        n = self._turn.get(key, 0)
                        self._turn[key] = n + 1
                        return lines[n % len(lines)]
                    return lines
        return None

    def _send(self, lines):
        out = b""
        for line in lines:
            if isinstance(line, tuple):
                text, data = line[0], line[1]
                soh = True
            else:
                text, soh, data = line.partition("\x01")
                data = data.encode()
            out += b"\r\n" + text.encode()
            if soh:
                out += b"\x01" + len(data).to_bytes(2, "big") + data
        os.write(self._fd, out + b"\r\n")

    def handle(self, cmd, data=b""):
        cmd = cmd[3:] if cmd.startswith("+++") else cmd   # the escape from data mode has no line end
        self.log.append(cmd)
        if data:
            self.payloads.append((cmd, bytes(data)))
        if not cmd[:2].upper() == "AT":
            reply = next((v for k, v in self.data.items() if fnmatch.fnmatchcase(cmd, k)), None)
            if reply is not None:
                os.write(self._fd, reply.encode())
            return
        answer = self.net.handle(cmd, data) if self.net else None
        if answer is None:
            answer = (self._lookup(self.commands, cmd) or self.default, self._lookup(self.urcs, cmd))
        self.state.update(self._lookup(self.setters, cmd) or {})
        self._send(answer[0])
        urc = answer[1]
        if isinstance(urc, dict):   # {after: s, lines: [...], times: n}: later, e.g. a link that drops
            done = self._urc_sent.get(id(urc), 0)
            if done < urc.get("times", 1 << 30):
                self._urc_sent[id(urc)] = done + 1
                threading.Timer(urc["after"], self._send_quiet, [urc["lines"]]).start()
        elif urc:
            time.sleep(0.05)
            self._send(urc)

    def _send_quiet(self, lines):
        try:
            self._send(lines)
        except OSError:   # stop() closed the pty first
            pass

    def start(self):
        """Opens the pty and serves it in a thread; returns the port name for the app.

        The tty keeps the kernel's default (cooked) settings, as a fresh /dev/ttyUSB has: ICRNL, IXON, ECHO and OPOST
        on. The app's port layer must make it raw itself, or binary data with 0x0D, 0x11 or 0x13 gets changed."""
        self._fd, slave = os.openpty()
        self._slave = slave
        self.port = os.ttyname(slave)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self.port

    def _serve(self):
        import select

        buf = b""
        while not self._stop.is_set():
            try:
                if not select.select([self._fd], [], [], 0.1)[0]:
                    continue
                buf += os.read(self._fd, 4096)
            except (OSError, ValueError):   # stop() closed the pty
                return
            while True:
                cr, soh = buf.find(b"\r"), buf.find(b"\x01")
                data = b""
                if soh >= 0 and (cr < 0 or soh < cr):
                    end = soh + 3 + int.from_bytes(buf[soh + 1:soh + 3], "big")
                    if len(buf) < soh + 3 or len(buf) < end:
                        break
                    line, data, buf = buf[:soh], buf[soh + 3:end], buf[end:]   # binary data follows the command
                elif cr >= 0:
                    line, buf = buf[:cr], buf[cr + 1:]
                else:
                    break
                line = line.strip(b"\n").decode(errors="replace").strip()
                if line:
                    self.handle(line, data)

    def stop(self):
        """Stops the thread before closing the pty: a closed fd number is reused by the next open(), and a thread
        still in select/read/write would then read or write that file (a test's saved file read back empty)."""
        self._stop.set()
        self._thread.join(timeout=5)
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
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
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


def main():
    if sys.argv[1:2] == ["--json"]:
        print(json.dumps(table(sys.argv[2]), indent=1))
        return
    v = Vucx(sys.argv[1] if len(sys.argv) > 1 else None)
    print(v.start(), flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        v.stop()
