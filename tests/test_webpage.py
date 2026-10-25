"""The web page (app/web/index.html, served by ucxapp_cmds_web.c): static checks and an optional browser run."""

import json
import re
import shutil
import subprocess
import threading
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
    routes = set(re.findall(r'uWebServerAddRoute\(gServer, \w+, "([^"]+)"', SRC.read_text(encoding="utf-8")))
    for path in set(re.findall(r"'(/(?:events|api/\w+))", SCRIPT)):
        assert path in routes, path


def test_script_listens_for_the_server_events():
    source = SRC.read_text(encoding="utf-8")
    for name in re.findall(r"addEventListener\('(\w+)'", SCRIPT):
        assert f'"{name}"' in source, name


@pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")
def test_script_syntax(tmp_path):
    js = tmp_path / "page.js"
    js.write_text(SCRIPT, encoding="utf-8")
    result = subprocess.run(["node", "--check", str(js)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


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
        if self.path == "/":
            self.reply(PAGE, "text/html")
        elif self.path.startswith("/api/groups"):
            self.reply(json.dumps(["wifi"]))
        elif self.path.startswith("/api/commands"):
            self.reply(json.dumps([
                {"n": "wifi.connect", "h": "Connect", "u": "<ssid> [password]",
                 "f": [["ssid", 0, ""], ["password", 1, ""]]},
                {"n": "wifi.security", "h": "Security", "u": "[wpa|open]", "f": [["mode", 1, "wpa|open"]]},
            ]))
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
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector("#st.ok")
        assert "ready" in page.inner_text("#out")
        assert page.inner_text("#out .fg") == "green"
        assert "\x1b" not in page.inner_text("#out")
        assert page.inner_text(".chip.on") == "Wi-Fi up" and page.inner_text(".chip.off") == "BLE -"
        assert "wifi:link up" in page.inner_text("#ev")
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
        browser.close()
