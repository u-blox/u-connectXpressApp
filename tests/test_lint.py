"""Static checks: ruff for Python, shellcheck for shell scripts and CI bash steps."""

import fnmatch
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SKIP_DIRS = {"u-connectClient", "build", ".git", "node_modules", "third-party"}


def _files(pattern):
    found = []
    for top, dirs, names in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]  # prune: walking build/ is very slow on /mnt
        found += (Path(top) / n for n in fnmatch.filter(names, pattern))
    return sorted(found)


def _run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)


def _bash_steps():
    """Yield (name, script) for every `shell: bash` run block in the GitHub workflows."""
    pattern = re.compile(
        r"^(?P<indent>[ ]*)(?:- )?run:[ ]*\|[ ]*\n(?P<body>(?:(?P=indent)[ ]{2,}.*\n|[ ]*\n)+)", re.MULTILINE
    )
    for wf in (ROOT / ".github" / "workflows").glob("*.y*ml"):
        text = wf.read_text(encoding="utf-8")
        for i, m in enumerate(pattern.finditer(text)):
            # Only check steps that explicitly select bash.
            window = text[max(0, m.start() - 200) : m.end() + 200]
            if "shell: bash" in window:
                body = "\n".join(line[len(m.group("indent")) + 2 :] for line in m.group("body").splitlines())
                yield f"{wf.name}#{i}", body


@pytest.mark.skipif(shutil.which("ruff") is None, reason="ruff not installed")
def test_ruff_check():
    result = _run(["ruff", "check", "."])
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.skipif(shutil.which("shellcheck") is None, reason="shellcheck not installed")
@pytest.mark.parametrize("script", _files("*.sh"), ids=lambda p: str(p.relative_to(ROOT)))
def test_shellcheck_scripts(script):
    result = _run(["shellcheck", "--severity=style", str(script)])
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.skipif(shutil.which("shellcheck") is None, reason="shellcheck not installed")
@pytest.mark.parametrize("step", list(_bash_steps()), ids=lambda s: s[0])
def test_shellcheck_workflow_bash_steps(step):
    _, script = step
    result = subprocess.run(
        ["shellcheck", "--shell=bash", "-"], input=script.encode(), cwd=ROOT, capture_output=True, check=False
    )
    out = (result.stdout + result.stderr).decode(errors="replace")
    assert result.returncode == 0, out
