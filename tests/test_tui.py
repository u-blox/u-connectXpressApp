"""The three-pane text UI driven through a pseudo terminal (Linux/WSL, run by ucxapp --coverage)."""

import os
import subprocess
import sys
import time

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="drives the TUI through a pty (Linux/WSL)")]

UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, DEL, PGUP, PGDN, F1, F10 = "\x1b[H", "\x1b[F", "\x1b[3~", "\x1b[5~", "\x1b[6~", "\x1bOP", "\x1b[21~"
ESC, ENTER, TAB, BS = "\x1b", "\r", "\t", "\x7f"


def _size(fd, cols, rows):
    import fcntl
    import struct
    import termios

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


class Tui:
    def __init__(self, exe, port, cwd, cols=140, rows=45):
        self.master, slave = os.openpty()
        _size(slave, cols, rows)
        self.proc = subprocess.Popen([str(exe), port, "--tui"], stdin=slave, stdout=slave, stderr=slave, cwd=cwd,
                                     close_fds=True)
        os.close(slave)
        self.out = b""
        self.pump(2.0)

    def pump(self, seconds):
        import select

        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([self.master], [], [], 0.05)[0]:
                try:
                    self.out += os.read(self.master, 65536)
                except OSError:
                    return

    def keys(self, *keys, gap=0.12):
        for k in keys:
            os.write(self.master, k.encode())
            self.pump(gap)

    def type(self, text):
        self.keys(*text, gap=0.03)

    def resize(self, cols, rows):
        _size(self.master, cols, rows)
        self.pump(0.3)

    def quit(self, key=F10):
        os.write(self.master, key.encode())
        deadline = time.monotonic() + 20
        while self.proc.poll() is None and time.monotonic() < deadline:
            self.pump(0.1)
        self.pump(0.2)
        if self.proc.poll() is None:
            self.proc.kill()
        os.close(self.master)
        return self.proc.wait(timeout=5), self.out.decode(errors="replace")


def test_tui_list_navigation_and_commands(app_exe, tmp_path):
    t = Tui(app_exe, "COM255", tmp_path)
    # without a module the list is short: stay on the first groups, END lands on Quit
    t.keys(DOWN, RIGHT, DOWN, DOWN, RIGHT, LEFT, LEFT, LEFT, END, HOME, DOWN, UP)
    t.keys(PGUP, PGUP, PGDN, PGDN, ENTER)
    t.pump(0.5)
    t.keys(ESC)
    t.type("help wifi")
    t.keys(ENTER)
    t.pump(1.0)
    t.keys(ESC)
    t.type("app.ver")
    t.keys(TAB, ENTER)
    t.pump(1.0)
    t.keys(ESC)
    t.type("xyz")
    t.keys(LEFT, LEFT, DEL, BS, HOME, END, RIGHT, BS, BS, UP, UP, DOWN, ESC, F1)
    t.resize(70, 22)
    t.keys(DOWN, DOWN, ESC)
    t.type("nosuch.command")
    t.keys(ENTER)
    t.pump(0.5)
    rc, out = t.quit()
    assert rc == 0, out[-3000:]
    assert "\x1b[?1049h" in out and "\x1b[?1049l" in out
    assert "text UI" in out
    assert "wifi.scan" in out           # help wifi
    assert "u-connectXpressApp" in out  # app.version via tab completion
    assert "[exit" in out


def test_tui_tab_completes_keywords_and_lists(app_exe, tmp_path):
    t = Tui(app_exe, "COM255", tmp_path)
    t.keys(ESC)
    t.type("app.log.lev")
    t.keys(TAB)
    t.type("w")
    t.keys(TAB, ENTER)               # app.log.level warn
    t.pump(1.0)
    t.type("gnss.bridge ")
    t.keys(TAB, TAB)                 # the second Tab lists the keywords
    t.keys(*([BS] * len("gnss.bridge ")))
    t.type("app.ver extra")
    t.keys(*([LEFT] * 6), TAB)       # Tab before the cursor keeps the rest of the line
    rc, out = t.quit()
    assert rc == 0, out[-3000:]
    assert "app.log.level warn" in out
    assert "matches:" in out and "ntrip" in out and "status" in out
    assert "app.version extra" in out


def test_tui_with_a_module_shows_the_dashboard(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    try:
        t = Tui(app_exe, module.start(), tmp_path)
        t.type("dev.info")
        t.keys(ENTER)
        t.pump(2.5)          # the dashboard refreshes every second
        t.keys(ESC)
        t.type("at")
        t.keys(ENTER)
        t.pump(0.5)
        t.type("AT+GMM")
        t.keys(ENTER)
        t.pump(0.5)
        t.keys(ESC)          # leaves the AT terminal
        t.pump(0.5)
        rc, out = t.quit(key="\x03")   # Ctrl+C quits too
    finally:
        module.stop()
    assert rc == 0, out[-3000:]
    assert "NORA-W36" in out, out[-3000:]
    assert "AT+GMI" in module.log or "AT+GMM" in module.log, module.log


def test_tui_status_row_shows_the_thread_role_and_the_summary(app_exe, tmp_path):
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    profile = load("nora-b26-thread.yaml")
    profile["commands"]["AT+UTHE?"] = ["+UTHE:1", "OK"]
    module = Vucx(profile)
    try:
        t = Tui(app_exe, module.start(), tmp_path)
        t.pump(2.5)
        rc, out = t.quit()
    finally:
        module.stop()
    assert rc == 0, out[-3000:]
    assert "THRD leader" in out, out[-3000:]
    assert "Radios:" in out and "Thread" in out, out[-3000:]


def test_tui_needs_a_terminal(app_exe, tmp_path):
    out = subprocess.run([str(app_exe), "COM255", "--tui"], capture_output=True, text=True, timeout=30,
                         errors="replace", stdin=subprocess.DEVNULL, cwd=tmp_path)
    assert out.returncode != 0
    assert "needs an interactive terminal" in out.stdout
