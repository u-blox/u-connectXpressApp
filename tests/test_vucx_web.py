"""web.start against scripted HTTP clients of the virtual module (Network requests): the routes, the token,
uploads and downloads, and the error paths of u_webserver.c, all without a browser or a network."""

import re
import subprocess
import time

import pytest

pytestmark = pytest.mark.app

TOKEN = "0123456789abcdef0123456789abcdef"


def _get(path, token=True, extra=""):
    auth = f"X-Token: {TOKEN}\r\n" if token else ""
    return f"GET {path} HTTP/1.1\r\nHost: m\r\n{auth}{extra}\r\n"


def _post(path, body, token=True):
    auth = f"X-Token: {TOKEN}\r\n" if token else ""
    return f"POST {path} HTTP/1.1\r\nHost: m\r\n{auth}Content-Length: {len(body)}\r\n\r\n{body}"


def _serve(app_exe, tmp_path, requests, wait_s=20, then=()):
    """Starts web.start 8080 <token>; the clients connect as it listens. `then`: commands typed in the app once
    every client has its answer. Returns (stdout, replies as text)."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8080: requests}
    module = Vucx(profile)
    proc = None
    try:
        proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                cwd=tmp_path)
        proc.stdin.write(f"web.start 8080 {TOKEN} http\n")
        proc.stdin.flush()
        net = module.net
        end = time.monotonic() + wait_s
        while time.monotonic() < end:
            if len(net.replies) == len(requests) and all(b"\r\n\r\n" in r for r in net.replies):
                time.sleep(0.3)        # let the bodies finish
                break
            time.sleep(0.05)
        try:
            out = proc.communicate("".join(c + "\n" for c in then) + "web.status\nweb.stop\nweb.status\n",
                                   timeout=30)[0]
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail(f"the app hung on exit:\n{proc.communicate()[0][-3000:]}\nlast AT: {module.log[-20:]}")
        replies = [bytes(r).decode("latin-1") for r in net.replies]
        assert len(replies) == len(requests), out[-2000:] + "\n" + "\n".join(map(str, module.log[-40:]))
        return out, replies
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        module.stop()


def test_web_routes_answer_with_and_without_the_token(app_exe, tmp_path):
    reqs = [_get("/"), _get("/icon.svg"), _get("/api/version"), _get("/api/groups"),
            _get("/api/commands?g=wifi"), _get("/api/find?q=scan"), _get("/api/find?q=wifi&p=1"),
            _get("/api/complete?q=wifi.s"), _get("/api/examples?g=wifi"), _get("/api/manual"),
            _get("/api/version", token=False), _get("/api/groups", token=False),
            _get("/api/find", token=True), _get("/nope"), "BREW /pot HTTP/1.1\r\n\r\n",
            _get("/api/version?t=" + TOKEN, token=False),
            _get("/api/version", token=False, extra=f"Cookie: ucx={TOKEN}\r\n"),
            _get("/api/version", token=False, extra="X-Token: wrong\r\n")]
    out, replies = _serve(app_exe, tmp_path, reqs)
    assert "Web server" in out or "web" in out.lower(), out
    assert "note: plain HTTP" in out and "leave out http" in out, out
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert status[0].startswith("HTTP/1.1 200"), replies[0][:300]
    assert "<html" in replies[0].lower() or "<!doctype" in replies[0].lower(), replies[0][:300]
    assert status[1].startswith("HTTP/1.1 200") and "svg" in replies[1], replies[1][:300]
    for i in range(2, 10):
        assert status[i].startswith("HTTP/1.1 200"), (i, replies[i][:400])
    assert "Set-Cookie" in replies[3], replies[3][:400]
    assert "wifi.scan" in replies[5], replies[5][:400]
    assert "# " in replies[9] or "|" in replies[9], replies[9][:400]
    assert "403" in status[10] and "403" in status[11], status
    assert "200" not in status[12], replies[12][:300]
    assert "404" in status[13], status
    assert "200" not in status[14], status
    assert "200" in status[15], status
    assert "403" in status[17], status


def test_web_rest_field_shows_what_an_empty_field_gives(app_exe, tmp_path):
    import json

    _, replies = _serve(app_exe, tmp_path, [_get("/api/commands?g=matter")])
    cmds = json.loads(replies[0].split("\r\n\r\n", 1)[1])
    start = next(c for c in cmds if c["n"] == "matter.start")
    # a list to pick from with the default named, or endpoints typed
    assert start["f"] == [["all|light|temp|contact|<endpoints>", 1, "all|light|temp|contact", "all", "o", ""]], start


def test_web_page_is_gzip_for_browsers_and_demo_flag_follows_the_module(app_exe, tmp_path):
    import gzip

    reqs = [_get("/", extra="Accept-Encoding: deflate, GZIP;q=1\r\n"), _get("/"),
            _get("/", extra="accept-encoding: br\r\n"), _get("/api/version")]
    _, replies = _serve(app_exe, tmp_path, reqs)
    head, body = replies[0].split("\r\n\r\n", 1)
    assert "Content-Encoding: gzip" in head and "Vary: Accept-Encoding" in head, head
    page = gzip.decompress(body.encode("latin-1")).decode("utf-8")
    assert "<html" in page.lower() or "<!doctype" in page.lower(), page[:200]
    plain = replies[1].split("\r\n\r\n", 1)
    assert "Content-Encoding" not in plain[0] and plain[1] == page, plain[0]
    assert "Content-Encoding" not in replies[2].split("\r\n\r\n", 1)[0], replies[2][:300]
    assert len(body) * 2 < len(plain[1]), (len(body), len(plain[1]))
    assert '"real":0' in replies[3], replies[3][-200:]


def test_web_exec_runs_commands_and_guards_dangerous_ones(app_exe, tmp_path):
    reqs = [_post("/api/exec?cmd=help%20wifi", ""), _post("/api/exec?cmd=app.version&r=1", ""),
            _post("/api/exec?cmd=nosuch.cmd", ""), _post("/api/exec", ""),
            _post("/api/exec?cmd=dev.factory", ""), _post("/api/exec?cmd=help", "", token=False),
            _get("/events", token=False), _get("/events")]
    out, replies = _serve(app_exe, tmp_path, reqs)
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert "200" in status[0] and "accepted" in replies[0], replies[0][:500]
    assert "200" in status[1], replies[1][:500]
    assert "200" not in status[3], replies[3][:300]
    assert "AT+UFWFACTORY" not in out, out
    assert "403" in status[5] and "403" in status[6], status
    assert "200" in status[7] and "text/event-stream" in replies[7], replies[7][:500]


def test_web_exec_never_cuts_an_argument(app_exe, tmp_path):
    """A field sent %-encoded takes three times its length: it is decoded whole, or refused (414), never cut into
    another value. A name is matched whole: a11= is not a1=."""
    slashes = "%2F" * 32
    reqs = [_post("/api/exec?cmd=help&a0=" + "%2F" * 96, ""), _post("/api/exec?cmd=help&a0=" + "x" * 600, ""),
            _post("/api/exec?cmd=help&a11=zz&a0=" + slashes, "")]
    out, replies = _serve(app_exe, tmp_path, reqs)
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert "414" in status[0] and "argument too long" in replies[0], replies[0][:300]
    assert "414" in status[1] and "URI too long" in replies[1], replies[1][:300]
    assert "200" in status[2], replies[2][:300]
    assert "No commands match '" + "/" * 32 + "'" in out, out[-1500:]
    assert "zz" not in out, out[-1500:]

def test_web_upload_and_download_a_file(app_exe, tmp_path):
    reqs = [_post("/api/upload?n=note.txt&o=0", "hello "), _post("/api/upload?n=note.txt&o=6&e=1", "web\n"),
            _get("/api/files"), _get("/api/file?n=note.txt"), _get("/api/file?n=note.txt&o=6"),
            _get("/api/file?n=missing.txt"), _post("/api/upload?n=../x&o=0", "x"),
            _post("/api/upload?n=other.txt&o=5", "x"), _post("/api/upload?n=key.pem&o=0", "x"),
            _get("/api/file?n=../etc/passwd")]
    out, replies = _serve(app_exe, tmp_path, reqs)
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert "200" in status[0] and "200" in status[1], replies[:2]
    assert "note.txt" in replies[2], replies[2][:400]
    assert replies[3].endswith("hello web\n"), replies[3][-200:]
    assert replies[4].endswith("\r\n\r\nweb\n"), replies[4][-200:]
    assert "404" in status[5], status
    assert "400" in status[6] and "409" in status[7], status
    assert "200" not in status[9], status


def test_a_body_that_did_not_arrive_whole_is_refused_not_stored(app_exe, tmp_path):
    """The request buffer takes 2 KB: a larger upload slice arrives cut off. Storing what came and answering ok
    would leave a damaged file that looks uploaded."""
    whole = _post("/api/upload?n=big.bin&o=0&e=1", "x" * 3000)
    big = [whole[:1200], whole[1200:2400], whole[2400:]]   # as the network delivers it: in segments
    negative = _post("/api/upload?n=neg.bin&o=0&e=1", "hello").replace("Content-Length: 5", "Content-Length: -5")
    words = _post("/api/upload?n=abc.bin&o=0&e=1", "hello").replace("Content-Length: 5", "Content-Length: lots")
    _, replies = _serve(app_exe, tmp_path, [big, negative, words, _post("/api/upload?n=ok.bin&o=0&e=1", "hello")])
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert "413" in status[0] and "request too large" in replies[0], replies[0][:300]
    assert "400" in status[1] and "400" in status[2] and "bad Content-Length" in replies[1], status
    assert "200" in status[3], status
    stored = sorted(p.name for p in (tmp_path / "storage").iterdir())
    assert "ok.bin" in stored and not {"big.bin", "neg.bin", "abc.bin"} & set(stored), stored


class _Silent:
    """A connection that is opened and then left alone, as a browser's spare connection is."""
    idle = True      # the next client connects beside it
    closed = False

    def __call__(self, data):
        return b""


