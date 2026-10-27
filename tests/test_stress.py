"""Offline stress: the shell must survive every command with bad arguments and a flood of garbage input."""

import json
import random
import re
import subprocess

import pytest

pytestmark = pytest.mark.app

# Commands that wait for a peer, probe serial ports or never return on their own.
BLOCKING = re.compile(r"detect|bridge|listen|echo|start|connect|\.use$|scan|monitor|dash|tui|sleep|wait|run|shell|"
                      r"exit|quit|flash|download|ntrip|repeat|watch|serve|portal|iperf|ping|\.ex$")
BAD_ARGS = ["", "x", "-1", "99999999999999999999", "0 0 0 0 0 0 0 0 0", '"unterminated', "A" * 600,
            "\u00e6\u00f8\u00e5 \u2603"]


def _shell(exe, lines, timeout=300):
    out = subprocess.run([str(exe), "COM255", "--shell"], input="\n".join([*lines, "quit", ""]),
                         capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    return out


def _commands(exe):
    text = _shell(exe, ["help --json"], timeout=60).stdout
    return json.loads(text[text.index("[\n{"):text.rindex("]") + 1])


def test_every_command_survives_bad_arguments(app_exe):
    names = [c["name"] for c in _commands(app_exe) if not c["danger"] and not BLOCKING.search(c["name"])]
    assert len(names) > 50
    lines = [f"{name} {arg}".rstrip() for name in names for arg in BAD_ARGS]
    out = _shell(app_exe, [*lines, "app.version"])
    assert out.returncode == 0, f"exit {out.returncode:#x}\n{out.stdout[-1500:]}"
    assert "u-connectXpressApp" in out.stdout.rsplit("ucxapp>", 3)[-2], out.stdout[-800:]


def test_garbage_input_flood(app_exe):
    rng = random.Random(1234)
    alphabet = "abcxyz.0129 -_\"'\\|;&%$#\t\u00e6\u2603"
    lines = ["".join(rng.choice(alphabet) for _ in range(rng.randint(0, 80))) for _ in range(3000)]
    lines += ["x" * 20000, "\t" * 500, ".", "..", "help " * 400]
    out = _shell(app_exe, [*lines, "app.version"])
    assert out.returncode == 0, f"exit {out.returncode:#x}\n{out.stdout[-1500:]}"
    assert "u-connectXpressApp" in out.stdout[-3000:], out.stdout[-800:]


def test_repeated_start_and_quit(app_exe):
    for i in range(30):
        out = _shell(app_exe, ["help"], timeout=30)
        assert out.returncode == 0, f"run {i}: exit {out.returncode:#x}\n{out.stdout[-800:]}"


def test_many_exec_commands(app_exe):
    args = [str(app_exe), "--offline"] + ["--exec", "app.uptime"] * 600
    out = subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace")
    assert out.returncode == 0, out.stdout[-800:]
    assert len(re.findall(r"\d+ days \d\d:\d\d:\d\d", out.stdout)) == 600, out.stdout[-800:]
    out = subprocess.run(args + ["--exec", "app.uptime"] * 500, capture_output=True, text=True, timeout=60,
                         errors="replace")
    assert out.returncode == 2 and "more than 1024 --exec" in out.stdout, out.stdout[-800:]
