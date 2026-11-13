"""Browser build (ucxapp --wasm): the app as WebAssembly answers the page in the same tab and talks to the module
over Web Serial. The serial port here is a fake that answers every AT command with OK."""

import re
import sys
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "build" / "wasm" / "site"
WASM = ROOT / "app" / "wasm"

FAKE_SERIAL = """
(() => {
  window.__tx = '';
  let push = null, isOpen = false, rd, wr;
  /* like a real port: new streams on every open; a lost port (__dead) rejects writes and, after a moment, the
     writer's close; a port with a locked writer cannot close, and an open port cannot open again; a silent
     module (__mute) takes the commands and never answers */
  const fresh = () => {
    rd = new ReadableStream({ start(c) { push = c; } });
    wr = new WritableStream({
      write(chunk) {
        if (window.__dead) throw new DOMException('The device has been lost.', 'NetworkError');
        const text = new TextDecoder().decode(chunk);
        window.__tx += text;
        if (window.__mute) return;
        if (/\\r/.test(text)) setTimeout(() => { try { push.enqueue(new TextEncoder().encode('\\r\\nOK\\r\\n')); }
                                                 catch (e) { /* closed: a real port drops it too */ } }, 5);
      },
      close() {
        if (window.__dead) return new Promise((_, no) => setTimeout(
          () => no(new DOMException('The device has been lost.', 'NetworkError')), 300));
      },
    });
  };
  fresh();
  const port = {
    get readable() { return rd; }, get writable() { return wr; },
    open: async () => {
      if (isOpen) throw new DOMException('The port is already open.', 'InvalidStateError');
      isOpen = true;
      window.__dead = false;
      fresh();
    },
    close: async () => {
      if (wr.locked) throw new TypeError('Cannot close a port whose writable stream is locked.');
      isOpen = false;
    },
    getInfo: () => ({ usbVendorId: 0x0403, usbProductId: 0x6015 }),
  };
  window.__asked = [];
  Object.defineProperty(navigator, 'serial', { value: {
    requestPort: async options => {
      window.__asked.push(options || null);
      if (window.__cancelPicks > 0) {
        window.__cancelPicks--;
        throw new DOMException('No port selected by the user.', 'NotFoundError');
      }
      return port;
    },
    addEventListener() {}, getPorts: async () => [port],
  } });
})();
"""


def test_site_sources_are_ascii():
    for f in WASM.iterdir():
        f.read_bytes().decode("ascii")


@pytest.mark.parametrize("profile, site_file", [("alma-w1-thread.yaml", "valma-w1.json"),
                                                ("nora-w36.yaml", "vnora-w36.json"),
                                                ("nora-b26-ble-thread.yaml", "vnora-b26-thread.json"),
                                                ("nora-b26-ble.yaml", "vnora-b26.json")])
def test_virtual_modules_are_the_vucx_profiles(profile, site_file):
    import json

    pytest.importorskip("yaml")
    from harness import vucx
    expected = json.loads(json.dumps(vucx.browser_table(profile)))
    assert json.loads((WASM / site_file).read_text(encoding="ascii")) == expected, \
        f"regenerate: cd tests; python -m harness.vucx --json {profile} > ../app/wasm/{site_file}"


def test_page_script_is_injected_before_the_page_script(tmp_path):
    import subprocess
    out = tmp_path / "index.html"
    subprocess.run(["cmake", f"-DPAGE={ROOT / 'app' / 'web' / 'index.html'}", f"-DOUT={out}", "-P",
                    str(WASM / "site.cmake")], check=True)
    html = out.read_text(encoding="ascii")
    assert html.index('src="ucxapp.js"') < html.index('src="ucxapp-wasm.js"') < html.index("<script>\n")
    assert "coi-serviceworker.js" in html


def test_site_is_an_installable_offline_app(tmp_path):
    import json
    import subprocess
    out = tmp_path / "index.html"
    subprocess.run(["cmake", f"-DPAGE={ROOT / 'app' / 'web' / 'index.html'}", f"-DOUT={out}", "-P",
                    str(WASM / "site.cmake")], check=True)
    html = out.read_text(encoding="ascii")
    assert html.index('rel="manifest" href="manifest.webmanifest"') < html.index("</head>")
    manifest = json.loads((WASM / "manifest.webmanifest").read_text(encoding="ascii"))
    assert manifest["display"] == "standalone" and manifest["start_url"] == "./"
    assert manifest["icons"] and all(i["src"] == "icon.svg" for i in manifest["icons"])
    # every file the site ships is kept for offline use, and every kept file is shipped
    worker = (WASM / "coi-serviceworker.js").read_text(encoding="ascii")
    cached = set(re.findall(r"'([\w.-]+\.\w+)'", worker[worker.index("FILES"):worker.index("];")]))
    cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
    copied = cmake[cmake.index("add_custom_command(TARGET ucxapp POST_BUILD"):cmake.index("VERBATIM")]
    shipped = {Path(p).name for p in re.findall(r"/(?:wasm|web|web/public)/([\w.-]+\.(?:json|js|css|svg|webmanifest))",
                                                 copied)} - {"coi-serviceworker.js"}
    assert cached == shipped | {"index.html", "ucxapp.js", "ucxapp.wasm"}