def test_the_event_stream_goes_to_the_connection_that_asked_for_it(app_exe, tmp_path):
    """Another connection that is open when the page asks for /events, with no request and no token, must not get
    the stream (the commands' output, the node's setup codes) instead of the page or beside it."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8080: [_Silent(), _get("/events")]}
    module = Vucx(profile)
    proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path)

    def stream(i):
        return bytes(module.net.replies[i]) if len(module.net.replies) > i else b""

    def until(check, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end and not check():
            time.sleep(0.05)
        return check()

    try:
        proc.stdin.write(f"web.start 8080 {TOKEN} http\n")
        proc.stdin.flush()
        assert until(lambda: b"event: connected" in stream(1)), (stream(1), module.log[-20:])
        # nothing runs: a page that (re)connects is told so, or a command that ended while it was away leaves Run off
        assert until(lambda: b"event: done" in stream(1)), stream(1)[-600:]
        proc.stdin.write("app.version\n")
        proc.stdin.flush()
        assert until(lambda: b"event: out" in stream(1) and b"u-connectXpressApp " in stream(1)), stream(1)[-600:]
        assert stream(0) == b"", stream(0)[:300]
        proc.stdin.write("web.stop\n")
        proc.stdin.close()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()


def test_a_client_that_keeps_its_connection_is_closed_after_the_answer(app_exe, tmp_path):
    """The module has six sockets. A client that does not hang up after its answer would keep one for good."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    class Stays:
        closed = False

        def __call__(self, data):
            return _get("/api/version").encode() if data is None else b""

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8080: [Stays()]}
    module = Vucx(profile)
    proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path)
    try:
        proc.stdin.write(f"web.start 8080 {TOKEN} http\n")
        proc.stdin.flush()
        end = time.monotonic() + 20
        while time.monotonic() < end and not any(c.startswith("AT+USOCL=") for c in module.log):
            time.sleep(0.05)
        closed = [c for c in module.log if c.startswith("AT+USOCL=")]
        answer = bytes(module.net.replies[0]) if module.net.replies else b""
        proc.stdin.write("web.stop\n")
        proc.stdin.close()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert b"HTTP/1.1 200" in answer and b'{"v":"u-connectXpressApp ' in answer, answer[:300]
    assert closed, "the server never closed the connection it had answered"


