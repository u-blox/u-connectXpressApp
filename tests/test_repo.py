"""Repository sanity checks that don't need a compiler or hardware."""

import importlib.util
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def test_submodule_is_u_connect_client():
    text = (ROOT / ".gitmodules").read_text(encoding="utf-8")
    assert "path = u-connectClient" in text
    assert "u-blox/u-connectClient.git" in text
    assert "ucxclient" not in text


def test_cmake_references_existing_submodule_files():
    cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
    for rel in re.findall(r"\.\./(u-connectClient/[\w./-]+\.(?:cmake|c))\b", cmake):
        assert (ROOT / rel).is_file(), f"{rel} referenced by CMakeLists.txt is missing (submodule not initialised?)"


def test_no_stale_ucxclient_paths_in_build_files():
    for rel in ("ucxapp.py", "app/CMakeLists.txt"):
        text = (ROOT / rel).read_text(encoding="utf-8")
        assert not re.search(r"(?<![\w-])ucxclient[\\/]", text), f"{rel} still references ucxclient/"


def test_cmd_files_use_crlf():
    for cmd in ROOT.glob("*.cmd"):
        data = cmd.read_bytes()
        assert b"\n" not in data.replace(b"\r\n", b""), f"{cmd.name} has LF-only line endings"


def _launcher():
    spec = importlib.util.spec_from_file_location("launcher", ROOT / "ucxapp.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_old_launchers_forward_to_the_python_launcher():
    assert "ucxapp.py" in (ROOT / "ucxapp.cmd").read_text(encoding="utf-8")
    assert "--stm32" in (ROOT / "ucxapp-stm32.cmd").read_text(encoding="utf-8")
    assert "ucxapp.py" in (ROOT / "ucxapp.sh").read_text(encoding="utf-8")


@pytest.mark.parametrize(
    "argv, flags",
    [
        (["--clean", "--build", "--run"], {"clean", "build", "run"}),
        (["rebuild", "debug"], {"rebuild", "debug"}),
        (["clean", "release"], {"clean"}),
        (["sign", "ABC123"], set()),
        (["--stm32", "--board", "f767", "--build", "--flash", "--log"], {"stm32", "build", "flash"}),
        (["--stm32-freertos", "--build"], {"stm32", "build"}),
        (["--stm32-zephyr", "--board", "h753", "--build", "--flash"], {"stm32_zephyr", "build", "flash"}),
    ],
)
def test_launcher_flags(argv, flags):
    args, extra, _ = _launcher().parse(argv)
    on = {k for k, v in vars(args).items() if v is True}
    assert on == flags
    assert extra == []


def test_launcher_passes_unknown_and_raw_args_to_the_app():
    args, extra, _ = _launcher().parse(["COM13", "--exec", "dev.info", "--", "--build"])
    assert not args.build
    assert extra == ["COM13", "--exec", "dev.info", "--build"]


@pytest.mark.parametrize(
    "vid, pid, desc, role",
    [
        (0x1546, 0x01A9, "u-blox GNSS receiver", "GNSS"),
        (None, None, "u-blox EVK-F9P UART1 (COM6)", "GNSS"),
        (0x0403, 0x6010, "USB Serial Port", "EVK (FTDI)"),
        (None, None, "USB-SERIAL CH340 (COM15)", "USB-UART (IMU/GNSS?)"),
        (None, None, "STMicroelectronics STLink Virtual COM Port (COM9)", "STM32 console"),
        (None, None, "Intel(R) Active Management Technology - SOL (COM3)", "skip"),
    ],
)
def test_launcher_port_roles(vid, pid, desc, role):
    assert _launcher().classify(vid, pid, desc) == role


def test_launcher_module_ports_ignore_log_lines():
    out = "[00:00:00.000][DBG  ] Loaded FTDI DLL from: C:\\Temp\\ftd2xx64_embedded.dll\nCOM13\nCOM155\n"
    assert _launcher().module_ports(out) == ["COM13", "COM155"]
    assert _launcher().module_ports("/dev/ttyUSB0\n") == ["/dev/ttyUSB0"]