def test_public_look_is_only_on_the_site(tmp_path):
    import subprocess
    out = tmp_path / "index.html"
    page = ROOT / "app" / "web" / "index.html"
    subprocess.run(["cmake", f"-DPAGE={page}", f"-DOUT={out}", "-P", str(WASM / "site.cmake")], check=True)
    html = out.read_text(encoding="ascii")
    assert html.index('href="fx.css"') < html.index("</head>")
    assert "if(localStorage.getItem('fxoff')!=='0')document.getElementById('fx-css').disabled=true" in html
    assert html.index("</script>", html.index("<script>\n")) < html.index('src="fx.js"') < html.index("</body>")
    assert html.index('src="fx.js"') < html.index('src="guide.js"') < html.index("</body>")
    assert "guide.js" not in page.read_text(encoding="ascii")
    assert "fx.css" not in page.read_text(encoding="ascii") and "fx.js" not in page.read_text(encoding="ascii")
    for f in (ROOT / "app" / "web" / "public").iterdir():
        f.read_bytes().decode("ascii")
    assert "body.fx-off" in (ROOT / "app" / "web" / "public" / "fx.css").read_text(encoding="ascii")


@pytest.fixture
def site():
    if not (SITE / "ucxapp.wasm").exists():
        pytest.skip("no browser build (ucxapp --wasm --build)")
    import ucxapp
    srv = ucxapp.wasm_server(SITE, 0)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}/"
    srv.shutdown()


def run(page, cmd):
    """Like a user: wait until Run is enabled again (a disabled button drops the click), then run."""
    page.wait_for_function("!document.getElementById('run').disabled", timeout=60000)
    page.fill("#cmd", cmd)
    page.click("#run")


def test_fx_off_has_no_colors(site):
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        # a first visit (nothing stored) is plain
        page.goto(site)
        page.wait_for_selector("#fx-tg", state="attached")  # it sits in the closed overflow menu
        assert page.evaluate("document.getElementById('fx-css').disabled")
        assert "gradient" not in page.eval_on_selector("#ucs", "e => getComputedStyle(e).backgroundImage")
        assert "gradient" not in page.eval_on_selector("h1", "e => getComputedStyle(e).backgroundImage")
        assert not page.query_selector("#fx-bg")
        # a direct link turns it back on and remembers it
        page.goto(site + "?fx=1")
        page.wait_for_selector("#fx-bg")
        assert not page.evaluate("document.getElementById('fx-css').disabled")
        assert page.evaluate("localStorage.getItem('fxoff')") == "0"
        browser.close()


FX_COST_TAG = "--ucx-fx-cost"   # marks the browser process whose CPU the FX test sums


def _fx_cost(page, browser_pid, seconds=3.0):
    """CPU of the whole browser (every process, so paint, raster and GPU too) in % of one core while the page
    sits idle."""
    psutil = pytest.importorskip("psutil")
    procs = [psutil.Process(browser_pid)] + psutil.Process(browser_pid).children(recursive=True)

    def cpu():
        total = 0.0
        for pr in procs:
            try:
                t = pr.cpu_times()
                total += t.user + t.system
            except psutil.NoSuchProcess:
                pass
        return total

    # the lowest of three windows: other work on the machine only ever adds to a sample
    costs = []
    for _ in range(3):
        t0, c0 = time.monotonic(), cpu()
        page.wait_for_timeout(seconds * 1000)
        costs.append((cpu() - c0) / (time.monotonic() - t0) * 100)
    return min(costs)


