"""Matter: the ucxmatter plug-in (ucxapp --matter --build) on a real module; a second module looks for its
commissioning advertisement over the air."""

import os
import queue
import re
import subprocess
import threading
import time
from pathlib import Path

import pytest
from hwutil import _args, _reboot, _run, _stop


@pytest.fixture
def plugin(app_exe):
    path = Path(app_exe).parent / ("ucxmatter.dll" if os.name == "nt" else "libucxmatter.so")
    if not path.exists():
        pytest.skip("the Matter plug-in is not built (ucxapp --matter --build)")
    return path


@pytest.mark.two_devices
def test_matter_node_is_found_by_a_second_module(app_exe, port_a, port_b, plugin):
    _reboot(app_exe, port_a, port_b)
    address = re.search(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}", _run(app_exe, port_a, "bt.addr").stdout, re.M | re.I).group(0)
    low12 = int(address.replace(":", "")[-4:], 16) & 0x0FFF
    node = subprocess.Popen(_args(app_exe, port_a, "matter.start") + ["--shell"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            env=dict(os.environ, UCX_MATTER_PLUGIN=str(plugin)))
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(line) for line in node.stdout], daemon=True).start()
    seen = ""
    deadline = time.time() + 60
    try:
        while "matter: node started" not in seen and time.time() < deadline and node.poll() is None:
            try:
                seen += lines.get(timeout=1)
            except queue.Empty:
                pass
        assert "matter: node started (Matter SDK v" in seen, seen
        assert f"discriminator {low12}" in seen and "QR payload: MT:" in seen, seen
        # only advertisers with the Matter service data (AD type 0x16, UUID FFF6) pass the filter
        out = _run(app_exe, port_b, "bt.filter clear", "bt.filter 0 0x16 F6FF", "bt.scan full filter")
        assert out.returncode == 0 and address.upper() in out.stdout.upper(), out.stdout
    finally:
        _stop(node)
        _run(app_exe, port_b, "bt.filter clear")
    out = _run(app_exe, port_a, "bt.adv")
    assert re.search(r"^Advertising:   off", out.stdout, re.M), out.stdout   # the node took its advertisement down
