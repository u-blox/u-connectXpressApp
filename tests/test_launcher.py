"""ucxapp.py: when a host build is needed."""

import os

import ucxapp


def _tree(tmp_path):
    for rel in ("app/src/ucxapp_main.c", "app/CMakeLists.txt", "u-connectClient/src/u_cx_at_client.c",
                "u-connectClient/ports/uart/u_port_uart_linux.c",
                "u-connectClient/ports/extra/stm32h7/STM32CubeH7/hal.c", "app/bin/ucxapp"):
        f = tmp_path / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text("x")
        os.utime(f, (1000, 1000))
    exe = tmp_path / "app" / "bin" / "ucxapp"
    os.utime(exe, (2000, 2000))
    return exe


def test_a_u_connect_client_change_needs_a_rebuild(tmp_path, monkeypatch):
    monkeypatch.setattr(ucxapp, "ROOT", tmp_path)
    exe = _tree(tmp_path)
    assert not ucxapp.stale(exe)
    os.utime(tmp_path / "u-connectClient/ports/uart/u_port_uart_linux.c", (3000, 3000))
    assert ucxapp.stale(exe)


def test_vendor_trees_the_host_build_does_not_compile_are_ignored(tmp_path, monkeypatch):
    monkeypatch.setattr(ucxapp, "ROOT", tmp_path)
    exe = _tree(tmp_path)
    os.utime(tmp_path / "u-connectClient/ports/extra/stm32h7/STM32CubeH7/hal.c", (3000, 3000))
    assert not ucxapp.stale(exe)
