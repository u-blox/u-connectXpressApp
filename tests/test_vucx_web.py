"""web.start against scripted HTTP clients of the virtual module (Network requests): the routes, the token,
uploads and downloads, and the error paths of u_webserver.c, all without a browser or a network."""

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


def _serve(app_exe, tmp_path, requests, wait_s=20):
    """Starts web.start 8080 <token>; the clients connect as it listens. Returns (stdout, replies as text)."""
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
        proc.stdin.write("web.status\nweb.stop\nweb.status\n")
        proc.stdin.close()
        out = proc.stdout.read()
        proc.wait(timeout=20)
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
    assert start["f"] == [["endpoint ...", 1, "", "light temp contact", "r", ""]], start


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
