"""The command manual (COMMANDS.md) built into the app: manual, manual.search and the web page's Manual dialog."""

import subprocess

import pytest
from test_webpage import ROOT, Mock, server  # noqa: F401  (server is a fixture)

MANUAL = (ROOT / "COMMANDS.md").read_text(encoding="utf-8")


def run(app_exe, *cmds):
    args = [str(app_exe), "--offline"]
    for c in cmds:
        args += ["--exec", c]
    p = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
    return p.returncode, p.stdout + p.stderr


def test_manual_is_embedded_from_commands_md():
    cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "../COMMANDS.md" in cmake and "ucxapp_manual_text.h" in cmake
    assert cmake.count("src/ucxapp_cmds_manual.c") == 2


def test_manual_lists_the_sections(app_exe):
    rc, out = run(app_exe, "manual")
    assert rc == 0
    for heading in ("Command reference", "Wi-Fi", "Cellular modules", "Exit codes and errors"):
        assert f"  {heading}\n" in out, heading
    assert "One-shot" not in out   # '#' lines inside code blocks are not headings


@pytest.mark.parametrize("topic", ["wifi.ap.enable", "WI-FI", "wifi.ap"])
def test_manual_shows_the_wifi_section_for_a_topic_or_command(app_exe, topic):
    rc, out = run(app_exe, f"manual {topic}")
    assert rc == 0
    assert out.lstrip().startswith("Wi-Fi\n-----")
    assert "wifi.ap.enable" in out and "Start a WPA2 access point" in out
    assert "### Network" not in out and "\nNetwork\n" not in out   # stops at the next section
    assert "`" not in out and "**" not in out                      # markdown markup is stripped


def test_manual_tables_keep_escaped_pipes_in_one_cell(app_exe):
    rc, out = run(app_exe, "manual cell.bridge")
    assert rc == 0
    row = next(line for line in out.splitlines() if line.strip().startswith("cell.bridge"))
    assert "tcp [port]|tcpc <host> <port>|udp" in row and "\\|" not in row


def test_manual_code_blocks_are_indented(app_exe):
    rc, out = run(app_exe, "manual wifi")
    assert rc == 0
    assert '    ucxapp COM13 --exec "wifi.connect MySsid MyPassword"' in out


def test_manual_all_prints_every_section(app_exe):
    rc, out = run(app_exe, "manual all")
    assert rc == 0
    for heading in ("Command Layer", "Porting to another target"):
        assert heading in out


def test_manual_unknown_topic_fails_with_a_hint(app_exe):
    rc, out = run(app_exe, "manual nosuchtopicxyz")
    assert rc == 1
    assert "manual.search nosuchtopicxyz" in out


def test_manual_rejects_extra_arguments(app_exe):
    rc, _ = run(app_exe, "manual wifi extra")
    assert rc != 0


def test_manual_search_groups_hits_by_section(app_exe):
    rc, out = run(app_exe, "manual.search hotspot")
    expected = sum("hotspot" in line.lower() for line in MANUAL.splitlines() if not line.startswith("#"))
    if expected == 0:
        assert rc == 1 and "No match" in out
        return
    assert rc == 0
    assert out.count("\n  ") >= 1


def test_manual_search_finds_the_ap_commands(app_exe):
    rc, out = run(app_exe, "manual.search wifi.ap.status")
    assert rc == 0
    assert "Wi-Fi:" in out and "Show AP parameters" in out


def test_manual_search_without_a_match_fails(app_exe):
    rc, out = run(app_exe, "manual.search zzqqxxnotthere")
    assert rc == 1 and "No match" in out


def test_manual_search_needs_one_word(app_exe):
    rc, _ = run(app_exe, "manual.search")
    assert rc != 0


class ManualMock(Mock):
    def do_GET(self):
        if self.path.startswith("/api/manual"):
            self.reply(MANUAL, "text/markdown")
        else:
            super().do_GET()


@pytest.fixture
def manual_server():
    import threading
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", 0), ManualMock)
    srv.posted, srv.stop = [], threading.Event()
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield srv
    srv.stop.set()
    srv.shutdown()


def test_manual_dialog_renders_the_markdown(manual_server):
    sync_api = pytest.importorskip("playwright.sync_api")
    with sync_api.sync_playwright() as p:
        try:
            browser = p.chromium.launch()
        except Exception as exc:
            pytest.skip(f"no browser: {exc}")
        page = browser.new_page(viewport={"width": 1280, "height": 900})
        errors = []
        page.on("pageerror", lambda exc: errors.append(str(exc)))
        page.goto(f"http://127.0.0.1:{manual_server.server_address[1]}/?t=x")
        page.wait_for_selector("#st.ok")
        page.evaluate("document.getElementById('man').click()")
        page.wait_for_selector("#mc table")
        assert page.locator("#mt a", has_text="Wi-Fi").count() == 1
        assert page.locator("#mc h3#m-cellular-modules").count() == 1
        assert page.locator("#mc pre").count() > 5
        assert "<script" not in page.inner_html("#mc")
        cell = page.locator("#mc td", has_text="tcpc <host> <port>").first
        assert "\\|" not in cell.inner_text()
        page.fill("#mq", "wifi.ap.enable")
        assert page.locator("#mc mark").count() >= 1
        # a command in the manual goes to the command line
        page.locator("#mc td:first-child code.ck", has_text="wifi.ap.status").first.click()
        assert page.input_value("#cmd") == "wifi.ap.status"
        assert not page.evaluate("document.getElementById('md').open")
        # typing 'manual <topic>' opens the dialog instead of running it
        page.fill("#cmd", "manual wifi.ap.disable")
        page.press("#cmd", "Enter")
        page.wait_for_function("document.getElementById('md').open")
        assert page.input_value("#mq") == "wifi.ap.disable"
        assert not any("cmd=manual" in u for u in manual_server.posted)
        assert not errors, errors
        browser.close()
