"""ft.* paths that need a peer which talks back: the app's ft file server (ft.on) against a scripted client behind
the virtual module, the ft client (ft.get, ft.put, ft.list, ft.remove) against a scripted server, and what both
sides refuse; then the fw.* checks made before anything is flashed. The ft protocol is described at the top of
app/src/ucxapp_cmds_ft.c."""

import subprocess
import time
import zlib

import pytest

pytestmark = pytest.mark.app

HOST = "192.168.1.10"
PEER = "C0:FF:EE:00:11:22"
LIST, ACK, END = b"\x03", b"\x05", b"\x83\xff"


def _u16(v):
    return v.to_bytes(2, "little")


def _u32(v):
    return v.to_bytes(4, "little")


def _le(b):
    return int.from_bytes(b, "little")


def _pattern(n):
    return bytes((i * 7 + 3 + (i >> 8)) & 0xFF for i in range(n))


def _frame(pkt):
    """A packet on the TCP link: [len u16][channel: 0 CTRL, 1 DATA][packet]. A packet is (is DATA, bytes); plain
    bytes are a CTRL packet."""
    data, body = pkt if isinstance(pkt, tuple) else (False, pkt)
    return _u16(len(body)) + (b"\x01" if data else b"\0") + bytes(body)


def _blocks(body, blk, first=0):
    """The DATA packets of body from block `first`; the short (or empty) last one ends the file."""
    return [(True, _u16(n) + body[n * blk:(n + 1) * blk]) for n in range(first, len(body) // blk + 1)]


def _put(name, body, blk, op=0x02, size=None, crc=None):
    """A PUT request (op 0x08: PUT resume) announcing body, or another size or CRC-32."""
    crc = zlib.crc32(body) if crc is None else crc
    return bytes([op]) + _u16(blk) + _u32(len(body) if size is None else size) + _u32(crc) + name.encode()


def _head(body, crc=None):
    """The answer to a GET: status ok, size and CRC-32."""
    return b"\x81\0" + _u32(len(body)) + _u32(zlib.crc32(body) if crc is None else crc)


def _is(pkt, want):
    """True for the DATA block with the number `want`, or for a CTRL packet that starts with the bytes `want`."""
    data, body = pkt
    return data and body[:2] == _u16(want) if isinstance(want, int) else not data and body.startswith(want)


def _wait(want, reply=()):
    """Part of a script: sends `reply`, then nothing until the awaited packet comes (see _is), and returns it."""
    pkt = yield list(reply)
    while not _is(pkt, want):
        pkt = yield []
    return pkt


def _take(got, blk, reply, first=None):
    """Part of a server script: sends `reply`, takes the blocks of an upload into `got` and returns the last ACK.
    Every 8 blocks are acked; the first window is answered with `first` instead and expected again."""
    pkt = yield reply
    want = 0
    while True:
        out = []
        if pkt[0] and _le(pkt[1][:2]) == want:
            got += pkt[1][2:]
            want += 1
            if len(pkt[1]) - 2 < blk:
                return [b"\x85" + _u16(want)]
            if want == 8 and first is not None:
                out, first, want = first, None, 0
                del got[:]
            elif want % 8 == 0:
                out = [b"\x85" + _u16(want)]
        pkt = yield out


class _Client:
    """An ft client on the network behind the module (vucx `requests:`). `script` is a generator: it yields the
    packets to send and is sent each packet the app's server writes back, as (is DATA, bytes). The client hangs up
    when the script ends."""

    def __init__(self, script):
        self.script = script
        self.closed = False
        self.seen = []
        self._acc = bytearray()

    def __call__(self, data):
        if data is None:
            return b"".join(_frame(p) for p in next(self.script))
        self._acc += data
        out = b""
        while not self.closed and len(self._acc) >= 3 and len(self._acc) >= 3 + _le(self._acc[:2]):
            n = 3 + _le(self._acc[:2])
            self.seen.append((self._acc[2] == 1, bytes(self._acc[3:n])))
            del self._acc[:n]
            try:
                out += b"".join(_frame(p) for p in self.script.send(self.seen[-1]))
            except StopIteration:
                self.closed = True
        return out

    def ctrl(self):
        return [pkt for data, pkt in self.seen if not data]

    def blocks(self):
        return [_le(pkt[:2]) for data, pkt in self.seen if data]


class _Server:
    """Stands in for the harness FtServer behind port 6970. `script` is a generator that is sent each packet the
    app's client writes, as (is DATA, bytes), and yields the packets to answer with; it runs to its first yield
    before the first packet comes, and a script that has ended answers nothing."""

    def __init__(self, script):
        self.script = script
        self.seen = []
        next(script)

    def packet(self, link, data, pkt):
        self.seen.append((bool(data), bytes(pkt)))
        try:
            return [p if isinstance(p, tuple) else (False, p) for p in self.script.send(self.seen[-1])]
        except StopIteration:
            return []

    def ctrl(self, op):
        return [pkt for data, pkt in self.seen if not data and pkt[:1] == op]

    def blocks(self):
        return [_le(pkt[:2]) for data, pkt in self.seen if data]


def _module(clients=None, server=None, commands=None, urcs=None, edit=None):
    """A NORA-W36 vucx. `clients` connect in turn to the app's listener on port 7003, `server` stands in for the
    ft server behind port 6970, `edit` changes the profile before the module is made."""
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["commands"].update(commands or {})
    profile["urcs"].update(urcs or {})
    if clients:
        profile["sockets"]["requests"] = {7003: list(clients)}
    if edit:
        edit(profile)
    module = Vucx(profile)
    if server is not None:
        module.net.ft = server
    module.start()
    return module


def _run(exe, port, *commands, cwd):
    args = [str(exe), port] if port else [str(exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd)


def _shell(exe, port, cwd, *lines):
    """Runs the lines in the app's command shell: unlike --exec it goes on after a command that failed."""
    return subprocess.run([str(exe), port, "--shell"], input="\n".join(lines) + "\n", capture_output=True, text=True,
                          timeout=60, errors="replace", cwd=cwd)


def _drive(exe, module, cwd, *steps, timeout=20):
    """Types the lines of each step into the app's shell, then waits until the step's check of the module's log
    holds; ends with ft.status and ft.off and returns what the app printed."""
    app = subprocess.Popen([str(exe), module.port, "--shell"], cwd=cwd, stdin=subprocess.PIPE, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, errors="replace")
    try:
        for lines, done in steps:
            app.stdin.write(lines + "\n")
            app.stdin.flush()
            end = time.monotonic() + timeout
            while not done(module.log) and app.poll() is None and time.monotonic() < end:
                time.sleep(0.01)
        return app.communicate("ft.status\nft.off\n", timeout=30)[0]
    finally:
        app.kill()


def _serve(exe, module, cwd, mode, client=None):
    """Runs the app's ft server (ft.on <mode>) until the scripted client has hung up, or without one until the app
    has closed a socket. What a script did is settled by then if it ends with a LIST: the server answers one
    request after the other."""
    if client is None:
        return _drive(exe, module, cwd, (f"ft.on {mode}", lambda log: any(c.startswith("AT+USOCL=") for c in log)))
    return _drive(exe, module, cwd, (f"ft.on {mode}", lambda log: client.closed))


# ---- the app as ft server on a module TCP socket ----

def test_ft_server_stores_an_upload_and_lists_it(app_exe, tmp_path):
    """A PUT to a read/write server is acked every 8 blocks, checked against its CRC-32 and stored under its name."""
    body = _pattern(300)

    def script():
        yield from _wait(b"\x82", [_put("up.bin", body, 32)])
        yield from _wait(b"\x87", _blocks(body, 32))
        yield from _wait(END, [LIST])

    client = _Client(script())
    module = _module([client])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", client)
    finally:
        module.stop()
    assert client.ctrl()[:4] == [b"\x82\0", b"\x85\x08\0", b"\x85\x0a\0", b"\x87\0"], (client.seen, out)
    assert b"\x83\0" + _u32(300) + b"up.bin" in client.ctrl(), client.seen
    assert (tmp_path / "up.bin").read_bytes() == body and not (tmp_path / "up.bin.part").exists()
    assert "ft: TCP port 7003, read/write, " in out and ", 1 file(s), 300 bytes" in out, out
    assert "last: received up.bin, 300 bytes, conn 1" in out, out


def test_ft_server_answers_each_bad_request_with_its_status(app_exe, tmp_path):
    """Requests that are cut short, name a path or a missing file, or ask too much get illegal, access denied or not
    found; stray packets get nothing, an unknown opcode an error, and hidden files are not listed."""
    (tmp_path / "keep.txt").write_bytes(b"keep this")
    (tmp_path / "other.txt").write_bytes(b"other")
    (tmp_path / ".secret").write_bytes(b"hidden")
    (tmp_path / "dir.bin.part").mkdir()
    asks = [(b"\x01", b"\x81\x04"),                                           # GET without a name
            (b"\x01\x10\0missing.txt", b"\x81\x01"),
            (b"\x01\x10\0../keep.txt", b"\x81\x02"),
            (b"\x07\x10\0" + _u32(1000) + b"keep.txt", b"\x81\x04"),         # from an offset past its end
            (b"\x02\x10\0", b"\x82\x04"),                                     # PUT without size, CRC and name
            (_put("../x.bin", b"abc", 16), b"\x82\x02"),
            (_put("big.bin", b"", 16, size=64 * 1024 * 1024 + 1), b"\x82\x04"),
            (_put("dir.bin", b"abc", 16), b"\x82\x02"),                       # dir.bin.part cannot be created
            (b"\x04", b"\x84\x04"),                                           # DEL without a name
            (b"\x04missing.txt", b"\x84\x01"),
            (b"\x04../keep.txt", b"\x84\x02"),
            (b"\x04keep.txt", b"\x84\0")]

    def script():
        yield from _wait(b"\x86", [(True, b"\0\0stray"), ACK + b"\0\0", b"\x06", b"\x7f"])
        for ask, _ in asks:
            yield [ask]
        yield from _wait(END, [LIST])

    client = _Client(script())
    module = _module([client])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", client)
    finally:
        module.stop()
    replies = client.ctrl()
    assert replies[:len(asks) + 1] == [b"\x86\x04"] + [reply for _, reply in asks], (client.seen, out)
    names = [pkt[6:] for pkt in replies[len(asks) + 1:-1]]
    assert b"other.txt" in names and b".secret" not in names and b"keep.txt" not in names, (names, out)
    assert not (tmp_path / "keep.txt").exists() and not (tmp_path / "x.bin").exists()
    assert "read/write, " in out and ", 0 file(s), 0 bytes" in out and "last: delete keep.txt: ok" in out, out


def test_ft_readonly_server_sends_a_file_and_refuses_writes(app_exe, tmp_path):
    """A read-only server denies PUT and DEL; a GET is sent 8 blocks at a time, stops at an ABORT, starts over at an
    ACK of block 0, goes on from the block the receiver acks and is counted when the last block is acked."""
    body = _pattern(310)
    (tmp_path / "served.bin").write_bytes(body)
    get = b"\x01" + _u16(16) + b"served.bin"

    def script():
        yield from _wait(b"\x82", [_put("x.bin", b"abc", 16)])
        yield from _wait(b"\x84", [b"\x04served.bin"])
        yield from _wait(7, [get])
        yield from _wait(7, [b"\x06", get])
        yield from _wait(7, [ACK + _u16(0)])
        yield from _wait(19, [ACK + _u16(12)])
        yield from _wait(END, [ACK + _u16(20), LIST])

    client = _Client(script())
    module = _module([client])
    try:
        out = _serve(app_exe, module, tmp_path, "tcp:7003", client)
    finally:
        module.stop()
    assert client.ctrl()[:4] == [b"\x82\x02", b"\x84\x02", _head(body), _head(body)], (client.seen, out)
    assert client.blocks() == list(range(8)) * 3 + list(range(12, 20)), (client.seen, out)
    assert all(pkt[2:] == body[_le(pkt[:2]) * 16:][:16] for data, pkt in client.seen if data), client.seen
    assert (tmp_path / "served.bin").read_bytes() == body and not (tmp_path / "x.bin.part").exists()
    assert "ft: TCP port 7003, read only, " in out and ", 1 file(s), 310 bytes" in out, out
    assert "last: sent served.bin, 310 bytes, conn 1" in out, out


def test_ft_server_rejects_an_upload_that_does_not_verify(app_exe, tmp_path):
    """More data than announced ends the upload with an error, a PUT resume starts over when the .part left behind
    is larger than the file, and a file whose CRC-32 does not match is refused and removed."""
    body = _pattern(40)

    def script():
        yield from _wait(b"\x82", [_put("long.bin", body[:10], 16)])
        yield from _wait(b"\x87", [(True, _u16(0) + body[:16])])
        yield from _wait(b"\x88", [_put("long.bin", body[:10], 16, op=0x08)])
        yield from _wait(b"\x87", _blocks(body[:10], 16))
        yield from _wait(b"\x82", [_put("bad.bin", body, 16, crc=1)])
        yield from _wait(b"\x87", _blocks(body, 16))
        yield from _wait(END, [LIST])

    client = _Client(script())
    module = _module([client])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", client)
    finally:
        module.stop()
    assert client.ctrl()[:9] == [b"\x82\0", b"\x86\x04", b"\x87\x04", b"\x88\0" + _u32(0), b"\x85\x01\0", b"\x87\0",
                                 b"\x82\0", b"\x85\x03\0", b"\x87\x06"], (client.seen, out)
    assert (tmp_path / "long.bin").read_bytes() == body[:10]
    assert sorted(p.name for p in tmp_path.glob("*.bin*")) == ["long.bin"]
    assert "1 file(s), 10 bytes" in out and "last: receive bad.bin failed: CRC mismatch" in out, out


def test_ft_server_resumes_an_aborted_upload(app_exe, tmp_path):
    """An ABORT keeps the .part, a PUT resume is told how much the server has and completes the file; a name taken
    by a folder is refused after the transfer and its .part removed."""
    body = _pattern(40)
    (tmp_path / "folder").mkdir()
    (tmp_path / "folder" / "inside.txt").write_bytes(b"x")

    def script():
        yield from _wait(b"\x82", [_put("cut.bin", body, 16)])
        yield from _wait(b"\x87", [(True, _u16(0) + body[:16]), b"\x06"])
        have = yield from _wait(b"\x88", [_put("cut.bin", body, 16, op=0x08)])
        yield from _wait(b"\x87", _blocks(body[_le(have[1][2:6]):], 16))
        yield from _wait(b"\x82", [_put("folder", body, 16)])
        yield from _wait(b"\x87", _blocks(body, 16))
        yield from _wait(END, [LIST])

    client = _Client(script())
    module = _module([client])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", client)
    finally:
        module.stop()
    assert client.ctrl()[:8] == [b"\x82\0", b"\x87\x04", b"\x88\0" + _u32(16), b"\x85\x02\0", b"\x87\0",
                                 b"\x82\0", b"\x85\x03\0", b"\x87\x02"], (client.seen, out)
    assert (tmp_path / "cut.bin").read_bytes() == body and not (tmp_path / "cut.bin.part").exists()
    assert (tmp_path / "folder" / "inside.txt").exists() and not (tmp_path / "folder.part").exists()
    assert "1 file(s), 40 bytes" in out and "last: receive folder failed: access denied" in out, out


def test_ft_server_keeps_the_part_of_a_dropped_upload(app_exe, tmp_path):
    """A client that hangs up in the middle of a PUT leaves <name>.part with what was acked; a server started later
    continues from it on a PUT resume."""
    body = _pattern(400)

    def dropped():
        yield from _wait(b"\x82", [_put("drop.bin", body, 16)])
        yield from _wait(b"\x85", _blocks(body, 16)[:8])

    def resumed():
        have = yield from _wait(b"\x88", [_put("drop.bin", body, 16, op=0x08)])
        yield from _wait(b"\x87", _blocks(body[_le(have[1][2:6]):], 16))
        yield from _wait(END, [LIST])

    first, second = _Client(dropped()), _Client(resumed())
    module = _module([first])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", first)
    finally:
        module.stop()
    assert first.ctrl() == [b"\x82\0", b"\x85\x08\0"], (first.seen, out)
    assert (tmp_path / "drop.bin.part").read_bytes() == body[:128] and not (tmp_path / "drop.bin").exists()
    assert ", 0 file(s), 0 bytes" in out and "received" not in out, out
    module = _module([second])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", second)
    finally:
        module.stop()
    assert second.ctrl()[0] == b"\x88\0" + _u32(128) and b"\x87\0" in second.ctrl(), (second.seen, out)
    assert (tmp_path / "drop.bin").read_bytes() == body and not (tmp_path / "drop.bin.part").exists()
    assert "last: received drop.bin, 400 bytes, conn 1" in out, out


def test_ft_server_serves_the_next_client_on_a_reused_handle(app_exe, tmp_path):
    """+UESOCL gives the socket handle back to the module: the server sends no AT+USOCL for it, so a client that
    connects just then and gets the same handle is answered."""
    (tmp_path / "a.txt").write_bytes(b"hello")
    first, second = _Client(_wait(END, [LIST])), _Client(_wait(END, [LIST]))
    module = _module([first, second])
    served = []

    def both_left(log):
        if second.closed and not served:
            served.extend(log)
        return second.closed

    try:
        out = _drive(app_exe, module, tmp_path, ("ft.on tcp:7003", both_left))
    finally:
        module.stop()
    listed = b"\x83\0" + _u32(5) + b"a.txt"
    assert listed in first.ctrl() and first.ctrl()[-1] == END, (first.seen, out)
    assert second.ctrl() == first.ctrl(), (second.seen, module.log, out)
    assert {c for c in served if c.startswith("AT+USOWB=")} == {"AT+USOWB=1"}, served
    assert not any(c.startswith("AT+USOCL=") for c in served), served


def test_ft_server_sends_nothing_to_a_client_that_left(app_exe, tmp_path):
    """When a client hangs up in the middle of a PUT the verdict is not written to its handle: the next client, who
    got that handle, sees only the answer to its own LIST, and the upload counts as stopped."""
    body = _pattern(400)

    def dropped():
        yield from _wait(b"\x82", [_put("drop.bin", body, 16)])
        yield from _wait(b"\x85", _blocks(body, 16)[:8])

    first, second = _Client(dropped()), _Client(_wait(END, [LIST]))
    module = _module([first, second])
    try:
        out = _serve(app_exe, module, tmp_path, "rw tcp:7003", second)
    finally:
        module.stop()
    assert first.ctrl() == [b"\x82\0", b"\x85\x08\0"], (first.seen, out)
    assert all(pkt[:1] == b"\x83" for pkt in second.ctrl()) and not second.blocks(), (second.seen, out)
    assert b"\x83\0" + _u32(128) + b"drop.bin.part" in second.ctrl() and second.ctrl()[-1] == END, (second.seen, out)
    assert ", 0 file(s), 0 bytes" in out and "last: receive drop.bin failed: stopped" in out, out


def test_ft_server_hangs_up_on_a_peer_that_is_not_ft(app_exe, tmp_path):
    """A frame longer than any ft packet (a browser's GET on the ft port) is not answered and the socket, still
    open on the module, is closed with AT+USOCL."""
    module = _module(["GET / HTTP/1.1\r\n\r\n"])
    try:
        out = _serve(app_exe, module, tmp_path, "tcp:7003")
    finally:
        module.stop()
    assert "AT+USOCL=1" in module.log and bytes(module.net.replies[0]) == b"", (module.log, out)
    assert "ft: TCP port 7003, read only, " in out and ", 0 file(s), 0 bytes" in out and "last:" not in out, out


# ---- the app as ft server over BLE ----

def _written(pkt, conn=0):
    """The URC of a peer's write to the server's CTRL or DATA characteristic."""
    data, body = pkt if isinstance(pkt, tuple) else (False, pkt)
    return f"+UEBTGCW:{conn},{{h:7A3F000{3 if data else 2}}},{body.hex().upper()},0"


def _notified(packet):
    """True once the server notified this packet on its CTRL characteristic (handle 42)."""
    return lambda log: f"AT+UBTGNS=0,42,{packet.hex().upper()}" in log


def test_ft_ble_server_takes_an_upload_from_a_peer(app_exe, tmp_path):
    """Writes to the CTRL and DATA characteristics are the requests and blocks of the BLE server: it answers with
    notifications on the same connection and ignores a block from another one."""
    body = _pattern(100)
    listed = b"\x83\0" + _u32(100) + b"ble.bin"
    module = _module(urcs={
        "AT+PEER=PUT": [_written(_put("ble.bin", body, 20))],
        "AT+PEER=DATA": [_written((True, b"\0\0another link"), conn=1)] + [_written(p) for p in _blocks(body, 20)],
        "AT+PEER=LIST": [_written(LIST)]})
    try:
        out = _drive(app_exe, module, tmp_path, ("ft.on rw\nat +PEER=PUT", _notified(b"\x82\0")),
                     ("at +PEER=DATA", _notified(b"\x87\0")), ("at +PEER=LIST", _notified(END)))
    finally:
        module.stop()
    sent = [c for c in module.log if c.startswith("AT+UBTGNS=")]
    assert sent[:3] == ["AT+UBTGNS=0,42,8200", "AT+UBTGNS=0,42,850600", "AT+UBTGNS=0,42,8700"], (sent, out)
    assert _notified(listed)(module.log) and sent[-1] == "AT+UBTGNS=0,42,83FF", (sent, out)
    assert (tmp_path / "ble.bin").read_bytes() == body
    assert "ft: BLE, read/write, CTRL handle 42, DATA handle 45, 1 file(s), 100 bytes" in out, out
    assert "last: received ble.bin, 100 bytes, conn 0" in out, out


# ---- the app as ft client against a scripted server ----

def test_ft_client_names_what_the_server_refuses(app_exe, tmp_path):
    """Every status of a refused request is printed by name, a refused PUT and a LIST that ends in an error fail,
    and a server whose frame is no ft packet counts as no reply."""
    (tmp_path / "up.bin").write_bytes(b"data")

    def script():
        yield from _wait(b"\x04")
        for status in (2, 3, 4, 5, 6):
            yield from _wait(b"\x04", [bytes([0x84, status])])
        yield from _wait(b"\x02", [b"\x84\x09"])
        yield from _wait(LIST, [b"\x82\x02"])
        yield from _wait(b"\x04", [b"\x86\x04"])
        yield [bytes(1500)]

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, *[f"ft.remove {HOST} x.bin"] * 6, f"ft.put {HOST} up.bin",
                     f"ft.list {HOST}", f"ft.remove {HOST} x.bin")
    finally:
        module.stop()
    errors = [line.split("ERROR: ")[1] for line in out.stdout.splitlines() if "ERROR: " in line]
    assert errors == ["access denied", "disk full", "illegal request", "busy", "CRC mismatch", "unknown",
                      "access denied", "put up.bin: request refused", "list incomplete",
                      "no reply from the peer"], out.stdout
    assert len(server.ctrl(b"\x04")) == 7 and not server.blocks() and "deleted" not in out.stdout, server.seen
    assert out.returncode != 0, out.stdout


def test_ft_get_drops_or_keeps_the_partial_file(app_exe, tmp_path):
    """A download whose CRC-32 does not match is removed; one the server aborts, or that brings more than announced
    (the client then aborts), keeps its .part for a resume. Blocks out of order are asked for again."""
    body = _pattern(40)

    def script():
        yield from _wait(b"\x01")
        yield from _wait(b"\x01", [(True, b"\0\0early"), _head(body, crc=1)] + _blocks(body, 16))
        yield from _wait(b"\x01", [_head(body), (True, _u16(1) + body[16:32]), (True, b"\x09"), b"\x85\0\0",
                                   (True, _u16(0) + body[:16]), b"\x86\x03"])
        yield [_head(body[:10]), (True, _u16(0) + body[:16])]

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.get {HOST} a.bin a.bin 16", f"ft.get {HOST} b.bin b.bin 16",
                     f"ft.get {HOST} c.bin c.bin 16")
    finally:
        module.stop()
    assert f"ERROR: get a.bin: CRC-32 {zlib.crc32(body):08X} or size 40 does not match" in out.stdout, out.stdout
    assert "ERROR: get b.bin: aborted by peer (kept b.bin.part; add 'resume' to continue)" in out.stdout, out.stdout
    assert "ERROR: get c.bin: more data than announced (kept c.bin.part; add 'resume'" in out.stdout, out.stdout
    assert sorted(p.name for p in tmp_path.glob("*.bin*")) == ["b.bin.part", "c.bin.part"]
    assert (tmp_path / "b.bin.part").read_bytes() == body[:16]
    assert server.ctrl(ACK)[:2] == [ACK + _u16(3), ACK + _u16(0)] and server.ctrl(b"\x06") == [b"\x06\x04"], server.seen


def test_ft_get_survives_a_stalled_server_and_lost_blocks(app_exe, tmp_path):
    """When no block comes the client acks again after 2 s; of 40 blocks sent at once those beyond its queue of 31
    are lost, and half a second later it acks the first missing one so the sender goes back to it."""
    body = _pattern(629)

    def script():
        yield from _wait(b"\x01")
        yield from _wait(ACK, [_head(body)])
        yield from _wait(ACK + _u16(31), _blocks(body, 16))
        yield _blocks(body, 16, 31)

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.get {HOST} d.bin d.bin 16")
    finally:
        module.stop()
    acks = [_le(pkt[1:]) for pkt in server.ctrl(ACK)]
    assert acks[0] == 0 and 31 in acks and acks[-1] == 40, (acks, out.stdout)
    assert (tmp_path / "d.bin").read_bytes() == body and not (tmp_path / "d.bin.part").exists()
    assert out.returncode == 0 and "got d.bin -> d.bin: 629 bytes" in out.stdout, out.stdout


def test_ft_put_resumes_and_resends(app_exe, tmp_path):
    """A PUT resume sends from the offset the server has; an ACK of the window's first block, or no ACK for 2 s,
    makes the client send the window again."""
    body = _pattern(200)
    (tmp_path / "up.bin").write_bytes(body)
    got = [bytearray(), bytearray(), bytearray()]

    def script():
        yield from _wait(b"\x08")
        last = yield from _take(got[0], 16, [b"\x88\0" + _u32(64)])
        yield from _wait(b"\x02", last + [b"\x87\0"])
        last = yield from _take(got[1], 16, [b"\x82\0"], first=[b"\x85\0\0"])
        yield from _wait(b"\x02", last + [b"\x87\0"])
        last = yield from _take(got[2], 16, [b"\x82\0"], first=[])
        yield last + [b"\x87\0"]

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.put {HOST} up.bin up.bin 16 resume",
                     f"ft.put {HOST} up.bin again.bin 16", f"ft.put {HOST} up.bin late.bin 16")
    finally:
        module.stop()
    assert out.returncode == 0 and out.stdout.count("resuming at 64 bytes") == 1, out.stdout
    assert [bytes(g) for g in got] == [body[64:], body, body], (server.seen, out.stdout)
    assert server.blocks() == list(range(9)) + (list(range(8)) + list(range(13))) * 2, server.seen
    for name in ("up.bin", "again.bin", "late.bin"):
        assert f"put up.bin -> {name}: 200 bytes" in out.stdout, out.stdout


