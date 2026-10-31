"""Which module commands (UCXAPP_CMD_NEEDS_DEVICE) the hardware tests exercise.

A new module command must get a hardware test or be listed in UNTESTED with a reason.
`python tests/test_hw_coverage.py` prints the coverage table.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTRY = re.compile(r'\{\s*"([a-z][a-z0-9]*(?:\.[a-z0-9]+)+)",\s*"[^\n]*?(UCXAPP_CMD_NEEDS_DEVICE|\b0\b)\s*,\s*\w+\s*\}')

# Destructive, interactive or needing extra hardware; covered by hand.
UNTESTED = {
    "dev.factory": "erases the module",
    "at.reset": "alias of dev.reboot (same handler), tested there",
    "fw.flash": "rewrites the firmware",
    "fw.download": "large download",
    "bt.confirm": "needs a pairing peer waiting for an answer",
    "bt.passkey": "needs a pairing peer waiting for an answer",
    "hid.key": "needs a HID host (a PC or phone) connected",
    "hid.text": "needs a HID host (a PC or phone) connected",
    "hid.mouse": "needs a HID host (a PC or phone) connected",
    "hid.click": "needs a HID host (a PC or phone) connected",
    "hid.media": "needs a HID host (a PC or phone) connected",
    "hid.client": "needs a bonded BLE keyboard, mouse or remote",
    "tftp.off": "the hardware test runs tftp.on with a time limit, which stops the server itself",
    "tftp.status": "host-side state only, covered by tests/test_tftp.py",
    "ft.on": "needs two connected modules; no two-module BLE rig yet",
    "ft.off": "needs two connected modules; no two-module BLE rig yet",
    "ft.status": "host-side state only, covered by tests/test_ft.py",
    "ft.get": "needs two connected modules; no two-module BLE rig yet",
    "ft.put": "needs two connected modules; no two-module BLE rig yet",
    "ft.ls": "needs two connected modules; no two-module BLE rig yet",
    "ft.rm": "needs two connected modules; no two-module BLE rig yet",
    "ancs.on": "needs an iPhone pairing by hand",
    "ancs.sub": "needs a paired iPhone",
    "ancs.off": "needs a paired iPhone",
    "ancs.list": "needs a paired iPhone",
    "ancs.show": "needs a paired iPhone",
    "ancs.action": "needs a paired iPhone",
    "notify.on": "needs a phone companion app writing notifications",
    "notify.action": "needs a phone companion app subscribed to Action",
    "prod.test": "writes report files; run by hand on a production rig",
    "gatt.client.wit": "needs a WitMotion BLE IMU",
    "gatt.server.hostchar": "needs a scripted peer reading/writing at the right moment",
    "gatt.server.readresp": "needs a scripted peer reading/writing at the right moment",
    "gatt.server.writeresp": "needs a scripted peer reading/writing at the right moment",
    "gatt.server.error": "needs a scripted peer reading/writing at the right moment",
}


def module_commands():
    cmds = {}
    for src in sorted((ROOT / "app" / "src").glob("ucxapp_cmds_*.c")):
        for name, flag in ENTRY.findall(src.read_text(encoding="utf-8", errors="replace")):
            if flag == "UCXAPP_CMD_NEEDS_DEVICE":
                cmds[name] = src.name
    return cmds


def covered(cmds):
    text = "\n".join(p.read_text(encoding="utf-8") for p in sorted((ROOT / "tests" / "hardware").glob("*.py")))
    # Expand f-string commands such as gatt.client.{key} with their parametrize values.
    for name, values in re.findall(r'parametrize\("(\w+)", \[([^\]]*)\]', text):
        lines = [line for line in text.splitlines() if "{" + name + "}" in line]
        for value in re.findall(r'"([\w.-]+)"', values):
            text += "\n" + "\n".join(line.replace("{" + name + "}", value) for line in lines)
    return {c for c in cmds if re.search(r'["\s{]' + re.escape(c) + r'(?=[\s"{])', text)}


def test_module_commands_are_found():
    assert len(module_commands()) > 50


def test_every_module_command_has_a_hardware_test_or_a_reason():
    cmds = module_commands()
    missing = sorted(set(cmds) - covered(cmds) - set(UNTESTED))
    report = ROOT / "build" / "hw_coverage.txt"
    report.parent.mkdir(exist_ok=True)
    report.write_text("".join(f"{c}  ({cmds[c]})\n" for c in missing), encoding="utf-8")
    assert len(missing) <= BASELINE, (f"{len(missing)} module commands without a hardware test (was {BASELINE}); "
                                      f"add a test or a reason in UNTESTED:\n  " + "\n  ".join(missing))


# Ratchet: lower this whenever tests are added, never raise it.
BASELINE = 0


if __name__ == "__main__":
    all_cmds = module_commands()
    hit = covered(all_cmds)
    for name in sorted(all_cmds):
        mark = "ok  " if name in hit else ("skip" if name in UNTESTED else "--  ")
        print(f"{mark} {name}")
    print(f"\n{len(hit)}/{len(all_cmds)} module commands covered by tests/hardware "
          f"({100 * len(hit) // max(len(all_cmds), 1)}%), {len(UNTESTED)} excused")
