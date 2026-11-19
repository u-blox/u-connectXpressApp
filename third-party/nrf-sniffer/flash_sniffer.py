#!/usr/bin/env python3
"""Flash the nRF Sniffer for Bluetooth LE firmware (4.1.1, this folder) for sniff.*.

  py -3 third-party/nrf-sniffer/flash_sniffer.py            # finds the board and flashes it
  py -3 third-party/nrf-sniffer/flash_sniffer.py --board 52840dk

The board must be waiting for new firmware:
  UF2 bootloader (Makerdiary MDK dongle, Seeed XIAO ...): double-tap or hold the button while plugging in;
    a drive with INFO_UF2.TXT appears and the firmware is copied to it as sniffer.uf2.
  Nordic nRF52840 Dongle (PCA10059): press the side RESET button, the red LED pulses; flashed over USB DFU.
  Development kits (nRF52840/52833/52832 DK): over the on-board J-Link.
nrfutil is needed for the last two (with "nrfutil install device nrf5sdk-tools"). Afterwards the board shows up
as "nRF Sniffer" in ".\\ucxapp --detect".
"""
import argparse
import json
import os
import string
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
VERSION = "4.1.1"
IMAGES = {
    "dongle": f"sniffer_nrf52840dongle_nrf52840_{VERSION}.hex",
    "52840dk": f"sniffer_nrf52840dk_nrf52840_{VERSION}.hex",
    "52833dk": f"sniffer_nrf52833dk_nrf52833_{VERSION}.hex",
    "52dk": f"sniffer_nrf52dk_nrf52832_{VERSION}.hex",
}
UF2_FAMILY = {"dongle": 0xADA52840, "52840dk": 0xADA52840, "52833dk": 0x621E937A, "52dk": 0x1B57745F}


def read_hex(path):
    """Intel HEX to {address: byte}."""
    mem, base = {}, 0
    for line in path.read_text().splitlines():
        if not line.startswith(":"):
            continue
        n, addr, kind = int(line[1:3], 16), int(line[3:7], 16), int(line[7:9], 16)
        data = bytes.fromhex(line[9:9 + 2 * n])
        if kind == 0:
            for i, b in enumerate(data):
                mem[base + addr + i] = b
        elif kind == 2:
            base = int.from_bytes(data, "big") << 4
        elif kind == 4:
            base = int.from_bytes(data, "big") << 16
    return mem


def to_uf2(mem, family):
    """256-byte UF2 blocks (https://github.com/microsoft/uf2), gaps filled with 0xFF."""
    pages = sorted({a & ~0xFF for a in mem})
    out = bytearray()
    for i, page in enumerate(pages):
        data = bytes(mem.get(page + j, 0xFF) for j in range(256))
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, page, 256, i, len(pages), family)
        out += data + bytes(476 - 256) + struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def uf2_drives():
    if os.name == "nt":
        roots = [Path(f"{c}:/") for c in string.ascii_uppercase]
    else:
        roots = [p for base in ("/media", "/run/media", "/mnt") for p in Path(base).glob("**/") if p.is_dir()][:200]
    return [r for r in roots if (r / "INFO_UF2.TXT").is_file()]


def nrfutil(*args, capture=False):
    cmd = ["nrfutil", *args]
    print("> " + " ".join(cmd))
    try:
        return subprocess.run(cmd, check=True, text=True, capture_output=capture)
    except FileNotFoundError:
        sys.exit("nrfutil not found: install it from nordicsemi.com (nRF Util), then: nrfutil install device")


def devices():
    out = nrfutil("device", "list", "--json", capture=True).stdout
    found = []
    for line in out.splitlines():
        try:
            found += json.loads(line).get("data", {}).get("devices", [])
        except (ValueError, AttributeError):
            pass
    return found


def flash_uf2(drive, board):
    info = (drive / "INFO_UF2.TXT").read_text(errors="replace")
    print(f"UF2 bootloader on {drive}: " + " / ".join(info.splitlines()[1:3]))
    if board in ("dongle", "52840dk") and "52840" not in info:
        sys.exit("this UF2 board is not an nRF52840; give --board")
    (drive / "sniffer.uf2").write_bytes(to_uf2(read_hex(HERE / IMAGES[board]), UF2_FAMILY[board]))
    print("copied sniffer.uf2: the board restarts as the sniffer")


def flash_dfu(serial):
    with tempfile.TemporaryDirectory() as tmp:
        pkg = Path(tmp) / "sniffer_dfu.zip"
        nrfutil("nrf5sdk-tools", "pkg", "generate", "--hw-version", "52", "--sd-req", "0x00", "--application",
                str(HERE / IMAGES["dongle"]), "--application-version", "1", str(pkg))
        nrfutil("device", "program", "--serial-number", serial, "--firmware", str(pkg), "--traits", "nordicDfu")


def flash_jlink(serial, board):
    nrfutil("device", "program", "--serial-number", serial, "--firmware", str(HERE / IMAGES[board]),
            "--traits", "jlink", "--options", "chip_erase_mode=ERASE_ALL,verify=VERIFY_READ")
    nrfutil("device", "reset", "--serial-number", serial)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", choices=sorted(IMAGES), help="which image (default: dongle, or 52840dk over J-Link)")
    ap.add_argument("--serial", help="nrfutil serial number when more than one board is attached")
    args = ap.parse_args()

    drives = uf2_drives()
    if drives and args.serial is None:
        return flash_uf2(drives[0], args.board or "dongle")
    devs = [d for d in devices() if args.serial in (None, d.get("serialNumber"))]
    dfu = [d for d in devs if d.get("traits", {}).get("nordicDfu")]
    jlink = [d for d in devs if d.get("traits", {}).get("jlink")]
    if dfu and args.board in (None, "dongle"):
        return flash_dfu(dfu[0]["serialNumber"])
    if jlink:
        return flash_jlink(jlink[0]["serialNumber"], args.board or "52840dk")
    sys.exit("no board waiting for firmware: put it in its bootloader (see --help) and run this again")


if __name__ == "__main__":
    main()