def test_connections_to_another_listener_are_not_the_web_servers(app_exe, tmp_path):
    """Every incoming connection is told to the web server too. One to another listener (sock.listen, a bridge) is
    not a page: taken as one, the server reads what the client sent before its owner does."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {9000: ["hello from a client\n"]}
    module = Vucx(profile)
    proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path)
    try:
        proc.stdin.write(f"web.start 8080 {TOKEN} http\nsock.open tcp\nsock.listen 9000\n")
        proc.stdin.flush()
        end = time.monotonic() + 20
        while time.monotonic() < end and not module.net.replies:
            time.sleep(0.05)
        assert module.net.replies, module.log[-20:]
        time.sleep(1.0)   # many rounds of the web server's loop
        reads = [c for c in module.log if c.startswith("AT+USORB=")]
        proc.stdin.write("sock.read 2 100 1\nweb.stop\n")   # 0 the page's listener, 1 ours, 2 its client
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=20)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    assert not reads, f"read by the web server: {reads}"
    assert "hello from a client" in out, out[-600:] + " | ".join(module.log[-15:])


def test_the_page_cannot_take_away_the_module_or_the_server_it_is_served_by(app_exe, tmp_path):
    """dev.disconnect and dev.use close the AT client the web server's thread is using; web.stop from a command
    the page ran (an example) would stop the worker it runs on."""
    reqs = [_post("/api/exec?cmd=dev.disconnect", ""), _get("/api/version"),
            _post("/api/exec?cmd=examples%20web%202", ""), _get("/api/version")]
    out, replies = _serve(app_exe, tmp_path, reqs, then=("dev.use tcp:127.0.0.1:1", "dev.disconnect"))
    status = [r.split("\r\n", 1)[0] for r in replies]
    assert all("200" in s for s in status), status   # both accepted, and the server still answers after each
    assert out.count("ERROR: the web server runs on this module; web.stop first, then dev.disconnect") == 2, out
    assert "ERROR: the web server runs on this module; web.stop first, then dev.use" in out, out
    assert "ERROR: web.stop cannot run from the page it serves; run it in the app" in out, out
    assert "web server: http port 8080" in out and "web server stopped" in out, out


# ---- HTTPS: TLS clients behind the module (harness/tlsclient.py) do a real handshake with the app's server --------


def _serve_https(app_exe, tmp_path, clients, start, before=(), wait_s=40):
    """Runs `before` and `start` (a web.start command on port 8443); the clients connect as it listens, one after
    the other. Returns the app's output once every client has hung up."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8443: clients}
    module = Vucx(profile)
    proc = None
    try:
        proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                cwd=tmp_path)
        proc.stdin.write("".join(line + "\n" for line in (*before, start)))
        proc.stdin.flush()
        end = time.monotonic() + wait_s
        while time.monotonic() < end and not all(getattr(c, "closed", True) for c in clients):
            time.sleep(0.05)
        time.sleep(0.3)
        try:
            out = proc.communicate("web.status\nweb.stop\n", timeout=30)[0]
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail(f"the app hung on exit:\n{proc.communicate()[0][-3000:]}\nlast AT: {module.log[-20:]}")
        open_ = [i for i, c in enumerate(clients) if not getattr(c, "closed", True)]
        assert not open_, f"clients {open_} got no answer:\n{out[-2000:]}\n" + "\n".join(map(str, module.log[-30:]))
        return out
    finally:
        if proc and proc.poll() is None:
            proc.kill()
        module.stop()


