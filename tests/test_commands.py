"""Every registered command must be documented, listed by `help`, and every source file must be built."""

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "app"
SRC = APP / "src"
ENTRY = re.compile(
    r'^\s*\{ "([a-z][a-z0-9.]*)",\s*"[^"]*",\s*"(?:[^"\\]|\\.)*",\s*'
    r'(?:UCXAPP_CMD_\w+(?:\s*\|\s*UCXAPP_CMD_\w+)*|0),\s*\w+\s*\},',
    re.M,
)

NEW_COMMANDS = (
    "web.start", "web.stop", "web.status",
    "hid.setup", "hid.status", "hid.key", "hid.text", "hid.mouse", "hid.click", "hid.media", "hid.client",
    "utils.qr.text", "utils.qr.wifi", "utils.qr.geo", "utils.zip", "utils.unzip",
    "gnss.detect", "gnss.open", "gnss.close", "gnss.status", "gnss.sats", "gnss.log", "gnss.send",
    "gnss.restart", "gnss.rate", "gnss.bridge",
    "cell.detect", "cell.open", "cell.close", "cell.status", "cell.at", "cell.apn", "cell.attach",
    "cell.ping", "cell.bridge", "cell.connect", "cell.disconnect",
    "imu.detect", "imu.open", "imu.close", "imu.status", "imu.log", "imu.send", "imu.rate", "imu.bridge",
    "wifi.ap.enable", "wifi.ap.disable", "wifi.ap.status",
    "wifi.profile.add", "wifi.profile.list", "wifi.profile.remove", "wifi.profile.connect",
    "wifi.profile.auto", "wifi.regdomain",
    "gatt.writelong", "gatt.wps", "file.hash",
    "bt.defaults", "bt.bonds", "bt.unbond",
    "ft.on", "ft.off", "ft.status", "ft.get", "ft.put", "ft.ls", "ft.rm", "ft.selftest", "app.time",
)


def registered_commands():
    names = set()
    for path in SRC.glob("ucxapp_cmds_*.c"):
        names.update(ENTRY.findall(path.read_text(encoding="utf-8")))
    return names


def _doc_name(name):
    # The reference documents the per-service examples as gatt.client.<svc>.
    return re.sub(r"^(gatt\.(?:client|server)\.)(?:hrs|cts|ess|lns|aio|bas|dis|nus|sps|wit|wps|ras)$", r"\1<svc>", name)


def test_command_scan_finds_the_new_packs():
    names = registered_commands()
    assert len(names) > 60
    for name in NEW_COMMANDS:
        assert name in names, f"{name} is not registered in src/"


def test_every_command_is_documented():
    doc = (ROOT / "COMMANDS.md").read_text(encoding="utf-8")
    missing = sorted(n for n in registered_commands() if f"`{_doc_name(n)}" not in doc)
    assert not missing, f"undocumented in COMMANDS.md: {missing}"


def test_every_source_file_is_built_by_both_targets():
    cmake = (APP / "CMakeLists.txt").read_text(encoding="utf-8")
    platform_only = {"ucxapp_platform_win.c", "ucxapp_platform_posix.c"}
    for src in sorted(SRC.glob("*.c")):
        count = len(re.findall(rf"src/{re.escape(src.name)}\b", cmake))
        want = 1 if src.name in platform_only else 2
        assert count == want, f"{src.name} appears {count}x in CMakeLists.txt, expected {want} (one per target)"


def test_web_server_sources_are_present():
    for name in ("u_webserver.c", "u_webserver.h", "ucxapp_cmds_web.c", "ucxapp_thread.c", "ucxapp_thread.h"):
        assert (SRC / name).is_file(), name
    assert not (SRC / "ucxapp_web.c").exists(), "PC-hosted web server was replaced by the module-hosted one"


def test_web_server_reads_up_to_2048_bytes():
    text = (SRC / "u_webserver.c").read_text(encoding="utf-8")
    assert text.count("rx_space > 2048") == 2
    assert "rx_space > 1000" not in text


@pytest.mark.app
def test_help_lists_every_command(app_exe):
    out = subprocess.run([str(app_exe), "COM255", "--exec", "help"], capture_output=True, text=True, timeout=30,
                         errors="replace")
    assert out.returncode == 0
    missing = sorted(n for n in registered_commands() if n not in out.stdout)
    assert not missing, f"missing from help: {missing}"
