"""Helpers shared by the hardware tests (real NORA-W36 modules; a second module is the peer, never the PC)."""

import os
import queue
import re
import subprocess
import threading
import time
from pathlib import Path

import pytest

PROFILES = Path(__file__).resolve().parents[2] / "storage" / "ucxapp.wifi"
DEVICE_NAME = "u-blox HID"
GATT_NAME = "ucxgatt"
PING_RE = re.compile(r"sent (\d+), received (\d+), average (\d+) ms")
PING_MAX_AVG_MS = 1000
SOURCES = [pytest.param("gnss", marks=pytest.mark.gnss), pytest.param("imu", marks=pytest.mark.imu)]


def _args(exe, port, *commands):
    args = [str(exe), port]
    for command in commands:
        args += ["--exec", command]
    return args


def _run(exe, port, *commands, timeout=120, cwd=None):
    return subprocess.run(_args(exe, port, *commands), capture_output=True, text=True, timeout=timeout,
                          errors="replace", cwd=cwd)


def _start_server(exe, port):
    return subprocess.Popen(_args(exe, port, "hid.setup", "gatt.listen 40"), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def _wifi_connect_command(cwd=None):
    """UCX_WIFI_SSID/_PASS when set, else the saved profiles in storage/ucxapp.wifi (last used first)."""
    ssid = os.environ.get("UCX_WIFI_SSID")
    if ssid:
        return f"wifi.connect {ssid} {os.environ.get('UCX_WIFI_PASS', '')}"
    if not PROFILES.is_file() or not PROFILES.read_text(errors="replace").strip():
        pytest.skip("set UCX_WIFI_SSID/UCX_WIFI_PASS or connect once with wifi.connect <ssid> <password>")
    if cwd is not None:
        # A test running in its own folder gets a copy of the saved profiles.
        (Path(cwd) / "storage").mkdir(exist_ok=True)
        for f in PROFILES.parent.glob("ucxapp.wifi*"):
            (Path(cwd) / "storage" / f.name).write_bytes(f.read_bytes())
    return "wifi.profile.auto"


def _join_wifi(exe, port, connect):
    """Joins the Wi-Fi and returns (module ip, gateway) from the link details."""
    out = _run(exe, port, connect, "wifi.status", timeout=180)
    assert out.returncode == 0, out.stdout
    ip, gateway = re.search(r"ip=(\S+)", out.stdout), re.search(r"gateway (\S+)", out.stdout)
    assert ip and gateway, out.stdout
    return ip.group(1), gateway.group(1)


def _assert_ping(out, target):
    match = PING_RE.search(out.stdout)
    assert match, f"ping {target}: {out.stdout}"
    sent, received, avg = map(int, match.groups())
    assert received == sent, f"ping {target} lost packets: {out.stdout}"
    assert avg < PING_MAX_AVG_MS, f"ping {target} too slow: {out.stdout}"


def _close_listeners(exe, port):
    """Close TCP listeners left on the module by an earlier run, they keep the bridge port bound."""
    handles = re.findall(r"socket (\d+): TCP, listening", _run(exe, port, "sock.list").stdout)
    if handles:
        _run(exe, port, *[f"sock.close {handle}" for handle in handles])


def _reset_module(exe, port):
    """Close sockets, BLE links and Wi-Fi, reboot, and check the module comes back with no sockets or links."""
    socks = re.findall(r"socket (\d+):", _run(exe, port, "sock.list").stdout)
    conns = re.findall(r"connection (\d+):", _run(exe, port, "bt.list").stdout)
    _run(exe, port, *[f"sock.close {h}" for h in socks], *[f"bt.disconnect {h}" for h in conns], "wifi.disconnect")
    out = _run(exe, port, "device.reboot", timeout=60)
    assert "module rebooted" in out.stdout, f"reset of {port} failed:\n{out.stdout}"
    time.sleep(5)
    out = _run(exe, port, "sock.list", "bt.list")
    clean = "0 socket(s)" in out.stdout and "0 connection(s)" in out.stdout
    assert clean, f"{port} not clean after reset:\n{out.stdout}"


def _start_bridge(exe, port, source, found, setup, bridge, cwd=None):
    """Module A runs the source bridge in a shell; returns (process, line queue)."""
    _close_listeners(exe, port)
    args = _args(exe, port, *setup, f"{source}.open {found[0]} {found[1]}".strip(), bridge) + ["--shell"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", cwd=cwd)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    seen = []
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            line = lines.get(timeout=1)
        except queue.Empty:
            continue
        seen.append(line)
        if "bridge:" in line:
            return proc, seen
        if line.startswith("ERROR:"):
            _stop(proc)
            pytest.fail(f"{source} bridge failed to start: {''.join(seen)[-400:]}")
        if f"no {source.upper()} found" in line or "no receiver" in line or "no IMU" in line:
            break
    _stop(proc)
    pytest.skip(f"{source} not available for the bridge: {''.join(seen)[-400:]}")


def _stop(proc):
    try:
        proc.stdin.write("exit\n")
        proc.stdin.flush()
        proc.wait(timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        proc.kill()


def _check_stream(source, text):
    if source == "gnss":
        assert "$G" in text, text
    else:
        assert re.search(r"received [1-9]\d* bytes|\[NUS RX\]", text), text


def _background(exe, port, *commands, ready, timeout=60, cwd=None):
    """Runs commands on a module in a child app until `ready` is printed; returns (process, seen lines)."""
    proc = subprocess.Popen(_args(exe, port, *commands), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace", cwd=cwd)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in proc.stdout], daemon=True).start()
    seen = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = lines.get(timeout=1)
        except queue.Empty:
            if proc.poll() is not None:
                break
            continue
        seen.append(line)
        if re.search(ready, line):
            return proc, seen, lines
    proc.kill()
    pytest.fail(f"'{ready}' not seen: {''.join(seen)[-400:]}")


def _finish(proc, seen, lines, timeout=90):
    """Waits for the background app to end and returns everything it printed."""
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
    time.sleep(0.5)
    while not lines.empty():
        seen.append(lines.get())
    return "".join(seen)


def _gatt_server(exe, port, key, seconds=40):
    """A fresh module A serving one example service under GATT_NAME."""
    _run(exe, port, "device.reboot")
    time.sleep(5)
    return _background(exe, port, f'at +UBTLN="{GATT_NAME}"', f"gatt.server.{key} {seconds}",
                       ready=r"^(serving|simulating) ")
