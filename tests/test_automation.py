"""--json output, ${NAME} expansion and the launcher's --script runner (no module needed)."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
pytestmark = pytest.mark.app


def _json(app_exe, *commands, env=None):
    args = [str(app_exe), "--offline", "--json"]
    for command in commands:
        args += ["--exec", command]
    out = subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace",
                         stdin=subprocess.DEVNULL, env=env, cwd=ROOT)
    return out, [json.loads(line) for line in out.stdout.splitlines() if line.startswith('{"command":')]


def test_json_result_per_command(app_exe):
    out, res = _json(app_exe, "app.version", "wifi.status")
    assert out.returncode == 1
    assert [r["command"] for r in res] == ["app.version", "wifi.status"]
    assert res[0]["success"] and res[0]["code"] == "OK"
    assert res[1]["code"] == "NOT_CONNECTED" and "not connected" in res[1]["error"]


def test_json_codes(app_exe):
    assert _json(app_exe, "no.such.command")[1][0]["code"] == "UNKNOWN_CMD"
    assert _json(app_exe, "dev.connect")[1][0]["code"] == "USAGE"


def test_help_json_lists_metadata(app_exe):
    out = subprocess.run([str(app_exe), "--offline", "--exec", "help --json dev."], capture_output=True,
                         text=True, timeout=60, errors="replace", stdin=subprocess.DEVNULL)
    cmds = {c["name"]: c for c in json.loads(out.stdout[out.stdout.index("["):])}
    assert cmds["dev.factory"]["danger"] and cmds["dev.factory"]["needsDevice"]
    assert not cmds["dev.info"]["danger"]
    assert "dev.caps" in cmds and "dev.diagnose" in cmds


def test_env_expansion(app_exe):
    env = {**os.environ, "UCX_TEST_PREFIX": "dev."}
    _, res = _json(app_exe, "help ${UCX_TEST_PREFIX}", env=env)
    assert res[0]["success"] and any("dev.info" in line for line in res[0]["output"])
    assert res[0]["command"] == "help ${UCX_TEST_PREFIX}"
    _, res = _json(app_exe, "help ${UCX_TEST_UNSET_VAR}", env=env)
    assert not res[0]["success"] and "UCX_TEST_UNSET_VAR" in res[0]["error"]


def test_script_runner_offline(app_exe, tmp_path):
    junit = tmp_path / "r.xml"
    out = subprocess.run([sys.executable, "ucxapp.py", "--script", "examples/automation/offline.ucx",
                          "--junit", str(junit), "--offline"], cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "2/2 passed" in out.stdout
    assert 'failures="0"' in junit.read_text()


def test_script_runner_reports_failure(app_exe, tmp_path):
    script = tmp_path / "s.ucx"
    script.write_text("app.version\nexpect no-such-text\napp.uptime\n")
    out = subprocess.run([sys.executable, "ucxapp.py", "--script", str(script), "--offline"], cwd=ROOT,
                         capture_output=True, text=True, timeout=120)
    assert out.returncode == 1
    assert "FAIL  app.version" in out.stdout and "PASS  app.uptime" in out.stdout


def test_example_scripts_use_real_commands(app_exe):
    out = subprocess.run([str(app_exe), "--offline", "--exec", "help --json"], capture_output=True,
                         text=True, timeout=60, errors="replace", stdin=subprocess.DEVNULL)
    names = {c["name"] for c in json.loads(out.stdout[out.stdout.index("["):])}
    scripts = sorted((ROOT / "examples").rglob("*.ucx"))
    assert any(s.parent.name == "use-cases" for s in scripts)
    for script in scripts:
        for raw in script.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or line == "expect-fail":
                continue
            if line.startswith("expect "):
                re.compile(line[len("expect "):].strip())
                continue
            assert line.split()[0] in names, f"{script.name}: unknown command in '{line}'"
        assert script.read_text(encoding="utf-8").startswith("#"), f"{script.name}: explain the goal first"


def test_wifi_survey_needs_a_module(app_exe):
    _, res = _json(app_exe, "wifi.survey")
    assert res[0]["code"] == "NOT_CONNECTED"
