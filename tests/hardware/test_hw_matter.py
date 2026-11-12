"""Matter: the ucxmatter plug-in (ucxapp --matter --build) on a real module; a second module looks for its
commissioning advertisement over the air and for the Matter service on its GATT server."""

import os
import queue
import re
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest
from harness.matter import (
    PAIRING_EXCHANGE,
    PAIRING_NODE,
    PBKDF_PARAM_RESPONSE,
    at_burst,
    codes_allowed,
    manual_decode,
    pairing_request,
    unsecured_message,
)
from hwutil import _args, _finish, _has_wifi, _reboot, _run, _stop, _wifi_connect_command


@pytest.fixture
def plugin(app_exe):
    path = Path(app_exe).parent / ("ucxmatter.dll" if os.name == "nt" else "libucxmatter.so")
    if not path.exists():
        pytest.skip("the Matter plug-in is not built (ucxapp --matter --build)")
    return path


def _start_node(app_exe, port, plugin, cwd, *before):
    """An app that runs `before`, starts the node and stays in its shell: (process, what it printed so far, the
    lines still to come). It runs in the test's own folder: a node that was never commissioned, whatever the app's
    own store holds."""
    node = subprocess.Popen(_args(app_exe, port, *before, "matter.start") + ["--shell"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", cwd=cwd,
                            env=dict(os.environ, UCX_MATTER_PLUGIN=str(plugin)))
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in node.stdout], daemon=True).start()
    seen = ""
    deadline = time.time() + 240
    while "matter: node started" not in seen and time.time() < deadline and node.poll() is None:
        try:
            seen += lines.get(timeout=1)
        except queue.Empty:
            pass
    if "matter: node started (Matter SDK v" not in seen:
        _stop(node)
        pytest.fail(f"the node did not start: {seen[-1500:]}", pytrace=False)
    return node, seen, lines


@pytest.mark.two_devices
def test_matter_node_is_found_by_a_second_module(app_exe, port_a, port_b, plugin, tmp_path):
    _reboot(app_exe, port_a, port_b)
    address = re.search(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}", _run(app_exe, port_a, "bt.addr").stdout, re.M | re.I).group(0)
    node, seen, _ = _start_node(app_exe, port_a, plugin, tmp_path)
    try:
        # a new node draws its own codes: the log has the discriminator, the manual code its top bits and the passcode
        discriminator = int(re.search(r"discriminator (\d+)", seen)[1])
        manual = re.search(r"manual code: ([\d-]{13})\n", seen)
        assert manual and "QR payload: MT:" in seen, seen
        short, passcode = manual_decode(manual[1])
        assert short == discriminator >> 8 and codes_allowed(discriminator, passcode), seen
        # only advertisers with the Matter service data (AD type 0x16, UUID FFF6) pass the filter
        out = _run(app_exe, port_b, "bt.filter clear", "bt.filter 0 0x16 F6FF", "bt.scan full filter")
        assert out.returncode == 0 and address.upper() in out.stdout.upper(), out.stdout
        # the Matter service on the node's GATT server: C1 for the commissioner's writes, C2 for its indications
        out = _run(app_exe, port_b, f"bt.connect {address}", "gatt.services", "gatt.chars", "bt.disconnect 0")
        found = out.stdout.upper().replace("-", "")
        assert out.returncode == 0 and "FFF6" in found, out.stdout
        assert "18EE2EF5263D4559959F4F9C429F9D11" in found and "18EE2EF5263D4559959F4F9C429F9D12" in found, out.stdout
        # a commissioner's first packet over the air: the BTP handshake request on C1 (version 4, ATT MTU 247,
        # window 5); the Matter server answers with an indication on C2
        handles = {m[2].replace("-", "").upper(): int(m[1], 16) for m in re.finditer(
            r"handle 0x([0-9A-F]{4}).*?\b([0-9A-F]{8}-?(?:[0-9A-F]{4}-?){3}[0-9A-F]{12})\b", out.stdout, re.I)}
        c1, c2 = handles["18EE2EF5263D4559959F4F9C429F9D11"], handles["18EE2EF5263D4559959F4F9C429F9D12"]
        out = _run(app_exe, port_b, f"bt.connect {address}", f"gatt.subscribe 0 {c2} indicate",
                   f"gatt.write 0 {c1} 656C04000000F70005", "gatt.listen 3", "bt.disconnect 0")
        # handshake response: version 4, then the fragment size and the window the node chose
        answer = re.search(rf"\[notify conn=0 handle=0x{c2:04X}\] (.*)", out.stdout)
        assert answer and re.search(r"\b656C04[0-9A-F]{6}\b", answer[1].upper()), out.stdout
    finally:
        _stop(node)
        _run(app_exe, port_b, "bt.filter clear")
    out = _run(app_exe, port_a, "bt.adv")
    assert re.search(r"^Advertising:   off", out.stdout, re.M), out.stdout   # the node took its advertisement down


