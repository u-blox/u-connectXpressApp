"""Run ucxapp commands from a Python program (docs/INTEGRATION.md, way 1)."""

import subprocess
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[2] / "app" / "bin" / ("ucxapp.exe" if sys.platform == "win32" else "ucxapp")


def ucxapp_exec(port, commands, timeout=120):
    """Run commands in one ucxapp session; return (all passed, output)."""
    args = [str(APP), port]
    for command in commands:
        args += ["--exec", command]
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return result.returncode == 0, result.stdout + result.stderr


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit("usage: ucxapp_exec.py <port> <command> [command ...]")
    ok, output = ucxapp_exec(sys.argv[1], sys.argv[2:])
    print(output, end="")
    sys.exit(0 if ok else 1)