def test_ft_put_reports_a_server_that_gives_up(app_exe, tmp_path):
    """The sender goes on from the block the receiver acks, even past what it sent; a file the server then does not
    verify, and an error in the middle of the blocks, fail the upload with the reason."""
    body = _pattern(310)
    (tmp_path / "up.bin").write_bytes(body)

    def script():
        yield from _wait(b"\x02")
        yield from _wait(7, [b"\x82\0"])
        yield from _wait(19, [b"\x85" + _u16(12)])
        yield from _wait(b"\x02", [b"\x85" + _u16(20), b"\x87\x06"])
        yield from _wait(0, [b"\x82\0"])
        yield [b"\x86\x03"]

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.put {HOST} up.bin a.bin 16", f"ft.put {HOST} up.bin b.bin 16")
    finally:
        module.stop()
    assert "ERROR: put a.bin: peer did not verify the file (CRC mismatch)" in out.stdout, out.stdout
    assert "ERROR: put b.bin: aborted by peer" in out.stdout and out.returncode != 0, out.stdout
    assert server.blocks()[:16] == list(range(8)) + list(range(12, 20)), server.seen


def test_ft_put_says_when_no_verdict_comes(app_exe, tmp_path):
    """A server that acks every block but never says whether the file verified fails the upload after 5 s with
    'no reply', not with the status of the answer to the request."""
    body = _pattern(40)
    (tmp_path / "up.bin").write_bytes(body)
    got = bytearray()

    def script():
        yield from _wait(b"\x02")
        yield (yield from _take(got, 16, [b"\x82\0"]))

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.put {HOST} up.bin up.bin 16")
    finally:
        module.stop()
    assert "ERROR: put up.bin: peer did not verify the file (no reply)" in out.stdout, out.stdout
    assert out.returncode != 0 and "CRC-32 ok" not in out.stdout, out.stdout
    assert bytes(got) == body and server.blocks() == [0, 1, 2], server.seen