def _status(client):
    assert client.error is None, client.error
    return client.response.split(b"\r\n", 1)[0].decode()


def test_https_serves_the_api_and_files_with_a_certificate_from_the_local_ca(app_exe, tmp_path):
    from harness.tlsclient import TlsClient

    ca = tmp_path / "storage" / "tls-ca.crt"   # made by web.start, read when the first client connects
    payload = bytes(range(256))
    upload = (f"POST /api/upload?n=hw.bin&o=0&e=1 HTTP/1.1\r\nHost: m\r\nX-Token: {TOKEN}\r\n"
              f"Content-Length: {len(payload)}\r\n\r\n").encode() + payload
    clients = [TlsClient(r, ca=ca) for r in (
        _get("/api/version"), _get("/api/version", token=False), upload, _get("/api/file?n=hw.bin&o=0"),
        _get("/api/file?n=tls-web.key&o=0"), _get("/api/file?n=tls-ca.crt&o=0"), _get("/"))]
    out = _serve_https(app_exe, tmp_path, clients, f"web.start 8443 {TOKEN}")
    assert "created certificate tls-web for 192.168.1.50, signed by the local CA (tls-ca)" in out, out
    assert f"https://192.168.1.50:8443/#t={TOKEN}" in out, out
    assert "web server: https port 8443" in out, out
    status = [_status(c) for c in clients]
    assert all(c.version in ("TLSv1.2", "TLSv1.3") for c in clients), [c.version for c in clients]
    assert "200" in status[0] and b'{"v":"u-connectXpressApp ' in clients[0].response, clients[0].response[:300]
    assert "403" in status[1], status
    assert "200" in status[2] and "200" in status[3], status
    assert clients[3].response.split(b"\r\n\r\n", 1)[1] == payload, "the file must come back byte for byte"
    assert "403" in status[4], "private keys never leave the device"
    assert "200" in status[5] and b"BEGIN CERTIFICATE" in clients[5].response, status
    page = clients[6].response.split(b"\r\n\r\n", 1)[1]
    assert "200" in status[6] and len(page) > 50000 and b"</html>" in page[-200:], (status[6], len(page))
    key = (tmp_path / "storage" / "tls-web.key").read_bytes()
    assert b"PRIVATE KEY" not in key, "the server key must be sealed in the store"


