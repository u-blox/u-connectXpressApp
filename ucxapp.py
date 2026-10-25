#!/usr/bin/env python3
"""ucxapp, the u-connectXpressApp launcher: build, run, test and find hardware on Windows, Linux (also WSL)
and STM32.

  ucxapp                                    build if needed and start the app (this OS)
  ucxapp COM13 --exec "device.info"
  ucxapp --linux                            Linux build; on Windows it runs inside WSL
  ucxapp --stm32 --board f767 --build --flash --log
  ucxapp --detect                           find NORA modules, GNSS receivers and IMUs
  ucxapp --detect --test tests/hardware   hardware tests on what was found
  ucxapp --coverage                         line coverage of app/src from the offline tests (WSL on Windows)
  ucxapp --doctor                           first-run checks: tools, serial access, modules
  ucxapp --script examples/automation/smoke-test.ucx --junit report.xml

Arguments the launcher does not know go to the app (to pytest with --test); everything after "--" too.
Windows: ucxapp.cmd calls this script. Linux: ./ucxapp.py or python3 ucxapp.py.
"""

import argparse
import binascii
import concurrent.futures
import glob
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT / "app" / "bin"
SIGNED = ROOT / "app" / "release" / "ucxapp-signed.exe"
IS_WINDOWS = platform.system() == "Windows"
SUBMODULE_BRANCH = "master"

BOARDS = {
    "h753": ("nucleo_h753zi", "NUCLEO-H753ZI", "H7", "STM32CubeH7", "stm32h7"),
    "f767": ("nucleo_f767zi", "NUCLEO-F767ZI", "F7", "STM32CubeF7", "stm32f7"),
    "f439": ("nucleo_f439zi", "NUCLEO-F439ZI", "F4", "STM32CubeF4", "stm32f4"),
    "f407": ("f407_disc", "STM32F4DISCOVERY", "F4", "STM32CubeF4", "stm32f4"),
    "f401": ("nucleo_f401re", "NUCLEO-F401RE", "F4", "STM32CubeF4", "stm32f4"),
    "f446": ("nucleo_f446re", "NUCLEO-F446RE", "F4", "STM32CubeF4", "stm32f4"),
    # Lite builds (Wi-Fi + BT/GATT, no net/cert) for 128 KB flash.
    "f103": ("nucleo_f103rb", "NUCLEO-F103RB", "F1", "STM32CubeF1", "stm32f1"),
    "g071": ("nucleo_g071rb", "NUCLEO-G071RB", "G0", "STM32CubeG0", "stm32g0"),
}
MEMORY_DOC = ROOT / "docs" / "STM32_MEMORY.md"

UBLOX_VID = 0x1546
FTDI_VID = 0x0403
FTDI_PIDS = {0x6010, 0x6011, 0x6014, 0x601C, 0x6015}
UBLOX_GNSS_PIDS = {0x01A4, 0x01A5, 0x01A6, 0x01A7, 0x01A8, 0x01A9}
GNSS_DESC = re.compile(r"gnss|gps|\b(?:zed|neo|max|lea|evk)-[a-z]?\d", re.IGNORECASE)

# Old verbs of the former u-connectClientAppWin.cmd, kept so existing habits and docs still work.
LEGACY = {
    "debug": ["--debug"], "build": ["--build"], "clean": ["--clean"], "rebuild": ["--rebuild"], "all": ["--all"],
    "signed": ["--signed"], "hid": ["--hid"], "selftest": ["--selftest"], "update": ["--update"],
    "make-release": ["--release"], "sign": ["--sign"], "help": ["--help"],
}