def _hang_up(module):
    """The far end of the app's sockets hangs up at the write the module is handling: +UESOCL follows it (the
    harness sends that for a socket whose scripted far end says it is closed)."""
    def far_end(data):
        return b""

    far_end.closed = True
    for sock in module.net.socks.values():
        sock["talk"] = far_end


def test_ft_get_leaves_a_socket_the_module_closed(app_exe, tmp_path):
    """When the far end hangs up in the middle of a download (+UESOCL) the handle is the module's again: the client
    keeps the .part and sends no AT+USOCL for it; the resume gets the same handle, completes the file and closes."""
    body = _pattern(400)

    def script():
        yield from _wait(b"\x01")
        yield from _wait(ACK, [_head(body)] + _blocks(body, 16)[:8])
        _hang_up(module)
        ask = yield from _wait(b"\x07")
        yield [_head(body)] + _blocks(body[_le(ask[1][3:7]):], 16)

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.get {HOST} drop.bin drop.bin 16",
                     f"ft.get {HOST} drop.bin drop.bin 16 resume")
    finally:
        module.stop()
    sockets = [c for c in module.log if c.startswith(("AT+USOCR=", "AT+USOCL="))]
    assert "ERROR: get drop.bin: stopped (kept drop.bin.part; add 'resume' to continue)" in out.stdout, out.stdout
    assert "resuming at 128 bytes" in out.stdout and "got drop.bin -> drop.bin: 400 bytes" in out.stdout, out.stdout
    assert (tmp_path / "drop.bin").read_bytes() == body and out.returncode == 0, out.stdout
    assert [c[:8] for c in sockets] == ["AT+USOCR", "AT+USOCR", "AT+USOCL"] and sockets[-1] == "AT+USOCL=0", sockets


