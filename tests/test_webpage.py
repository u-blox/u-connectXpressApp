"""The web page (app/web/index.html, served by ucxapp_cmds_web.c): static checks and an optional browser run."""

import gzip
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
    # Hosts send the page in place (body_static) in 1000-byte chunks and the exe carries it raw; the boards keep it
    # gzip-compressed in flash (embed.cmake GZIP), which is the size that counts there.
    assert len(data) < 114688, len(data)
    assert len(gzip.compress(data, 9)) < 40960, len(gzip.compress(data, 9))


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
    for name in re.findall(r"\bes\.addEventListener\('(\w+)'", SCRIPT):
        assert f'"{name}"' in source, name


def test_ppk_measure_output_is_drawn_as_a_chart():
    """ppk.measure's Chart: line (min,avg,max uA per slot) becomes an SVG band, average line and hover readout."""
    assert "if(!rc&&/^ppk\\.measure/.test(b.cmd))ppkChart(b)" in SCRIPT
    assert "function ppkChart(b)" in SCRIPT and "/^Chart:\\s+([\\d.]+) s uA avg ([-\\d.]+)" in SCRIPT
    for cls in ('class="pb"', 'class="pa"', 'class="pm"', "onpointermove"):
        assert cls in SCRIPT, cls


def test_connections_view_zooms_pans_and_drags():
    """The Connections graph zooms (buttons, wheel, pinch), pans and lets nodes be dragged; reset restores both."""
    page = PAGE_FILE.read_text(encoding="utf-8")
    assert '<div id="cnz">' in page and 'data-z="1.25"' in page and 'data-z="0"' in page
    for name in ("wheel", "pointerdown", "pointermove", "pointerup", "dblclick"):
        assert f"cnS.addEventListener('{name}'" in SCRIPT, name
    assert "CN.off[k]" in SCRIPT
    # reset restores a view box centred on the origin (the default zoom may change, the centring must not)
    vb = re.search(r"cnReset=\(\)=>\{CN\.vb=\[(-?\d+),(-?\d+),(\d+),(\d+)\];CN\.off=\{\}", SCRIPT)
    assert vb, "cnReset must restore CN.vb and CN.off"
    x, y, w, h = map(int, vb.groups())
    assert abs(w + 2 * x) <= 1 and abs(h + 2 * y) <= 1, vb.group(0)   # rounding of an odd width or height
    assert "touch-action:none" in page
    # .card sets display, which would otherwise override the hidden attribute and keep Close from hiding it
    assert "#cnd[hidden]{display:none}" in page


def test_use_case_diagram_shares_the_connections_palette():
    """Use case nodes are coloured by kind through the k-* classes of the Connections view, never by a raw colour
    name per node; every kind has a legend entry, and both panes show the same legend."""
    uc = SCRIPT[SCRIPT.index("const UC={"):SCRIPT.index("const KC=")]
    assert not re.search(r",'(warm|pu|ok|ac|ble)'\]", uc), "a raw colour on a use case node"
    kinds = set(re.findall(r"\w+:'(\w+)'", SCRIPT[SCRIPT.index("const KC="):SCRIPT.index("KN=")]))
    legends = re.findall(r'<span class="cnleg">(.*?)</span></span>', PAGE)
    assert len(legends) == 2 and legends[0].startswith(legends[1]), "the same legend, Use cases adds Host"
    legend_kinds = set(re.findall(r'class="k-(\w+)"', legends[0]))
    assert kinds | {"host"} <= legend_kinds, kinds - legend_kinds
    assert 'class="k-${kind(n)}"' in SCRIPT and '<g id="n-hub" class="hub">' in SCRIPT
    # the diagram's rings and links take their colour from the kind, dimmed until lit
    assert "#dg rect{fill:var(--bg2);stroke:var(--k);stroke-opacity:.45" in PAGE
    assert "#dg line{stroke:var(--k);stroke-opacity:.25" in PAGE and "#dg .hub{--k:var(--ac)}" in PAGE
    # every node id in the use cases is a known kind or host-side
    known = set(re.findall(r"(\w+):'\w+'", SCRIPT[SCRIPT.index("const KC="):SCRIPT.index("KN=")]))
    ids = set(re.findall(r"\['(\w+)','(?:[^'\\]|\\.)*','(?:[^'\\]|\\.)*'\]", uc))
    host = {"can", "fw", "soup", "trace", "cfg", "imu", "safe", "id", "rep", "i2c", "host", "disp", "act"}
    assert ids - known - host == set(), ids - known - host


def test_cellular_card_and_status_chip():
    """The live cellular card sits next to the GNSS one, and the Cell chip of the status bar runs cell.status."""
    page = PAGE_FILE.read_text(encoding="utf-8")
    assert '<pre id="gc" hidden' in page and '<pre id="cc" hidden' in page
    assert "addEventListener('cell'" in SCRIPT and "Cell:'cell.status'" in SCRIPT


