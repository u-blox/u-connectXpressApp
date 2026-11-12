"""matter.* with the real plug-in (ucxmatter, built from matter/ by "ucxapp --matter --build") against vucx, the
virtual module. The plug-in needs the Matter SDK, so these tests run only where it has been built."""

import os
import re
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.app


@pytest.fixture
def plugin(app_exe):
    path = Path(app_exe).parent / ("ucxmatter.dll" if os.name == "nt" else "libucxmatter.so")
    if not path.exists():
        pytest.skip("the Matter plug-in is not built (ucxapp --matter --build)")
    return path


@pytest.fixture
def module():
    pytest.importorskip("yaml")
    from harness.vucx import Vucx

    # Bluetooth address C0FFEE001144: discriminator 0x144
    vucx = Vucx("nora-w36.yaml")
    vucx.start()
    yield vucx
    vucx.stop()


def _run(exe, module, plugin, cwd, *commands):
    args = [str(exe), module.port]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace",
                          stdin=subprocess.DEVNULL, cwd=cwd, env=dict(os.environ, UCX_MATTER_PLUGIN=str(plugin)))


def test_start_advertises_the_matter_service_and_shows_the_setup_codes(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start", "matter.status")
    assert out.returncode == 0, out.stdout
    assert "matter: node started (Matter SDK v" in out.stdout, out.stdout
    assert re.search(r"node: NORA-\S+ over Wi-Fi, 3 endpoint\(s\)", out.stdout), out.stdout
    assert "discriminator 324, vendor 0xFFF1, product 0x8000, passcode 20202021" in out.stdout, out.stdout
    # service UUID FFF6, then its service data: commissionable, discriminator, vendor, product
    assert "AT+UBTADL=0303F6FF0B16F6FF004401F1FF008000" in module.log, module.log
    assert "AT+UBTAL" in module.log, module.log
    # from the SDK's own generators: short discriminator 1 and the passcode, then the check digit
    assert re.search(r"manual code: 0169-331-233\d\n", out.stdout), out.stdout
    assert re.search(r"QR payload: MT:[0-9A-Z.\-]{19}\n", out.stdout), out.stdout
    # leaving the app stops the advertising
    assert "AT+UBTALD" in module.log[module.log.index("AT+UBTAL"):], module.log


def test_endpoints_are_chosen_at_start_and_set_from_the_app(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start light light contact", "matter.set light2 40",
               "matter.set contact open", "matter.endpoints")
    assert out.returncode == 0, out.stdout
    assert re.search(r"1 +light +off +Dimmable Light", out.stdout), out.stdout
    assert re.search(r"2 +light2 +on 40% +Dimmable Light", out.stdout), out.stdout
    assert re.search(r"3 +contact +open +Contact Sensor", out.stdout), out.stdout
    assert "kinds: light temp contact" in out.stdout, out.stdout


def test_unknown_endpoint_does_not_start_or_advertise(app_exe, module, plugin, tmp_path):
    out = _run(app_exe, module, plugin, tmp_path, "matter.start fridge")
    assert out.returncode != 0, out.stdout
    assert "unknown endpoint fridge; this plug-in builds: light temp contact" in out.stdout, out.stdout
    assert "AT+UBTAL" not in module.log, module.log