def test_ft_get_stores_a_file_when_the_server_hangs_up_after_the_last_block(app_exe, tmp_path):
    """A far end that hangs up as soon as the last block is acked has sent everything: the file is checked and
    stored, not reported as stopped and left as a .part."""
    body = _pattern(40)

    def script():
        yield from _wait(b"\x01")
        yield from _wait(ACK + _u16(3), [_head(body)] + _blocks(body, 16))
        _hang_up(module)
        yield []

    server = _Server(script())
    module = _module(server=server)
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.get {HOST} done.bin done.bin 16")
    finally:
        module.stop()
    assert out.returncode == 0 and "got done.bin -> done.bin: 40 bytes" in out.stdout, out.stdout
    assert "ERROR" not in out.stdout and "stopped" not in out.stdout, out.stdout
    assert (tmp_path / "done.bin").read_bytes() == body and not (tmp_path / "done.bin.part").exists()
    assert server.ctrl(ACK) == [ACK + _u16(3)] and not any(c.startswith("AT+USOCL=") for c in module.log), module.log


# ---- the ft client's own checks ----

def test_ft_client_refuses_bad_targets_and_arguments(app_exe, tmp_path):
    """A port or connection out of range is named, and a block size out of range, a remote name with a path and a
    wrong argument count show the usage: all before a socket is opened or a file is made."""
    (tmp_path / "up.bin").write_bytes(b"data")
    module = _module()
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.get {HOST}:0 a.bin", f"ft.put {HOST}:0 up.bin",
                     f"ft.list {HOST}:0", f"ft.remove {HOST}:0 x.bin", "ft.list -1", f"ft.get {HOST} a.bin a.bin 5",
                     f"ft.put {HOST} up.bin up.bin 9999", f"ft.put {HOST} up.bin ../x", "ft.list",
                     f"ft.remove {HOST}", "ft.on bogus", "ft.off x", "ft.status x")
    finally:
        module.stop()
    usage = [line.split("usage: ")[1].split()[0] for line in out.stdout.splitlines() if "usage: " in line]
    assert usage == ["ft.get", "ft.put", "ft.put", "ft.list", "ft.remove", "ft.on", "ft.off", "ft.status"], out.stdout
    errors = [line.split("ERROR: ")[1] for line in out.stdout.splitlines() if "ERROR: " in line]
    assert errors == [f"{HOST}:0 is not host[:port] (port 1 to 65535)"] * 4 + [
        "-1 is not a BLE connection number (bt.list) or host[:port]"], out.stdout
    assert out.returncode != 0, out.stdout
    assert not any(c.startswith(("AT+USOCR", "AT+UBTG")) for c in module.log), module.log
    assert [p.name for p in tmp_path.glob("*.part")] == []


