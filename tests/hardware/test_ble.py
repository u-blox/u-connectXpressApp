"""Bluetooth LE: scan, profiles, bonding settings, HID and GATT server/client between the two modules."""

import queue
import re
import subprocess
import threading
import time

import pytest
from hwutil import DEVICE_NAME, GATT_NAME, _args, _finish, _gatt_server, _run, _start_server, _stop


@pytest.mark.hardware
def test_ble_scan_completes(app_exe, port_a):
    out = _run(app_exe, port_a, "bt.scan")
    assert out.returncode == 0, out.stdout


@pytest.mark.hardware
def test_ble_profile_auto_falls_back_to_scan(app_exe, port_a, tmp_path):
    (tmp_path / "storage").mkdir()
    (tmp_path / "storage" / "ucxapp.blelast").write_text("ghost")
    out = subprocess.run(
        _args(app_exe, port_a, "bt.profile.add ghost 11:22:33:44:55:66", "bt.profile.list", "bt.profile.auto"),
        capture_output=True, text=True, timeout=180, errors="replace", cwd=tmp_path)
    assert "ghost: 11:22:33:44:55:66" in out.stdout, out.stdout
    assert "Trying last used profile 'ghost'" in out.stdout, out.stdout
    assert "Scanning for known devices" in out.stdout, out.stdout
    assert "no known device found" in out.stdout, out.stdout
    assert out.returncode != 0
    out = _run(app_exe, port_a, "bt.profile.remove ghost")
    assert "no profile 'ghost'" in out.stdout or out.returncode == 0


@pytest.mark.hardware
def test_bt_settings_and_bonds(app_exe, port_a):
    out = _run(app_exe, port_a, "bt.defaults", "bt.pairing on", "bt.iocap none", "bt.security none",
               "bt.autoconfirm on", "bt.bonds", "bt.list")
    assert out.returncode == 0, out.stdout


@pytest.mark.hardware
def test_hid_and_gatt_server_definitions(app_exe, port_a):
    out = _run(app_exe, port_a, "hid.setup", "hid.status", "gatt.server.list")
    assert out.returncode == 0, out.stdout


@pytest.mark.hardware
def test_gatt_server_example_runs_and_stops(app_exe, port_a):
    out = _run(app_exe, port_a, "gatt.server.nus 3", "device.reboot")
    assert out.returncode == 0, out.stdout


@pytest.mark.two_devices
def test_ble_module_b_discovers_module_a(app_exe, port_a, port_b):
    server = _start_server(app_exe, port_a)
    try:
        out = _run(app_exe, port_b, "gatt.listen 5", "bt.scan")
        assert DEVICE_NAME in out.stdout, out.stdout
    finally:
        server.terminate()
        server.wait(timeout=30)


@pytest.mark.two_devices
@pytest.mark.parametrize("key", ["hrs", "cts", "ess", "lns", "aio", "bas", "sps", "wps", "ras"])
def test_gatt_example_server_and_client_between_two_modules(app_exe, port_a, port_b, key):
    """A serves the example service; B discovers it, runs the matching client example and listens."""
    proc, seen, lines = _gatt_server(app_exe, port_a, key)
    extra = ["gatt.wps status"] if key == "wps" else ["gatt.client.dis"]
    try:
        out = _run(app_exe, port_b, "bt.scan", f"bt.connect {GATT_NAME}", "gatt.services", "gatt.chars",
                   f"gatt.client.{key}", *extra, "gatt.listen 5", "bt.disconnect", timeout=240)
    finally:
        _finish(proc, seen, lines)
    assert out.returncode == 0, out.stdout


