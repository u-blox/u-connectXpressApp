"""Shared fixtures for tests that run the real ucxapp executable.

The tests use whatever is attached: one NORA module runs the single-module tests, a second one adds the
two-module tests, and a GNSS receiver or IMU adds the tests and bridges that need it.

Environment (an empty value means "not attached", as set by `ucxapp --detect --test`):
  UCX_APP_EXE   path to the exe (default: app/bin/ucxapp.exe, app/bin/ucxapp on Linux)
  UCX_PORT_A    COM port of the first NORA module (default: first FTDI port found)
  UCX_PORT_B    COM port of the second NORA module (default: second FTDI port found)
Tests marked wifi skip when a module they use reports "Wi-Fi: no" in dev.caps (NORA-B26).
  UCX_GNSS_PORT, UCX_GNSS_BAUD   GNSS receiver (default: what gnss.detect finds)
  UCX_IMU_PORT, UCX_IMU_BAUD     IMU (default: what imu.detect finds)
"""

import concurrent.futures
import functools
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "app" / "bin" / ("ucxapp.exe" if sys.platform == "win32" else "ucxapp")
HARDWARE_MARKERS = ("hardware", "two_devices", "gnss", "imu")


FAILURE_LOG = ROOT / "build" / "test_failures.log"


def pytest_runtest_logreport(report):
    """Append every failed or errored test to build/test_failures.log, kept across runs to spot flaky tests."""
    if not report.failed:
        return
    import datetime

    message = report.longreprtext.strip().splitlines()
    reason = message[-1][:200] if message else ""
    FAILURE_LOG.parent.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().isoformat(timespec="seconds")
    # The full report (with the app output) goes next to the one-line log, one file per failure.
    details = FAILURE_LOG.parent / "test_failures"
    details.mkdir(exist_ok=True)
    name = re.sub(r"[^\w.-]+", "_", report.nodeid.split("::", 1)[-1])
    (details / f"{stamp.replace(':', '')}_{name}.txt").write_text(report.longreprtext, encoding="utf-8")
    with FAILURE_LOG.open("a", encoding="utf-8") as log:
        log.write(f"{stamp}\t{report.when}\t{report.nodeid}\t{reason}\n")


def _exe():
    return Path(os.environ.get("UCX_APP_EXE", DEFAULT_EXE))


@functools.lru_cache(maxsize=1)
def _detected():
    if sys.platform != "win32":
        return []
    sys.path.insert(0, str(ROOT / "tests"))
    from harness.portenum import find_at_ports

    return find_at_ports(_exe())


@functools.lru_cache(maxsize=1)
def _ports():
    if "UCX_PORT_A" in os.environ:  # set, even empty: never probe the serial ports (probing can block)
        return os.environ["UCX_PORT_A"] or None, os.environ.get("UCX_PORT_B") or None
    found = _detected()
    return (found[0] if found else None), (found[1] if len(found) > 1 else None)


@functools.lru_cache(maxsize=None)
def _found(name):
    """(port, baud, details) of the GNSS receiver or IMU, or None; details is empty when set by UCX_*."""
    key = f"UCX_{name.upper()}_PORT"
    if key in os.environ:
        port = os.environ[key]
        return (port, os.environ.get(f"UCX_{name.upper()}_BAUD", ""), "") if port else None
    sys.path.insert(0, str(ROOT))
    import ucxapp

    return (ucxapp.find_gnss if name == "gnss" else ucxapp.find_imu)(_exe())


def _source(name):
    """(port, baud) of the GNSS receiver or IMU, or None."""
    found = _found(name)
    return found[:2] if found else None


def _found_text(name):
    """'ZED-F9P on COM20 at 115200 baud' (model when the receiver reported one)."""
    found = _found(name)
    if not found:
        return "none"
    port, baud, details = found
    model = details
    if name == "gnss":  # "NMEA + UBX, ZED-F9P, PROTVER 27.12, FW HPG 1.32"
        parts = details.split(", ")
        model = parts[1] if len(parts) > 1 and not parts[1].startswith(("PROTVER", "FW")) else ""
    return f"{model + ' on ' if model else ''}{port} at {baud} baud"


@functools.lru_cache(maxsize=None)
def _describe(port):
    """'NORA-W36 (3.5.0) on COM155' from dev.info, or just the port if it does not answer."""
    try:
        out = subprocess.run([str(_exe()), port, "--exec", "dev.info"], capture_output=True, text=True,
                             timeout=30, errors="replace", stdin=subprocess.DEVNULL).stdout
    except (OSError, subprocess.TimeoutExpired):
        return port
    model = re.search(r"Model:\s*(\S+)", out)
    version = re.search(r"Firmware Version:\s*(\S+)", out)
    if not model:
        return port
    return f"{model.group(1)} ({version.group(1) if version else '?'}) on {port}"


