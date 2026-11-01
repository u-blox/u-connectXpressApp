"""The web page (app/web/index.html, served by ucxapp_cmds_web.c): static checks and an optional browser run."""

import json
import re
import shutil
import subprocess
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "app" / "src" / "ucxapp_cmds_web.c"
PAGE_FILE = ROOT / "app" / "web" / "index.html"


def load_page():
    return PAGE_FILE.read_text(encoding="utf-8")


PAGE = load_page()
SCRIPT = PAGE[PAGE.index("<script>") + 8: PAGE.index("</script>")]


def test_page_is_a_complete_document():
    assert PAGE.startswith("<!doctype html>") and PAGE.rstrip().endswith("</html>")


def test_page_is_ascii_and_fits_one_response():
    data = PAGE.encode("utf-8")
    assert data.isascii()
    assert len(data) < 36000   # U_WEBSERVER_MAX_RESPONSE_SIZE is 40960 including headers


def test_page_is_built_into_the_app():
    cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "web/index.html" in cmake and "ucxapp_webpage.h" in cmake
    assert '#include "ucxapp_webpage.h"' in SRC.read_text(encoding="utf-8")


def test_script_ids_exist_in_the_markup():
    markup = PAGE[: PAGE.index("<script>")]
    for ident in set(re.findall(r"\$\('(\w+)'\)", SCRIPT)):
        assert f'id="{ident}"' in markup, ident


def test_script_only_calls_registered_routes():
    routes = set(re.findall(r'\{ U_HTTP_METHOD_\w+, +"([^"]+)"', SRC.read_text(encoding="utf-8")))
    assert "/api/exec" in routes
    for path in set(re.findall(r"'(/(?:events|api/\w+))", SCRIPT)):
        assert path in routes, path


def test_script_listens_for_the_server_events():
    source = SRC.read_text(encoding="utf-8")
    for name in re.findall(r"addEventListener\('(\w+)'", SCRIPT):
        assert f'"{name}"' in source, name


W3C_NU = "https://validator.w3.org/nu/?out=json"


def vnu_messages(path):
    """Nu Html Checker (the W3C validator) messages: local vnu.jar (pip html5validator + Java), else the W3C service."""
    java = shutil.which("java")
    try:
        import html5validator
        jar = Path(html5validator.__file__).parent / "vnu" / "vnu.jar"
    except ImportError:
        jar = None
    if java and jar and jar.exists():
        result = subprocess.run([java, "-jar", str(jar), "--format", "json", "--also-check-css", str(path)],
                                capture_output=True, text=True, timeout=120)
        return json.loads(result.stderr or '{"messages": []}')["messages"]
    request = urllib.request.Request(W3C_NU, path.read_bytes(),
                                     {"Content-Type": "text/html; charset=utf-8", "User-Agent": "ucxapp-tests"})
    try:
        with urllib.request.urlopen(request, timeout=60) as reply:
            return json.load(reply)["messages"]
    except OSError as exc:
        pytest.skip(f"no vnu.jar/Java and the W3C validator is unreachable: {exc}")


def test_page_passes_the_w3c_validator():
    problems = [f"{m.get('lastLine')}: {m.get('type')} {m.get('message')}" for m in vnu_messages(PAGE_FILE)
                if m.get("type") != "info" or m.get("subType") == "warning"]
    assert not problems, "\n".join(problems)


