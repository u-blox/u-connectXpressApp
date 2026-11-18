"""fw.*, dtls.* and file.* paths that need a module or a peer: fw.flash and fw.bootloader against a vucx that turns
into an XMODEM receiver, DTLS client and server bridged through two vucx modules, and the usage and error paths."""

import io
import socket
import subprocess
import threading
import time
import zipfile

import pytest

pytestmark = pytest.mark.app

SOH, STX, EOT, ACK, CAN = 0x01, 0x02, 0x04, b"\x06", b"\x18"


def _run(exe, port, *commands, cwd, timeout=60):
    args = [str(exe), port] if port else [str(exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd)


def _profile(**identity):
    from harness.vucx import load

    profile = load("nora-w36.yaml")
    profile["identity"] = {**profile.get("identity", {}), **identity}
    profile["commands"] = dict(profile["commands"])
    return profile


def _fw_module(modes, pages=None, flashed=None, **identity):
    """A vucx served over TCP whose connections after the first behave as listed: at, startup (AT that sends
    +STARTUP), xmodem, xmodem-cancel, xmodem-close (closes the listener after the transfer), at-close, bootloader,
    bootloader-cancel. pages: its fake HTTP server (vucx.http_pages); flashed: the version it reports after a
    transfer."""
    from harness.vucx import Vucx, http_pages

    class FwModule(Vucx):
        def __init__(self):
            super().__init__(http_pages(_profile(**identity), pages) if pages else _profile(**identity))
            self.modes = list(modes)
            self.received = bytearray()
            self.prompted = False

        def _serve_tcp(self):
            while not self._stop.is_set():
                try:
                    conn, _ = self._listen.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                conn.settimeout(0.1)
                mode = self.modes.pop(0) if self.modes else "at"
                if mode.startswith(("xmodem", "bootloader")):
                    if mode == "xmodem-close":   # before the last ACK, or the app's reopen lands in the backlog
                        self._listen.close()
                    if mode.startswith("bootloader"):
                        self.prompted = self._prompt(conn)
                    self._xmodem(conn, cancel=mode.endswith("-cancel"))
                    conn.close()
                    continue
                self._conn, buf = conn, b""
                if mode == "startup":
                    threading.Timer(0.3, self._send_quiet, [["+STARTUP"]]).start()
                while not self._stop.is_set():
                    try:
                        chunk = conn.recv(4096)
                        if not chunk:
                            break
                        buf = self._feed(buf + chunk)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                self._conn = None
                conn.close()
                if mode == "at-close":
                    self._listen.close()

        def _read_until(self, conn, marker, seconds=10):
            buf, end = b"", time.monotonic() + seconds
            while marker not in buf and time.monotonic() < end:
                try:
                    chunk = conn.recv(256)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
            return marker in buf

        def _prompt(self, conn):
            if not self._read_until(conn, b"\r"):
                return False
            conn.sendall(b"\r\n>")
            return self._read_until(conn, b"x\r\n")

        def _xmodem(self, conn, cancel):
            buf, started, end = bytearray(), False, time.monotonic() + 30
            while time.monotonic() < end and not self._stop.is_set():
                if not started:
                    conn.sendall(b"C")
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                started = True
                buf += chunk
                while buf:
                    if buf[0] == EOT:
                        conn.sendall(ACK)
                        if flashed:
                            self.commands["AT+GMR"] = [f'"{flashed}"', "OK"]
                        return
                    if buf[0] not in (SOH, STX):
                        del buf[0]
                        continue
                    size = 128 if buf[0] == SOH else 1024
                    if len(buf) < size + 5:
                        break
                    if cancel:
                        conn.sendall(CAN * 3)
                        return
                    self.received += buf[3:3 + size]
                    del buf[:size + 5]
                    conn.sendall(ACK)

    module = FwModule()
    module.start_tcp()
    return module


@pytest.fixture
def image(tmp_path):
    data = bytes((i * 13 + 5) & 0xFF for i in range(2100))
    path = tmp_path / "NORA-W36X-SW-9.9.9.bin"
    path.write_bytes(data)
    return path, data


def test_fw_flash_sends_the_image_and_reconnects(app_exe, tmp_path, image):
    path, data = image
    module = _fw_module(["at", "xmodem", "startup"])
    try:
        out = _run(app_exe, module.port, f"fw.flash {path.name} 115200", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "flashing 2100 bytes via" in out.stdout and "transfer complete" in out.stdout, out.stdout
    assert "transferred 100%" in out.stdout and "firmware version: 3.5.0" in out.stdout, out.stdout
    assert "no +STARTUP" not in out.stdout, out.stdout
    assert bytes(module.received[:len(data)]) == data
    assert any(c.startswith("AT+USYFWUS") for c in module.log), module.log


def test_fw_flash_reports_a_cancelled_transfer(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module(["at", "xmodem-cancel", "at"])
    try:
        out = _run(app_exe, module.port, f"fw.flash {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0, out.stdout
    assert "ERROR: transfer failed" in out.stdout and "firmware version: 3.5.0" in out.stdout, out.stdout


def test_fw_flash_without_startup_warns_and_continues(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module(["at", "xmodem", "at"])
    try:
        out = _run(app_exe, module.port, f"fw.flash {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "transfer complete" in out.stdout and "no +STARTUP received" in out.stdout, out.stdout


def test_fw_flash_cannot_reopen_the_port(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module(["at", "xmodem-close"])
    try:
        out = _run(app_exe, module.port, f"fw.flash {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert "transfer complete" in out.stdout and "cannot reopen" in out.stdout, out.stdout
    assert out.returncode != 0, out.stdout


def test_fw_flash_cannot_open_the_port_for_xmodem(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module(["at-close"])
    try:
        out = _run(app_exe, module.port, f"fw.flash {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert "for XMODEM" in out.stdout and "cannot reopen" in out.stdout, out.stdout
    assert out.returncode != 0, out.stdout


def test_fw_flash_refuses_bad_images_on_a_module(app_exe, tmp_path):
    (tmp_path / "empty.bin").write_bytes(b"")
    module = _fw_module([])
    try:
        out = "".join(_run(app_exe, module.port, cmd, cwd=tmp_path).stdout
                       for cmd in ("fw.flash fw.hex", "fw.flash empty.bin"))
    finally:
        module.stop()
    assert "not a .bin file" in out and "cannot read empty.bin" in out, out
    assert not any(c.startswith("AT+USYFWUS") for c in module.log), module.log


def test_fw_bootloader_flashes_a_zip_after_the_prompt(app_exe, tmp_path, image):
    path, data = image
    with zipfile.ZipFile(tmp_path / "NORA-W36X-SW-9.9.9.ZIP", "w") as z:
        z.writestr("readme.txt", "x")
        z.write(path, "NORA-W36X-SW-9.9.9.bin")
    module = _fw_module(["bootloader"])
    try:
        out = _run(app_exe, None, f"fw.bootloader {module.port} NORA-W36X-SW-9.9.9.ZIP 115200", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "firmware image:" in out.stdout and "waiting up to 30 s" in out.stdout, out.stdout
    assert "the module reboots with the new firmware" in out.stdout, out.stdout
    assert module.prompted and bytes(module.received[:len(data)]) == data


def test_fw_bootloader_reports_a_cancelled_transfer(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module(["bootloader-cancel"])
    try:
        out = _run(app_exe, None, f"fw.bootloader {module.port} {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "ERROR: transfer failed" in out.stdout, out.stdout


def test_fw_bootloader_needs_a_disconnected_app(app_exe, tmp_path, image):
    path, _ = image
    module = _fw_module([])
    try:
        out = _run(app_exe, module.port, f"fw.bootloader COM250 {path.name}", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "disconnect first" in out.stdout, out.stdout


def test_fw_github_commands_need_a_known_module(app_exe, tmp_path):
    module = _fw_module([], model="LARA-R6")
    try:
        outs = [_run(app_exe, module.port, cmd, cwd=tmp_path)
                for cmd in ("fw.github.list", "fw.download", "fw.download latest x.zip",
                            "fw.update force eval 115200", "fw.update a b")]
    finally:
        module.stop()
    text = "".join(o.stdout for o in outs)
    assert all(o.returncode != 0 for o in outs), text
    assert text.count("unknown module model 'LARA-R6'") == 4, text
    assert "current firmware: 3.5.0" in text, text


def test_fw_update_without_a_version_says_unknown(app_exe, tmp_path):
    module = _fw_module([], model="LARA-R6")
    module.commands["AT+GMR"] = ["ERROR"]
    try:
        out = _run(app_exe, module.port, "fw.update", cwd=tmp_path)
    finally:
        module.stop()
    assert "current firmware: unknown" in out.stdout and out.returncode != 0, out.stdout


def test_fw_github_list_for_a_family_needs_the_module(app_exe, tmp_path):
    """GitHub is only reached through the module's Wi-Fi, never the host network."""
    out = _run(app_exe, None, "fw.github.list NORA-W36", cwd=tmp_path, timeout=120)
    assert "NORA-W36 firmware releases on GitHub, newest first:" in out.stdout, out.stdout
    assert out.returncode != 0 and "GitHub is reached through the module's Wi-Fi" in out.stdout, out.stdout


def test_fw_download_url_checks_the_sha256(app_exe, tmp_path):
    module = _fw_module([])
    try:
        bad = _run(app_exe, module.port, "fw.download http://example.com/fw.bin fw.bin " + "00" * 32, cwd=tmp_path)
        good = _run(app_exe, module.port, "fw.download http://example.com/fw.bin fw2.bin", cwd=tmp_path)
    finally:
        module.stop()
    assert bad.returncode != 0 and "sha256 mismatch" in bad.stdout and "file removed" in bad.stdout, bad.stdout
    assert not (tmp_path / "storage" / "fw.bin").exists()
    assert good.returncode == 0 and "sha256: " in good.stdout, good.stdout
    assert (tmp_path / "storage" / "fw2.bin").exists(), good.stdout


_RELEASES = "/repos/u-blox/u-connectXpress/releases?per_page=100"
_DOWNLOAD = "https://github.com/u-blox/u-connectXpress/releases/download"


def _releases(*assets, pad=1500):
    """The GitHub releases JSON listing the assets, padded so it streams in several chunks."""
    return "[" + ",".join('{"notes":"' + "n" * pad + f'","browser_download_url" : "{_DOWNLOAD}/v/{a}"' + "}"
                          for a in assets) + "]"


def _fw_zip(version, data):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(f"NORA-W36X-SW-{version}.bin", data)
    return buf.getvalue()


def test_fw_github_lookup_and_download_go_through_the_module(app_exe, tmp_path):
    """The release list and the zip come from the module's HTTP client (AT+UHTC*), never the host network; the
    releases JSON is scanned as it streams, so assets far apart and past the first chunks are all found."""
    from harness.vucx import Vucx, http_pages

    assets = ["NORA-W36-3.5.1.zip", "NORA-W36-3.6.0-019-EVAL.zip", "NORA-B26-3.6.0.zip", "NORA-W36-3.4.0.zip"]
    module = Vucx(http_pages(_profile(), {
        _RELEASES: ("200 OK", ["Content-Type: application/json"], _releases(*assets)),
        f"/u-blox/u-connectXpress/releases/download/v/{assets[0]}":
            ("302 Found", ["Location: https://objects.example/cdn/fw.zip"], ""),
        "/cdn/fw.zip": ("200 OK", ["Content-Length: 7"], "ZIPDATA")}))
    module.start()
    try:
        listing = _run(app_exe, module.port, "fw.github.list", cwd=tmp_path)
        download = _run(app_exe, module.port, "fw.download", cwd=tmp_path)
    finally:
        module.stop()
    assert listing.returncode == 0, listing.stdout
    assert ("NORA-W36-3.5.1.zip\nNORA-W36-3.6.0-019-EVAL.zip  (evaluation)\nNORA-W36-3.4.0.zip\n"
            in listing.stdout.replace("\r", "")), listing.stdout
    assert "NORA-B26" not in listing.stdout, listing.stdout
    assert download.returncode == 0 and "through the module" in download.stdout, download.stdout
    assert (tmp_path / "storage" / assets[0]).read_bytes() == b"ZIPDATA"
    assert any(c.startswith('AT+UHTCRP=0,"/cdn/fw.zip"') for c in module.log), module.log
    # GitHub redirects to another host, so the firmware download always follows redirects in the app
    assert "AT+UHTCFRE=0" in module.log and "AT+UHTCFRE=1" not in module.log, module.log


def test_fw_download_url_failure_is_passed_on(app_exe, tmp_path):
    module = _fw_module([])
    module.commands["AT+UHTCRG=*"] = ["ERROR"]
    module.commands["AT+UHTCGH=*"] = ["ERROR"]
    module.commands["AT+UHTCGBB=*"] = ["ERROR"]
    try:
        out = _run(app_exe, module.port, "fw.download http://example.com/fw.bin fw.bin", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "sha256:" not in out.stdout, out.stdout


_ZIP_PATH = "/u-blox/u-connectXpress/releases/download/v/"
_W36 = ["NORA-W36-3.6.0-019-EVAL.zip", "NORA-W36-3.5.1.zip", "NORA-W36-3.4.0.zip"]


def test_fw_update_downloads_flashes_and_checks_the_newest_release(app_exe, tmp_path):
    """fw.update picks the highest release (not the newest published, evaluation builds left out), downloads it
    through the module, flashes the image inside the zip and checks the version the module reports after."""
    image = bytes((i * 11) & 0xFF for i in range(1500))
    module = _fw_module(["at", "xmodem", "startup"], version="3.4.0-0", flashed="3.5.1-0", pages={
        _RELEASES: ("200 OK", [], _releases(*_W36)),
        _ZIP_PATH + _W36[1]: ("200 OK", [], _fw_zip("3.5.1", image))})
    try:
        out = _run(app_exe, module.port, "fw.update 115200", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode == 0, out.stdout
    assert "latest firmware:  3.5.1-0 (NORA-W36-3.5.1.zip)" in out.stdout, out.stdout
    assert "downloading NORA-W36-3.5.1.zip through the module" in out.stdout, out.stdout
    assert "updated to 3.5.1-0" in out.stdout, out.stdout
    assert bytes(module.received[:len(image)]) == image
    assert (tmp_path / "storage" / "ucxapp.fwlatest").read_text().startswith("NORA-W36-3.5.1.zip")


def test_fw_update_uses_a_stored_zip_and_notices_a_wrong_version(app_exe, tmp_path):
    """A zip already in the store is not downloaded again; a module that still reports the old version after the
    flash is an error."""
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / _W36[0]).write_bytes(_fw_zip("3.6.0", b"\x5a" * 300))
    module = _fw_module(["at", "xmodem", "startup"], version="3.5.1-0",
                        pages={_RELEASES: ("200 OK", [], _releases(*_W36))})
    try:
        out = _run(app_exe, module.port, "fw.update eval", cwd=tmp_path)
    finally:
        module.stop()
    assert "using NORA-W36-3.6.0-019-EVAL.zip already in the store" in out.stdout, out.stdout
    assert out.returncode != 0 and "ERROR: module reports 3.5.1-0, expected 3.6.0-19" in out.stdout, out.stdout
    assert not any(_ZIP_PATH in c for c in module.log), module.log


@pytest.mark.parametrize("command,version,expect", [
    ("fw.update", "3.5.1-0", "already up to date; use 'fw.update force' to flash anyway"),
    ("fw.update force", "3.5.1-0", "ERROR: download of NORA-W36-3.5.1.zip failed"),
    ("fw.download eval my.zip", "3.5.1-0", "flash it with: fw.flash my.zip"),
    ("fw.github.list", "3.5.1-0", "NORA-W36-3.4.0.zip\n"),
])
def test_fw_github_update_paths(app_exe, tmp_path, command, version, expect):
    module = _fw_module([], version=version, pages={
        _RELEASES: ("200 OK", [], _releases(*_W36, pad=10)),
        _ZIP_PATH + _W36[0]: ("200 OK", [], "EVALZIP"),
        _ZIP_PATH + _W36[1]: ("404 Not Found", [], "")})
    try:
        out = _run(app_exe, module.port, command, cwd=tmp_path)
    finally:
        module.stop()
    assert expect in out.stdout.replace("\r", ""), out.stdout
    assert (out.returncode == 0) == ("ERROR" not in expect), out.stdout
    if "my.zip" in command:
        assert (tmp_path / "storage" / "my.zip").read_bytes() == b"EVALZIP"


def test_fw_github_list_with_no_release_for_the_family(app_exe, tmp_path):
    module = _fw_module([], pages={_RELEASES: ("200 OK", [], _releases(*_W36, pad=10))})
    try:
        out = _run(app_exe, module.port, "fw.github.list NORA-B26", cwd=tmp_path)
    finally:
        module.stop()
    assert "no firmware releases for NORA-B26" in out.stdout, out.stdout


def test_fw_github_lookup_reports_a_module_without_internet(app_exe, tmp_path):
    module = _fw_module([], pages={_RELEASES: ("503 Service Unavailable", [], "")})
    module.commands["AT+UHTCGBB=*"] = ["ERROR"]
    try:
        out = _run(app_exe, module.port, "fw.update", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "cannot query GitHub through the module" in out.stdout, out.stdout


def test_fw_list_shows_the_firmware_in_the_store(app_exe, tmp_path):
    (tmp_path / "storage").mkdir()
    for name in ("a.zip", "b.BIN", "notes.txt"):
        (tmp_path / "storage" / name).write_bytes(b"x" * 3)
    out = _run(app_exe, None, "fw.list", cwd=tmp_path)
    assert out.returncode == 0 and "a.zip" in out.stdout and "b.BIN" in out.stdout, out.stdout
    assert "notes.txt" not in out.stdout and "no firmware downloaded" not in out.stdout, out.stdout


# ---- dtls.* on a module that is not virtual (no VUCX serial number) ----

def _tls_store(app_exe, cwd):
    out = _run(app_exe, None, "utils.tls.ca.create", "utils.tls.server.create server localhost 127.0.0.1", cwd=cwd)
    assert out.returncode == 0, out.stdout


def _real_module(profile=None):
    from harness.vucx import Vucx

    module = Vucx(profile or _profile(serial="NORA0001"))
    module.start()
    return module


def test_dtls_client_and_server_over_bridged_modules(app_exe, tmp_path):
    from harness.vucx import Network

    _tls_store(app_exe, tmp_path)

    class Bridge(Network):
        def __init__(self, cfg, src):
            super().__init__(cfg)
            self.src, self.other = src, None

        def _serve(self, s, data, ip="192.168.1.10", port=None):
            if self.other.socks:
                self.other.socks[min(self.other.socks)]["rx"].append((*self.src, bytes(data)))

    profile = _profile(serial="NORA0001")
    server, client = _real_module(profile), _real_module(profile)
    server.net, client.net = Bridge(profile["sockets"], ("192.168.1.10", 5684)), \
        Bridge(profile["sockets"], ("192.168.1.20", 40000))
    server.net.other, client.net.other = client.net, server.net
    srv = subprocess.Popen([str(app_exe), server.port, "--exec", "dtls.server 5684 server"], cwd=tmp_path,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                           stdin=subprocess.DEVNULL)
    try:
        cli = _run(app_exe, client.port, "dtls.client localhost 5684 hello-dtls", cwd=tmp_path, timeout=90)
        srv_out = srv.communicate(timeout=90)[0]
    finally:
        srv.kill()
        server.stop()
        client.stop()
    assert "DTLS established" in cli.stdout and "reply: hello-dtls" in cli.stdout, cli.stdout + srv_out
    assert cli.returncode == 0, cli.stdout
    assert "DTLS established" in srv_out and "echoed 10 bytes" in srv_out, srv_out


def test_dtls_client_without_a_ca_or_peer(app_exe, tmp_path):
    module = _real_module()
    try:
        no_ca = _run(app_exe, module.port, "dtls.client localhost 7", cwd=tmp_path)
        _tls_store(app_exe, tmp_path)
        echo = _run(app_exe, module.port, "dtls.client localhost 7", cwd=tmp_path)
    finally:
        module.stop()
    assert no_ca.returncode != 0 and "ERROR: certificate not found" in no_ca.stdout, no_ca.stdout
    assert echo.returncode != 0 and "DTLS handshake with localhost:7 failed" in echo.stdout, echo.stdout


def test_dtls_server_without_its_certificate(app_exe, tmp_path):
    module = _real_module()
    try:
        out = _run(app_exe, module.port, "dtls.server 8000 missing", cwd=tmp_path)
    finally:
        module.stop()
    assert out.returncode != 0 and "ERROR: certificate not found" in out.stdout, out.stdout


def test_dtls_socket_errors(app_exe, tmp_path):
    profile = _profile(serial="NORA0001")
    del profile["sockets"]
    profile["commands"].update({"AT+USOCR=*": [["ERROR"], ["+USOCR:0", "OK"]], "AT+USOC=*": ["ERROR"],
                                "AT+USOB=*": ["ERROR"]})
    module = _real_module(profile)
    try:
        outs = [_run(app_exe, module.port, cmd, cwd=tmp_path)
                for cmd in ("dtls.client localhost 5684", "dtls.client localhost 5684", "dtls.server 5684",
                            "dtls.server 5684", "dtls.client h 0", "dtls.server 70000", "dtls.selftest a b")]
    finally:
        module.stop()
    text = "".join(o.stdout for o in outs)
    assert all(o.returncode != 0 for o in outs), text
    assert "failed to create a UDP socket for dtls.client" in text, text
    assert "connect to localhost:5684 failed" in text, text
    assert "failed to create a UDP socket for dtls.server" in text, text
    assert "bind to port 5684 failed" in text, text


# ---- file.* (YMODEM) on a console without raw serial access ----

def test_file_send_and_recv_argument_checks(app_exe, tmp_path):
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "data.txt").write_bytes(b"x" * 600)
    outs = [_run(app_exe, None, cmd, cwd=tmp_path)
            for cmd in ("file.send data.txt", "file.send missing.txt", "file.send dir/..", "file.recv a/..",
                        "file.recv a b", "file.send", "file.selftest x")]
    text = "".join(o.stdout for o in outs)
    assert all(o.returncode != 0 for o in outs), text
    assert text.count("no raw serial access") == 1, text
    assert "ERROR: cannot open missing.txt" in text, text