def test_fx_is_light(site):
    """FX on the site was too heavy (a full core at 1080p): it stays within a budget while you use the page and
    costs nothing extra once you leave it alone."""
    sync_api = pytest.importorskip("playwright.sync_api")
    psutil = pytest.importorskip("psutil")
    with sync_api.sync_playwright() as p:
        try:
            # the GPU when there is one, like a desktop browser; without one Chromium renders in software
            gpu_args = ["--enable-gpu", "--ignore-gpu-blocklist"]
            if sys.platform == "win32":
                gpu_args.append("--use-angle=d3d11")
            browser = p.chromium.launch(args=[FX_COST_TAG] + gpu_args)
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        pid = next(pr.pid for pr in psutil.process_iter(["cmdline"])
                   if FX_COST_TAG in (pr.info["cmdline"] or []) and "--type=" not in " ".join(pr.info["cmdline"]))
        page = browser.new_page(viewport={"width": 1920, "height": 1080})
        page.add_init_script(FAKE_SERIAL)
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        cost = {}
        for fx in ("0", "1"):
            page.goto(f"{site}?fx={fx}")
            page.wait_for_selector("#st.ok", timeout=30000)
            page.mouse.move(400, 300)
            page.wait_for_timeout(2500)   # splash and entrance animations are over
            cost[fx] = _fx_cost(page, pid)
        gpu = page.evaluate("(() => { const g = document.createElement('canvas').getContext('webgl'); "
                            "const e = g && g.getExtension('WEBGL_debug_renderer_info'); "
                            "return e ? g.getParameter(e.UNMASKED_RENDERER_WEBGL) : ''; })()")
        if gpu and not re.search(r"SwiftShader|llvmpipe|Basic Render", gpu):
            assert cost["1"] - cost["0"] < 50, f"FX on costs {cost['1']:.0f}% of a core, off {cost['0']:.0f}% ({gpu})"
        # left alone, the endless motion stops: as light as the plain page
        page.wait_for_selector("body.fx-idle", timeout=20000)
        page.wait_for_timeout(500)
        idle = _fx_cost(page, pid)
        assert idle - cost["0"] < 10, f"idle FX costs {idle:.0f}% of a core, off {cost['0']:.0f}%"
        # and the next move brings it back
        page.mouse.move(600, 400)
        page.wait_for_selector("body:not(.fx-idle)", timeout=2000)
        browser.close()
    assert not js_errors, js_errors


