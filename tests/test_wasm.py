"""Browser build (ucxapp --wasm): the app as WebAssembly answers the page in the same tab and talks to the module
over Web Serial. The serial port here is a fake that answers every AT command with OK."""

import re
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "build" / "wasm" / "site"
WASM = ROOT / "app" / "wasm"

FAKE_SERIAL = """
(() => {
  window.__tx = '';
  let push = null;
  const port = {
    readable: new ReadableStream({ start(c) { push = c; } }),
    writable: new WritableStream({ write(chunk) {
      const text = new TextDecoder().decode(chunk);
      window.__tx += text;
      if (/\\r/.test(text)) setTimeout(() => { try { push.enqueue(new TextEncoder().encode('\\r\\nOK\\r\\n')); }
                                               catch (e) { /* closed: a real port drops it too */ } }, 5);
    } }),
    open: async () => {}, close: async () => {}, getInfo: () => ({ usbVendorId: 0x0403, usbProductId: 0x6015 }),
  };
  Object.defineProperty(navigator, 'serial', { value: {
    requestPort: async () => port, addEventListener() {}, getPorts: async () => [port],
  } });
})();
"""


def test_site_sources_are_ascii():
    for f in WASM.iterdir():
        f.read_bytes().decode("ascii")


@pytest.mark.parametrize("profile, site_file", [("nora-w36.yaml", "vnora.json"),
                                                ("alma-w1-thread.yaml", "valma-w1.json")])
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
        # the module over (fake) Web Serial
        page.click("#wsc")
        page.wait_for_function("window.__tx.includes('AT')", timeout=30000)
        page.wait_for_function("document.querySelectorAll('.blk .badge.ok, .blk .badge.bad').length >= 2",
                               timeout=60000)
        assert page.inner_text("#wsc") == "Disconnect module"
        assert "connected to webserial" in page.inner_text("#out")
        # log timestamps come from a working clock (no negative fields)
        assert not re.search(r"\[[^\]]*-\d", page.inner_text("#out"))
        page.click("#wsc")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Connect module'", timeout=30000)
        # no hardware: the virtual NORA-W36 answers like the vucx profile
        page.click("#wsv")
        page.wait_for_function("document.getElementById('wsc').textContent === 'Disconnect module'", timeout=30000)
        page.wait_for_function("document.getElementById('out').innerText.split('connected to webserial').length > 2",
                               timeout=60000)
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