def test_ft_client_reports_local_file_problems(app_exe, tmp_path):
    """ft.put names a file it cannot open or that has more blocks than the protocol counts, ft.get a .part it
    cannot create; a resume without a .part downloads the whole file, and BLE targets need the local server off."""
    (tmp_path / "huge.bin").write_bytes(bytes(16 * 0xFFFF))
    module = _module()
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.put {HOST} nofile.bin", f"ft.put {HOST} huge.bin huge.bin 16",
                     f"ft.get {HOST} notes.txt nodir/notes.txt", f"ft.get {HOST} notes.txt notes.txt 1455 resume",
                     "ft.on", "ft.list 0", "ft.off")
    finally:
        module.stop()
    errors = [line.split("ERROR: ")[1] for line in out.stdout.splitlines() if "ERROR: " in line]
    assert errors == ["cannot open nofile.bin", "huge.bin is too large", "cannot create nodir/notes.txt.part",
                      "stop the local server first (ft.off)"], out.stdout
    assert "resuming" not in out.stdout and "got notes.txt -> notes.txt: 48 bytes" in out.stdout, out.stdout
    assert (tmp_path / "notes.txt").read_bytes().startswith(b"notes.txt: a demo file"), out.stdout
    assert "huge.bin" not in module.net.ft.files, module.net.ft.files.keys()


