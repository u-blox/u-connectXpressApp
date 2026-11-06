"""Cellular modem (UCX_CELL_PORT, e.g. the SARA-R5 of an XPLR-IOT-1): status, data and the bridges over it.

The modem gets a carrier NAT address, so nothing can connect to it: the stream bridges connect out through
cell.connect to a public TCP echo server (UCX_CELL_ECHO, "<host> <port>") and the test checks that the bytes
sent come back in. Every test ends the data link (cell.disconnect), so the next one finds the modem on AT.
"""

import os
import queue
import re
import socket
import subprocess
import threading
import time

import pytest
from hwutil import _wifi_connect_command

ECHO = os.environ.get("UCX_CELL_ECHO", "tcpbin.com 4242")


def _open(cell):
    return f"cell.open {cell[0]} {cell[1]}".strip()


def _cell(exe, cell, *commands, timeout=300):
    args = [str(exe)]
    for command in (_open(cell), *commands):
        args += ["--exec", command]
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout, errors="replace",
                              stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as e:
        partial = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"no result after {timeout} s running {list(commands)}\n{partial[-2000:]}", pytrace=False)


class _Shell:
    """The app runs the commands (--exec) and stays in --shell; lines are collected in the background."""

    def __init__(self, exe, port, *commands):
        args = [str(exe)] + ([port] if port else [])
        for command in commands:
            args += ["--exec", command]
        self.proc = subprocess.Popen([*args, "--shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, errors="replace")
        self.lines = queue.Queue()
        self.seen = []
        self.reader = threading.Thread(target=lambda: [self.lines.put(line) for line in self.proc.stdout], daemon=True)
        self.reader.start()

    def send(self, *commands):
        self.proc.stdin.write("".join(f"{c}\n" for c in commands))
        self.proc.stdin.flush()

    def wait(self, pattern, timeout):
        """The first match of pattern in a new line; fails on an ERROR line or the timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=1)
            except queue.Empty:
                if self.proc.poll() is not None:
                    break
                continue
            self.seen.append(line)
            m = re.search(pattern, line)
            if m:
                return m
            if "ERROR" in line:
                break
        pytest.fail(f"'{pattern}' not seen:\n{self.text[-3000:]}", pytrace=False)

    @property
    def text(self):
        return "".join(self.seen)

    def close(self, *commands):
        """Runs the last commands, then exit; returns everything printed."""
        try:
            self.send(*commands, "exit")
            self.proc.stdin.close()
            self.proc.wait(timeout=60)
        except (OSError, ValueError, subprocess.TimeoutExpired):
            self.proc.kill()
        self.reader.join(timeout=10)
        while not self.lines.empty():
            self.seen.append(self.lines.get())
        return self.text

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)


@pytest.mark.cell
def test_cell_reports_modem_sim_and_network(app_exe, cell):
    out = _cell(app_exe, cell, "cell.info", "cell.signal", "cell.radio", "cell.operator", "cell.status")
    text = out.stdout
    assert out.returncode == 0, text
    assert re.search(r"^IMEI:\s+\d{15}$", text, re.M), text
    assert re.search(r"^ICCID:\s+\d{18,22}F?$", text, re.M), "no SIM? " + text
    assert re.search(r"^RSSI:\s+-\d+ dBm|^RSRP:\s+-\d+ dBm", text, re.M), "no signal: " + text
    assert "radio: on" in text, text
    assert re.search(r"^Network:\s+registered", text, re.M), "not registered (SIM active?): " + text


@pytest.mark.cell
def test_cell_attach_and_ping(app_exe, cell):
    out = _cell(app_exe, cell, "cell.attach on", "cell.ping 8.8.8.8")
    assert out.returncode == 0, out.stdout
    assert re.search(r"attached, IP \S+", out.stdout), out.stdout
    assert re.search(r"^\d sent, [1-4] received, average \d+ ms$", out.stdout, re.M), out.stdout


SOURCES = [pytest.param("gnss", marks=pytest.mark.gnss), pytest.param("imu", marks=pytest.mark.imu),
           pytest.param("can")]


@pytest.mark.cell
@pytest.mark.parametrize("source", SOURCES)
def test_bridge_over_cell_round_trip(app_exe, cell, source, request):
    """The source goes out over cellular to the echo server and the same bytes come back into the bridge."""
    if source == "can":
        setup = ["can.open"]
    else:
        found = request.getfixturevalue(source)
        setup = [f"{source}.open {found[0]} {found[1]}".strip()]
    shell = _Shell(app_exe, None, _open(cell), "cell.attach on", f"cell.connect {ECHO}", *setup,
                   f"{source}.bridge cell")
    try:
        shell.wait(r"bridge: .*(<->|cellular)", 300)
        if source == "can":
            # 0a is a newline: tcpbin.com echoes line by line, and a binary record has no other line end
            shell.send(*[f"can.send 0x12{i} 11 22 33 0a" for i in range(5)])
        time.sleep(15)
        text = shell.close(f"{source}.bridge status", f"{source}.bridge off", "cell.disconnect")
    finally:
        shell.stop()
    m = re.search(r"(\d+) bytes out, (\d+) in", text)
    assert m, text[-3000:]
    sent, received = int(m.group(1)), int(m.group(2))
    assert sent > 0, f"nothing went out over cellular:\n{text[-3000:]}"
    assert received > 0, f"nothing came back from the echo server {ECHO}:\n{text[-3000:]}"
    assert "ERROR" not in text, text[-3000:]


@pytest.mark.cell
@pytest.mark.hardware
@pytest.mark.wifi
def test_cell_at_console_over_a_tcp_bridge(app_exe, port_a, cell):
    """The NORA module serves the modem's AT console on TCP; a PC socket on the same Wi-Fi asks for the maker."""
    shell = _Shell(app_exe, port_a, _wifi_connect_command(), _open(cell), "cell.bridge tcp 2949")
    try:
        m = shell.wait(r"connect a TCP client to ([\d.]+):2949", 120)
        with socket.create_connection((m.group(1), 2949), timeout=20) as s:
            got = b""
            deadline = time.time() + 20
            s.sendall(b"AT+CGMI\r\n")
            while time.time() < deadline and b"OK" not in got:
                try:
                    got += s.recv(256)
                except socket.timeout:
                    break
        text = shell.close("cell.bridge status", "cell.bridge off")
    finally:
        shell.stop()
    assert b"OK" in got, f"{got!r}\n{text[-3000:]}"
    assert re.search(rb"u-blox|Quectel|SIMCOM|Telit|Nordic", got, re.I), got


@pytest.mark.cell
@pytest.mark.hardware
@pytest.mark.wifi
def test_cell_relays_a_wifi_client_to_the_internet(app_exe, port_a, cell):
    """Wi-Fi to cellular relay: a PC socket to the NORA module's TCP server reaches the echo server over the modem."""
    shell = _Shell(app_exe, port_a, _wifi_connect_command(), _open(cell), "cell.attach on", f"cell.connect {ECHO}",
                   "cell.bridge tcp 2950")
    try:
        m = shell.wait(r"connect a TCP client to ([\d.]+):2950", 300)
        shell.wait(r"relaying the data link to ", 20)
        line = f"ucxapp relay {time.time():.0f}\n".encode()
        got = b""
        with socket.create_connection((m.group(1), 2950), timeout=20) as s:
            time.sleep(1)
            s.sendall(line)
            deadline = time.time() + 30
            while time.time() < deadline and line not in got:
                try:
                    got += s.recv(256)
                except socket.timeout:
                    break
        text = shell.close("cell.bridge status", "cell.bridge off", "cell.status", "cell.disconnect")
    finally:
        shell.stop()
    assert line in got, f"{got!r}\n{text[-3000:]}"
    assert "the data link to " in text and "stays up" in text, text[-3000:]
    assert "disconnected, command mode on" in text, text[-3000:]

@pytest.mark.cell
def test_cell_power_cycles_the_sara_r5_of_an_xplr_iot_1(app_exe, cell):
    """cell.power through the NORA-B106 shell; last, since the modem registers again afterwards."""
    args = [str(app_exe), "--exec", "cell.power cycle", "--exec", "cell.power on"]
    out = subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", stdin=subprocess.DEVNULL)
    if "no XPLR-IOT-1 shell" in out.stdout:
        pytest.skip("the modem is not on an XPLR-IOT-1")
    assert out.returncode == 0, out.stdout
    assert out.stdout.count(f"SARA-R5 on, answering on {cell[0]} ") == 2, out.stdout
    assert "power cycling the SARA-R5" in out.stdout, out.stdout