def fail(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def require(tool, hint):
    if shutil.which(tool) is None:
        fail(f"{tool} not found. {hint}")


def check(cmd, what, **kw):
    print(">", " ".join(str(c) for c in cmd))
    if subprocess.call([str(c) for c in cmd], cwd=kw.pop("cwd", ROOT), **kw) != 0:
        fail(f"{what} failed")


def parse(argv):
    raw = []
    if "--" in argv:
        i = argv.index("--")
        argv, raw = argv[:i], argv[i + 1:]
    if argv and argv[0].lower() in LEGACY:
        head, argv = list(LEGACY[argv[0].lower()]), argv[1:]
        if head[0] in ("--clean", "--rebuild") and argv and argv[0].lower() in ("debug", "release"):
            head += ["--debug"] if argv[0].lower() == "debug" else []
            argv = argv[1:]
        argv = head + argv

    p = argparse.ArgumentParser(prog="ucxapp", description=__doc__, allow_abbrev=False,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_argument_group("target (default: this OS)")
    g.add_argument("--linux", action="store_true", help="Linux build; on Windows run it inside WSL")
    g.add_argument("--stm32", action="store_true", help="STM32 Nucleo firmware")
    g.add_argument("--board", default="h753", choices=[*BOARDS, "all"],
                   help="STM32 board (default h753)")
    g = p.add_argument_group("build")
    g.add_argument("--build", action="store_true", help="build, then stop (add --run to start the app)")
    g.add_argument("--clean", action="store_true", help="remove build output")
    g.add_argument("--rebuild", action="store_true", help="clean, reconfigure and build")
    g.add_argument("--debug", action="store_true", help="Debug configuration (host builds)")
    g.add_argument("--all", action="store_true", help="build Debug and Release (Windows)")
    g.add_argument("--everything", action="store_true",
                   help="with --build/--rebuild/--clean: Windows, Linux (WSL) and every STM32 board in parallel, "
                        "one table of results and warnings, then the offline tests")
    g.add_argument("--run", action="store_true", help="start the app after --build/--clean/--rebuild/--sign")
    g = p.add_argument_group("STM32")
    g.add_argument("--erase", action="store_true", help="erase the whole flash")
    g.add_argument("--recover", action="store_true", help="same as --erase (unlocks a stuck target)")
    g.add_argument("--flash", action="store_true", help="flash the built image over ST-Link")
    g.add_argument("--log", nargs="?", const="", metavar="PORT", help="console on the ST-Link VCP")
    g.add_argument("--put", metavar="FILE", help="copy a file to the board over the console (YMODEM, file.recv)")
    g.add_argument("--get", metavar="FILE", help="copy a file from the board over the console (YMODEM, file.send)")
    g.add_argument("--port", metavar="PORT", help="console port for --put/--get (default: the ST-Link VCP)")
    g = p.add_argument_group("run")
    g.add_argument("--signed", action="store_true", help="run app/release/ucxapp-signed.exe")
    g.add_argument("--hid", action="store_true", help="start in HID keyboard mode")
    g = p.add_argument_group("tools")
    g.add_argument("--detect", action="store_true", help="find NORA modules, GNSS receivers and IMUs")
    g.add_argument("--test", action="store_true", help="run pytest (with --detect: on the detected hardware)")
    g.add_argument("--coverage", action="store_true",
                   help="gcov build (Linux; on Windows inside WSL), run pytest on it and report line coverage of "
                        "app/src to build/coverage.txt")
    g.add_argument("--selftest", action="store_true", help="check the workspace and the built executables")
    g.add_argument("--doctor", action="store_true",
                   help="--selftest plus Python, serial access and the attached modules (first-run troubleshooting)")
    g.add_argument("--script", metavar="FILE",
                   help="run a command script (one command per line, 'expect <regex>', 'expect-fail') and report "
                        "PASS/FAIL per step; ${NAME} takes environment variables")
    g.add_argument("--junit", metavar="FILE", help="with --script: also write a JUnit XML report")
    g.add_argument("--update", action="store_true", help=f"update u-connectClient to {SUBMODULE_BRANCH}")
    g.add_argument("--sign", metavar="THUMBPRINT", help="sign the Release exe into app/release (Windows)")
    g.add_argument("--release", action="store_true", help="run scripts/release.py with the remaining arguments")
    args, extra = p.parse_known_args(argv)
    return args, extra + raw, argv


# ---- Host build (Windows / Linux) ----

def host_exe(debug):
    if not IS_WINDOWS:
        return BIN / "ucxapp"
    return BIN / ("ucxapp-debug.exe" if debug else "ucxapp.exe")


def newest_source():
    files = [*(ROOT / "app" / "src").iterdir(), ROOT / "app" / "CMakeLists.txt"]
    return max(f.stat().st_mtime for f in files if f.is_file())


def stale(exe):
    return not exe.exists() or newest_source() > exe.stat().st_mtime


def build_dir():
    if IS_WINDOWS:
        return ROOT / "build" / "windows"
    # WSL on a Windows drive (/mnt/x) is slow; build on the Linux disk, the exe still goes to app/bin.
    if str(ROOT).startswith("/mnt/"):
        return Path.home() / "ucxapp-build"
    return ROOT / "build" / "linux"


def host_clean(configs):
    bd = build_dir()
    if IS_WINDOWS:
        for c in configs:
            if (bd / "CMakeCache.txt").exists():
                subprocess.call(["cmake", "--build", str(bd), "--config", c, "--target", "clean"], cwd=ROOT)
    else:
        shutil.rmtree(bd, ignore_errors=True)
        (BIN / "ucxapp").unlink(missing_ok=True)
    print("Cleaned", bd)


def cache_moved(cache):
    """True when the CMake cache was made in another folder or for other sources (repo moved or renamed)."""
    try:
        text = cache.read_text(errors="replace")
    except OSError:
        return False
    m = re.search(r"^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$", text, re.M)
    src = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", text, re.M)
    return ((m is not None and Path(m.group(1).strip()).resolve() != cache.parent.resolve())
            or (src is not None and Path(src.group(1).strip()).resolve() != (ROOT / "app").resolve()))


def host_build(configs, fresh=False):
    require("cmake", "Install CMake 3.15+ from https://cmake.org/download/ and add it to PATH.")
    if not (ROOT / "u-connectClient" / "u-connectClient.cmake").exists():
        check(["git", "submodule", "update", "--init", "u-connectClient"], "submodule init")
    bd = build_dir()
    cache = bd / "CMakeCache.txt"
    if cache_moved(cache):
        print("Build folder was configured for another path; reconfiguring")
        fresh = True
        shutil.rmtree(bd / "CMakeFiles", ignore_errors=True)
    if fresh:
        cache.unlink(missing_ok=True)
    if IS_WINDOWS and not cache.exists():
        if subprocess.call(["cmake", "-S", "app", "-B", str(bd), "-A", "x64"], cwd=ROOT) != 0:
            fail("CMake configure failed. Visual Studio 2022 with 'Desktop development with C++' is needed: "
                 "https://aka.ms/vs/17/release/vs_BuildTools.exe")
    elif IS_WINDOWS and not (bd / "ucxapp.vcxproj").exists():
        # Build dir from before the exe was renamed; the cache keeps its platform.
        check(["cmake", "-S", "app", "-B", bd], "CMake reconfigure")
    for c in configs:
        if not IS_WINDOWS:
            check(["cmake", "-S", "app", "-B", bd, f"-DCMAKE_BUILD_TYPE={c}"], "CMake configure")
        check(["cmake", "--build", bd, "--config", c, "--target", "ucxapp", "--parallel", str(os.cpu_count() or 4),
               *(["--clean-first"] if fresh else [])], f"{c} build")


def pick_exe(args):
    """The exe to start: builds when sources are newer; prefers the signed exe when it is at least as new."""
    config = "Debug" if args.debug else "Release"
    exe = host_exe(args.debug)
    if args.signed:
        if not SIGNED.exists():
            fail(f"{SIGNED.relative_to(ROOT)} missing, create it with --sign THUMBPRINT")
        return SIGNED
    use_signed = IS_WINDOWS and not args.debug and SIGNED.exists()
    if use_signed and not exe.exists() and newest_source() <= SIGNED.stat().st_mtime + 60:
        return SIGNED
    if stale(exe):
        host_build([config])
    if use_signed and SIGNED.stat().st_mtime >= exe.stat().st_mtime:
        return SIGNED
    return exe


def wsl_serial_hint():
    try:
        if "microsoft" not in Path("/proc/version").read_text().lower():
            return
    except OSError:
        return
    if not glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"):
        print("NOTE: WSL sees no USB serial ports. Attach the EVK from Windows (admin PowerShell):\n"
              "  usbipd list; usbipd bind --busid <id>; usbipd attach --wsl --busid <id>")


def run_app(exe, app_args):
    print("Starting", exe.relative_to(ROOT), *app_args)
    if not IS_WINDOWS:
        wsl_serial_hint()
        os.chdir(ROOT)
        os.execv(str(exe), [str(exe), *app_args])
    # The app handles Ctrl-C itself; the launcher must not die and kill it.
    old = signal.signal(signal.SIGINT, signal.SIG_IGN)
    try:
        return subprocess.call([str(exe), *app_args], cwd=ROOT)
    finally:
        signal.signal(signal.SIGINT, old)


def sign(thumbprint):
    if not IS_WINDOWS:
        fail("--sign needs Windows (signtool and the certificate token)")
    sys.path.insert(0, str(ROOT / "scripts"))
    from release import find_signtool

    exe = host_exe(False)
    if not exe.exists():
        fail(f"{exe.relative_to(ROOT)} missing, build it first (--rebuild)")
    tool = find_signtool()
    check([tool, "sign", "/sha1", thumbprint, "/tr", "http://timestamp.digicert.com", "/td", "sha256",
           "/fd", "sha256", exe], "signing (is the certificate token inserted?)")
    check([tool, "verify", "/pa", exe], "signature verification")
    SIGNED.parent.mkdir(exist_ok=True)
    shutil.copy2(exe, SIGNED)
    print("Signed:", SIGNED.relative_to(ROOT))


def host(args, extra):
    configs = ["Debug", "Release"] if args.all else ["Debug" if args.debug else "Release"]
    if args.clean or args.rebuild:
        host_clean(configs if (args.debug or args.all or args.rebuild) else ["Debug", "Release"])
    if args.build or args.rebuild or args.all:
        host_build(configs, fresh=args.rebuild)
    if args.sign:
        sign(args.sign)
    if (args.build or args.clean or args.rebuild or args.all or args.sign) and not args.run:
        return 0
    exe = pick_exe(args)
    return run_app(exe, [*extra, "--exec", "hid.setup", "--shell"] if args.hid else extra)


# ---- Hardware detection ----

def classify(vid, pid, desc):
    d = desc.lower()
    if "stlink" in d or "st-link" in d:
        return "STM32 console"
    if "bluetooth" in d or "active management" in d:
        return "skip"
    if (vid == UBLOX_VID and pid in UBLOX_GNSS_PIDS) or GNSS_DESC.search(desc):
        return "GNSS"
    if vid == FTDI_VID and pid in FTDI_PIDS:
        return "EVK (FTDI)"
    if vid == UBLOX_VID:
        return "u-blox"
    if any(k in d for k in ("ch340", "cp210", "witmotion", "pl2303")):
        return "USB-UART (IMU/GNSS?)"
    return ""


def list_ports(exe):
    """(name, description, role) for every serial port; pyserial when installed, else the app's --list-ports."""
    try:
        from serial.tools import list_ports as lp

        return [(p.device, p.description or "", classify(p.vid, p.pid, p.description or "")) for p in lp.comports()]
    except ImportError:
        out = app_output(exe, "--list-ports", timeout=20)
        ports = []
        for line in out.splitlines():
            m = re.match(r"(\S+)\s+(?:available|no access)\s*(.*)", line)
            if m:
                ports.append((m.group(1), m.group(2), classify(None, None, m.group(2))))
        return ports


def app_output(exe, *app_args, timeout):
    try:
        r = subprocess.run([str(exe), *app_args], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        print(f"  ({' '.join(app_args)} timed out after {timeout} s)")
        return ""


def module_ports(list_modules_output):
    """Port names from `--list-modules`; log lines such as '[..][DBG ] Loaded FTDI DLL' are ignored."""
    return re.findall(r"^\s*(COM\d+|/dev/\S+)\s*$", list_modules_output, re.MULTILINE)


def find_gnss(exe):
    """(port, baud, details) of the receiver gnss.detect reports first, or None."""
    out = app_output(Path(exe), "--exec", "gnss.detect", timeout=240)
    m = re.search(r"^\s*(COM\d+|/dev/[^\s:(]+)[^\n]*?GNSS at (\d+) baud:?\s*(.*)$", out, re.MULTILINE)
    return (m.group(1), m.group(2), m.group(3)) if m else None


def find_imu(exe):
    """(port, baud, kind) of the IMU imu.detect reports first, or None."""
    out = app_output(Path(exe), "--exec", "imu.detect", timeout=240)
    m = re.search(r"^\s*(COM\d+|/dev/[^\s:(]+)[^\n]*?:\s+(\S[^\n]*?) IMU at (\d+) baud", out, re.MULTILINE)
    return (m.group(1), m.group(3), m.group(2)) if m else None


def detect(exe):
    """Prints what is attached and returns the matching UCX_* test variables."""
    env = {}
    print("Serial ports:")
    for name, desc, role in list_ports(exe):
        print(f"  {name:<14} {role:<22} {desc}")

    modules = module_ports(app_output(exe, "--list-modules", timeout=30))
    print("NORA modules:")
    for i, port in enumerate(modules[:2]):
        env["UCX_PORT_" + "AB"[i]] = port
    with concurrent.futures.ThreadPoolExecutor() as pool:
        infos = list(pool.map(lambda p: app_output(exe, p, "--exec", "device.info", timeout=30), modules))
    for port, info in zip(modules, infos, strict=True):
        m = re.search(r"\b(NORA-[A-Z0-9]+[\w-]*)", info)
        v = re.search(r"Firmware Version:\s*(\S+)", info)
        print(f"  {port:<14} {m.group(1) + (f' ({v.group(1)})' if v else '') if m else '(no answer to device.info)'}")
    if not modules:
        print("  none")

    print("GNSS (gnss.detect):")
    gnss = find_gnss(exe)
    if gnss:
        env["UCX_GNSS_PORT"], env["UCX_GNSS_BAUD"] = gnss[0], gnss[1]
        print(f"  {gnss[0]} at {gnss[1]} baud {gnss[2]}")
    else:
        print("  none")

    print("IMU (imu.detect):")
    imu = find_imu(exe)
    if imu:
        env["UCX_IMU_PORT"], env["UCX_IMU_BAUD"] = imu[0], imu[1]
        print(f"  {imu[0]} {imu[2]} at {imu[1]} baud")
    else:
        print("  none")

    if env:
        print("Test variables:")
        for k, v in env.items():
            print(f'  $env:{k} = "{v}"' if IS_WINDOWS else f"  export {k}={v}")
    print("Wi-Fi for the tests:")
    for line in wifi_summary():
        print(f"  {line}")
    return env


def wifi_summary():
    """Lines saying which Wi-Fi the hardware tests will join."""
    profiles = ROOT / "storage" / "ucxapp.wifi"
    last = ROOT / "storage" / "ucxapp.wifilast"
    if os.environ.get("UCX_WIFI_SSID"):
        password = "set" if os.environ.get("UCX_WIFI_PASS") else "not set"
        return [f"UCX_WIFI_SSID={os.environ['UCX_WIFI_SSID']} (UCX_WIFI_PASS {password})"]
    if profiles.is_file() and profiles.read_text(encoding="utf-8", errors="replace").strip():
        text = profiles.read_text(encoding="utf-8", errors="replace")
        names = [line.split("\t")[0] for line in text.splitlines() if "\t" in line]
        used = last.read_text(encoding="utf-8", errors="replace").strip() if last.is_file() else ""
        return [f"UCX_WIFI_SSID not set: using saved profiles {', '.join(names)} (wifi.profile.auto"
                f"{', last used ' + used if used else ''})"]
    return ["none: set UCX_WIFI_SSID and UCX_WIFI_PASS, or run wifi.connect <ssid> <password> once to save a profile"]


def pytest_python():
    """This Python if it has pytest; on Linux (system Python is often externally managed) else a venv in ~."""
    if subprocess.call([sys.executable, "-c", "import pytest"], stderr=subprocess.DEVNULL) == 0:
        return sys.executable
    venv = Path.home() / ".ucxapp-venv"
    py = venv / "bin" / "python"
    if IS_WINDOWS:
        fail(f"pytest missing for {sys.executable}: {sys.executable} -m pip install -r tests/requirements.txt")
    if not py.exists():
        check([sys.executable, "-m", "venv", venv], "venv (sudo apt install python3-venv)")
    if subprocess.call([py, "-c", "import pytest"], stderr=subprocess.DEVNULL) != 0:
        check([py, "-m", "pip", "install", "-q", "-r", ROOT / "tests" / "requirements.txt"], "pytest install")
    return str(py)


def run_tests(pytest_args, found):
    py = pytest_python()
    env = dict(os.environ)
    for k, v in found.items():
        env.setdefault(k, v)
    return subprocess.call([py, "-m", "pytest", *(pytest_args or ["tests"])], cwd=ROOT, env=env)


def coverage(pytest_args):
    """Instrumented Linux build, the offline tests on it, then line coverage per app/src file."""
    require("cmake", "Install CMake 3.15+.")
    require("gcov", "Install gcc (gcov comes with it).")
    bd = build_dir().parent / ("ucxapp-coverage" if str(ROOT).startswith("/mnt/") else "coverage")
    check(["cmake", "-S", "app", "-B", bd, "-DCMAKE_BUILD_TYPE=Debug", "-DUCXAPP_COVERAGE=ON"], "CMake configure")
    check(["cmake", "--build", bd, "--target", "ucxapp", "--parallel", str(os.cpu_count() or 4)], "coverage build")
    for old in bd.rglob("*.gcda"):
        old.unlink()
    if not (bd / "ucxapp").is_file():
        fail(f"instrumented exe not found: {bd / 'ucxapp'}")
    absent = {k: "" for k in ("UCX_PORT_A", "UCX_PORT_B", "UCX_GNSS_PORT", "UCX_IMU_PORT")}
    # Only tests that run the app add C coverage; the repo checks crawl /mnt (slow 9P in WSL).
    pytest_args = pytest_args or ["tests", "-m", "app", "-p", "no:cacheprovider", "-v"]
    env = {**absent, "UCX_APP_EXE": str(bd / "ucxapp"), "PYTHONUNBUFFERED": "1"}
    rc = run_tests(pytest_args, env)

    src = (ROOT / "app" / "src").resolve()
    files = {}
    for gcda in bd.rglob("*.gcda"):
        out = subprocess.run(["gcov", "-n", "-o", str(gcda.parent), str(gcda)], cwd=bd, capture_output=True,
                             text=True, errors="replace").stdout
        for name, pct, total in re.findall(r"File '([^']+)'\nLines executed:([\d.]+)% of (\d+)", out):
            path = Path(name) if Path(name).is_absolute() else (bd / name)
            if path.resolve().parent == src:
                files[path.name] = (round(float(pct) * int(total) / 100), int(total))
    if not files:
        fail("no coverage data; did the app tests run? (they need the app built and not skipped)")
    rows = [f"{n:<32} {hit:>6}/{total:<6} {100 * hit / total:6.1f}%"
            for n, (hit, total) in sorted(files.items(), key=lambda kv: kv[1][0] / kv[1][1])]
    hit = sum(h for h, _ in files.values())
    total = sum(t for _, t in files.values())
    rows.append(f"{'TOTAL':<32} {hit:>6}/{total:<6} {100 * hit / total:6.1f}%")
    report = ROOT / "build" / "coverage.txt"
    report.parent.mkdir(exist_ok=True)
    report.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print("\n".join(rows))
    print("Report:", report)
    return rc


# ---- STM32 ----

def stm32_tools():
    if IS_WINDOWS:
        extra = [Path(r"C:\Program Files\CMake\bin"),
                 Path(r"C:\Program Files\Arm\GNU Toolchain mingw-w64-x86_64-arm-none-eabi\bin"),
                 ROOT.parent / "u-connectMatter" / "tools"]
        os.environ["PATH"] = os.pathsep.join([*(str(d) for d in extra if d.is_dir()), os.environ["PATH"]])
    for _, _, family, hal_dir, portdir in BOARDS.values():
        var = f"STM32CUBE{family}_PATH"
        if os.environ.get(var):
            continue
        candidates = [ROOT / "u-connectClient" / "ports" / "extra" / portdir / hal_dir,
                      ROOT.parent / "u-connectMatter" / "u-connectClient" / "ports" / "extra" / portdir / hal_dir,
                      ROOT.parent / hal_dir]
        hal = next((c for c in candidates if (c / "Drivers").is_dir()), None)
        if hal is not None:
            os.environ[var] = str(hal)


def programmer():
    win = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe")
    found = shutil.which("STM32_Programmer_CLI") or (str(win) if win.exists() else None)
    if found is None:
        fail("STM32CubeProgrammer not found (STM32_Programmer_CLI)")
    return found


def find_stlink(prog, board_name):
    """ST-Link serial number of the probe whose board name matches; the list has an SN line before each name."""
    out = subprocess.run([prog, "-l", "st"], capture_output=True, text=True, errors="replace").stdout
    last = None
    serials = []
    for line in out.splitlines():
        key, _, value = line.partition(":")
        value = value.replace(" ", "")
        if "ST-LINK SN" in key:
            last = value
            serials.append(value)
        elif "Board Name" in key and value == board_name:
            return last
    # Older ST-Link/V2 probes (e.g. on the STM32F4DISCOVERY) report no board name.
    return serials[0] if len(serials) == 1 else None


def stm32_build(board, family, hal_dir, portdir, bd):
    for tool in ("cmake", "ninja", "arm-none-eabi-gcc"):
        require(tool, "See .github/skills/ucx-stm32/SKILL.md for the STM32 toolchain.")
    hal = os.environ.get(f"STM32CUBE{family}_PATH")
    if hal is None:
        fail(f"{hal_dir} not found. Set STM32CUBE{family}_PATH or run: "
             f"git -C u-connectClient submodule update --init ports/extra/{portdir}/{hal_dir}")
    if not (bd / "build.ninja").exists():
        toolchain = ROOT / "u-connectClient" / "cmake" / "arm-none-eabi-gcc.cmake"
        check(["cmake", "-G", "Ninja", "-S", ROOT / "app" / "stm32", "-B", bd,
               f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}", f"-DSTM32_BOARD={board}",
               f"-DSTM32_HAL_PATH={Path(hal).as_posix()}", "-DCMAKE_BUILD_TYPE=Release"], "CMake configure")
    check(["cmake", "--build", bd], "STM32 build")


def stm32_usage(board):
    """(flash used, flash size, RAM used, RAM size) in bytes from the .map regions and the ELF sections."""
    elf = ROOT / "build" / "bin" / f"ucxapp_{board}.elf"
    mapfile = elf.with_suffix(".map")
    if not elf.exists() or not mapfile.exists() or shutil.which("arm-none-eabi-size") is None:
        return None
    regions = []
    in_cfg = False
    for line in mapfile.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("Memory Configuration"):
            in_cfg = True
        elif in_cfg and line.startswith("Linker script and memory map"):
            break
        elif in_cfg:
            m = re.match(r"(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", line)
            if m and m.group(1) != "*default*":
                regions.append((int(m.group(2), 16), int(m.group(3), 16)))
    out = subprocess.run(["arm-none-eabi-size", "-A", "-d", str(elf)], capture_output=True, text=True).stdout
    sections = {}
    for line in out.splitlines():
        m = re.match(r"(\.\S+)\s+(\d+)\s+(\d+)", line)
        if m and int(m.group(2)):
            sections[m.group(1)] = (int(m.group(2)), int(m.group(3)))

    def region(addr):
        return next((r for r in regions if r[0] <= addr < r[0] + r[1]), None)

    flash_r = region(sections.get(".isr_vector", (0, 0x08000000))[1]) or region(0x08000000)
    ram_r = region(sections[".bss"][1]) if ".bss" in sections else None
    if flash_r is None or ram_r is None:
        return None
    flash = sum(s for s, a in sections.values() if region(a) == flash_r) + sections.get(".data", (0, 0))[0]
    ram = sum(s for s, a in sections.values() if region(a) == ram_r)
    return flash, flash_r[1], ram, ram_r[1]


def usage_row(key, usage):
    board, name = BOARDS[key][:2]
    flash, flash_size, ram, ram_size = usage
    return (f"| `{key}` | {name} | {flash // 1024} KB | {flash_size // 1024} KB | {100 * flash / flash_size:.0f}% "
            f"| {ram // 1024} KB | {ram_size // 1024} KB | {100 * ram / ram_size:.0f}% |")


def record_usage(results, quiet=False):
    """Print the usage and keep the per-board table in docs/STM32_MEMORY.md up to date."""
    rows = {}
    if MEMORY_DOC.exists():
        for line in MEMORY_DOC.read_text(encoding="utf-8").splitlines():
            m = re.match(r"\| `(\w+)` \|", line)
            if m and m.group(1) in BOARDS:
                rows[m.group(1)] = line
    for key, usage in results.items():
        row = usage_row(key, usage)
        if not quiet:
            print("Memory:", row)
        rows[key] = row
    table = "\n".join(rows[k] for k in BOARDS if k in rows)
    MEMORY_DOC.parent.mkdir(exist_ok=True)
    MEMORY_DOC.write_text(
        "# STM32 flash and RAM usage\n\n"
        "Updated by `ucxapp --stm32 --build` (Release builds). RAM includes the static FreeRTOS heap\n"
        "(`configTOTAL_HEAP_SIZE`, set per board in `app/stm32/CMakeLists.txt`), so free RAM is the headroom left\n"
        "after that heap. Flash is code, read-only data and the initial values of `.data`.\n\n"
        "| Board | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |\n"
        "|---|---|---|---|---|---|---|---|\n" + table + "\n", encoding="utf-8")
    update_readme_memory({k: rows[k] for k in BOARDS if k in rows})


README = ROOT / "README.md"
README_MEMORY = re.compile(r"(<!-- memory -->\n)(.*?)(<!-- /memory -->)", re.S)
HOST_TARGETS = (("win", "Windows x64", "ucxapp.exe"), ("linux", "Linux x64", "ucxapp"))


def update_readme_memory(new_rows):
    """Replace rows in the README memory table between <!-- memory --> and <!-- /memory -->."""
    text = README.read_text(encoding="utf-8")
    m = README_MEMORY.search(text)
    if m is None:
        return
    rows = {}
    for line in m.group(2).splitlines():
        k = re.match(r"\| `(\w+)` \|", line)
        if k:
            rows[k.group(1)] = line
    rows.update(new_rows)
    order = [h[0] for h in HOST_TARGETS] + list(BOARDS)
    table = ("| Target | Name | Flash used | Flash size | Flash % | RAM used | RAM size | RAM % |\n"
             "|---|---|---|---|---|---|---|---|\n" + "\n".join(rows[k] for k in order if k in rows) + "\n")
    README.write_text(text[:m.start(2)] + table + text[m.end(2):], encoding="utf-8")


def host_usage(keys):
    """{key: (exe bytes, peak RAM bytes after start)} from app.memory (offline) for the Windows and Linux builds."""
    usage = {}
    for key, _, file in HOST_TARGETS:
        exe = BIN / file
        if key not in keys or not exe.is_file() or (key == "win" and not IS_WINDOWS) or (
                key == "linux" and IS_WINDOWS and not shutil.which("wsl")):
            continue
        cmd = ["wsl", "./app/bin/ucxapp"] if key == "linux" and IS_WINDOWS else [str(exe)]
        try:
            out = subprocess.run([*cmd, "--offline", "--exec", "app.memory"], cwd=ROOT, capture_output=True,
                                 text=True, errors="replace", timeout=60, stdin=subprocess.DEVNULL).stdout
        except (OSError, subprocess.TimeoutExpired):
            continue
        m = re.search(r"App RAM peak\s+size:\s*\S+\s+used:\s*(\d+)([MG]?)", out)
        if m is None:
            continue
        usage[key] = (exe.stat().st_size, int(m.group(1)) * {"": 1, "M": 1 << 20, "G": 1 << 30}[m.group(2)])
    return usage


def host_row(key, usage):
    name = next(h[1] for h in HOST_TARGETS if h[0] == key)
    return f"| `{key}` | {name} | {usage[0] / (1 << 20):.1f} MB | - | - | {usage[1] / (1 << 20):.1f} MB | - | - |"


def stm32_log(port):
    if IS_WINDOWS:
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               str(ROOT / "app" / "stm32" / "stm32tool.ps1"), "-Action", "log", *(["-Port", port] if port else [])]
        return subprocess.call(cmd, cwd=ROOT)
    port = port or next(iter(sorted(glob.glob("/dev/serial/by-id/*STLINK*") + glob.glob("/dev/serial/by-id/*STM*"))),
                        None)
    if port is None:
        fail("no ST-Link VCP found, pass it: --log /dev/ttyACM0")
    return subprocess.call([sys.executable, "-m", "serial.tools.miniterm", port, "115200"])


# ---- YMODEM over the STM32 console (file.recv / file.send on the board) ----

YM_SOH, YM_STX, YM_EOT, YM_ACK, YM_NAK, YM_CAN, YM_C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, ord("C")


def ymodem_packet(seq, data, size=1024):
    data = bytes(data).ljust(size, b"\x1a")
    seq &= 0xFF
    return (bytes([YM_STX if size == 1024 else YM_SOH, seq, 0xFF - seq]) + data
            + binascii.crc_hqx(data, 0).to_bytes(2, "big"))


def ymodem_header(name, size):
    body = f"{name}\0{size}".encode() if name else b""
    return ymodem_packet(0, body.ljust(128, b"\0"), 128)


def _ym_wait(ser, wanted, seconds):
    """wanted or NAK; other bytes are skipped; None on timeout."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        b = ser.read(1)
        if b and b[0] in (wanted, YM_NAK):
            return b[0]
        if b and b[0] in (YM_CAN, 0x03):
            raise RuntimeError("cancelled by the board")
    return None


def _ym_send_packet(ser, pkt):
    for _ in range(10):
        ser.write(pkt)
        if _ym_wait(ser, YM_ACK, 10) == YM_ACK:
            return
    ser.write(bytes([YM_CAN] * 5))
    raise RuntimeError("no ACK from the board")


def ymodem_send(ser, name, data, progress=None):
    if _ym_wait(ser, YM_C, 60) != YM_C:
        raise RuntimeError("the board did not start the transfer")
    _ym_send_packet(ser, ymodem_header(name, len(data)))
    if _ym_wait(ser, YM_C, 10) != YM_C:
        raise RuntimeError("the board did not ask for data")
    for seq, off in enumerate(range(0, len(data), 1024), start=1):
        chunk = data[off:off + 1024]
        _ym_send_packet(ser, ymodem_packet(seq, chunk, 128 if len(chunk) <= 128 else 1024))
        if progress:
            progress(off + len(chunk), len(data))
    _ym_send_packet(ser, bytes([YM_EOT]))
    if _ym_wait(ser, YM_C, 10) == YM_C:
        _ym_send_packet(ser, ymodem_header("", 0))


def _ym_read_packet(ser):
    """(seq, data), 'eot', 'bad' or None on timeout."""
    end = time.monotonic() + 1
    while True:
        b = ser.read(1)
        if not b:
            if time.monotonic() >= end:
                return None
            continue
        if b[0] == YM_EOT:
            return "eot"
        if b[0] == YM_CAN:
            raise RuntimeError("cancelled by the board")
        if b[0] in (YM_SOH, YM_STX):
            break
    n = 128 if b[0] == YM_SOH else 1024
    rest = ser.read(n + 4)
    if len(rest) < n + 4 or rest[0] != 0xFF - rest[1]:
        return "bad"
    if rest[2 + n:] != binascii.crc_hqx(rest[2:2 + n], 0).to_bytes(2, "big"):
        return "bad"
    return rest[0], rest[2:2 + n]


def ymodem_recv(ser, progress=None):
    """(name, data) of one file."""
    name, size, out, seq = None, -1, bytearray(), 0
    expect_header, after_eot, errors = True, False, 0
    ser.write(bytes([YM_C]))
    while True:
        p = _ym_read_packet(ser)
        if p is None or p == "bad":
            errors += 1
            if errors >= (60 if name is None else 10):
                ser.write(bytes([YM_CAN] * 5))
                raise RuntimeError("timeout" if p is None else "too many bad blocks")
            ser.write(bytes([YM_C if expect_header and p is None else YM_NAK]))
            continue
        if p == "eot":
            ser.write(bytes([YM_ACK]))
            if not expect_header:
                ser.write(bytes([YM_C]))
                expect_header, after_eot = True, True
            continue
        pseq, data = p
        if expect_header:
            if pseq != 0:
                ser.write(bytes([YM_NAK]))
                continue
            ser.write(bytes([YM_ACK]))
            fields = data.split(b"\0")
            if after_eot:
                return name, bytes(out)
            if not fields[0]:
                raise RuntimeError("the board sent no file")
            name = fields[0].decode(errors="replace")
            digits = fields[1].split(b" ")[0] if len(fields) > 1 else b""
            size = int(digits) if digits.isdigit() else -1
            ser.write(bytes([YM_C]))
            expect_header, seq, errors = False, 1, 0
            continue
        if pseq == (seq - 1) & 0xFF:
            ser.write(bytes([YM_ACK]))
            continue
        if pseq != seq:
            ser.write(bytes([YM_CAN] * 5))
            raise RuntimeError(f"block {pseq} out of order (expected {seq})")
        out += data
        if size >= 0:
            del out[size:]
        seq, errors = (seq + 1) & 0xFF, 0
        ser.write(bytes([YM_ACK]))
        if progress:
            progress(len(out), size)


def _ym_progress(done, total):
    print(f"\r  {done} / {total if total >= 0 else '?'} bytes", end="", flush=True)


def stm32_console_port(port):
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        fail("--put/--get need pyserial: python -m pip install pyserial")
    if port:
        return port
    found = [p.device for p in lp.comports() if classify(p.vid, p.pid, p.description or "") == "STM32 console"]
    if not found:
        fail("no ST-Link console found, pass it: --port COM5")
    return found[0]


def stm32_transfer(args):
    import serial

    port = stm32_console_port(args.port)
    path = Path(args.put or args.get)
    if args.put and not path.is_file():
        fail(f"{path} not found")
    command = f"file.recv {path.name}" if args.put else f"file.send {path.name}"
    with serial.Serial(port, 115200, timeout=1) as ser:
        ser.write(b"\r")
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(command.encode() + b"\r")
        text = b""
        end = time.monotonic() + 10
        while b"ymodem: ready" not in text or not text.endswith(b"\n"):
            text += ser.read(1)
            if b"ERROR" in text and text.endswith(b"\n") or time.monotonic() > end:
                fail(f"{port}: {command} did not start: {text.decode(errors='replace').strip()}")
        try:
            if args.put:
                data = path.read_bytes()
                ymodem_send(ser, path.name, data, _ym_progress)
            else:
                _, data = ymodem_recv(ser, _ym_progress)
                path.write_bytes(data)
        except RuntimeError as e:
            print()
            fail(f"{command}: {e}")
        print()
        reply = b""
        end = time.monotonic() + 5
        while not re.search(rb"(file\.(recv|send): .*|ERROR.*)\r?\n", reply) and time.monotonic() < end:
            reply += ser.read(1)
    reply = reply.decode(errors="replace").strip()
    print(reply.splitlines()[-1] if reply else "(no reply from the board)")
    print(f"{path}: {len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
    return 0 if "ERROR" not in reply else 1


ALL_PROBLEM = re.compile(r"warning( C\d+)?:|error( C\d+| MSB\d+)?:|undefined reference|overflowed|FAILED:"
                         r"|CMake Error|^ERROR:")


def all_problem_line(line):
    """Compiler/linker line without directories and code offsets, so repeats collapse into one."""
    line = re.sub(r"^.*?ld(\.exe)?: ", "", line.strip())
    line = re.sub(r"(?:[A-Za-z]:)?[^\s:]*[/\\]", "", line)
    return re.sub(r":\d+:\(\.text[^)]*\)", "", line)


def all_progress(path):
    """(done, total, problem lines) from a child build log."""
    text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
    steps = re.findall(r"^\[(\d+)/(\d+)\]", text, re.M)
    done, total = (int(steps[-1][0]), int(steps[-1][1])) if steps else (0, 0)
    problems = list(dict.fromkeys(all_problem_line(ln) for ln in text.splitlines() if ALL_PROBLEM.search(ln)))
    return done, total, problems


def stm32_all(args):
    if args.erase or args.recover or args.log is not None:
        fail("--board all supports only --clean, --build, --rebuild and --flash")
    flags = [f for f, on in (("--clean", args.clean), ("--build", args.build), ("--rebuild", args.rebuild),
                             ("--flash", args.flash)) if on]
    if not flags:
        fail("--board all needs --build, --clean, --rebuild or --flash")
    return build_many(args, {b: ["--stm32", "--board", b, *flags] for b in BOARDS})


def everything(args):
    flags = [f for f, on in (("--clean", args.clean), ("--build", args.build), ("--rebuild", args.rebuild)) if on]
    if not flags:
        fail("--everything needs --build, --clean or --rebuild")
    jobs = {"win": flags} if IS_WINDOWS else {}
    jobs["linux"] = ["--linux", *flags]
    if IS_WINDOWS and not shutil.which("wsl"):
        del jobs["linux"]
        print("WSL not found, skipping the Linux build")
    stm32_tools()
    jobs.update({b: ["--stm32", "--board", b, *flags] for b in BOARDS})
    rc = build_many(args, jobs)
    if args.clean and not (args.build or args.rebuild):
        return rc
    print("\nOffline tests (pytest, no hardware):")
    out = subprocess.run([sys.executable, "-m", "pytest", "tests", "-q", "--ignore=tests/hardware"],
                         cwd=ROOT, capture_output=True, text=True, errors="replace")
    (ROOT / "build" / "all_tests.log").write_text(out.stdout + out.stderr, encoding="utf-8")
    lines = [ln for ln in out.stdout.splitlines() if ln.startswith(("FAILED", "ERROR"))]
    print("\n".join(["  " + ln for ln in lines[:20]] + ["  " + (out.stdout.strip().splitlines() or ["no output"])[-1]]))
    return rc or out.returncode


def build_many(args, jobs):
    """Runs ucxapp.py once per job in parallel, logs to build/all_<name>.log, prints one table."""
    (ROOT / "build").mkdir(exist_ok=True)
    procs = {}
    start = time.monotonic()
    for b, job in jobs.items():
        log = open(ROOT / "build" / f"all_{b}.log", "w", encoding="utf-8")
        procs[b] = (subprocess.Popen([sys.executable, __file__, *job], cwd=ROOT,
                                     stdout=log, stderr=subprocess.STDOUT,
                                     env={**os.environ, "UCXAPP_ALL_CHILD": "1"}), log)
    tty = sys.stdout.isatty()
    ended = {}
    while len(ended) < len(procs):
        cells = []
        for b, (proc, log) in procs.items():
            if b not in ended and proc.poll() is not None:
                log.close()
                ended[b] = (proc.returncode, time.monotonic() - start)
                if not tty:
                    print(f"[{b}] {'done' if proc.returncode == 0 else 'FAILED'} after {ended[b][1]:.0f} s",
                          flush=True)
            done, total, _ = all_progress(ROOT / "build" / f"all_{b}.log")
            state = ("ok" if ended[b][0] == 0 else "FAIL") if b in ended else (f"{done}/{total}" if total else "...")
            cells.append(f"{b} {state}")
        if tty:
            print(f"\r{time.monotonic() - start:4.0f} s  " + "  ".join(cells) + "\033[K", end="", flush=True)
        if len(ended) < len(procs):
            time.sleep(1)
    if tty:
        print()

    usage = {}
    if args.build or args.rebuild:
        usage = {b: u for b in jobs if b in BOARDS and (u := stm32_usage(BOARDS[b][0])) is not None}
        host = host_usage(jobs)
    else:
        host = {}
    print(f"\n{'target':6} {'result':7} {'time':>6} {'warn':>5} {'flash':>12} {'ram':>12}")
    failed = False
    report = []
    for b in jobs:
        rc, secs = ended[b]
        _, _, problems = all_progress(ROOT / "build" / f"all_{b}.log")
        warns = sum("warning:" in p for p in problems)
        u = usage.get(b)
        flash = f"{u[0] // 1024}K {100 * u[0] / u[1]:3.0f}%" if u else "-"
        ram = f"{u[2] // 1024}K {100 * u[2] / u[3]:3.0f}%" if u else "-"
        if b in host:
            flash, ram = (f"{v / (1 << 20):.1f}M     " for v in host[b])
        print(f"{b:6} {'ok' if rc == 0 else 'FAILED':7} {secs:5.0f}s {warns:5} {flash:>12} {ram:>12}")
        failed |= rc != 0
        if problems:
            report.append((b, problems))
    for b, problems in report:
        print(f"\n--- {b} (build/all_{b}.log)")
        for p in problems[:20]:
            print("  " + p)
        if len(problems) > 20:
            print(f"  ... {len(problems) - 20} more")
    if usage:
        record_usage(usage, quiet=True)
    if host:
        update_readme_memory({k: host_row(k, u) for k, u in host.items()})
    print(f"\n{'FAILED' if failed else 'All targets OK'}"
          f"{'' if failed or report else ', zero warnings'} in {time.monotonic() - start:.0f} s")
    return 1 if failed else 0


def stm32(args, extra):
    if extra:
        fail(f"unknown STM32 argument(s): {' '.join(extra)}")
    if not (args.clean or args.build or args.rebuild or args.erase or args.recover or args.flash
            or args.log is not None or args.put or args.get):
        fail("--stm32 needs --build, --rebuild, --clean, --erase, --recover, --flash, --log, --put or --get")
    if args.put and args.get:
        fail("use --put or --get, not both")
    if (args.put or args.get) and not (args.clean or args.build or args.rebuild or args.erase or args.recover
                                       or args.flash or args.log is not None):
        return stm32_transfer(args)
    stm32_tools()
    if args.board == "all":
        return stm32_all(args)
    board, name, family, hal_dir, portdir = BOARDS[args.board]
    bd = ROOT / "build" / f"stm32_{args.board}"
    image = ROOT / "build" / "bin" / f"ucxapp_{board}.bin"
    if args.clean or args.rebuild:
        shutil.rmtree(bd, ignore_errors=True)
        for f in image.parent.glob(f"ucxapp_{board}.*"):
            f.unlink()
        print("Cleaned", bd.relative_to(ROOT))
    if args.build or args.rebuild:
        stm32_build(board, family, hal_dir, portdir, bd)
        print("Built", image.relative_to(ROOT))
        usage = stm32_usage(board)
        if usage is not None and not os.environ.get("UCXAPP_ALL_CHILD"):
            record_usage({args.board: usage})
        elif usage is not None:
            print("Memory:", usage_row(args.board, usage))
    if args.erase or args.recover:
        check([programmer(), "-c", "port=SWD", "mode=UR", "reset=HWrst", "-e", "all"], "erase")
    if args.flash:
        if not image.exists():
            fail(f"{image.relative_to(ROOT)} missing, add --build")
        prog = programmer()
        sn = find_stlink(prog, name)
        if sn is None:
            fail(f"no ST-Link found for {name}")
        print(f"Flashing {name} via ST-Link {sn}")
        check([prog, "-c", "port=SWD", f"sn={sn}", "mode=UR", "reset=HWrst", "-d", image, "0x08000000", "-v",
               "-rst"], "flash")
    if args.put or args.get:
        if args.flash:
            time.sleep(3)   # let the board boot and mount its files
        rc = stm32_transfer(args)
        if rc or args.log is None:
            return rc
    if args.log is not None:
        return stm32_log(args.log)
    return 0


# ---- Workspace tools ----

def update():
    require("git", "Install Git from https://git-scm.com/downloads")
    sub = ROOT / "u-connectClient"
    if not (sub / ".git").exists():
        check(["git", "submodule", "update", "--init", "u-connectClient"], "submodule init")
    else:
        check(["git", "-C", sub, "fetch", "origin"], "fetch")
        check(["git", "-C", sub, "checkout", SUBMODULE_BRANCH], "checkout")
        check(["git", "-C", sub, "pull", "origin", SUBMODULE_BRANCH], "pull")
    subprocess.call(["git", "submodule", "status"], cwd=ROOT)
    print("Commit the new submodule reference with: git add u-connectClient; git commit")
    return 0


def selftest():
    errors = 0
    for rel in ("u-connectClient/src", "u-connectClient/inc", "u-connectClient/ucx_api/generated",
                "u-connectClient/src/u_cx_at_client.c", "app/src/ucxapp_main.c", "app/CMakeLists.txt",
                "app/stm32/CMakeLists.txt", "README.md"):
        ok = (ROOT / rel).exists()
        errors += not ok
        print(f"[{'OK' if ok else 'ERROR'}] {rel}")
    headers = len(list((ROOT / "u-connectClient" / "ucx_api" / "generated").rglob("u_cx_*.h")))
    print(f"[{'OK' if headers > 10 else 'WARNING'}] {headers} generated UCX API headers")
    for exe in (host_exe(False), host_exe(True), SIGNED):
        if exe.exists():
            out = app_output(exe, "-v", timeout=10).strip()
            errors += not out
            print(f"[{'OK' if out else 'ERROR'}] {exe.relative_to(ROOT)}: {out or 'does not run'}")
        else:
            print(f"[INFO] {exe.relative_to(ROOT)} not built")
    stm32_tools()
    for tool in ("cmake", "git", "ninja", "arm-none-eabi-gcc"):
        print(f"[{'OK' if shutil.which(tool) else 'INFO'}] {tool} {'found' if shutil.which(tool) else 'not on PATH'}")
    for _, _, family, _, _ in BOARDS.values():
        var = f"STM32CUBE{family}_PATH"
        print(f"[{'OK' if os.environ.get(var) else 'INFO'}] {var} {os.environ.get(var) or 'not set'}")
    print("Selftest passed" if errors == 0 else f"Selftest: {errors} error(s)")
    return 1 if errors else 0


def json_results(output):
    """The JSON result objects of an app run with --json; other lines (driver logs) are ignored."""
    results = []
    for line in output.splitlines():
        if line.startswith('{"command":'):
            try:
                results.append(json.loads(line))
            except ValueError:
                pass
    return results


def doctor():
    errors = selftest()
    print("Doctor:")
    ok = sys.version_info >= (3, 10)
    errors += not ok
    print(f"[{'OK' if ok else 'ERROR'}] Python {platform.python_version()} (3.10+ needed)")
    try:
        import serial  # noqa: F401
        print("[OK] pyserial installed")
    except ImportError:
        print("[INFO] pyserial missing (optional, better port names): pip install pyserial")
    if not IS_WINDOWS:
        groups = subprocess.run(["id", "-Gn"], capture_output=True, text=True).stdout.split()
        ok = "dialout" in groups or os.geteuid() == 0
        errors += not ok
        print(f"[{'OK' if ok else 'ERROR'}] serial access"
              f"{'' if ok else ': add yourself to dialout: sudo usermod -aG dialout $USER, then log in again'}")
        wsl_serial_hint()
    exe = host_exe(False)
    if not exe.exists():
        print("[INFO] app not built; ports and modules not checked (run: ucxapp --build)")
        return 1 if errors else 0
    ports = list_ports(exe)
    print(f"[{'OK' if ports else 'WARNING'}] {len(ports)} serial port(s)")
    modules = module_ports(app_output(exe, "--list-modules", timeout=30))
    if not modules:
        print("[WARNING] no NORA module found: check the USB cable, the EVK power and the FTDI driver")
    for port in modules:
        res = json_results(app_output(exe, port, "--json", "--exec", "device.caps", timeout=30))
        caps = next((r for r in res if r["command"] == "device.caps"), None)
        if caps and caps["success"]:
            d = caps.get("data", {})
            print(f"[OK] {port}: {d.get('model', '?')} firmware {d.get('firmware', '?')}, Wi-Fi {d.get('wi_fi', '?')}")
        else:
            errors += 1
            print(f"[ERROR] {port}: no answer ({caps['error'] if caps else 'no output'}); "
                  "is another program using the port, or is the module in bootloader mode?")
    print("Doctor: no problems found" if errors == 0 else f"Doctor: {errors} problem(s)")
    return 1 if errors else 0


def parse_script(path):
    """[(command, [expect regex...], expect_fail)] from a script file."""
    steps = []
    for n, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith(("expect ", "expect-fail")):
            if not steps:
                fail(f"{path}:{n}: '{line}' before the first command")
            if line == "expect-fail":
                steps[-1][2] = True
            else:
                steps[-1][1].append(line[len("expect "):].strip())
            continue
        steps.append([line, [], False])
    if not steps:
        fail(f"{path}: no commands")
    if len(steps) > 64:
        fail(f"{path}: {len(steps)} commands, the app takes at most 64 per run")
    return steps


def write_junit(path, name, cases):
    from xml.sax.saxutils import escape, quoteattr

    failures = sum(1 for c in cases if c[1] == "FAIL")
    skipped = sum(1 for c in cases if c[1] == "SKIP")
    out = [f'<testsuite name={quoteattr(name)} tests="{len(cases)}" failures="{failures}" skipped="{skipped}">']
    for command, status, detail in cases:
        out.append(f"  <testcase name={quoteattr(command)}>")
        if status == "FAIL":
            out.append(f"    <failure message={quoteattr(detail)}/>")
        elif status == "SKIP":
            out.append("    <skipped/>")
        out.append(f"    <system-out>{escape(detail)}</system-out>")
        out.append("  </testcase>")
    out.append("</testsuite>")
    Path(path).write_text("\n".join(out) + "\n", encoding="utf-8")


def run_script(exe, path, app_args, junit=None):
    steps = parse_script(path)
    cmd = [str(exe), *app_args, "--json"]
    for command, _, _ in steps:
        cmd += ["--exec", command]
    r = subprocess.run(cmd, cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    results = json_results(r.stdout)
    if results and results[0]["command"].startswith("device.connect") and not steps[0][0].startswith("device.connect"):
        connect = results.pop(0)
        if not connect["success"]:
            print(f"connect: {connect.get('error', '')}")
    cases = []
    stopped = False
    for i, (command, expects, expect_fail) in enumerate(steps):
        res = results[i] if i < len(results) and not stopped else None
        if res is None:
            cases.append((command, "SKIP", "not run (an earlier command failed)"))
            continue
        text = "\n".join(res["output"])
        problems = []
        if res["success"] == expect_fail:
            problems.append(f"expected failure, got {res['code']}" if expect_fail else
                            f"{res['code']}: {res.get('error', '')}")
        problems += [f"no match for '{e}'" for e in expects if not re.search(e, text, re.MULTILINE)]
        cases.append((command, "FAIL" if problems else "PASS", "; ".join(problems) or text))
        # The app stops at the first failing command; an expected failure stops it too.
        stopped = not res["success"]
    for command, status, detail in cases:
        print(f"{status:<5} {command}" + (f"\n      {detail}" if status == "FAIL" else ""))
    passed = sum(1 for c in cases if c[1] == "PASS")
    print(f"{passed}/{len(cases)} passed")
    if junit:
        write_junit(junit, Path(path).stem, cases)
        print("JUnit report:", junit)
    return 0 if passed == len(cases) else 1


def main(argv):
    args, extra, parsed_argv = parse(argv)
    if args.release:
        return subprocess.call([sys.executable, str(ROOT / "scripts" / "release.py"), *extra], cwd=ROOT)
    if args.update:
        return update()
    if args.selftest:
        return selftest()
    if args.doctor:
        return doctor()
    if args.script:
        exe = host_exe(args.debug)
        if stale(exe):
            host_build(["Debug" if args.debug else "Release"])
        return run_script(exe, args.script, extra, args.junit)
    if args.everything:
        return everything(args)
    if (args.linux or args.coverage) and IS_WINDOWS:
        require("wsl", "Install WSL with: wsl --install")
        forward = [a for a in parsed_argv if a != "--linux"]
        if "--" in argv:
            forward += argv[argv.index("--"):]
        return subprocess.call(["wsl", "--cd", str(ROOT), "--", "python3", "ucxapp.py", *forward])
    if args.stm32:
        return stm32(args, extra)
    if args.coverage:
        return coverage(extra)
    found = {}
    if args.detect or args.test:
        # Detection and tests use the development exe, never an older signed one.
        exe = host_exe(args.debug)
        if stale(exe):
            host_build(["Debug" if args.debug else "Release"])
        if args.detect:
            found = detect(exe)
            if not args.test:
                return 0 if found else 1
            # Empty = detected as absent, so the tests do not probe the ports again.
            for key in ("UCX_PORT_A", "UCX_PORT_B", "UCX_GNSS_PORT", "UCX_IMU_PORT"):
                found.setdefault(key, "")
        return run_tests(extra, {**found, "UCX_APP_EXE": str(exe)})
    return host(args, extra)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
