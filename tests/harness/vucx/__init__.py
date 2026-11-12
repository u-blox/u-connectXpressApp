"""vucx: a virtual u-connectXpress module on a pseudo terminal (Linux/WSL) or a TCP port (any OS).

Empty by default: it only answers the AT setup commands. A YAML profile decides what else it can do:

    identity: {manufacturer: u-blox, model: NORA-W36, version: 3.5.0-0, serial: "VUCX0000"}
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
(SOH, 2-byte length, bytes; one byte per character, so "\xB5" is 0xB5), as the module does; commands with binary
data (AT+X=... SOH len data) are matched on the text before the SOH. State setters apply after the answer, so a
command can answer from the old state.

    peer:                           # the connected BLE peer's GATT database, from handle `start` up
      start: 100
      services:
        - uuid: "180F"
          chars:                    # [uuid, properties R w W N I, value hex or text:..., {descriptor uuid: value}]
            - ["2A19", R N, "5A"]

becomes AT+UBTGPSD lines, one AT+UBTGSCD per service, AT+UBTGCDD per characteristic and AT+UBTGR per readable
value (a CCCD follows every notifiable value). Static, so the browser's virtual module has it too.

    gatt_server: true               # our own GATT server hands out handles per service (see GattServer)
    gatt_services: 3                # services kept until AT+CPWROFF (a service cannot be removed; default: no limit)

    sockets:                        # the network behind the module (the browser's virtual module runs it too)
      servers: {2101: ntrip, 5683: coap}   # remote port -> behaviour (echo for the rest; sink drops)
                                    # also ntp (SNTP time), tftp (any file is one block, writes are acked),
                                    # http (200 with a Date header) and ft (the ft file server, see FtServer)
      accept: [8000]                # listening ports a client connects to (it echoes); a UDP socket bound to
                                    # one gets a datagram from a client

answers AT+USOCR/USOC/USOWB/USOSTB/USORB/USORFB/USOCL/USOL/USOST? with real per-socket state.
Only lines starting with AT are commands; anything else (data mode, a bridged stream) is logged and dropped, unless

    data:                           # fnmatch pattern on a data-mode line -> raw bytes sent back as they are
      "GET /*": "ICY 200 OK\\r\\n\\r\\n"   # an NTRIP caster behind a modem in transparent mode

Run alone: python -m harness.vucx tests/harness/vucx/basic.yaml  (prints the port to open)
On any OS over TCP: python -m harness.vucx --tcp [PORT] nora-b26-ble.yaml  (prints tcp:127.0.0.1:PORT; the launcher's
`ucxapp --virtual nora-b26` does this and starts the app on it)
As JSON for the browser's virtual modules: python -m harness.vucx --json alma-w1-thread.yaml > app/wasm/valma-w1.json
(also nora-w36 > vnora-w36, nora-b26-ble-thread > vnora-b26-thread, nora-b26-ble > vnora-b26)
"""

import base64
import email.utils
import fnmatch
import hashlib
import json
import os
import re
import sys
import threading
import time
import zlib
from pathlib import Path

from .. import corrections

PROFILES = Path(__file__).parent

SETUP = {"AT": ["OK"], "ATE0": ["OK"], "ATE1": ["OK"], "AT+USYEE=1": ["OK"]}

PROPS = {"R": 0x02, "w": 0x04, "W": 0x08, "N": 0x10, "I": 0x20}


def _hex(value):
    value = str(value)
    return value[5:].encode().hex().upper() if value.startswith("text:") else value.upper()