def test_app_runs_in_the_browser(site):
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        js_errors, warnings = [], []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.on("console", lambda msg: msg.type == "warning" and warnings.append(msg.text))
        page.goto(site)
        assert page.evaluate("self.crossOriginIsolated")
        page.wait_for_selector("#st.ok", timeout=30000)
        page.wait_for_selector("summary:has-text('wifi')")
        # a host command runs in the WebAssembly app and its output comes back as events
        page.fill("#cmd", "usecase.tool")
        page.click("#run")
        page.wait_for_selector(".blk .badge.ok", timeout=30000)
        assert "usecase.tool" in page.inner_text(".blk .bh")
        # Tab completes the command, then its keyword, through /api/complete in the app
        page.fill("#cmd", "gnss.brid")
        page.press("#cmd", "Tab")
        page.wait_for_function("document.getElementById('cmd').value === 'gnss.bridge '", timeout=10000)
        page.type("#cmd", "ntr")
        page.press("#cmd", "Tab")
        page.wait_for_function("document.getElementById('cmd').value === 'gnss.bridge ntrip '", timeout=10000)
        page.fill("#cmd", "")
        # a GNSS receiver that sends gets a tab in What's happening, drawn as a picture; it goes when it closes
        assert page.locator("#pet button[data-k='g']").count() == 0
        run(page, "gnss.virtual 55.5 13.25 20")
        page.click("#pet button[data-k='g']", timeout=30000)
        page.wait_for_function("document.querySelector('#cnd .gv a') && "
                               "document.querySelector('#cnd .gv a').textContent"
                               ".includes('55.5000000 N  13.2500000 E  alt 20.0 m')", timeout=30000)
        assert page.is_visible("#cnd") and page.is_hidden("#ev") and page.locator("#cnd .gv .seg i").count() == 3
        assert page.inner_text("#cnd .gv span").startswith("FIX")
        run(page, "gnss.virtual off")
        page.wait_for_selector("#pet button[data-k='g']", state="detached", timeout=30000)
        assert page.is_hidden("#cnd") and page.is_visible("#ev")
        # the module over (fake) Web Serial
        page.click("#wsc")
        page.wait_for_function("window.__tx.includes('AT')", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                               timeout=60000)
        assert page.inner_text("#wsc") == "Disconnect module"
        # log timestamps come from a working clock (no negative fields)
        assert not re.search(r"\[[^\]]*-\d", page.inner_text("#out"))
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        # no hardware: a virtual module per radio set, ALMA-W1 (BLE, Wi-Fi and Thread) by default
        assert page.locator("#wsvm option").all_inner_texts() == [
            "Wi-Fi + BLE + Thread", "Wi-Fi + BLE", "BLE + Thread", "BLE"]
        assert page.input_value("#wsvm") == "valma-w1.json"
        page.click("#wsv")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.split('connected to webserial').length > 2",
                               timeout=60000)
        # the app's own state: advertising and listening sockets as chips, its clock and source in the footer
        page.wait_for_function("[...document.querySelectorAll('#state .chip')].some(c => c.textContent === "
                               "'Advertising on')", timeout=30000)
        assert "Listening off" in page.locator("#state .chip").all_inner_texts()
        page.wait_for_function(r"/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ \/ \S/.test("
                               "document.getElementById('clock').textContent)", timeout=30000)
        assert page.inner_text("#loc") == "Location: unknown"
        run(page, "wifi.scan")
        page.wait_for_function(
            "document.getElementById('out').innerText.includes('no Wi-Fi regulatory domain chosen')", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.includes('vucx-net')", timeout=30000)
        run(page, "wifi.regdomain world")
        page.wait_for_function("document.getElementById('out').innerText.includes('regdomain=world')", timeout=30000)
        run(page, "wifi.scan")
        page.wait_for_function("document.getElementById('out').innerText.split('vucx-net').length > 2", timeout=60000)
        assert page.inner_text("#out").count("no Wi-Fi regulatory domain chosen") == 1
        page.click("#wsc")
        page.wait_for_function("!document.getElementById('wsv').hidden", timeout=30000)
        # the BLE-only module blocks Wi-Fi
        page.select_option("#wsvm", "vnora-b26.json")
        page.click("#wsv")
        page.wait_for_function("document.getElementById('out').innerText.split('connected to webserial').length > 3",
                               timeout=60000)
        run(page, "wifi.status")
        page.wait_for_function("document.getElementById('out').innerText.includes('wifi.status needs Wi-Fi')",
                               timeout=30000)
        page.click("#wsc")
        page.wait_for_function("!document.getElementById('wsv').hidden", timeout=30000)
        browser.close()
    assert not js_errors, js_errors
    assert not [w for w in warnings if "serial" in w], warnings


def test_last_port_is_reopened_until_disconnected(site):
    """A port granted and opened before comes back on the next visit; the Disconnect button forgets it."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.goto(site)
        page.wait_for_selector("#st.ok", timeout=30000)
        assert page.inner_text("#wsc") == "Connect module"   # nothing remembered yet
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        assert '"vid":1027' in page.evaluate("localStorage.getItem('ucxapp.lastPort')")
        page.reload()
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                               timeout=60000)
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        assert page.evaluate("localStorage.getItem('ucxapp.lastPort')") is None
        page.reload()
        page.wait_for_selector("#st.ok", timeout=30000)
        page.wait_for_timeout(500)
        assert page.inner_text("#wsc") == "Connect module"
        browser.close()
    assert not js_errors, js_errors


def test_a_lost_port_closes_and_connects_again(site):
    """A port lost under the app (unplugged) rejects the writer's close: the lock is still released and the port
    closed, so Connect opens it again instead of failing with 'already open', also when it is clicked while the
    close is still under way."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        js_errors, console = [], []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.on("console", lambda msg: console.append(msg.text))
        page.goto(site)
        page.wait_for_selector("#st.ok", timeout=30000)
        page.click("#wsc")
        page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                               timeout=60000)
        page.evaluate("window.__dead = true")
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        page.click("#wsc")
        try:
            page.wait_for_function(
                "document.getElementById('out').innerText.split('connected to webserial').length > 2", timeout=60000)
        except sync_api.TimeoutError:
            pytest.fail("not connected again; the page shows:\n" + page.inner_text("#out")[-1500:] +
                        "\nconsole:\n" + "\n".join(console[-20:]))
        assert "already open" not in page.inner_text("#out")
        browser.close()
    assert not js_errors, js_errors