@pytest.mark.two_devices
def test_gatt_read_write_notify_between_two_modules(app_exe, port_a, port_b):
    """A serves SPS from a shell and pushes values; B reads, writes, sends and grants credits."""
    _run(app_exe, port_a, "device.reboot")
    time.sleep(5)
    server = subprocess.Popen(_args(app_exe, port_a, f'at +UBTLN="{GATT_NAME}"', "gatt.server.sps",
                                    "gatt.server.bas") + ["--shell"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in server.stdout], daemon=True).start()
    client = subprocess.Popen(_args(app_exe, port_b, "bt.scan", f"bt.connect {GATT_NAME}", "gatt.client.sps",
                                    "gatt.client.bas", "gatt.send 0 sps ucx-sps", "gatt.credits 0 10",
                                    "gatt.listen 60", "bt.disconnect"),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    seen = ""
    deadline = time.time() + 90
    while "handle=31" not in seen and time.time() < deadline:  # B subscribes to bas.level
        try:
            seen += lines.get(timeout=1)
        except queue.Empty:
            pass
    try:
        for command in ("gatt.server.set bas.level 42", "gatt.server.notify 0 bas.level 2a",
                        "gatt.server.notify 0 sps.fifo text:ucx-ind", "gatt.server.servicechanged 0 1 65535",
                        "gatt.server.demo 0 bas 2"):
            server.stdin.write(command + "\n")
            server.stdin.flush()
            time.sleep(1)
        out, _ = client.communicate(timeout=120)
    finally:
        _stop(server)
        client.kill()
    text = seen
    while not lines.empty():
        text += lines.get()
    assert client.returncode == 0, out
    assert "ucx-sps" in text, f"server:\n{text}\nclient:\n{out}"      # B -> A write
    assert "ucx-ind" in out, f"server:\n{text}\nclient:\n{out}"        # A -> B indication


@pytest.mark.two_devices
def test_gatt_server_indication_between_two_modules(app_exe, port_a, port_b):
    """B enables indications on A's SPS FIFO; A sends one and B receives it."""
    _run(app_exe, port_a, "device.reboot")
    time.sleep(5)
    server = subprocess.Popen(_args(app_exe, port_a, f'at +UBTLN="{GATT_NAME}"', "gatt.server.sps",
                                    "gatt.server.list") + ["--shell"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace")
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in server.stdout], daemon=True).start()
    client = None
    seen = ""

    def wait_for(pattern, seconds):
        nonlocal seen
        deadline = time.time() + seconds
        while not re.search(pattern, seen) and time.time() < deadline:
            try:
                seen += lines.get(timeout=1)
            except queue.Empty:
                pass
        return re.search(pattern, seen)

    try:
        fifo = wait_for(r"sps\.fifo\s+value handle (\d+)", 60)
        assert fifo, seen
        client = subprocess.Popen(_args(app_exe, port_b, "bt.scan", f"bt.connect {GATT_NAME}",
                                        f"gatt.subscribe 0 {fifo.group(1)} indicate", "gatt.listen 30",
                                        "bt.disconnect"),
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        client_lines = queue.Queue()
        threading.Thread(target=lambda: [client_lines.put(line) for line in client.stdout], daemon=True).start()
        out = ""
        deadline = time.time() + 120
        while "indications enabled" not in out and client.poll() is None and time.time() < deadline:
            try:
                out += client_lines.get(timeout=1)
            except queue.Empty:
                pass
        assert "indications enabled" in out, out
        server.stdin.write("gatt.server.indicate 0 sps.fifo text:ucx-indication\n")
        server.stdin.flush()
        client.wait(timeout=120)
        time.sleep(0.5)
        while not client_lines.empty():
            out += client_lines.get()
    finally:
        _stop(server)
        if client:
            client.kill()
    assert client.returncode == 0, out
    assert "ucx-indication" in out or "7563782d696e6469636174696f6e" in out.lower(), f"server:\n{seen}\nclient:\n{out}"


@pytest.mark.two_devices
def test_gatt_raw_handles_between_two_modules(app_exe, port_a, port_b):
    """B uses the raw handle commands on A's Battery service."""
    proc, seen, lines = _gatt_server(app_exe, port_a, "bas")
    try:
        chars = _run(app_exe, port_b, "bt.scan", f"bt.connect {GATT_NAME}", "gatt.chars", timeout=240)
        handle = re.search(r"handle\s+(0x[0-9a-f]+|\d+)\s+\S*\s+0x2a19\b", chars.stdout, re.I)
        assert handle, chars.stdout
        h = int(handle.group(1), 0)
        out = _run(app_exe, port_b, "gatt.chars", f"gatt.read {h}", f"gatt.subscribe 0 {h} notify", "gatt.listen 3",
                   f"gatt.subscribe 0 {h} off")
        # Battery Level is read/notify only, so the server must refuse a write with an ATT error.
        write = _run(app_exe, port_b, f"gatt.write {h} 01")
        write_long = _run(app_exe, port_b, f"gatt.writelong {h} 01")
        _run(app_exe, port_b, "bt.disconnect")
    finally:
        _finish(proc, seen, lines)
    assert out.returncode == 0, out.stdout
    assert re.search(r"\b(100|level)\b", out.stdout, re.I), out.stdout
    for w in (write, write_long):
        assert w.returncode == 1 and "failed" in w.stdout, w.stdout