def test_header_overflow_menu_keeps_the_main_buttons_visible():
    """Manual, Files and Clear live in the overflow menu; Guide, Demo and the module buttons stay in the header."""
    menu = re.search(r'<span id="morm" hidden>(.*?)</span>', PAGE, re.S)
    assert menu and all(f'id="{i}"' in menu.group(1) for i in ("man", "files", "clr"))
    assert 'id="demo"' not in menu.group(1)
    assert "getElementById('more')" in WASM_JS.read_text(encoding="utf-8")


def test_graph_labels_separate_and_link_to_the_logs():
    """Graph nodes push apart, a clicked node filters the Log and events, and log blocks carry a time."""
    for name in ("function cnSep(", "function fltSet(", "function cnPulse("):
        assert name in SCRIPT, name
    assert 'id="flt-on"' in PAGE and "body.flt" in PAGE
    assert "'tm'" in SCRIPT
    # a Channel Sounding distance pulls the BLE node onto its ring unless it is being dragged
    assert "R=csR(o.cs);tx=tx*R/l" in SCRIPT and "q.k===k&&q.moved" in SCRIPT


def test_demo_ends_with_a_verdict_banner():
    """A demo prints 'N of M examples ran to the end'; the page turns that into a pass/fail banner."""
    assert '<div id="verdict"' in PAGE and "function verdict(" in SCRIPT
    assert "examples ran to the end" in SCRIPT


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


WASM_SITE_PAGE = ROOT / "build" / "wasm" / "site" / "index.html"
WASM_JS = ROOT / "app" / "wasm" / "ucxapp-wasm.js"
FX_JS = ROOT / "app" / "web" / "public" / "fx.js"


@pytest.mark.parametrize("path", [PAGE_FILE, WASM_SITE_PAGE], ids=["app", "wasm-site"])
def test_page_passes_the_w3c_validator(path):
    if not path.exists():
        pytest.skip("no browser build (ucxapp --wasm --build)")
    problems = [f"{m.get('lastLine')}: {m.get('type')} {m.get('message')}" for m in vnu_messages(path)
                if m.get("type") != "info" or m.get("subType") == "warning"]
    assert not problems, "\n".join(problems)


@pytest.mark.parametrize("source", ["page", "wasm", "fx"])
def test_script_syntax(tmp_path, source):
    script = SCRIPT if source == "page" else (WASM_JS if source == "wasm" else FX_JS).read_text(encoding="utf-8")
    if shutil.which("node"):
        js = tmp_path / "page.js"
        js.write_text(script, encoding="utf-8")
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
            " catch (e) { return e.name + ': ' + e.message; } }", script)
        browser.close()
    assert error == ""


# Commands in the format web.export writes: fields [name, optional, choices, default, kind, key], forms in "v"
DEMO = [
    {"n": "gnss.bridge", "h": "Bridges", "u": "tcp [port] | ntrip <host> <port> [ble]", "f": None,
     "v": [{"l": "tcp", "p": "tcp", "f": [["port", 1, "", "", "t", ""]]},
           {"l": "ntrip", "p": "ntrip", "f": [["host", 0, "", "", "t", ""], ["port", 0, "", "2101", "t", ""],
                                              ["ble", 1, "", "", "f", ""]]}]},
    {"n": "mqtt.connect", "h": "Connect", "u": "[host] [wait_s] [id=<client>]",
     "f": [["host", 1, "", "", "t", ""], ["wait_s", 1, "", "5", "t", ""], ["client", 1, "", "", "t", "id="]]},
    {"n": "mqtt.publish", "h": "Publish", "u": "<topic> <text...>",
     "f": [["topic", 0, "", "", "t", ""], ["text ...", 0, "", "", "r", ""]]},
]

# /api/conn after the demo on a virtual ALMA-W1 (ucxapp_cmds_conn.c lines): every kind of node at once
CONN = "\n".join([
    "m|webserial|ALMA-W1|1.0.0-0|3600|1|C0:FF:EE:00:00:01",
    "w|vucx-net|-47|6|192.168.1.50|192.168.1.1",
    "s|0|tcp|open|", "s|1|tcp|open|",
    "q|mqtt:1883|ucx|home/garage/cmd|home/garage/door",
    "b|0|C0:FF:EE:00:11:22|-55|247|c|coded|5010|21",
    "t|leader|0400", "n|0800|disabled|-60|3",
    "g|virt:gnss|3D  10/10 sats HDOP 0.8",
    "c|virt:cell|registered (home)  vucx telecom -73 dBm",
]) + "\n"