def test_a_port_lost_during_a_command_ends_the_command_at_once(site):
    """A command waiting for the module when the port is lost ends with an I/O error at once, not after its AT
    timeout (10 s for every command still to run), so Connect works again within seconds."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        page.goto(site)
        page.wait_for_selector("#st.ok", timeout=30000)
        page.evaluate("window.__mute = true")   # dev.connect's first AT command gets no answer
        page.click("#wsc")
        page.wait_for_function("window.__tx.includes('AT')", timeout=60000)
        page.evaluate("window.__dead = true; window.__mute = false")
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        page.click("#wsc")
        try:
            page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                                   timeout=8000)
        except sync_api.TimeoutError:
            pytest.fail("the lost port held the app; the page shows:\n" + page.inner_text("#out")[-1500:])
        browser.close()


def test_port_picker_shows_the_evks_and_all_ports_on_request(site):
    """The picker lists the EVKs' USB bridges; cancelling it offers every serial port, and so does Shift+click."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script(FAKE_SERIAL)
        page.goto(site)
        page.wait_for_selector("#st.ok", timeout=30000)
        page.evaluate("window.__cancelPicks = 1")
        page.click("#wsc")
        page.wait_for_selector("#qd[open]")
        assert "Show all ports" == page.inner_text("#qy")
        page.click("#qy")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        evks, everything = page.evaluate("window.__asked")
        assert {"usbVendorId": 0x0403, "usbProductId": 0x6015} in evks["filters"]
        assert {"usbVendorId": 0x1546} in evks["filters"]
        assert everything == {}
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        page.reload()   # the fake port's stream is not readable twice; a fresh page gets a fresh one
        page.wait_for_selector("#st.ok", timeout=30000)
        page.click("#wsc", modifiers=["Shift"])
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        assert page.evaluate("window.__asked") == [{}]
        browser.close()


def test_no_web_serial_starts_the_virtual_module(site):
    """Firefox, Safari and phones have no Web Serial: the site opens on the default virtual module by itself."""
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script("delete Navigator.prototype.serial")
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.goto(site)
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                               timeout=60000)
        assert "virtual module" in page.inner_text("#wsnote")
        assert page.input_value("#wsvm") == "valma-w1.json"
        browser.close()
    assert not js_errors, js_errors


def test_first_visit_on_a_host_without_headers_isolates_itself():
    """GitHub Pages sets no COOP/COEP: on the first visit the service worker takes the page, which reloads once,
    isolated, and the app starts."""
    if not (SITE / "ucxapp.wasm").exists():
        pytest.skip("no browser build (ucxapp --wasm --build)")
    sync_api = pytest.importorskip("playwright.sync_api")
    from functools import partial
    from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

    class Plain(SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    srv = ThreadingHTTPServer(("127.0.0.1", 0), partial(Plain, directory=str(SITE)))
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        with sync_api.sync_playwright() as p:
            try:
                browser = p.chromium.launch()
            except Exception as exc:
                pytest.skip(f"no browser: {exc}")
            page = browser.new_context().new_page()
            page.add_init_script("delete Navigator.prototype.serial")
            # the slow case: the worker claims the page before register() resolves
            page.add_init_script("""{const r = ServiceWorkerContainer.prototype.register;
                ServiceWorkerContainer.prototype.register = function (...a) {
                  return r.apply(this, a).then(reg => this.controller ? reg :
                    new Promise(ok => this.addEventListener('controllerchange', () => ok(reg), { once: true })));
                };}""")
            page.goto(f"http://127.0.0.1:{srv.server_address[1]}/")
            page.wait_for_function("self.crossOriginIsolated", timeout=30000)
            page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                                   timeout=60000)
            browser.close()
    finally:
        srv.shutdown()


def test_matter_node_is_built_in(site):
    """ucxapp --wasm links the Matter node into the app: a browser loads no plug-in file."""
    if b"Matter SDK " not in (SITE / "ucxapp.wasm").read_bytes():
        pytest.skip("the browser build was made without the Matter node (--no-matter)")
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page()
        page.add_init_script("delete Navigator.prototype.serial")
        js_errors = []
        page.on("pageerror", lambda exc: js_errors.append(str(exc)))
        page.goto(site)
        page.wait_for_function("document.getElementById('out').innerText.includes('connected to webserial')",
                               timeout=60000)
        run(page, "matter.status")
        page.wait_for_function("document.getElementById('out').innerText.includes('plug-in: built in')",
                               timeout=30000)
        run(page, "app.sbom")
        page.wait_for_function("document.getElementById('out').innerText.includes('Matter node')", timeout=30000)
        assert "cannot load the Matter plug-in" not in page.inner_text("#out")
        # The node's card follows it: the app's clock must not start again when the module connects
        run(page, "matter.start")
        page.wait_for_function("document.getElementById('out').innerText.includes('node started')", timeout=60000)
        page.get_by_role("tab", name="MATTER").click()
        page.get_by_role("heading", name="MATTER node").wait_for(timeout=10000)
        page.get_by_role("button", name="on", exact=True).click()
        page.get_by_text("light on 100%").wait_for(timeout=10000)
        browser.close()
    assert not js_errors, js_errors