def test_matter_at_bursts_have_no_gaps_on_the_uart(app_exe, port_a, plugin, tmp_path, monkeypatch):
    """The node's runs of AT commands on a real UART, beside the app's own commands: without the session lock
    u-connectMatter needed, no command may wait for the receive thread."""
    _reboot(app_exe, port_a)
    monkeypatch.setenv("UCX_MATTER_PLUGIN", str(plugin))
    out = _run(app_exe, port_a, "at.trace t.trace", "matter.start", *["at AT"] * 40, "matter.status",
               "at.trace.stop", cwd=tmp_path)
    assert out.returncode == 0 and "matter: running" in out.stdout, out.stdout
    trace = (tmp_path / "storage" / "t.trace").read_text()
    for first, last, count in (("AT+UBTPM=", "AT+UBTGSA", 7), ("AT+UBTADL=", "AT+UBTAL", 5)):
        seconds, gap, sent = at_burst(trace, first, last)
        assert len(sent) == count, sent
        assert seconds < 1.5 and gap < 0.5, (seconds, gap, trace)
    # the app asks every module for Thread, which a Wi-Fi module refuses; no other command may fail
    trace = re.sub(r"^\S+ TX AT\+UTHE\?\n\S+ RX ERROR.*\n", "", trace, flags=re.M)
    assert len(re.findall(r" TX AT$", trace, re.M)) >= 40 and " RX ERROR" not in trace, trace


def _pc_reaches(ip):
    """The PC is on the module's network: one ping gets an answer."""
    args = ["ping", "-n", "1", "-w", "2000", ip] if os.name == "nt" else ["ping", "-c", "1", "-W", "2", ip]
    out = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=30)
    return out.returncode == 0 and "ttl=" in out.stdout.lower()


def test_matter_pairing_request_over_wifi_is_answered(app_exe, port_a, plugin, tmp_path):
    """The node's UDP path through a real module: a client on the Wi-Fi (this PC) sends the first message of a
    pairing to the Matter port, and the Matter server's answer comes back from the module. The node's card shows
    the network the module is on."""
    if not _has_wifi(app_exe, port_a):
        pytest.skip("the module has no Wi-Fi")
    _reboot(app_exe, port_a)
    node, seen, lines = _start_node(app_exe, port_a, plugin, tmp_path, _wifi_connect_command(tmp_path), "wifi.status")
    try:
        ip = re.search(r"ip=(\d+\.\d+\.\d+\.\d+)", seen)
        assert ip, seen
        if not _pc_reaches(ip[1]):
            pytest.skip(f"this PC is not on the module's network (no ping answer from {ip[1]})")
        answers = []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(1)
            deadline = time.time() + 12
            while not answers and time.time() < deadline:
                client.sendto(pairing_request(), (ip[1], 5540))   # the same message again is a duplicate to the node
                until = time.time() + 3
                while time.time() < until:
                    try:
                        answers.append(unsecured_message(client.recvfrom(1500)[0]))
                    except socket.timeout:
                        pass
        assert (PAIRING_NODE, PBKDF_PARAM_RESPONSE, PAIRING_EXCHANGE) in answers, answers
        node.stdin.write("matter.card\n")
        node.stdin.flush()
    finally:
        _stop(node)
    card = _finish(node, [], lines)
    assert "NODE  NORA-W36 over Wi-Fi, 3 endpoint(s)\nSTATE not commissioned\n" in card, card
    network = rf"NET   Wi-Fi up, {re.escape(ip[1])}, channel \d+\nCODE  \d{{4}}-\d{{3}}-\d{{4}}\nQR    MT:"
    assert re.search(network, card), card
    assert "EP    1 light = off  [on | off | 0..100 (percent)]\n" in card and "LOG   info\n" in card, card
    out = _run(app_exe, port_a, "sock.list")
    assert "0 socket(s)" in out.stdout, out.stdout   # the node closed its socket on the module