def test_ft_tcp_needs_a_socket_on_the_module(app_exe, tmp_path):
    """Without a socket, a listener or a connection the server and the client say which step failed, and the socket
    they made is closed again."""
    def no_network(profile):
        del profile["sockets"]

    module = _module(edit=no_network, commands={"AT+USOCR=*": [["ERROR"], ["+USOCR:0", "OK"]],
                                                "AT+USOL=*": ["ERROR"], "AT+USOC=*": ["ERROR"]})
    try:
        out = _shell(app_exe, module.port, tmp_path, f"ft.list {HOST}", "ft.on tcp", "ft.on tcp:7000",
                     f"ft.get {HOST} a.bin")
    finally:
        module.stop()
    errors = [line.split("ERROR: ")[1] for line in out.stdout.splitlines() if "ERROR: " in line]
    assert errors == ["cannot create a socket (is Wi-Fi up?)", "cannot listen on port 6970",
                      "cannot create a socket (is Wi-Fi up?)",
                      f"cannot connect to {HOST}:6970 (start ft.on tcp there)"], out.stdout
    assert module.log.count("AT+USOCL=0") == 2 and "AT+USOL=0,6970" in module.log, module.log
    assert not (tmp_path / "a.bin.part").exists() and "file transfer server on" not in out.stdout, out.stdout


