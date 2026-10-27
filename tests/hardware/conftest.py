"""Fixtures for the hardware tests; the ports, GNSS and IMU fixtures come from tests/conftest.py."""

import os

import pytest
from hwutil import _has_wifi, _join_wifi, _reset_module, _run, _wifi_connect_command


@pytest.fixture(scope="session", autouse=True)
def reset_modules(app_exe):
    """Reset every module once before the run so no GATT server, socket, link or Wi-Fi survives an earlier session."""
    for port in filter(None, (os.environ.get(v) for v in ("UCX_PORT_A", "UCX_PORT_B"))):
        _reset_module(app_exe, port)


@pytest.fixture(scope="session")
def wifi_peers(app_exe, port_a, port_b):
    """Both modules joined to the Wi-Fi: {port: (ip, gateway)}."""
    connect = _wifi_connect_command()
    return {port: _join_wifi(app_exe, port, connect) for port in (port_a, port_b)}


@pytest.fixture(autouse=True)
def close_stale_sockets(request, app_exe):
    """Close sockets an earlier test left open, so a stale listener cannot take this test's connections."""
    if request.node.get_closest_marker("two_devices") is None:
        return
    for port in filter(None, (os.environ.get(v) for v in ("UCX_PORT_A", "UCX_PORT_B"))):
        if _has_wifi(app_exe, port):
            _run(app_exe, port, "sock.close all", timeout=30)
