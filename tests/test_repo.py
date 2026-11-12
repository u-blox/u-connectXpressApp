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


def test_test_files_have_unique_basenames():
    """tests/ has no __init__.py, so pytest imports by basename: two test_x.py stop the whole collection."""
    seen = {}
    for path in sorted((ROOT / "tests").rglob("test_*.py")):
        assert path.name not in seen, f"{path} and {seen[path.name]} share a basename"
        seen[path.name] = path


# First major version of each action that runs on Node.js 24 (older ones print "Node.js 20 is deprecated").
NODE24_ACTIONS = {
    "actions/checkout": 5, "actions/setup-python": 6, "actions/cache": 5, "actions/upload-artifact": 5,
    "actions/download-artifact": 5, "actions/upload-pages-artifact": 5, "actions/deploy-pages": 5,
    "emscripten-core/setup-emsdk": 16,
}


def test_workflows_use_node24_actions():
    for wf in (ROOT / ".github" / "workflows").glob("*.yml"):
        for action, major in re.findall(r"uses:\s*([\w.-]+/[\w.-]+)@v(\d+)", wf.read_text(encoding="utf-8")):
            assert action in NODE24_ACTIONS, f"{wf.name}: {action} is not checked for its Node.js runtime"
            assert int(major) >= NODE24_ACTIONS[action], f"{wf.name}: {action}@v{major} runs on Node.js 20"


def test_ci_builds_what_the_release_ships():
    build = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
    release = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    refs = {re.search(r"ZEPHYR_REF: (\S+)", text).group(1) for text in (build, release)}
    assert len(refs) == 1, f"CI and release build different Zephyr versions: {refs}"
    assert "-rc" not in refs.pop(), "release a Zephyr release, not a release candidate"
    boards = [re.search(r"board: \[([^\]]+)\]", text).group(1) for text in (build, release)]
    assert boards[0] == boards[1]
    assert "build-wasm:" in build and "emcmake" in build and "tests/test_wasm.py" in build


def test_cmake_references_existing_submodule_files():
    cmake = (ROOT / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
    for rel in re.findall(r"\.\./(u-connectClient/[\w./-]+\.(?:cmake|c))\b", cmake):
        assert (ROOT / rel).is_file(), f"{rel} referenced by CMakeLists.txt is missing (submodule not initialised?)"


def test_cache_from_an_old_source_folder_is_detected(tmp_path):
    spec = importlib.util.spec_from_file_location("launcher", ROOT / "ucxapp.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    cache = tmp_path / "CMakeCache.txt"
    src = ROOT / "app" / "stm32_freertos"
    cache.write_text(f"CMAKE_CACHEFILE_DIR:INTERNAL={tmp_path.as_posix()}\n"
                     f"CMAKE_HOME_DIRECTORY:INTERNAL={(ROOT / 'app' / 'stm32').as_posix()}\n")
    assert mod.cache_moved(cache, src)
    cache.write_text(f"CMAKE_CACHEFILE_DIR:INTERNAL={tmp_path.as_posix()}\n"
                     f"CMAKE_HOME_DIRECTORY:INTERNAL={src.as_posix()}\n")
    assert not mod.cache_moved(cache, src)
    assert mod.cache_moved(cache)


def test_cache_made_by_another_cmake_is_detected(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("launcher", ROOT / "ucxapp.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    pinned = tmp_path / "pinned" / "cmake.exe"
    monkeypatch.setattr(mod.shutil, "which", lambda tool: str(pinned) if tool == "cmake" else None)
    cache = tmp_path / "CMakeCache.txt"
    head = (f"CMAKE_CACHEFILE_DIR:INTERNAL={tmp_path.as_posix()}\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={(ROOT / 'app').as_posix()}\n")
    # a system CMake made it; regenerating it with the pinned one mixes their modules and fails
    cache.write_text(head + "CMAKE_COMMAND:INTERNAL=C:/Program Files/CMake/bin/cmake.exe\n")
    assert mod.cache_moved(cache)
    cache.write_text(head + f"CMAKE_COMMAND:INTERNAL={pinned.as_posix()}\n")
    assert not mod.cache_moved(cache)


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


def test_every_stm32_board_has_a_zephyr_overlay():
    launcher = _launcher()
    boards = ROOT / "app" / "zephyr" / "boards"
    for key in launcher.BOARDS:
        name = launcher.zephyr_board(key)
        assert (boards / f"{name}.overlay").is_file(), f"--board {key}: app/zephyr/boards/{name}.overlay is missing"
        assert "ucxapp-module" in (boards / f"{name}.overlay").read_text(encoding="utf-8")


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