def test_script_syntax(tmp_path):
    if shutil.which("node"):
        js = tmp_path / "page.js"
        js.write_text(SCRIPT, encoding="utf-8")
        result = subprocess.run(["node", "--check", str(js)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        return
    sync_api = pytest.importorskip("playwright.sync_api", reason="needs node or playwright")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        # compile only (strict mode too), nothing runs
        error = browser.new_page().evaluate(
            "s => { try { new Function(s); new Function('\"use strict\";' + s); return ''; }"
            " catch (e) { return e.name + ': ' + e.message; } }", SCRIPT)
        browser.close()
    assert error == ""


class Mock(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body, ctype="application/json"):
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.split("?")[0] == "/":
            self.reply(PAGE, "text/html")
        elif self.path.startswith("/api/groups"):
            self.reply(json.dumps(["wifi"]))
        elif self.path.startswith("/api/commands"):
            self.reply(json.dumps([
                {"n": "wifi.connect", "h": "Connect", "u": "<ssid> [password]",
                 "f": [["ssid", 0, ""], ["password", 1, ""]]},
                {"n": "wifi.security", "h": "Security", "u": "[wpa|open]", "f": [["mode", 1, "wpa|open"]]},
            ]))
        elif self.path.startswith("/api/examples"):
            self.reply(json.dumps([{"t": "Fit check", "l": "usecase.tool\nusecase.tool 2.5"},
                                   {"t": "Join with PEAP", "l": "# factory\nwifi.peap FactoryNet u p ca.pem"}]))
        elif self.path.startswith("/events"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for name, data in (("out", '"ready\\n\\u001b[92mgreen\\u001b[0m\\n"'), ("evt", '"wifi:link up"'),
                               ("state", '"Wi-Fi up | BLE -"')):
                self.wfile.write(f"event: {name}\ndata: {data}\n\n".encode())
            self.wfile.flush()
            self.server.stop.wait(5)

    def do_POST(self):
        self.server.posted.append(self.path)
        self.reply("accepted", "text/plain")


@pytest.fixture
def server():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Mock)
    srv.posted, srv.stop = [], threading.Event()
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield srv
    srv.stop.set()
    srv.shutdown()


def test_page_in_a_browser(server):
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.on("console", lambda msg: msg.type == "error" and js_errors.append(msg.text))
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector("#st.ok")
        assert "ready" in page.inner_text("#out")
        assert page.inner_text("#out .fg") == "green"
        assert "\x1b" not in page.inner_text("#out")
        assert page.inner_text(".chip.on") == "Wi-Fi up" and page.inner_text(".chip.off") == "BLE -"
        assert page.inner_text("#ev .e.wifi .x") == "link up"
        assert page.locator("#ev .e.wifi .t").count() == 1
        page.click("summary:has-text('wifi')")
        page.wait_for_selector(".c")
        assert page.locator(".c select option").count() == 3
        page.fill(".c input >> nth=0", "stenmo")
        page.fill(".c input >> nth=1", "secret")
        page.click(".c >> nth=0 >> button")
        page.wait_for_function("window.__done || true")
        for _ in range(50):
            if server.posted:
                break
            page.wait_for_timeout(100)
        assert any("cmd=wifi.connect" in u and "a0=stenmo" in u and "a1=secret" in u for u in server.posted)
        assert "wifi.connect stenmo secret" in page.inner_text(".blk .bh")
        assert page.is_disabled("#run")
        # the password is not remembered, the ssid is
        assert "secret" not in page.evaluate("localStorage.getItem('ucxf')")
        assert "stenmo" in page.evaluate("localStorage.getItem('ucxf')")
        # use case view: diagram lit from the live state, steps from usecase.ex
        page.click("#vw button[data-v='1']")
        assert page.is_visible("#pu") and not page.is_visible("#pc")
        assert "on" in page.get_attribute("#n-mes", "class") and "on" in page.get_attribute("#n-tool", "class")
        page.wait_for_selector("#steps .card")
        assert page.locator("#steps .card").count() == 1
        assert page.inner_text("#steps h3") == "Join with PEAP"
        page.click("#vw button[data-v='0']")
        assert page.is_visible("#pc")
        browser.close()
    assert not js_errors, js_errors


@pytest.mark.app
def test_web_export_writes_the_page_json(app_exe, tmp_path):
    out = subprocess.run([str(app_exe), "COM255", "--exec", f'web.export "{tmp_path.as_posix()}"'],
                         capture_output=True, text=True, timeout=30, errors="replace")
    assert "exported" in out.stdout, out.stdout + out.stderr
    groups = json.loads((tmp_path / "groups.json").read_text(encoding="utf-8"))
    names = {g.split(":")[0]: g for g in groups}
    assert names["wifi"].startswith("wifi:module") and names["usecase"].startswith("usecase:host")
    for g in groups:
        name = g.split(":")[0]
        assert json.loads((tmp_path / f"commands-{name}.json").read_text(encoding="utf-8")), name
    wifi = {c["n"]: c for c in json.loads((tmp_path / "commands-wifi.json").read_text(encoding="utf-8"))}
    assert wifi["wifi.connect"]["f"][0][0].startswith("ssid")
    examples = json.loads((tmp_path / "examples-usecase.json").read_text(encoding="utf-8"))
    assert any("(PEAP)" in e["t"] and "wifi." in e["l"] for e in examples)
    assert not (tmp_path / "commands-web.json").exists()


def test_preview_composes_lines_like_the_app():
    import ucxapp

    assert ucxapp.compose("wifi.connect", ["My Net", 'a"b', ""], False) == 'wifi.connect "My Net" "a\\"b"'
    assert ucxapp.compose("at", ["AT+GMM ?"], True) == "at AT+GMM ?"
    assert ucxapp.compose("dev.info", [""], False) == "dev.info"


@pytest.mark.app
def test_web_preview_serves_the_real_commands():
    proc = subprocess.Popen([sys.executable, "-u", str(ROOT / "ucxapp.py"), "--web-preview", "0"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    try:
        lines = []
        for line in proc.stdout:   # a stale exe is rebuilt first
            lines.append(line)
            m = re.search(r"http://127\.0\.0\.1:(\d+)/#t=(\w+)", line)
            if m:
                break
        assert m, "".join(lines)
        base, token = f"http://127.0.0.1:{m.group(1)}", m.group(2)

        def get(path, tok=token):
            req = urllib.request.Request(base + path, headers={"X-Token": tok} if tok else {})
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.read().decode()

        assert get("/", None).startswith("<!doctype html>")
        with pytest.raises(urllib.error.HTTPError, match="403"):
            get("/api/groups", "wrong")
        assert "usecase:host:x" in json.loads(get("/api/groups"))
        assert any(c["n"] == "wifi.scan" for c in json.loads(get("/api/commands?g=wifi")))
        assert [c["n"] for c in json.loads(get("/api/find?q=usecase.t&p=1"))] == ["usecase.tool"]
        events = urllib.request.urlopen(urllib.request.Request(base + "/events", headers={"X-Token": token}),
                                        timeout=30)
        post = urllib.request.Request(base + "/api/exec?cmd=usecase.tool&a0=4", method="POST",
                                      headers={"X-Token": token})
        assert urllib.request.urlopen(post, timeout=10).read() == b"accepted"
        seen = ""
        while "event: done" not in seen:
            seen += events.readline().decode()
        assert "Preview | no module" in seen and "900 MB/30 min" in seen
    finally:
        proc.terminate()
        proc.wait(10)