def pytest_configure(config):
    for marker in (
        "app: runs the built ucxapp.exe (Windows only, no hardware needed)",
        "hardware: needs a NORA module on UCX_PORT_A",
        "two_devices: needs NORA modules on both UCX_PORT_A and UCX_PORT_B",
        "wifi: every module the test uses needs Wi-Fi (dev.caps); skipped on Bluetooth LE only modules",
        "gnss: needs a GNSS receiver (UCX_GNSS_PORT or gnss.detect)",
        "imu: needs an IMU (UCX_IMU_PORT or imu.detect)",
        "stress: long repeated runs; only with -m stress",
    ):
        config.addinivalue_line("markers", marker)


def _skip_reason(item, exe, hw):
    if item.get_closest_marker("stress") and "stress" not in (item.config.option.markexpr or ""):
        return "stress test: select with -m stress"
    needs_app = any(item.get_closest_marker(m) for m in ("app", *HARDWARE_MARKERS))
    if not needs_app:
        return None
    if sys.platform != "win32" and not (item.get_closest_marker("app") and exe.is_file()):
        return "needs Windows"
    if not exe.is_file():
        return f"app not built: {exe}"
    if (item.get_closest_marker("hardware") or item.get_closest_marker("two_devices")) and not hw["a"]:
        return "no NORA module found (set UCX_PORT_A)"
    if item.get_closest_marker("two_devices") and not hw["b"]:
        return "only one NORA module found (set UCX_PORT_B)"
    for name in ("gnss", "imu"):
        if item.get_closest_marker(name) and not hw[name]:
            return f"no {name.upper()} found (set UCX_{name.upper()}_PORT)"
    if item.get_closest_marker("wifi"):
        sys.path.insert(0, str(ROOT / "tests" / "hardware"))
        from hwutil import _has_wifi

        used = [hw["a"]] + ([hw["b"]] if "port_b" in getattr(item, "fixturenames", ()) else [])
        for port in filter(None, used):
            if not _has_wifi(exe, port):
                return f"no Wi-Fi on {port} (dev.caps)"
    return None


@pytest.hookimpl(trylast=True)  # after -k/-m deselection, so only the selected tests trigger detection
def pytest_collection_modifyitems(config, items):
    exe = _exe()
    wanted = {m for item in items for m in HARDWARE_MARKERS if item.get_closest_marker(m)}
    hw = {"a": None, "b": None, "gnss": None, "imu": None}
    if wanted and exe.is_file() and sys.platform == "win32":
        hw["a"], hw["b"] = _ports()
        for name in ("gnss", "imu"):
            if name in wanted:
                hw[name] = _source(name)

    run, skipped = 0, {}
    for item in items:
        reason = _skip_reason(item, exe, hw)
        if reason:
            item.add_marker(pytest.mark.skip(reason=reason))
            skipped[reason] = skipped.get(reason, 0) + 1
        elif any(item.get_closest_marker(m) for m in HARDWARE_MARKERS):
            run += 1

    reporter = config.pluginmanager.get_plugin("terminalreporter")
    if not wanted or reporter is None:
        return
    ports = [p for p in (hw["a"], hw["b"]) if p]
    with concurrent.futures.ThreadPoolExecutor() as pool:
        modules = ", ".join(pool.map(_describe, ports)) or "none"
    gnss = _found_text("gnss") if hw["gnss"] else "none"
    imu = _found_text("imu") if hw["imu"] else "none"
    reporter.write_line(f"Found: NORA {modules} | GNSS {gnss} | IMU {imu}", bold=True)
    reporter.write_line(f"Starting {run} hardware test(s) using what was found")
    sys.path.insert(0, str(ROOT))
    import ucxapp

    reporter.write_line("Wi-Fi for the tests:")
    for line in ucxapp.wifi_summary():
        reporter.write_line(f"  {line}")
    for reason, count in skipped.items():
        reporter.write_line(f"  skipping {count}: {reason}")


@pytest.fixture(scope="session")
def app_exe():
    return _exe()


@pytest.fixture(scope="session")
def port_a():
    return _ports()[0]


@pytest.fixture(scope="session")
def port_b():
    return _ports()[1]


@pytest.fixture(scope="session")
def gnss():
    return _source("gnss")


@pytest.fixture(scope="session")
def imu():
    return _source("imu")

