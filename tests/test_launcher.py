"""ucxapp.py: when a host build is needed."""

import io
import os
import subprocess
import tarfile

import ucxapp


def _tree(tmp_path):
    for rel in ("app/src/ucxapp_main.c", "app/CMakeLists.txt", "u-connectClient/src/u_cx_at_client.c",
                "u-connectClient/ports/uart/u_port_uart_linux.c",
                "u-connectClient/ports/extra/stm32h7/STM32CubeH7/hal.c", "app/bin/ucxapp"):
        f = tmp_path / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text("x")
        os.utime(f, (1000, 1000))
    exe = tmp_path / "app" / "bin" / "ucxapp"
    os.utime(exe, (2000, 2000))
    return exe


def test_a_u_connect_client_change_needs_a_rebuild(tmp_path, monkeypatch):
    monkeypatch.setattr(ucxapp, "ROOT", tmp_path)
    exe = _tree(tmp_path)
    assert not ucxapp.stale(exe)
    os.utime(tmp_path / "u-connectClient/ports/uart/u_port_uart_linux.c", (3000, 3000))
    assert ucxapp.stale(exe)


def test_vendor_trees_the_host_build_does_not_compile_are_ignored(tmp_path, monkeypatch):
    monkeypatch.setattr(ucxapp, "ROOT", tmp_path)
    exe = _tree(tmp_path)
    os.utime(tmp_path / "u-connectClient/ports/extra/stm32h7/STM32CubeH7/hal.c", (3000, 3000))
    assert not ucxapp.stale(exe)


def _git(cwd, *args):
    subprocess.run(["git", "-c", "core.autocrlf=false", *args], cwd=cwd, check=True, capture_output=True)


def test_linux_host_sync_sends_a_linux_checkout_and_only_changes(tmp_path, monkeypatch):
    repo, remote = tmp_path / "repo", tmp_path / "remote"
    repo.mkdir()
    _git(repo, "init", "-q")
    (repo / ".gitattributes").write_bytes(b"*.cmd text eol=crlf\n")
    (repo / ".gitignore").write_bytes(b"build/\n")
    (repo / "a.c").write_bytes(b"int a;\n")
    (repo / "run.cmd").write_bytes(b"@echo off\r\n")
    (repo / "blob.bin").write_bytes(b"\x00\r\n\x01")
    _git(repo, "add", ".")
    _git(repo, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "x")
    (repo / "a.c").write_bytes(b"int a;\r\nint b;\r\n")      # Windows checkout, uncommitted edit
    (repo / "new.py").write_bytes(b"print(1)\n")             # untracked
    monkeypatch.setattr(ucxapp, "ROOT", repo)
    monkeypatch.setattr(ucxapp, "require", lambda *a: None)
    sent = []
    real_run = subprocess.run

    def fake_run(cmd, **kw):
        if cmd[0] != "ssh":
            return real_run(cmd, **kw)
        if "input" not in kw:
            marker = remote / ".ucxapp-sync"
            return subprocess.CompletedProcess(cmd, 0, marker.read_text() if marker.exists() else "", "")
        remote.mkdir(exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(kw["input"])) as tar:
            names = [m.name for m in tar.getmembers()]
            tar.extractall(remote, filter="data")
        for name in filter(None, (remote / ".ucxapp-deleted").read_text().split("\n")):
            (remote / name).unlink()
        sent.append(sorted(set(names) - {".ucxapp-sync", ".ucxapp-deleted"}))
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(subprocess, "run", fake_run)
    ucxapp.linux_host_sync("me@box:/srv/ucx")
    assert sent[-1] == [".gitattributes", ".gitignore", "a.c", "blob.bin", "new.py", "run.cmd"]
    assert (remote / "a.c").read_bytes() == b"int a;\nint b;\n"
    assert (remote / "run.cmd").read_bytes() == b"@echo off\r\n"
    assert (remote / "blob.bin").read_bytes() == b"\x00\r\n\x01"

    (repo / "new.py").unlink()
    (repo / "a.c").write_bytes(b"int c;\r\n")
    ucxapp.linux_host_sync("me@box:/srv/ucx")
    assert sent[-1] == ["a.c"]
    assert not (remote / "new.py").exists()

    (remote / ".ucxapp-sync").write_text("someone else")    # remote tree replaced: send everything again
    ucxapp.linux_host_sync("me@box:/srv/ucx")
    assert len(sent[-1]) == 5


def test_linux_host_paths_are_quoted_for_the_remote_shell():
    assert ucxapp.linux_host_split("me@box") == ("me@box", "~/ucxapp")
    assert ucxapp.linux_host_split("me@box:/srv/a b") == ("me@box", "/srv/a b")
    assert ucxapp.remote_path("~/my dir") == "~/'my dir'"


def test_linux_host_run_forwards_the_test_variables(monkeypatch):
    calls = []
    monkeypatch.setattr(ucxapp, "linux_host_sync", lambda spec: ("me@box", "~/ucxapp"))
    monkeypatch.setattr(ucxapp.subprocess, "call", lambda cmd: calls.append(cmd) or 0)
    monkeypatch.setenv("UCX_WIFI_SSID", "my net")
    monkeypatch.setenv("UCX_LINUX_HOST", "me@box")
    ucxapp.linux_host_run("me@box", ["--test", "tests/hardware"])
    remote = calls[0][-1]
    assert "UCX_WIFI_SSID='my net'" in remote and "UCX_LINUX_HOST" not in remote
    assert remote.endswith("python3 ucxapp.py --test tests/hardware")