def test_ft_ble_needs_the_service_on_both_sides(app_exe, tmp_path):
    """ft.on fails when the module cannot be rebooted for a clean GATT server or refuses the service; the client
    names a peer that has no ft service."""
    def no_ft_peer(profile):
        profile["peer"]["services"] = [s for s in profile["peer"]["services"] if not s["uuid"].startswith("7A3F")]

    module = _module(edit=no_ft_peer, commands={"AT+CPWROFF": [["ERROR"], ["OK"]], "AT+UBTGS=*": ["ERROR"]})
    try:
        out = _shell(app_exe, module.port, tmp_path, "ft.on rw", "ft.on rw", "ft.status", f"bt.connect {PEER}",
                     "ft.list 0")
    finally:
        module.stop()
    errors = [line.split("ERROR: ")[1].split(" (")[0] for line in out.stdout.splitlines() if "ERROR: " in line]
    assert errors == ["reboot failed", "cannot define the file transfer service",
                      "the peer has no file transfer service"], out.stdout
    assert "ft: not running" in out.stdout and out.returncode != 0, out.stdout
    assert module.log.count("AT+CPWROFF") == 2, module.log
    assert not any(c.startswith(("AT+UBTGC=", "AT+UBTGWNR=")) for c in module.log), module.log


def test_ft_ble_client_reports_a_link_it_cannot_use(app_exe, tmp_path):
    """A peer that refuses the notifications (link not encrypted) and a request the module cannot write are
    reported, and no file is made."""
    refuses = _module(commands={"AT+UBTGCCW=*": ["ERROR"]})
    try:
        first = _shell(app_exe, refuses.port, tmp_path, f"bt.connect {PEER}", "ft.get 0 notes.txt")
    finally:
        refuses.stop()
    deaf = _module(commands={"AT+UBTGWNR=*": ["ERROR"]})
    deaf.ft_peer = None
    try:
        second = _shell(app_exe, deaf.port, tmp_path, f"bt.connect {PEER}", "ft.remove 0 x.bin", "ft.list 0")
    finally:
        deaf.stop()
    assert "ERROR: cannot enable notifications (the link must be encrypted: pair first)" in first.stdout, first.stdout
    assert first.returncode != 0 and not any(c.startswith("AT+UBTGWNR=") for c in refuses.log), refuses.log
    assert second.stdout.count("ERROR: cannot send the request") == 2 and second.returncode != 0, second.stdout
    assert "deleted" not in second.stdout and "file(s)" not in second.stdout, second.stdout
    assert not list(tmp_path.glob("notes.txt*"))


