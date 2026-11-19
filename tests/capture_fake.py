"""Stand-ins for the capture tools ucxapp starts (tests/test_capture.py): nrfutil, ssh and Wireshark.

Usage: capture_fake.py nrfutil|ssh|wireshark <args...>. Every call writes its argv to $UCX_FAKE_DIR/calls.txt.
Captures write a pcap with one packet every 20 ms until interrupted or until $UCX_FAKE_DIR/stop exists.
"""

import os
import signal
import struct
import sys
import time
from pathlib import Path

DIR = Path(os.environ.get("UCX_FAKE_DIR", "."))
PCAP_HEADER = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 105)
stopping = False


def _stop(*_):
    global stopping
    stopping = True


def _capture(out, max_packets=500):
    for sig in ("SIGINT", "SIGBREAK", "SIGTERM"):
        if hasattr(signal, sig):
            signal.signal(getattr(signal, sig), _stop)
    out.write(PCAP_HEADER)
    out.flush()
    for i in range(max_packets):
        if stopping or (DIR / "stop").exists():
            break
        data = bytes([i & 0xFF]) * 24
        out.write(struct.pack("<IIII", i, 0, len(data), len(data)) + data)
        out.flush()
        time.sleep(0.02)


def nrfutil(args):
    path = args[args.index("--output-pcap-file") + 1]
    if os.environ.get("UCX_FAKE_FAIL") == "nrfutil":
        print("Error: could not open port", file=sys.stderr)
        sys.exit(2)
    with open(path, "wb") as f:
        _capture(f)


def ssh(args):
    script = args[-1]
    if "kill -INT" in script:
        (DIR / "stop").touch()
    elif "tcpdump" in script:
        if os.environ.get("UCX_FAKE_FAIL") == "sudo":
            print("sudo: a password is required", file=sys.stderr)
            sys.exit(1)
        print("UCXPID=4242", file=sys.stderr, flush=True)
        print("tcpdump: listening on wlan0, link-type IEEE802_11_RADIO", file=sys.stderr, flush=True)
        _capture(sys.stdout.buffer)
    elif "UCXLOGIN" in script:
        print("UCXLOGIN")
        if os.environ.get("UCX_FAKE_FAIL") != "sudo":
            print("UCXSUDO")
    elif script.startswith("iw dev"):
        print("Interface wlan0\n\ttype monitor\n\tchannel 6 (2437 MHz)")


def wireshark(args):
    if "-r" in args:
        return
    with open(DIR / "wireshark.pcap", "wb") as f:
        while chunk := sys.stdin.buffer.read(4096):
            f.write(chunk)
            f.flush()


def main():
    tool, args = sys.argv[1], sys.argv[2:]
    with open(DIR / "calls.txt", "a", encoding="utf-8") as f:
        f.write(tool + " " + " ".join(args) + "\n")
    {"nrfutil": nrfutil, "ssh": ssh, "wireshark": wireshark}[tool](args)


if __name__ == "__main__":
    main()
