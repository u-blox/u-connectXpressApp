"""fw.flash against vucx: the serial port it reopens for XMODEM must be the connected one, not a leftover of the
command line (the AT client kept a pointer to a buffer that the next --exec overwrote; needs a pty: Linux/WSL)."""

import subprocess
import sys

import pytest

pytestmark = [pytest.mark.app,
              pytest.mark.skipif(sys.platform == "win32", reason="vucx needs a pty (Linux/WSL)")]


@pytest.fixture
def nora():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    module = Vucx("nora-w36.yaml")
    module.start()
    yield module
    module.stop()


def test_flash_uses_the_connected_port(app_exe, nora, tmp_path):
    image = tmp_path / ("long-directory-name-" * 3) / "NORA-W36X-SW-9.9.9.bin"
    image.parent.mkdir()
    image.write_bytes(b"\xff" * 2048)
    proc = subprocess.Popen([str(app_exe), nora.port, "--exec", "dev.info", "--exec", f"fw.flash {image}"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    try:
        line = ""
        for line in proc.stdout:
            if line.startswith("flashing "):
                break
        assert line.startswith("flashing 2048 bytes via ")
        assert f" via {nora.port} at " in line
    finally:
        proc.kill()
        proc.wait()