# True once every node's box (circle and labels) lies inside the view box.
ALL_IN_VIEW = """() => { const v = CN.vb, ns = Object.values(CN.n).filter(n => !n.gone && n.o);
  return ns.length >= 9 && ns.every(n => { const b = cnBox(n);
    return b[0] >= v[0] && b[1] >= v[1] && b[2] <= v[0] + v[2] && b[3] <= v[1] + v[3] }) }"""


def qr_text(matrix, border=2):
    """What ucxapp_qr_print prints for a module matrix: half blocks, two module rows per line, a 2-module border."""
    n = len(matrix)
    half = [" ", "\u2584", "\u2580", "\u2588"]

    def mod(x, y):
        return 0 <= x < n and 0 <= y < n and matrix[y][x]

    return "".join("".join(half[(2 if mod(x, y) else 0) | (1 if mod(x, y + 1) else 0)]
                           for x in range(-border, n + border)) + "\n" for y in range(-border, n + border, 2))


# A 21-module matrix with finder patterns and pseudo-random data, the shape of a version 1 QR code.
QR_MATRIX = [[(x < 7 and y < 7) or (x > 13 and y < 7) or (x < 7 and y > 13) or (x * 7 + y * 13) % 5 < 2
              for x in range(21)] for y in range(21)]


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
        elif self.path == "/icon.svg":
            self.reply((PAGE_FILE.parent / "icon.svg").read_text(encoding="utf-8"), "image/svg+xml")
        elif self.path.startswith("/api/groups"):
            self.reply(json.dumps(["wifi", "demo"]))
        elif self.path.startswith("/api/commands?g=demo"):
            self.reply(json.dumps(DEMO))
        elif self.path.startswith("/api/commands"):
            self.reply(json.dumps([
                {"n": "wifi.connect", "h": "Connect", "u": "<ssid> [password]",
                 "f": [["ssid", 0, ""], ["password", 1, ""]]},
                {"n": "wifi.security", "h": "Security", "u": "[wpa|open]", "f": [["mode", 1, "wpa|open"]]},
            ]))
        elif self.path.startswith("/api/examples"):
            self.reply(json.dumps([{"t": "Fit check", "l": "usecase.tool\nusecase.tool 2.5"},
                                   {"t": "Join with PEAP", "l": "# factory\nwifi.peap FactoryNet u p ca.pem"}]))
        elif self.path.startswith("/api/version"):
            self.reply(json.dumps({"v": "u-connectXpressApp 3.5.0.261104 (built today)"}))
        elif self.path.startswith("/api/conn"):
            self.reply(json.dumps({"t": CONN}))
        elif self.path.startswith("/events"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for name, data in (("out", '"ready\\n\\u001b[92mgreen\\u001b[0m\\n"'), ("evt", '"wifi:link up"'),
                               ("state", '"Wi-Fi up | BLE - | Sockets 1* +2 | Advertising on | Listening 2 | '
                                          'Time 2030-01-02T03:04:05Z | ntp pool.ntp.org, 1 s ago | '
                                          'Loc 55.61276, 12.98625 (gnss)"')):
                self.wfile.write(f"event: {name}\ndata: {data}\n\n".encode())
            self.wfile.flush()
            # the auto summary and the chip commands finish, other commands stay busy
            acked = 0
            while not self.server.stop.wait(0.05):
                for url in self.server.posted[acked:]:
                    acked += 1
                    if re.search(r"cmd=(dev\.summary|wifi\.status)(&|$)", url):
                        out = json.dumps("Module:           u-blox NORA-W36, firmware 3.5.0\n")
                        self.wfile.write(f"event: out\ndata: {out}\n\nevent: done\ndata: 0\n\n".encode())
                        self.wfile.flush()
                    elif re.search(r"cmd=utils\.qr", url):
                        out = json.dumps("WIFI:T:WPA;S:Setup-1234;P:setup1234;;\n" + qr_text(QR_MATRIX))
                        self.wfile.write(f"event: out\ndata: {out}\n\nevent: done\ndata: 0\n\n".encode())
                        self.wfile.flush()
                    elif re.search(r"cmd=examples(%20|\+| )uc-provision$", url):
                        out = json.dumps("Examples for uc-provision\n\n 1. Wait for a phone\n    > bt.pairing on\n"
                                         "\n 2. Or: own access point\n    > wifi.ap.status\n\n"
                                         "Run one with examples uc-provision <number> (stops at the first error)\n")
                        self.wfile.write(f"event: out\ndata: {out}\n\nevent: done\ndata: 0\n\n".encode())
                        self.wfile.flush()

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
        assert page.inner_text(".chip.on >> nth=0") == "Wi-Fi up" and page.inner_text(".chip.off") == "BLE -"
        # advertising and listening sockets get their own chips; the clock and its source go to the footer
        assert page.locator("#state .chip").all_inner_texts() == [
            "Wi-Fi up", "BLE -", "Sockets 1 +2", "Advertising on", "Listening 2"]
        # +2: sockets the app opened itself (web server); only handle 1 is offered for commands
        assert "+N: sockets the app uses itself" in page.get_attribute("#state .chip >> text=Sockets", "title")
        assert page.evaluate("HS.sock") == ["1*"]
        assert page.get_attribute("#state .chip >> text=Advertising on", "title") == "Run bt.adv"
        assert page.get_attribute("#state .chip >> text=Listening 2", "title") == "Run sock.list"
        assert "the underlined one is used when a command leaves the handle out" in \
            page.get_attribute(".chip.off", "title")
        # the clock keeps its text, the source is a tag (green for a network or GNSS time), the place opens a map
        assert page.text_content("#clock") == "2030-01-02T03:04:05Z / ntp pool.ntp.org"
        assert page.get_attribute("#clock .tag", "title") == "Clock source: ntp pool.ntp.org, 1 s ago"
        assert page.inner_text("#clock .t") == "03:04:05Z" and page.get_attribute("#clock .tag", "class") == "tag ok"
        assert page.inner_text("#loc a") == "55.61276, 12.98625" and page.inner_text("#loc .tag") == "gnss"
        assert page.get_attribute("#loc a", "href") == \
            "https://www.google.com/maps/search/?api=1&query=55.61276%2C%2012.98625"
        assert page.get_attribute("#loc a", "target") == "_blank"
        assert page.inner_text("#ev .e.wifi .x") == "link up"
        assert page.locator("#ev .e.wifi .t").count() == 1
        # a module that is up gets its summary once, and the version sits in the footer
        page.wait_for_selector(".blk .badge.ok")
        assert "cmd=dev.summary&y=1" in server.posted[0] and "Module:" in page.inner_text("#out")
        page.wait_for_function("document.getElementById('ver').textContent === 'v3.5.0.261104'")
        # a status chip runs its status command
        assert page.get_attribute(".chip.on >> nth=0", "title") == "Run wifi.status"
        page.click(".chip.on >> nth=0")
        page.wait_for_selector(".blk .badge.ok >> nth=1")
        assert server.posted[1].endswith("cmd=wifi.status")
        server.posted.clear()
        page.click("summary:has-text('wifi')")
        page.wait_for_selector(".c")
        assert page.locator(".c select option").count() == 3
        # an optional choice starts on "(default)": nothing is sent unless the user picks a value
        assert page.eval_on_selector(".c select", "e => e.value") == ""
        assert page.eval_on_selector(".c select", "e => e.options[e.options.length - 1].text") == "(default)"
        page.fill(".c input >> nth=0", "stenmo")
        page.fill(".c input >> nth=1", "secret")
        page.click(".c >> nth=0 >> button")
        page.wait_for_function("window.__done || true")
        for _ in range(50):
            if server.posted:
                break
            page.wait_for_timeout(100)
        assert any("cmd=wifi.connect" in u and "a0=stenmo" in u and "a1=secret" in u for u in server.posted)
        assert "wifi.connect stenmo secret" in page.locator(".blk .bh").last.inner_text()
        assert page.is_disabled("#run")
        # the password is not remembered, the ssid is
        assert "secret" not in page.evaluate("localStorage.getItem('ucxf')")
        assert "stenmo" in page.evaluate("localStorage.getItem('ucxf')")
        # use case view: diagram lit from the live state, steps from usecase.example
        page.click("#vw button[data-v='1']")
        assert page.is_visible("#pu") and not page.is_visible("#pc")
        assert "on" in page.get_attribute("#n-wifi", "class") and "on" in page.get_attribute("#n-hub", "class")
        page.wait_for_selector("#steps .card")
        assert page.locator("#steps .card").count() == 1
        assert page.inner_text("#steps h3") == "Join with PEAP"
        assert "usecase.example 2" in page.get_attribute("#steps .card button", "title")
        # another use case: its own page, diagram and steps run through 'examples uc-<name>'
        page.select_option("#ucs", "boat")
        assert page.inner_text("#uh").startswith("Boat") and page.locator("#n-gnss").count() == 1
        assert page.locator("#n-ap").count() == 1 and not page.is_visible("#mb")
        page.wait_for_selector("#steps .card button[title*='examples uc-boat 2']")
        assert page.locator("#steps .card").count() == 1
        page.select_option("#ucs", "tool")
        assert page.is_visible("#mb") and "on" in page.get_attribute("#n-wifi", "class")
        # every label of every use case fits its 160 px box, and every node wears a kind colour
        wide = page.evaluate("""() => { const out = []; for (const k in UC) { pick(k);
          document.querySelectorAll('#dg text:not(.kt)').forEach(t => {
            if (t.getBBox().width > 150) out.push([k, t.textContent]) });
          document.querySelectorAll('#dg g').forEach(g => {
            if (!/^(k-\\w+|hub)( on)?$/.test(g.getAttribute('class'))) out.push([k, g.id]) }) }
          return out }""")
        assert not wide, wide
        page.select_option("#ucs", "tool")
        page.click("#vw button[data-v='0']")
        assert page.is_visible("#pc")
        browser.close()
    assert not js_errors, js_errors


def test_connections_view_keeps_every_node_on_screen(server):
    """The demo's full ALMA-W1 graph: Thread and its neighbour used to be pushed below the canvas for seconds. The
    view follows the content until the user zooms or pans; reset hands it back."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector("#st.ok")
        page.click("#vw button[data-v='2']")
        page.wait_for_function("() => CN.n.t && CN.n.n0800 && CN.n.g && CN.n.c")   # Thread, its neighbour, GNSS, cell
        page.wait_for_function(ALL_IN_VIEW, timeout=15000)
        # the details card gives the Channel Sounding age in seconds
        page.evaluate("cnSelect('b0')")
        assert "5.01 m, 21 s ago" in page.inner_text("#cnd"), page.inner_text("#cnd")
        page.evaluate("cnSelect('b0')")
        assert page.evaluate("CN.auto") == 1
        size = page.evaluate("[CN.vb[2], CN.vb[3]]")
        assert size[0] >= 1075 and size[1] >= 625, size   # never closer than the default view
        page.wait_for_timeout(1500)   # and stays in view once the layout has settled
        assert page.evaluate(ALL_IN_VIEW)
        # the layout settles instead of exploding (pushes of 500+ before), and a node on its Channel Sounding ring
        # slides along it instead of piling up push it cannot follow
        assert page.evaluate("Math.max(...Object.values(CN.n).map(n => Math.hypot(n.px || 0, n.py || 0)))") < 300
        # every icon text fits its circle (THREAD needed a bigger one)
        tight = page.evaluate("Object.values(CN.n).filter(n => n.ic.getBBox().width > 2 * n.R - 2).map(n => n.o.ic)")
        assert not tight, tight
        # the user zooms: the view is theirs; reset gives it back to the content
        box = page.locator("#cn").bounding_box()
        page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
        page.mouse.wheel(0, -300)
        assert page.evaluate("CN.auto") == 0
        page.click("#cnz button[data-z='0']")
        assert page.evaluate("CN.auto") == 1
        page.wait_for_function(ALL_IN_VIEW, timeout=15000)
        browser.close()
    assert not js_errors, js_errors


def test_qr_code_in_the_log_becomes_a_picture(server):
    """The half-block QR text of utils.qr.* / wifi.ap.enable is replaced by an SVG: square modules, black on white
    with a 4-module quiet zone (the stretched text in the log was unreadable for a phone); the payload line stays."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector(".blk .badge.ok")
        page.fill("#cmd", "utils.qr.wifi Setup-1234 setup1234")
        page.press("#cmd", "Enter")
        page.wait_for_selector(".blk .qr svg")
        n = len(QR_MATRIX)
        # square although 21 modules + border print as 13 half-block lines: 2 printed + 2 added
        assert page.get_attribute(".qr svg", "viewBox") == f"-2 -2 {n + 8} {n + 8}"
        assert page.get_attribute(".qr svg", "width") == str((n + 8) * 7)
        modules = page.evaluate("document.querySelector('.qr svg path').getAttribute('d').split('M').length - 1")
        assert modules == sum(sum(row) for row in QR_MATRIX)
        # the first module (a finder corner) is where the matrix has it
        assert page.evaluate("document.querySelector('.qr svg path').getAttribute('d').startsWith('M2 2h1v1h-1z')")
        text = page.inner_text(".blk:last-child .bb")
        assert "WIFI:T:WPA;S:Setup-1234;P:setup1234;;" in text and "\u2588" not in text and "\u2580" not in text
        browser.close()


def test_demo_starts_on_a_clean_page(server):
    """Running the demo again clears the log and the events, resets the Connections graph (nodes, zoom, selection)
    and hides the last verdict before the tour starts."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector(".blk .badge.ok")
        page.click("#vw button[data-v='2']")
        page.wait_for_function("() => CN.n.t && CN.n.w")
        old_node = page.evaluate_handle("CN.n.t.nd")
        page.evaluate("cnSelect('w');CN.vb=[0,0,500,300];CN.auto=0;cnVB();verdict(true,'old run')")   # a used page
        assert page.locator("#ev .e").count() >= 1 and page.locator("#out .blk").count() >= 1
        assert page.is_visible("#verdict") and page.is_visible("#cnd")
        page.click("#demo")
        blocks = page.locator("#out .blk")
        assert blocks.count() == 1 and "> demo" in blocks.first.inner_text(), blocks.all_inner_texts()
        assert page.locator("#ev .e").count() == 0
        assert not old_node.evaluate("e => e.isConnected")
        assert page.evaluate("[CN.auto, CN.vb, CN.sel]") == [1, [-538, -313, 1075, 625], ""]
        assert page.is_hidden("#verdict") and page.is_hidden("#cnd") and page.is_hidden("#flt-on")
        page.wait_for_function("() => CN.n.t && CN.n.w")   # the next tick draws the module's links again
        browser.close()


def test_example_titles_in_the_log_run_on_click(server):
    """No typing 'examples uc-provision 2': each listed example is a link that runs it."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector(".blk .badge.ok")
        page.fill("#cmd", "examples uc-provision")
        page.press("#cmd", "Enter")
        page.wait_for_selector(".blk .scan >> nth=1")
        assert page.locator(".blk .scan").count() == 2
        server.posted.clear()
        page.click(".blk .scan:has-text('2. Or: own access point')")
        for _ in range(50):
            if server.posted:
                break
            page.wait_for_timeout(100)
        browser.close()
    assert server.posted and server.posted[0].endswith("cmd=examples&a0=uc-provision&a1=2"), server.posted


def test_page_forms_flags_keys_and_rest(server):
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.goto(f"http://127.0.0.1:{server.server_address[1]}/?t=x")
        page.wait_for_selector("#st.ok")
        page.wait_for_selector(".blk .badge.ok")   # the auto summary is done
        page.click("summary:has-text('demo')")
        page.wait_for_selector(".c select.fm")
        bridge = page.locator(".c").nth(0)
        assert bridge.locator("small").inner_text() == "Bridges"
        assert bridge.locator("select.fm option").all_inner_texts() == ["tcp", "ntrip"]
        # another form shows its own fields: a required default is filled in, a flag is a checkbox
        bridge.locator("select.fm").select_option("1")
        assert bridge.locator("input").nth(1).input_value() == "2101"
        bridge.locator("input").nth(0).fill("caster")
        bridge.locator("input[type=checkbox]").check()
        bridge.locator("button").click()
        page.wait_for_function("document.querySelector('#busy').textContent!==''")
        assert any("cmd=gnss.bridge%20ntrip&a0=caster&a1=2101&a2=ble" in u for u in server.posted), server.posted
        # an optional default is a hint, not a value; a key field sends key=value
        page.evaluate("setBusy('')")
        conn = page.locator(".c").nth(1)
        assert conn.locator("input").nth(1).input_value() == ""
        assert "default 5" in conn.locator("input").nth(1).get_attribute("placeholder")
        conn.locator("input").nth(2).fill("me")
        conn.locator("button").click()
        page.wait_for_function("document.querySelector('#busy').textContent!==''")
        assert any(u.endswith("cmd=mqtt.connect&a0=id%3Dme") for u in server.posted), server.posted
        # the rest of the line goes as typed, the other values quoted
        page.evaluate("setBusy('')")
        pub = page.locator(".c").nth(2)
        pub.locator("input").nth(0).fill("a b")
        pub.locator("input").nth(1).fill("hello  world")
        pub.locator("button").click()
        page.wait_for_function("document.querySelector('#busy').textContent!==''")
        want = "cmd=mqtt.publish%20%22a%20b%22%20hello%20%20world"
        assert any(u.endswith(want) for u in server.posted), server.posted
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
    order = [g.split(":")[0] for g in groups]
    assert order[:2] == ["dev", "cfg"] and order.index("thread") == order.index("wifi") + 1
    assert order.index("app") == order.index("perf") + 1 and order.index("hw") > order.index("app")
    assert "at" in order and "dash" not in order                 # dash and examples sit in the app group
    sections = [g.split(":")[3] for g in groups]
    assert sections == sorted(sections, key=lambda s: sections.index(s)), "a section must not repeat"
    assert "Peripherals" in sections and sections.index("Device") == 0
    for g in groups:
        name = g.split(":")[0]
        assert json.loads((tmp_path / f"commands-{name}.json").read_text(encoding="utf-8")), name
    wifi = {c["n"]: c for c in json.loads((tmp_path / "commands-wifi.json").read_text(encoding="utf-8"))}
    assert wifi["wifi.connect"]["f"][0][0].startswith("ssid")
    every = {c["n"]: c for f in tmp_path.glob("commands-*.json") for c in json.loads(f.read_text(encoding="utf-8"))}
    assert not [n for n, c in every.items() if c["u"] and not c["f"] and not c.get("v")], "commands without a form"
    forms = {v["l"]: v for v in every["gnss.bridge"]["v"]}
    assert forms["ntrip"]["p"] == "ntrip" and [x[0] for x in forms["ntrip"]["f"]][:5] == ["host", "port", "mount",
                                                                                           "user", "pass"]
    assert [x[4] for x in forms["ntrip"]["f"]][-2:] == ["f", "f"]           # ble, cell are flags
    assert [x[5] for x in every["mqtt.connect"]["v"][0]["f"]][2:] == ["id=", "user=", "pass="]
    assert every["sock.read"]["f"][2][:5] == ["wait_s", 1, "", "5", "t"]   # the default from the usage
    assert every["can.cfg"]["f"][0][3] == "500000"
    assert every["ancs.action"]["f"][1][2] == "pos|neg"                    # a choice, not two forms
    assert [x[4] for x in every["hid.key"]["f"]] == ["t", "r"]
    examples = json.loads((tmp_path / "examples-usecase.json").read_text(encoding="utf-8"))
    assert any("(PEAP)" in e["t"] and "wifi." in e["l"] for e in examples)
    boat = json.loads((tmp_path / "examples-uc-boat.json").read_text(encoding="utf-8"))
    assert any("gnss.bridge tcp 10110" in e["l"] for e in boat)
    assert not (tmp_path / "commands-web.json").exists()


def test_preview_composes_lines_like_the_app():
    import ucxapp

    assert ucxapp.compose("wifi.connect", ["My Net", 'a"b', ""], False) == 'wifi.connect "My Net" "a\\"b"'
    assert ucxapp.compose("at", ["AT+GMM ?"], True) == "at AT+GMM ?"
    assert ucxapp.compose("examples", ["uc-provision", "1"], True) == "examples uc-provision 1"
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
        groups = json.loads(get("/api/groups"))
        assert any(g.startswith("usecase:host:x:") for g in groups), groups
        assert [g.split(":")[3] for g in groups if g.startswith(("dev:", "bt:", "wifi:", "app:"))] == [
            "Device", "Bluetooth", "Wi-Fi, Thread", "App"]
        assert any(c["n"] == "wifi.scan" for c in json.loads(get("/api/commands?g=wifi")))
        assert [c["n"] for c in json.loads(get("/api/find?q=usecase.t&p=1"))] == ["usecase.tool"]
        assert json.loads(get("/api/complete?q=gnss.bridge%20nt")) == {"l": "gnss.bridge ntrip ", "n": 1, "c": ""}
        many = json.loads(get("/api/complete?q=bt.pairing%20"))
        assert many["n"] == 2 and many["c"].split() == ["on", "off"]
        events = urllib.request.urlopen(urllib.request.Request(base + "/events", headers={"X-Token": token}),
                                        timeout=30)
        post = urllib.request.Request(base + "/api/exec?cmd=usecase.tool&a0=4", method="POST",
                                      headers={"X-Token": token})
        assert urllib.request.urlopen(post, timeout=10).read() == b"accepted"
        seen = ""
        while "event: done" not in seen:
            seen += events.readline().decode()
        assert "Preview | no module" in seen and "900 MB/30 min" in seen
        # raw with several values keeps them all: the use-case "Run steps" button once lost the example number
        post = urllib.request.Request(base + "/api/exec?cmd=examples&a0=uc-provision&a1=1&r=1", method="POST",
                                      headers={"X-Token": token})
        assert urllib.request.urlopen(post, timeout=10).read() == b"accepted"
        seen = ""
        while "event: done" not in seen:
            seen += events.readline().decode()
        assert "Running uc-provision example 1" in seen, seen
        assert "u-connectXpressApp" in json.loads(get("/api/version"))["v"]
    finally:
        proc.terminate()
        proc.wait(10)


# Every form field, as the page lays it out: its box and the box of the form (.c) it belongs to.
LAYOUT_JS = """() => {
  const box = e => { const r = e.getBoundingClientRect(); return {l: r.left, r: r.right, t: r.top, b: r.bottom}; };
  const label = e => e.placeholder || e.textContent.trim().slice(0, 20) || e.tagName;
  const out = [];
  document.querySelectorAll('#side .c').forEach(c => {
    const items = Array.from(c.querySelectorAll(':scope > .fs > *, :scope > button, :scope > select'))
      .filter(e => e.offsetParent !== null)
      .map(e => Object.assign({
        ph: label(e),
        inner: !e.classList.contains('combo') ? []
          : Array.from(e.children).map(k => Object.assign({ph: label(k)}, box(k)))
      }, box(e)));
    out.push(Object.assign({name: c.querySelector('b').textContent, items}, box(c)));
  });
  return out;
}"""


@pytest.mark.app
def test_every_form_lays_out_without_overlaps():
    """Opens every group in the real page (19 rem side panel) and checks that no field runs into its neighbour
    or out of its form; the usage parser and the flex rules in index.html must agree for every command."""
    sync_api = pytest.importorskip("playwright.sync_api")
    proc = subprocess.Popen([sys.executable, "-u", str(ROOT / "ucxapp.py"), "--web-preview", "0"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    try:
        for line in proc.stdout:
            m = re.search(r"http://127\.0\.0\.1:(\d+)/#t=(\w+)", line)
            if m:
                break
        assert m
        with sync_api.sync_playwright() as p:
            try:
                browser = p.chromium.launch()
            except Exception as exc:
                pytest.skip(f"no browser: {exc}")
            page = browser.new_page(viewport={"width": 1400, "height": 900})
            page.goto(f"http://127.0.0.1:{m.group(1)}/#t={m.group(2)}")
            page.wait_for_selector("#st.ok")
            # Tab in the command line: a unique prefix completes and shows the usage hint, an ambiguous one
            # lists the candidates, a keyword completes too
            cmd = page.locator("#cmd")
            cmd.fill("wifi.con")
            cmd.press("Tab")
            page.wait_for_function("document.getElementById('cmd').value === 'wifi.connect '")
            page.wait_for_function("document.getElementById('hint').textContent.startsWith('wifi.connect [ssid')")
            cmd.fill("wifi.p")
            cmd.press("Tab")
            page.wait_for_function("document.getElementById('hint').textContent.includes('wifi.profile.auto')")
            cmd.fill("gnss.bridge nt")
            cmd.press("Tab")
            page.wait_for_function("document.getElementById('cmd').value === 'gnss.bridge ntrip '")
            cmd.fill("")
            page.evaluate("document.querySelectorAll('#grp > details').forEach(d => d.open = true)")
            page.wait_for_function("document.querySelectorAll('#side .c').length > 300")
            page.evaluate("document.querySelectorAll('#grp details').forEach(d => d.open = true)")
            forms = page.evaluate(LAYOUT_JS)
            browser.close()
    finally:
        proc.terminate()
        proc.wait(10)
    bad = []
    for f in forms:
        items = f["items"]
        for a in items:
            if a["r"] > f["r"] + 1 or a["l"] < f["l"] - 1:
                bad.append(f"{f['name']}: '{a['ph']}' sticks out of the form")
            if a["ph"] == "Run" and a["r"] < f["r"] - 2:
                bad.append(f"{f['name']}: Run is not at the right edge")
            for k in a["inner"]:   # a combo's select and input stay inside the combo
                if k["r"] > a["r"] + 1 or k["l"] < a["l"] - 1:
                    bad.append(f"{f['name']}: '{k['ph']}' sticks out of '{a['ph']}'")
        for i, a in enumerate(items):
            for b in items[i + 1:]:
                if a["l"] < b["r"] - 1 and b["l"] < a["r"] - 1 and a["t"] < b["b"] - 1 and b["t"] < a["b"] - 1:
                    bad.append(f"{f['name']}: '{a['ph']}' overlaps '{b['ph']}'")
    assert len(forms) > 300 and not bad, "\n".join(bad)

@pytest.mark.app
def test_history_never_stores_a_password():
    """The recent-command chips and Up/Down history live in localStorage: a password is cut off, also from old
    entries, and such a chip fills the command line instead of running."""
    sync_api = pytest.importorskip("playwright.sync_api")
    proc = subprocess.Popen([sys.executable, "-u", str(ROOT / "ucxapp.py"), "--web-preview", "0"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    try:
        for line in proc.stdout:
            m = re.search(r"http://127\.0\.0\.1:(\d+)/#t=(\w+)", line)
            if m:
                break
        assert m
        with sync_api.sync_playwright() as p:
            try:
                browser = p.chromium.launch()
            except Exception as exc:
                pytest.skip(f"no browser: {exc}")
            page = browser.new_page()
            url = f"http://127.0.0.1:{m.group(1)}/#t={m.group(2)}"
            page.goto(url)
            page.evaluate("localStorage.setItem('ucxh', JSON.stringify(['wifi.peap Corp anna OldSecret1 ca.pem']))")
            page.reload()
            page.wait_for_selector("#st.ok")
            page.wait_for_function("!localStorage.getItem('ucxh').includes('OldSecret1')")
            assert "wifi.peap Corp anna " in page.evaluate("localStorage.getItem('ucxh')")
            for line in ("wifi.connect 4 GqpZvmK8@r5yL#AP", 'wifi.ap.enable "My AP" Sup3rSecret',
                         "wifi.profile.add home homenet Hush123"):
                page.fill("#cmd", line)
                page.press("#cmd", "Enter")
                page.wait_for_selector("#run:not([disabled])")
            page.wait_for_function("JSON.parse(localStorage.getItem('ucxh')).length >= 4")
            stored = page.evaluate("localStorage.getItem('ucxh')")
            for secret in ("GqpZvmK8", "Sup3rSecret", "Hush123"):
                assert secret not in stored, stored
            assert "wifi.connect 4 " in stored and "wifi.profile.add home homenet " in stored, stored
            page.fill("#cmd", "")
            page.click("#rc button:has-text('wifi.connect 4')")
            assert page.input_value("#cmd") == "wifi.connect 4 "
            browser.close()
    finally:
        proc.terminate()
        proc.wait(10)