# ---- fw.* ----

def test_fw_arguments_are_checked_before_the_module_is_asked(app_exe, tmp_path):
    """A baud rate of 0 for fw.flash and a URL without a file for fw.download print the usage and send nothing."""
    module = _module()
    try:
        out = _shell(app_exe, module.port, tmp_path, "fw.flash x.bin 0", "fw.download http://example.com/fw.bin")
    finally:
        module.stop()
    assert "usage: fw.flash" in out.stdout and "usage: fw.download" in out.stdout and out.returncode != 0, out.stdout
    assert not any(c.startswith(("AT+USYFWUS", "AT+UHTC")) for c in module.log), module.log


def test_fw_bootloader_refuses_a_name_without_an_extension(app_exe, tmp_path):
    """A file name shorter than '.bin' is not a firmware image: nothing is read or opened."""
    (tmp_path / "a").write_bytes(b"\xff" * 64)
    out = _run(app_exe, None, "fw.bootloader COM250 a", cwd=tmp_path)
    assert out.returncode != 0 and "ERROR: refusing to flash a: not a .bin file" in out.stdout, out.stdout
    assert "waiting" not in out.stdout and "cannot open" not in out.stdout, out.stdout


def test_fw_list_reports_a_store_it_cannot_read(app_exe, tmp_path):
    """fw.list fails with an error, not an empty listing, when the store folder cannot be listed."""
    (tmp_path / "storage").write_bytes(b"a file where the store folder should be")
    out = _run(app_exe, None, "fw.list", cwd=tmp_path)
    assert out.returncode != 0 and "ERROR: cannot list the store" in out.stdout, out.stdout
    assert "no firmware downloaded" not in out.stdout, out.stdout


def test_fw_update_looks_up_the_family_of_the_module(app_exe, tmp_path):
    """fw.update reads the version and the model, then asks GitHub for that family (evaluation builds too with
    'eval'): without the HTTPS client or the internet that fails, with them nothing newer than 99.9.9 is found."""
    def newest(profile):
        profile["identity"]["version"] = "99.9.9-001"

    module = _module(edit=newest)
    try:
        out = _run(app_exe, module.port, "fw.update eval", cwd=tmp_path)
    finally:
        module.stop()
    assert "current firmware: 99.9.9-001" in out.stdout, out.stdout
    assert "looking for the latest NORA-W36 firmware on GitHub (evaluation builds included)" in out.stdout, out.stdout
    assert "AT+GMR" in module.log and "AT+GMM" in module.log, module.log
    if out.returncode == 0:
        assert "already up to date; use 'fw.update force' to flash anyway" in out.stdout, out.stdout
    else:
        assert "ERROR: cannot query GitHub" in out.stdout, out.stdout
    assert not any(c.startswith("AT+USYFWUS") for c in module.log) and "flashing" not in out.stdout, out.stdout
