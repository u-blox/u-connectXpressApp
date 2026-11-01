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
      if (/\\r/.test(text)) setTimeout(() => push.enqueue(new TextEncoder().encode('\\r\\nOK\\r\\n')), 5);
    } }),
    open: async () => {}, close: async () => {},
  };
  Object.defineProperty(navigator, 'serial', { value: {
    requestPort: async () => port, addEventListener() {}, getPorts: async () => [port],
  } });
})();
"""


def test_site_sources_are_ascii():
    for f in WASM.iterdir():
        f.read_bytes().decode("ascii")


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
        browser.close()
    assert not js_errors, js_errors
    assert not [w for w in warnings if "serial" in w], warnings