def test_https_needs_a_token_unless_opened(app_exe, tmp_path):
    from harness.tlsclient import TlsClient

    ca = tmp_path / "storage" / "tls-ca.crt"
    client = TlsClient(_get("/api/version", token=False), ca=ca)
    out = _serve_https(app_exe, tmp_path, [client], "web.start 8443")
    assert re.search(r"https://192\.168\.1\.50:8443/#t=[0-9a-f]{32}\n", out), out
    assert "403" in _status(client), client.response[:200]

    client = TlsClient(_get("/api/version", token=False), ca=ca)
    out = _serve_https(app_exe, tmp_path, [client], "web.start 8443 open")
    assert "no access token: anyone who can reach this address can use the page (leave out open for one)" in out
    assert "https://192.168.1.50:8443/\n" in out, out
    assert "200" in _status(client), client.response[:200]


def test_https_goes_on_after_a_client_that_distrusts_it_and_one_that_speaks_plain_http(app_exe, tmp_path):
    from harness.tlsclient import TlsClient

    other = tmp_path / "other"
    other.mkdir()
    made = subprocess.run([str(app_exe), "--offline", "--exec", "utils.tls.ca.create"], capture_output=True,
                          text=True, timeout=60, cwd=other, stdin=subprocess.DEVNULL)
    assert made.returncode == 0, made.stdout
    stranger = TlsClient(_get("/api/version"), ca=other / "storage" / "tls-ca.crt")
    friend = TlsClient(_get("/api/version"), ca=tmp_path / "storage" / "tls-ca.crt")
    plain = _get("/api/version")   # not TLS at all: the server must drop it and take the next client
    out = _serve_https(app_exe, tmp_path, [stranger, plain, friend], f"web.start 8443 {TOKEN}")
    assert stranger.error is not None and "CERTIFICATE_VERIFY_FAILED" in str(stranger.error), stranger.error
    assert stranger.response == b"", "nothing may be served to a client that did not finish the handshake"
    assert "200" in _status(friend), out[-1500:]


def test_https_with_a_named_certificate_and_one_that_does_not_exist(app_exe, tmp_path):
    from harness.tlsclient import TlsClient

    client = TlsClient(_get("/api/version"), ca=tmp_path / "storage" / "tls-ca.crt")
    out = _serve_https(app_exe, tmp_path, [client], f"web.start 8443 {TOKEN} https box",
                       before=["web.start 8443 tok https nosuch", "utils.tls.ca.create",
                               "utils.tls.server.create box 192.168.1.50 box.local"])
    assert "ERROR: TLS certificate 'nosuch':" in out, out
    assert "utils.tls.server.create nosuch 192.168.1.50" in out, out
    assert "created certificate tls-web" not in out, "a named certificate must not make tls-web"
    assert "200" in _status(client), client.response[:200]
    assert not (tmp_path / "storage" / "tls-web.crt").exists()