def peer_table(peer):
    """(service discovery lines, commands, {characteristic uuid: value handle}) for a peer GATT database; see the
    module docstring."""
    commands = {}
    services = []
    values = {}
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
            values.setdefault(uuid.upper(), val)
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
    return services, commands, values


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
    ft = None
    if p.get("peer"):
        services, generated, values = peer_table(p["peer"])
        base = [line for line in commands.get("AT+UBTGPSD=*", []) if line != "OK"]
        commands["AT+UBTGPSD=*"] = base + services + ["OK"]
        commands.update(generated)
        if FtServer.CTRL in values and FtServer.DATA in values:
            ft = [values[FtServer.CTRL], values[FtServer.DATA]]
    return {"commands": commands, "urcs": dict(p.get("urcs") or {}), "state": dict(p.get("state") or {}),
            "data": dict(p.get("data") or {}), "default": p.get("default") or ["ERROR"],
            "gatt_server": bool(p.get("gatt_server")), "gatt_services": p.get("gatt_services"), "ft_peer": ft}


class FtServer:
    """The ft file server (app/src/ucxapp_cmds_ft.c) behind the virtual module, read-write, with notes.txt and
    big.bin to fetch. packet() takes one client packet (CTRL or DATA) and returns the server's replies as
    (data, packet) pairs: a whole window of blocks at once, the next on each ACK, an ACK every 8 blocks of an upload.
    Network carries them over TCP (framed, servers: {6970: ft}) and Vucx over BLE as notifications on the peer's
    CTRL/DATA characteristics (the 7A3F0001 service in peer:). The browser's virtual module does the same."""

    CTRL = "7A3F00025C1E4D2B9F6A3E8B2C4D5E6F"
    DATA = "7A3F00035C1E4D2B9F6A3E8B2C4D5E6F"
    WINDOW = 8

    def __init__(self):
        self.files = {"notes.txt": b"notes.txt: a demo file on the virtual ft server\n",
                      "big.bin": bytes((i * 7 + (i >> 8)) & 0xFF for i in range(20000))}
        self.links = {}

    @staticmethod
    def _u16(v):
        return v.to_bytes(2, "little")

    @staticmethod
    def _u32(v):
        return v.to_bytes(4, "little")

    def _window(self, st, base):
        body, blk = st["body"], st["blk"]
        return [(True, self._u16(b) + body[b * blk:(b + 1) * blk])
                for b in range(base, min(base + self.WINDOW, len(body) // blk + 1))]

    def packet(self, link, data, pkt):
        st = self.links.setdefault(link, {})
        pkt = bytes(pkt)
        if data:
            return self._block(st, pkt)
        if not pkt:
            return []
        op, le = pkt[0], (lambda a, b: int.from_bytes(pkt[a:b], "little"))
        if op in (0x01, 0x07):         # GET, GET from an offset
            hdr = 7 if op == 0x07 else 3
            f, off = self.files.get(pkt[hdr:].decode("latin-1")), le(3, 7) if op == 0x07 else 0
            if f is None or off > len(f) or le(1, 3) < 16:
                return [(False, bytes([0x81, 1 if f is None else 4]))]
            st.clear()
            st.update(mode="get", body=f[off:], blk=le(1, 3))
            return [(False, b"\x81\0" + self._u32(len(f)) + self._u32(zlib.crc32(f)))] + self._window(st, 0)
        if op == 0x05 and st.get("mode") == "get":
            nxt = le(1, 3)
            if nxt > len(st["body"]) // st["blk"]:
                st.clear()
                return []
            return self._window(st, nxt)
        if op in (0x02, 0x08):         # PUT, PUT resume (no .part is kept: it starts over)
            if len(pkt) < 12 or le(1, 3) < 16:
                return [(False, bytes([0x80 | op, 4]))]
            st.clear()
            st.update(mode="put", blk=le(1, 3), size=le(3, 7), crc=le(7, 11), name=pkt[11:].decode("latin-1"),
                      buf=b"", want=0, since=0)
            return [(False, b"\x88\0" + self._u32(0) if op == 0x08 else b"\x82\0")]
        if op == 0x03:
            return ([(False, b"\x83\0" + self._u32(len(f)) + n.encode()) for n, f in self.files.items()]
                    + [(False, b"\x83\xff")])
        if op == 0x04:
            return [(False, bytes([0x84, 0 if self.files.pop(pkt[1:].decode("latin-1"), None) is not None else 1]))]
        if op == 0x06:
            st.clear()
        return []

    def _block(self, st, pkt):
        if st.get("mode") != "put" or len(pkt) < 2:
            return []
        if int.from_bytes(pkt[:2], "little") != st["want"]:
            return [(False, b"\x85" + self._u16(st["want"]))]
        st["buf"] += pkt[2:]
        st["want"] += 1
        st["since"] += 1
        if len(pkt) - 2 < st["blk"]:   # the short block ends the file
            ok = len(st["buf"]) == st["size"] and zlib.crc32(st["buf"]) == st["crc"]
            if ok:
                self.files[st["name"]] = st["buf"]
            out = [(False, b"\x85" + self._u16(st["want"])), (False, bytes([0x87, 0 if ok else 6]))]
            st.clear()
            return out
        if st["since"] >= self.WINDOW:
            st["since"] = 0
            return [(False, b"\x85" + self._u16(st["want"]))]
        return []


class GattServer:
    """The module's own GATT server (gatt_server: true in the profile), for the defines no command entry answers:
    handles count up from 40 since the last AT+CPWROFF like on the module: a service starts on the next multiple of 20,
    a characteristic takes a declaration, value and CCCD handle (the first 41/42/43, the next 44/45/46), a descriptor
    one. URC lines name a defined characteristic by UUID prefix, the first that matches: {h:7A3F0011} is its value
    handle, {c:7A3F0011} its CCCD; a line naming none is left out. The URCs of AT+UBTGSA only see the services being
    activated, so activating one service does not replay those of another. The browser's virtual module does too."""

    REF = re.compile(r"\{([hc]):([0-9A-Fa-f]+)\}")

    def __init__(self, room=None):
        self.room = room
        self.reset()

    def reset(self):
        self.next = 40
        self.chars = []
        self.active = 0
        self.services = 0

    def handle(self, cmd):
        if cmd.startswith("AT+UBTGS="):
            if self.room is not None and self.services >= self.room:
                return ["ERROR"]
            self.services += 1
            service = -(-self.next // 20) * 20
            self.next = service + 1
            return [f"+UBTGS:{service}", "OK"]
        m = re.match(r"AT\+UBTG(C|HCC)=([0-9A-Fa-f]*)", cmd)
        if m:
            value, cccd = self.next + 1, self.next + 2
            self.next += 3
            self.chars.append((m[2].upper(), value, cccd))
            return [f"+UBTG{m[1]}:{value},{cccd}", "OK"]
        if cmd.startswith("AT+UBTGD="):
            self.next += 1
            return [f"+UBTGD:{self.next - 1}", "OK"]
        return None

    def _ref(self, kind, prefix, chars):
        hit = [c for c in chars if c[0] and c[0].startswith(prefix.upper())]
        return None if not hit else str(hit[0][1 if kind == "h" else 2])

    def fill(self, lines, cmd=""):
        chars = self.chars[self.active:] if cmd == "AT+UBTGSA" else self.chars
        out = []
        for line in lines:
            refs = ({m[0]: self._ref(m[1], m[2], chars) for m in self.REF.finditer(line)}
                    if isinstance(line, str) else {})
            if None in refs.values():
                continue
            for ref, value in refs.items():
                line = line.replace(ref, value)
            out.append(line)
        return out


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

    def __init__(self, cfg, ft=None):
        self.servers = {int(k): v for k, v in (cfg.get("servers") or {}).items()}
        self.accept = {int(p) for p in cfg.get("accept") or []}
        # listening port -> requests; each is a client that connects, sends it and keeps what comes back in replies
        self.requests = {int(k): list(v) for k, v in (cfg.get("requests") or {}).items()}
        # bound UDP port -> datagrams the scripted client sends at once, without waiting for an answer in between
        self.bursts = {int(k): list(v) for k, v in (cfg.get("bursts") or {}).items()}
        # bound UDP port -> a client that talks: peer(None) gives its first datagrams, peer(data) its answers to
        # each datagram the module sends it (a Matter commissioner, tests/harness/pase.py); tests only
        self.peers = {int(k): v for k, v in (cfg.get("peers") or {}).items()}
        self.peer = None
        self.replies = []
        self.udp_replies = None        # what the module sent the scripted UDP client (port 50000)
        self.followups = []
        self.ft = ft or FtServer()
        self.socks = {}
        self.sps = {}   # SPS connection -> bytes echoed back, not read yet

    def _new(self, proto, port=0):
        n = next(i for i in range(64) if i not in self.socks)
        self.socks[n] = {"proto": proto, "port": port, "rx": [], "state": 0, "status": 1}
        return n

    def _serve(self, s, data, ip="192.168.1.10", port=None):
        if "reply" in s:               # a scripted client of one of our listeners
            s["reply"] += data
            return
        kind = self.servers.get(s["port"], "echo")
        port = port or s["port"]
        if kind == "ntrip" and s["state"] == 0:
            s["state"] = 1
            s["rx"].append((ip, port, b"ICY 200 OK\r\n\r\n" + self.RTCM))
        elif kind == "coap" and len(data) >= 4:
            tkl, kind_ = data[0] & 0x0F, (data[0] >> 4) & 3
            head = bytes([0x40 | ((2 if kind_ == 0 else 1) << 4) | tkl, 0x45])
            s["rx"].append((ip, port, head + data[2:4 + tkl] + b"\xff21.5"))
        elif kind == "ntp" and len(data) >= 48:
            sec = (int(time.time()) + 2208988800) & 0xFFFFFFFF
            s["rx"].append((ip, port, bytes([0x24, 2]) + bytes(38) + sec.to_bytes(4, "big") + bytes(4)))
        elif kind == "tftp" and len(data) >= 4:
            op = int.from_bytes(data[:2], "big")
            if op == 1:                # RRQ: one short DATA block is the whole file
                name = data[2:].split(b"\0")[0].decode("latin-1")
                s["rx"].append((ip, port, b"\0\3\0\1" + f"vucx demo file {name}\n".encode()))
            elif op in (2, 3):         # WRQ, DATA: acknowledge
                s["rx"].append((ip, port, b"\0\4" + (data[2:4] if op == 3 else b"\0\0")))
        elif kind == "http":
            body = b"" if data.startswith(b"HEAD ") else b"<html>vucx</html>"
            s["rx"].append((ip, port, f"HTTP/1.1 200 OK\r\nDate: {email.utils.formatdate(usegmt=True)}\r\n"
                                      f"Content-Type: text/html\r\nContent-Length: 17\r\nConnection: close\r\n\r\n"
                                      .encode() + body))
        elif kind == "ft":             # [len u16][channel][packet] frames both ways; the replies go as one read
            acc = s.setdefault("acc", bytearray())
            acc += data
            out = b""
            while len(acc) >= 3 and len(acc) >= 3 + int.from_bytes(acc[:2], "little"):
                n = 3 + int.from_bytes(acc[:2], "little")
                for is_data, pkt in self.ft.packet(id(s), acc[2] == 1, acc[3:n]):
                    out += len(pkt).to_bytes(2, "little") + bytes([1 if is_data else 0]) + pkt
                del acc[:n]
            if out:
                s["rx"].append((ip, port, out))
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
            if "reply" in s and self._complete(s["reply"]):   # the client has its answer and hangs up
                del self.socks[int(m[1])]
                nxt = self._next_client(0.05)
                close = f"+UESOCL:{m[1]}"
                return ([f"+USOWB:{m[1]},{len(data)}", "OK"],
                        {**nxt, "lines": [close] + nxt["lines"]} if nxt else [close])
            return [f"+USOWB:{m[1]},{len(data)}", "OK"], self._avail(m[1])
        # the far end as IPv4, or as IPv6 in brackets: a dual-stack socket takes an IPv4 host as ::ffff:a.b.c.d
        m = re.fullmatch(r"AT\+USOSTB=(\d+),([\d.]+|\[[0-9A-Fa-f:]+\]),(\d+),?", cmd)
        if m and int(m[1]) in self.socks:
            s = self.socks[int(m[1])]
            host = m[2]
            mapped = re.fullmatch(r"\[(?:0000:){5}ffff:([0-9a-f]{4}):([0-9a-f]{4})\]", host.lower())
            if mapped:
                host = ".".join(str(b) for b in bytes.fromhex(mapped[1] + mapped[2]))
            if int(m[3]) == 50000 and self.udp_replies is not None:   # to the scripted UDP client
                self.udp_replies.append(bytes(data))
                if self.peer is not None:
                    s["rx"] += [("192.168.1.10", 50000, bytes(d)) for d in self.peer(bytes(data))]
                if self.followups:     # its next datagram goes back to the socket that wrote
                    s["rx"].append(("192.168.1.10", 50000, self.followups.pop(0)))
                return [f"+USOSTB:{m[1]},{len(data)}", "OK"], self._avail(m[1])
            self._serve({**s, "port": int(m[3])}, data, host, int(m[3]))
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
            s = self.socks.pop(int(m[1]), None)
            if s is None:
                return ["ERROR"], []
            return ["OK"], self._next_client(0.05) if "reply" in s else []
        m = re.fullmatch(r"AT\+USO[BL]=(\d+)(?:,(\d+))?", cmd)
        if m and int(m[1]) in self.socks:
            port = int(m[2] or 0) or (self.socks[int(m[1])]["port"] if cmd.startswith("AT+USOL") else 0)
            self.socks[int(m[1])].update(port=port, status=3 if cmd.startswith("AT+USOL") else 1)
            if cmd.startswith("AT+USOL") and port in self.requests:
                self.pending = [(req, m[1]) for req in self.requests[port]]
                return ["OK"], self._next_client(0.3)
            if cmd.startswith("AT+USOB") and self.socks[int(m[1])]["proto"] == 17 and port in self.peers:
                self.peer = self.peers[port]
                self.udp_replies = []
                self.replies.append(self.udp_replies)
                self.socks[int(m[1])]["rx"] = [("192.168.1.10", 50000, bytes(d)) for d in self.peer(None)]
                return ["OK"], {"after": 0.3, "lines": self._avail(m[1])}
            if cmd.startswith("AT+USOB") and self.socks[int(m[1])]["proto"] == 17 and port in self.requests:
                first, *self.followups = [r.encode() if isinstance(r, str) else bytes(r)
                                          for r in self.requests[port]]
                self.udp_replies = []
                self.replies.append(self.udp_replies)
                self.socks[int(m[1])]["rx"] = [("192.168.1.10", 50000, first)]
                return ["OK"], {"after": 0.3, "lines": self._avail(m[1])}
            if cmd.startswith("AT+USOB") and self.socks[int(m[1])]["proto"] == 17 and port in self.bursts:
                self.udp_replies = []
                self.replies.append(self.udp_replies)
                self.socks[int(m[1])]["rx"] = [("192.168.1.10", 50000, r.encode() if isinstance(r, str) else bytes(r))
                                                for r in self.bursts[port]]
                return ["OK"], {"after": 0.3, "lines": self._avail(m[1])}
            if cmd.startswith("AT+USOL") and port in self.accept:
                return ["OK"], [f"+UESOIC:{self._new(6, port)},192.168.1.10,{m[1]}"]
            if cmd.startswith("AT+USOB") and self.socks[int(m[1])]["proto"] == 17 and port in self.accept:
                self.socks[int(m[1])]["rx"].append(("192.168.1.10", 50000, b"hello from the vucx demo client"))
                return ["OK"], self._avail(m[1])
            return ["OK"], []
        if cmd == "AT+USOST?":
            return [f"+USOST:{n},{s['proto']},{s['status']}" for n, s in self.socks.items()] + ["OK"], []
        # SPS: the BLE peer echoes (perf.echo sps on module B)
        m = re.fullmatch(r"AT\+USPSC=(\d+)(?:,\d+)?", cmd)
        if m:
            self.sps[int(m[1])] = b""
            return ["OK"], [f"+UESPSC:{m[1]}"]
        m = re.fullmatch(r"AT\+USPSWB=(\d+),?", cmd)
        if m and int(m[1]) in self.sps:
            self.sps[int(m[1])] += bytes(data)
            return [f"+USPSWB:{m[1]},{len(data)}", "OK"], [f"+UESPSDA:{m[1]},{len(self.sps[int(m[1])])}"]
        m = re.fullmatch(r"AT\+USPSRB=(\d+),(\d+)", cmd)
        if m and int(m[1]) in self.sps:
            buf = self.sps[int(m[1])]
            self.sps[int(m[1])] = buf[int(m[2]):]
            return [(f"+USPSRB:{m[1]}", buf[:int(m[2])]), "OK"], []
        return None

    @staticmethod
    def _complete(reply):
        head, sep, body = bytes(reply).partition(b"\r\n\r\n")
        m = re.search(rb"(?im)^content-length:\s*(\d+)", head)
        return bool(sep) and m is not None and len(body) >= int(m[1])

    def _next_client(self, after):
        """The next scripted client of a listener: it connects and sends its request a little later."""
        if not getattr(self, "pending", None):
            return []
        req, listener = self.pending.pop(0)
        n = self._new(6)
        req = req.encode() if isinstance(req, str) else bytes(req)
        self.socks[n].update(rx=[("192.168.1.10", 50000, req)], reply=bytearray())
        self.replies.append(self.socks[n]["reply"])
        # the firmware's order (see ucxapp_urc.c): listening socket first, the new one last
        return {"after": after, "lines": [f"+UESOIC:{listener},192.168.1.10,{n}", f"+UESODA:{n},{len(req)}"]}

    def _avail(self, n):
        rx = self.socks[int(n)]["rx"]
        return [f"+UESODA:{n},{sum(len(d) for _, _, d in rx)}"] if rx else []


class Vucx:
    def __init__(self, profile=None):
        t = table(profile)
        p = load(profile) if profile is not None else {}
        self.ft = FtServer()
        self.ft_peer = t["ft_peer"]
        self.net = Network(p["sockets"], self.ft) if p.get("sockets") else None
        self.commands = t["commands"]
        self.urcs = t["urcs"]
        self.setters = t["state"]
        self.data = t["data"]
        self.default = t["default"]
        self.gatt = GattServer(t["gatt_services"]) if t["gatt_server"] else None
        self.state = {}
        self.log = []   # every command received, for the tests
        self.payloads = []   # (command, binary data) for commands that carried data (AT+X=... SOH len data)
        self.certs = {}   # uploaded certificates by name, for their fingerprint
        self._turn = {}
        self._urc_sent = {}
        self._fd = None
        self._conn = None   # the app's TCP connection in start_tcp() mode
        self._listen = None
        self._thread = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()

    def _write(self, data):
        # Timer threads send delayed URCs while the reader answers commands: one lock keeps each send whole,
        # and a pty may take a large write in parts.
        with self._wlock:
            if self._listen is not None:
                conn = self._conn
                if conn is None:
                    raise OSError("no client")
                conn.sendall(data)
                return
            view = memoryview(data)
            while view:
                view = view[os.write(self._fd, view):]

    def _ft_ble(self, cmd):
        """A write to the peer's ft CTRL/DATA characteristic: the replies come back as notifications."""
        m = re.fullmatch(r"AT\+UBTGWNR=(\d+),(\d+),([0-9A-Fa-f]*)", cmd)
        if not m or not self.ft_peer or int(m[2]) not in self.ft_peer:
            return None
        ctrl, data = self.ft_peer
        out = self.ft.packet("ble", int(m[2]) == data, bytes.fromhex(m[3]))
        return ["OK"], [f"+UEBTGCN:{m[1]},{data if d else ctrl},{p.hex().upper()}" for d, p in out]

    def _cert(self, cmd, data):
        """Remembers uploaded certificates and answers the fingerprint of one, the SHA-256 of its DER, as the
        module does (the browser's virtual module does the same)."""
        m = re.match(r'AT\+USECUB=\d+,"([^"]+)"', cmd)
        if m and data:
            self.certs[m.group(1)] = bytes(data)
            return None
        m = re.match(r'AT\+USECD="([^"]+)",0$', cmd)
        if not m or m.group(1) not in self.certs:
            return None
        cert = self.certs[m.group(1)]
        pem = re.search(rb"-----BEGIN [^-]+-----(.*?)-----END", cert, re.S)
        der = base64.b64decode(b"".join(pem.group(1).split())) if pem else cert
        return [f"+USECD:0,{hashlib.sha256(der).hexdigest().upper()}", "OK"], None

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
                data = data.encode("latin-1")   # one byte per character, so \xB5 is the byte 0xB5
            out += b"\r\n" + text.encode()
            if soh:
                out += b"\x01" + len(data).to_bytes(2, "big") + data
        self._write(out + b"\r\n")

    def handle(self, cmd, data=b""):
        cmd = cmd[3:] if cmd.startswith("+++") else cmd   # the escape from data mode has no line end
        self.log.append(cmd)
        if data:
            self.payloads.append((cmd, bytes(data)))
        if not cmd[:2].upper() == "AT":
            reply = next((v for k, v in self.data.items() if fnmatch.fnmatchcase(cmd, k)), None)
            if reply is not None:
                self._write(reply.encode())
            return
        answer = self._cert(cmd, data) or self._ft_ble(cmd) or (self.net.handle(cmd, data) if self.net else None)
        if answer is None:
            reply = self._lookup(self.commands, cmd) or (self.gatt.handle(cmd) if self.gatt else None)
            answer = (reply or self.default, self._lookup(self.urcs, cmd))
        if self.gatt and cmd == "AT+CPWROFF":
            self.gatt.reset()
        self.state.update(self._lookup(self.setters, cmd) or {})
        self._send(answer[0])
        urc = answer[1]
        if isinstance(urc, dict):   # {after: s, lines: [...], times: n}: later, e.g. a link that drops
            done = self._urc_sent.get(id(urc), 0)
            if done < urc.get("times", 1 << 30):
                self._urc_sent[id(urc)] = done + 1
                lines = self.gatt.fill(urc["lines"], cmd) if self.gatt else urc["lines"]
                threading.Timer(urc["after"], self._send_quiet, [lines]).start()
        elif urc:
            time.sleep(0.05)
            self._send(self.gatt.fill(urc, cmd) if self.gatt else urc)
        if self.gatt and cmd == "AT+UBTGSA":
            self.gatt.active = len(self.gatt.chars)

    def _send_quiet(self, lines):
        try:
            self._send(lines)
        except OSError:   # stop() closed the pty first, or the TCP client left
            pass

    def start(self):
        """Opens the pty and serves it in a thread; returns the port name for the app. Without ptys (Windows) the
        module is served over TCP instead (start_tcp), which the app opens like a serial port.

        The tty keeps the kernel's default (cooked) settings, as a fresh /dev/ttyUSB has: ICRNL, IXON, ECHO and OPOST
        on. The app's port layer must make it raw itself, or binary data with 0x0D, 0x11 or 0x13 gets changed."""
        if not hasattr(os, "openpty"):
            return self.start_tcp()
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
                buf = self._feed(buf)
            except (OSError, ValueError):   # stop() closed the pty, or the app did while an answer was on its way
                return

    def start_tcp(self, port=0, host="127.0.0.1"):
        """Serves the module on a TCP port instead of a pty (any OS); returns "tcp:HOST:PORT" for the app, which
        opens that name like a serial port. One client at a time; a new one may connect after the last left."""
        import socket

        self._listen = socket.create_server((host, port))
        self._listen.settimeout(0.1)
        self.port = f"tcp:{host}:{self._listen.getsockname()[1]}"
        self._thread = threading.Thread(target=self._serve_tcp, daemon=True)
        self._thread.start()
        return self.port

    def _serve_tcp(self):
        import socket

        while not self._stop.is_set():
            try:
                conn, _ = self._listen.accept()
            except socket.timeout:
                continue
            except OSError:   # stop() closed the listener
                return
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.settimeout(0.1)
            self._conn, buf = conn, b""
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    buf = self._feed(buf + chunk)
                except socket.timeout:
                    continue
                except OSError:   # the app left, maybe while an answer was on its way
                    break
            self._conn = None
            conn.close()

    def _feed(self, buf):
        """Handles every complete command in buf; returns the rest."""
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
        return buf

    def stop(self):
        """Stops the thread before closing the pty: a closed fd number is reused by the next open(), and a thread
        still in select/read/write would then read or write that file (a test's saved file read back empty)."""
        self._stop.set()
        self._thread.join(timeout=5)
        for s in (getattr(self, "_listen", None), getattr(self, "_conn", None)):
            if s is not None:
                s.close()
        for fd in (self._fd, getattr(self, "_slave", None)) if self._fd is not None else ():
            try:
                os.close(fd)
            except OSError:
                pass


class Feeder:
    """A pty (or, without ptys, a TCP port) that repeats a byte stream (a virtual GNSS receiver or IMU) and records
    what it receives."""

    def __init__(self, data, interval=0.1):
        self.data = data.encode() if isinstance(data, str) else data
        self.interval = interval
        self.received = b""
        self._stop = threading.Event()
        self._fd = None
        self._conn = None
        self._listen = None

    def start(self):
        if not hasattr(os, "openpty"):
            return self.start_tcp()
        import tty

        self._fd, self._slave = os.openpty()
        tty.setraw(self._fd)
        tty.setraw(self._slave)
        self.port = os.ttyname(self._slave)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self.port

    def start_tcp(self, port=0, host="127.0.0.1"):
        """Serves the stream on a TCP port (any OS); returns "tcp:HOST:PORT" for gnss.open / imu.open."""
        import socket

        self._listen = socket.create_server((host, port))
        self._listen.settimeout(0.1)
        self.port = f"tcp:{host}:{self._listen.getsockname()[1]}"
        self._thread = threading.Thread(target=self._serve_tcp, daemon=True)
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

    def _serve_tcp(self):
        import socket

        while not self._stop.is_set():
            try:
                conn, _ = self._listen.accept()
            except socket.timeout:
                continue
            except OSError:   # stop() closed the listener
                return
            conn.settimeout(self.interval)
            self._conn = conn
            while not self._stop.is_set():
                try:
                    conn.sendall(self.data)
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                self.received += chunk
            self._conn = None
            conn.close()

    stop = Vucx.stop


def browser_table(profile):
    """table() plus the network behind the module, as app/wasm/valma-w1.json holds it for the browser."""
    out = table(profile)
    sockets = load(profile).get("sockets")
    if sockets:   # the browser runs the same network; it gets the caster's stream as hex
        out["sockets"] = {"servers": sockets.get("servers") or {}, "accept": sockets.get("accept") or [],
                          "ntrip": Network.RTCM.hex(),
                          "rtcm": corrections.rtcm(bytes.fromhex("3ED00003") + bytes(15)).hex()}
    return out


def main():
    if sys.argv[1:2] == ["--json"]:
        print(json.dumps(browser_table(sys.argv[2]), indent=1))
        return
    args = sys.argv[1:]
    tcp = None
    if "--tcp" in args:   # --tcp [PORT]: serve on 127.0.0.1 (any OS) instead of a pty
        i = args.index("--tcp")
        tcp = int(args.pop(i + 1)) if i + 1 < len(args) and args[i + 1].isdigit() else 0
        args.pop(i)
    v = Vucx(args[0] if args else None)
    print(v.start_tcp(tcp) if tcp is not None else v.start(), flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        v.stop()
