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
        self.pump(5.0, until="text UI")
        self.pump(0.2)

    def pump(self, seconds, until=None):
        import select

        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if until and until.encode() in self.out:
                return
            if select.select([self.master], [], [], 0.05)[0]:
                try:
                    self.out += os.read(self.master, 65536)
                except OSError:
                    return

    def keys(self, *keys, gap=0.06):
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
    t.pump(5.0, until="wifi.scan")
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
    t.pump(5.0, until="app.log.level warn")
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
        t.pump(5.0, until="NORA-W36")
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
        t.pump(15, until="THRD leader")
        t.pump(0.3)
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


F8 = "\x1b[19~"


def _walk_to(t, hint, key=DOWN, steps=80):
    """Presses key until the hint line names hint; True when found."""
    for _ in range(steps):
        mark = len(t.out)
        t.keys(key, gap=0.08)
        if hint.encode() in t.out[mark:]:
            return True
    return False


def test_tui_sub_menus_prompts_select_mode_and_snake(app_exe, tmp_path):
    """Sub-menus open and close, a pairing request takes the input line (Enter accepts, Esc denies), F8 freezes the
    screen for selecting text, clear and app.snake run in the UI itself and exit quits."""
    pytest.importorskip("yaml")
    from harness.vucx import Vucx, load

    peer = "C0FFEE001122p"
    profile = load("nora-w36.yaml")
    profile["urcs"]["AT+UBTB=*"] = [[f"+UEBTUC:{peer},654321", f"+UEBTB:{peer},0"],
                                    [f"+UEBTUPE:{peer}", f"+UEBTB:{peer},2"]]
    module = Vucx(profile)
    try:
        t = Tui(app_exe, module.start(), tmp_path)
        t.keys(HOME)
        assert _walk_to(t, "] gatt  -"), t.out[-2000:]
        t.keys(RIGHT)                                    # opens the group: its sub-menus come first
        assert _walk_to(t, ".*  (Enter opens, Left closes)"), t.out[-2000:]
        t.keys(ENTER, DOWN, LEFT)                        # opens the sub-menu, then Left goes back to it
        t.keys(LEFT, LEFT, LEFT)                         # closes it, back to the group, closes the group
        t.keys(F8)
        t.pump(0.3)
        t.keys("x", ESC)                                 # keys do nothing while selecting; Esc goes back
        t.keys(ESC)
        t.type("bt.autoconfirm off")
        t.keys(ENTER)
        t.pump(3.0, until="autoconfirm")
        t.keys(ESC)
        t.type("bt.bond 0 2")
        t.keys(ENTER)
        t.pump(5.0, until="PAIRING REQUEST, code 654321")
        t.pump(1.0)
        t.keys(ENTER)                                    # bt.confirm yes
        t.pump(3.0, until="AT+UBTUC")
        t.keys(ESC)
        t.type("bt.bond 0 2")
        t.keys(ENTER)
        t.pump(5.0, until="PASSKEY REQUEST")
        t.pump(1.0)
        t.keys(ESC)                                      # bt.passkey deny
        t.pump(2.0)
        t.keys(ESC)
        t.type("clear")
        t.keys(ENTER)
        t.type("app.snake")
        t.keys(ENTER)
        t.pump(1.0)
        t.keys("q")
        t.pump(2.0, until="snake: score")
        t.keys(ESC)
        t.type("exit")
        t.keys(ENTER)
        deadline = time.monotonic() + 20
        while t.proc.poll() is None and time.monotonic() < deadline:
            t.pump(0.1)
        rc, out = t.quit()
    finally:
        module.stop()
    assert rc == 0, out[-3000:]
    assert "SELECT: drag to select" in out, out[-3000:]
    assert "PAIRING REQUEST, code 654321: Enter accepts, Esc rejects" in out, out[-3000:]
    assert "PASSKEY REQUEST: type the 6 digits" in out, out[-3000:]
    assert "AT+UBTUC=C0FFEE001122p,1" in module.log, module.log     # Enter accepted
    assert "AT+UBTUPE=C0FFEE001122p,0" in module.log, module.log    # Esc denied
    assert "snake: score 0" in out, out[-3000:]