def test_kiosk_gets_the_triggers_a_qr_code_and_its_address(app_exe, tmp_path):
    """The kiosk view's parts: the "trigger" card on the event stream (and again when a rule changes), the QR code
    of the pairing code as an SVG path from /api/qr, and web.kiosk link printing the page's #kiosk address."""
    import json

    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"]["requests"] = {8080: [_get("/api/qr?q=MT:Y.K9042C00KA0648G00"), _get("/api/qr"),
                                             _get("/events")]}   # one after the other: the stream stays open
    module = Vucx(profile)
    proc = subprocess.Popen([str(app_exe), module.start(), "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path)

    def reply(i, text):
        end = time.monotonic() + 20
        seen = ""
        while time.monotonic() < end and text not in seen:
            time.sleep(0.05)
            seen = bytes(module.net.replies[i]).decode("latin-1") if len(module.net.replies) > i else ""
        assert text in seen, (i, text, [bytes(r)[-1500:] for r in module.net.replies])
        return seen

    try:
        proc.stdin.write(f"trigger.add boot event boot do app.wait 1\nweb.start 8080 {TOKEN} http\n")
        proc.stdin.flush()
        qr = json.loads(reply(0, "}").split("\r\n\r\n", 1)[1])
        assert qr["n"] >= 21 and re.fullmatch(r"(M\d+ \d+h\d+v1h-\d+z)+", qr["p"]), qr
        assert "200" not in reply(1, "\r\n\r\n").split("\r\n", 1)[0]        # nothing to draw
        reply(2, 'event: trigger\ndata: "RULE  boot | on | -1 | 0 | 0 | event \\"boot\\" |  | app.wait 1\\n"')
        proc.stdin.write("trigger.off boot\n")
        proc.stdin.flush()
        reply(2, 'data: "RULE  boot | off |')
        out = proc.communicate("web.kiosk link\nweb.stop\ntrigger.remove all\n", timeout=30)[0]
        assert re.search(rf"kiosk: http://[\d.]+:8080/#t={TOKEN}&kiosk$", out, re.M), out[-2000:]
        assert "kiosk opened" not in out, out[-2000:]
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()


def test_web_routes_refuse_without_the_token_and_name_what_is_missing(app_exe, tmp_path):
    import json

    arg = "x" * 90
    reqs = [_get("/api/conn"), _get("/api/conn", token=False), _get("/api/files", token=False),
            _get("/api/file?n=a.txt", token=False), _post("/api/upload?n=a.txt&o=0", "x", token=False),
            _get("/api/qr?q=x", token=False), _get("/api/examples"), _get("/api/examples?g=wifi", token=False),
            _get("/api/complete", token=False), _get("/api/complete"), _get("/api/commands"),
            _post("/api/upload?n=web.key&o=0", "x"), _post("/api/upload?n=tls-x.crt&o=0", "x"),
            _get("/api/version", token=False, extra=f"Cookie: theme=dark; ucxt={TOKEN}\r\n"),
            _get("/api/version", token=False, extra="Cookie: theme=dark; other=1\r\n"),
            _post("/api/exec?cmd=%20web.stop", ""), _post("/api/exec?cmd=", ""),
            _post(f"/api/exec?cmd=help&a0={arg}&a1={arg}&a2={arg}", ""),
            _post("/api/exec?cmd=app.wait%202000", ""), _post("/api/exec?cmd=help", ""),
            _get("/api/complete?q=wifi%2Esc")]
    out, replies = _serve(app_exe, tmp_path, reqs)
    status = [r.split("\r\n", 1)[0] for r in replies]
    body = [r.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in r else "" for r in replies]
    assert "200" in status[0] and "t" in json.loads(body[0]), replies[0][:300]
    for i in (1, 2, 3, 4, 5, 7, 8):
        assert "403" in status[i] and "token required" in body[i], (i, replies[i][:300])
    assert "400" in status[6] and "missing g" in body[6], replies[6][:300]
    assert "400" in status[9] and "missing q" in body[9], replies[9][:300]
    assert "400" in status[10] and "missing g" in body[10], replies[10][:300]
    for i in (11, 12):
        assert "403" in status[i] and "keys and certificates need https" in body[i], (i, replies[i][:300])
    assert "200" in status[13] and '"v":"u-connectXpressApp ' in body[13], replies[13][:300]
    assert "403" in status[14], replies[14][:300]
    assert "400" in status[15] and "command not allowed" in body[15], replies[15][:300]
    assert "400" in status[16] and "command not allowed" in body[16], replies[16][:300]
    assert "414" in status[17] and "command too long" in body[17], replies[17][:300]
    assert "200" in status[18] and "409" in status[19] and "busy" in body[19], status
    assert json.loads(body[20])["l"].startswith("wifi.scan"), body[20]
    assert not (tmp_path / "storage" / "web.key").exists()


def test_web_kiosk_starts_https_and_opens_a_browser(app_exe, tmp_path):
    """web.kiosk with no server starts one (HTTPS on 443 with the local CA's certificate) and opens the browser
    named by UCXAPP_BROWSER (tests/capture_fake.py here) full screen in its own profile."""
    import os
    import sys
    from pathlib import Path

    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    fake = [sys.executable, str(Path(__file__).with_name("capture_fake.py")), "browser"]
    browser = subprocess.list2cmdline(fake) if os.name == "nt" else " ".join(f"'{a}'" for a in fake)
    env = dict(os.environ, UCXAPP_BROWSER=browser, UCX_FAKE_DIR=str(tmp_path))
    (tmp_path / "exported").mkdir()
    module = Vucx("nora-w36.yaml")
    try:
        out = subprocess.run([str(app_exe), module.start(), "--shell"], input="web.export exported\nweb.kiosk\n"
                             "web.start\nweb.kiosk\nweb.export exported\nweb.kiosk link extra\nweb.stop\n",
                             capture_output=True, text=True, errors="replace", cwd=tmp_path, env=env,
                             timeout=120).stdout
    finally:
        module.stop()
    assert "exported " in out and (tmp_path / "exported" / "groups.json").exists(), out[-2000:]
    assert "created certificate tls-web for 192.168.1.50" in out, out[-2000:]
    assert re.search(r"kiosk: https://192\.168\.1\.50:443/#t=[0-9a-f]{32}&kiosk\n", out), out[-2000:]
    assert out.count("kiosk opened full screen; Alt+F4 closes it") == 2, out[-2000:]
    assert "already running; web.stop first" in out, out[-2000:]
    assert "ERROR: stop the web server first (web.stop)" in out, out[-2000:]
    end = time.monotonic() + 20
    calls = tmp_path / "calls.txt"
    while time.monotonic() < end and (not calls.exists() or calls.read_text().count("\n") < 1):
        time.sleep(0.1)
    lines = calls.read_text(encoding="utf-8").splitlines()
    assert re.match(r"browser --kiosk https://192\.168\.1\.50:443/#t=[0-9a-f]{32}&kiosk=window --no-first-run "
                    r"--no-default-browser-check --user-data-dir=.*ucxapp-kiosk$", lines[0]), lines


class _Idle:
    """A client that connects and says nothing; the next client connects beside it."""
    idle = True
    closed = False

    def __call__(self, data):
        return b""


def test_web_server_evicts_an_idle_client_when_every_slot_is_taken(app_exe, tmp_path):
    """Four silent connections fill the client slots: the fifth client takes the slot of the first, and a request
    that outgrows the request buffer is dropped without an answer."""
    big = "GET / HTTP/1.1\r\nX-Pad: " + "a" * 3000 + "\r\n\r\n"
    out, replies = _serve(app_exe, tmp_path, [_Idle(), _Idle(), _Idle(), _Idle(), _get("/api/version"), big],
                          wait_s=8)
    assert replies[4].startswith("HTTP/1.1 200") and '"v":"u-connectXpressApp ' in replies[4], replies
    assert replies[5] == "", replies[5][:200]
    assert all(r == "" for r in replies[:4]), replies[:4]


class _Events(_Idle):
    """A browser tab on the event stream that stays open; the next client connects beside it."""

    def __call__(self, data):
        return f"GET /events?t={TOKEN} HTTP/1.1\r\nHost: x\r\n\r\n".encode() if data is None else b""


def test_web_server_evicts_an_event_stream_when_only_streams_hold_the_slots(app_exe, tmp_path):
    """Four open event streams take every client slot: the next request takes the first stream's slot and gets its
    answer, and a request that fills the request buffer and keeps sending is dropped."""
    full = "GET / HTTP/1.1\r\nX-Pad: " + "a" * (2047 - 24)
    out, replies = _serve(app_exe, tmp_path, [_Events(), _Events(), _Events(), _Events(), _get("/api/version"),
                                             [full, "more\r\n\r\n"]], wait_s=8)
    assert replies[4].startswith("HTTP/1.1 200") and '"v":"u-connectXpressApp ' in replies[4], replies
    assert replies[5] == "", replies[5][:200]


def test_web_start_closes_sockets_an_earlier_run_left_and_says_when_listen_still_fails(app_exe, tmp_path):
    """Listening fails: the TCP sockets on the module that this run does not own (left by a killed earlier run) are
    closed and the listen is tried once more; when that fails too web.start says so and serves nothing."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-w36.yaml")
    profile["sockets"] = None
    profile["commands"].update({"AT+USOCR=6": ["+USOCR:2", "OK"], "AT+USOB=*": ["OK"], "AT+USOL=*": ["ERROR"],
                                "AT+USOST?": ["+USOST:3,6,3", "+USOST:4,17,1", "+USOST:5,6,2", "OK"],
                                "AT+USOCL=*": ["OK"]})
    module = Vucx(profile)
    try:
        out = subprocess.run([str(app_exe), module.start(), "--exec", f"web.start 8080 {TOKEN} http"],
                             capture_output=True, text=True, errors="replace", timeout=60, cwd=tmp_path)
        log = list(module.log)
    finally:
        module.stop()
    text = out.stdout
    assert "closing socket 3 left by an earlier run" in text and "closing socket 5 left by an earlier run" in text
    assert "socket 4" not in text and "ERROR: listen on port 8080 failed" in text and out.returncode != 0, text
    assert "AT+USOCL=3" in log and "AT+USOCL=5" in log and "AT+USOCL=4" not in log, log
    assert sum(c.startswith("AT+USOL=") for c in log) == 2, log


def test_web_only_mode_serves_until_it_is_told_to_stop(app_exe, tmp_path):
    """ucxapp <port> --web: on a module with Wi-Fi up it starts web.start and runs without the shell until a
    Ctrl+Break (Windows) or SIGTERM stops it, then stops the server and closes its socket."""
    pytest.importorskip("yaml")
    import os
    import signal
    import threading

    from harness.vucx import Vucx, load

    module = Vucx(load("nora-w36.yaml"))
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    lines = []
    proc = subprocess.Popen([str(app_exe), module.start(), "--web"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, errors="replace", cwd=tmp_path, creationflags=flags)
    try:
        threading.Thread(target=lambda: lines.extend(proc.stdout), daemon=True).start()
        end = time.monotonic() + 60
        while not any("web UI only" in line for line in lines) and proc.poll() is None and time.monotonic() < end:
            time.sleep(0.05)
        assert any("web UI only; q, Esc or Ctrl+C stops" in line for line in lines), "".join(lines)
        proc.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGTERM)
        proc.wait(timeout=30)
        time.sleep(0.2)
        log = list(module.log)
    finally:
        if proc.poll() is None:
            proc.kill()
        module.stop()
    text = "".join(lines)
    assert "stopping the web server" in text, text
    listen = next(c for c in log if c.startswith("AT+USOL="))
    assert listen.split("=")[1].split(",")[0] in [c.split("=")[1] for c in log if c.startswith("AT+USOCL=")], log


def test_plain_http_is_never_served_without_a_token(app_exe, tmp_path):
    """"open" serves without a token over HTTPS only: over plain HTTP anyone on the network could read the page."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    module = Vucx(load("nora-w36.yaml"))
    try:
        out = subprocess.run([str(app_exe), module.start(), "--exec", "web.start 8080 open http", "--exec",
                              "web.status"], capture_output=True, text=True, errors="replace", timeout=60, cwd=tmp_path)
        log = list(module.log)
    finally:
        module.stop()
    assert "ERROR: plain HTTP always needs a token" in out.stdout and out.returncode != 0, out.stdout
    assert not any(c.startswith(("AT+USOCR", "AT+USOL")) for c in log), log
