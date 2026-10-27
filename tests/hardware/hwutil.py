"""Helpers shared by the hardware tests (real NORA modules; a second module is the peer, never the PC)."""

import functools
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
    try:
        out = subprocess.run(_args(exe, port, *commands), capture_output=True, text=True, timeout=timeout,
                             errors="replace", cwd=cwd, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired as e:
        partial = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        pytest.fail(f"no result from {port} after {timeout} s running {list(commands)}\n--- output so far ---\n"
                    f"{partial[-2000:]}", pytrace=False)
    if "does not answer AT commands" in out.stdout:
        pytest.exit(f"module on {port} does not answer AT commands: power-cycle it and run again\n{out.stdout[-300:]}",
                    returncode=3)
    return out


@functools.lru_cache(maxsize=None)
def _caps(exe, port):
    """dev.caps of the module as {'wi-fi': 'yes', 'bluetooth le': 'yes', ...}; empty if it does not answer."""
    out = subprocess.run(_args(exe, port, "dev.caps"), capture_output=True, text=True, timeout=60,
                         errors="replace", stdin=subprocess.DEVNULL).stdout
    return {k.strip().lower(): v.strip().lower() for k, v in re.findall(r"^([A-Za-z][\w -]*):\s+(\S+)\s*$", out, re.M)}


def _has_wifi(exe, port):
    """From dev.caps; a module that does not answer counts as Wi-Fi so its tests fail instead of hiding."""
    return _caps(str(exe), port).get("wi-fi", "yes") == "yes"


def _reboot(exe, *ports):
    """Reboots the modules in parallel; dev.reboot already waits for the startup event."""
    procs = [subprocess.Popen(_args(exe, port, "dev.reboot"), stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL) for port in ports]
    for proc in procs:
        proc.wait(timeout=60)


def _start_server(exe, port):
    """A freshly rebooted module A advertising the HID service (a second hid.setup needs a reboot)."""
    _reboot(exe, port)
    proc, _, _ = _background(exe, port, "hid.setup", "gatt.listen 40", ready=r"HID service active")
    return proc


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
    if not _has_wifi(exe, port):
        return
    handles = re.findall(r"socket (\d+): TCP, listening", _run(exe, port, "sock.list").stdout)
    if handles:
        _run(exe, port, *[f"sock.close {handle}" for handle in handles])


def _reset_module(exe, port):
    """Close sockets, BLE links and Wi-Fi, reboot, and check the module comes back with no sockets or links."""
    wifi = _has_wifi(exe, port)
    socks = re.findall(r"socket (\d+):", _run(exe, port, "sock.list").stdout) if wifi else []
    conns = re.findall(r"connection (\d+):", _run(exe, port, "bt.list").stdout)
    cleanup = [*[f"sock.close {h}" for h in socks], *[f"bt.disconnect {h}" for h in conns]]
    if cleanup or wifi:
        _run(exe, port, *cleanup, *(["wifi.disconnect"] if wifi else []))
    out = _run(exe, port, "dev.reboot", timeout=60)
    assert "module rebooted" in out.stdout, f"reset of {port} failed:\n{out.stdout}"
    out = _run(exe, port, *(["sock.list"] if wifi else []), "bt.list")
    clean = (not wifi or "0 socket(s)" in out.stdout) and "0 connection(s)" in out.stdout
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


def _finish(proc, seen, lines, timeout=90, stop=False):
    """Waits for the background app to end (or ends it at once with stop) and returns everything it printed."""
    if stop:
        proc.kill()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
    time.sleep(0.5)
    while not lines.empty():
        seen.append(lines.get())
    return "".join(seen)


def _gatt_server(exe, port, key, seconds=40, peer=None):
    """A fresh module A serving one example service under GATT_NAME; a peer is rebooted too so no old link,
    subscription or client state carries over from the previous test."""
    _reboot(exe, port, *([peer] if peer else []))
    return _background(exe, port, f'at +UBTLN="{GATT_NAME}"', f"gatt.server.{key} {seconds}",
                       ready=r"^(serving|simulating) ")
