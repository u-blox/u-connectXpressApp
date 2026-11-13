"""ucxapp.py: when a host build is needed."""

import io
import os
import subprocess
import tarfile

import pytest

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


@pytest.mark.skipif(not (ucxapp.IS_WINDOWS and ucxapp.host_exe(True).with_suffix(".pdb").exists()),
                    reason="needs the Windows Debug build (.\\ucxapp --debug --build)")
def test_symbolize_names_the_function_of_an_absolute_address_or_an_offset(capsys):
    exe = ucxapp.host_exe(True)
    pe = exe.read_bytes()
    nt = int.from_bytes(pe[0x3C:0x40], "little")
    u16, u32 = (lambda o: int.from_bytes(pe[o:o + 2], "little")), (lambda o: int.from_bytes(pe[o:o + 4], "little"))
    sections = nt + 24 + u16(nt + 20)

    def at(rva):   # file offset of an address in the exe
        for s in range(sections, sections + 40 * u16(nt + 6), 40):
            if u32(s + 12) <= rva < u32(s + 12) + u32(s + 16):
                return rva - u32(s + 12) + u32(s + 20)

    entry = u32(nt + 40)   # AddressOfEntryPoint: a jump (incremental linking) to mainCRTStartup
    if pe[at(entry)] == 0xE9:
        entry = (entry + 5 + int.from_bytes(pe[at(entry) + 1:at(entry) + 5], "little", signed=True)) & 0xFFFFFFFF
    assert ucxapp.symbolize(exe, 0x140000000, [0x140000000 + entry, entry, 0x7FFFFFF0]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[0].split()[1] == lines[1].split()[1] == "mainCRTStartup+0x0"
    assert lines[2] == "0x7ffffff0  ?"


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


def test_publish_web_tests_then_commits_only_the_web_and_pushes_master(monkeypatch, capsys):
    steps = []
    outs = {("rev-parse", "--abbrev-ref", "HEAD"): "master",
            ("remote", "get-url", "origin"): "https://github.com/U-Blox/u-connectXpressApp.git"}
    monkeypatch.setattr(ucxapp, "require", lambda *a: None)
    monkeypatch.setattr(ucxapp, "git_out", lambda *cmd: outs[cmd])
    monkeypatch.setattr(ucxapp, "matter_wasm_build", lambda: steps.append("node"))
    monkeypatch.setattr(ucxapp, "wasm_build", lambda: steps.append("build"))
    monkeypatch.setattr(ucxapp, "check", lambda cmd, what, **kw: steps.append(what))
    monkeypatch.setattr(ucxapp.subprocess, "call", lambda cmd, **kw: 1)  # staged web changes
    assert ucxapp.main(["--publish-web", "new look"]) == 0
    assert steps == ["node", "build", "web tests", "git add", "git commit", "git push"]
    assert "https://u-blox.github.io/u-connectXpressApp/" in capsys.readouterr().out


def _matter_default(monkeypatch, tmp_path, gcc=True, plugin_age=None, build=None):
    """The launcher with its builds replaced by a list of what it would build; the plug-in file is plugin_age
    seconds older than its sources (None: not built yet)."""
    steps = []
    plugin = tmp_path / "ucxmatter.dll"
    if plugin_age is not None:
        plugin.write_text("x")
        os.utime(plugin, (5000 - plugin_age, 5000 - plugin_age))
    monkeypatch.delenv("UCX_APP_EXE", raising=False)
    monkeypatch.setattr(ucxapp, "host_build", lambda configs, fresh=False: steps.append("app"))
    monkeypatch.setattr(ucxapp, "matter_build", build or (lambda keylog=False: steps.append("plug-in")))
    monkeypatch.setattr(ucxapp, "matter_gcc", lambda: gcc)
    monkeypatch.setattr(ucxapp, "matter_plugin", lambda: plugin)
    monkeypatch.setattr(ucxapp, "matter_sources", lambda: 5000)
    monkeypatch.setattr(ucxapp, "stale", lambda exe: False)
    monkeypatch.setattr(ucxapp, "SIGNED", tmp_path / "none.exe")
    monkeypatch.setattr(ucxapp, "run_app", lambda exe, extra: steps.append("start") or 0)
    return steps


@pytest.mark.parametrize("argv, built", [(["--build"], ["app", "plug-in"]), (["--rebuild"], ["app", "plug-in"]),
                                         (["--build", "--no-matter"], ["app"])])
def test_a_host_build_includes_the_matter_plugin(monkeypatch, tmp_path, argv, built):
    steps = _matter_default(monkeypatch, tmp_path, plugin_age=0)   # an explicit build always brings it up to date
    monkeypatch.setattr(ucxapp, "host_clean", lambda configs: None)
    assert ucxapp.main(argv) == 0
    assert steps == built


def test_a_host_build_without_gcc_builds_the_app_and_says_what_matter_needs(monkeypatch, tmp_path, capsys):
    steps = _matter_default(monkeypatch, tmp_path, gcc=False)
    assert ucxapp.main(["--build"]) == 0
    assert steps == ["app"]
    assert "Matter plug-in not built: g++ not found" in capsys.readouterr().out


def test_a_failing_plugin_build_fails_an_explicit_build(monkeypatch, tmp_path):
    _matter_default(monkeypatch, tmp_path, build=lambda keylog=False: ucxapp.fail("ninja failed"))
    with pytest.raises(SystemExit):
        ucxapp.main(["--build"])


@pytest.mark.parametrize("plugin_age, gcc, steps_expected", [
    (0, True, ["start"]),                  # up to date: nothing to build
    (10, True, ["plug-in", "start"]),      # its sources changed
    (None, True, ["plug-in", "start"]),    # not built yet
    (None, False, ["start"]),              # no GCC: the app starts, and says nothing on every start
])
def test_a_plain_start_builds_the_plugin_only_when_it_is_stale(monkeypatch, tmp_path, capsys, plugin_age, gcc,
                                                               steps_expected):
    steps = _matter_default(monkeypatch, tmp_path, gcc=gcc, plugin_age=plugin_age)
    assert ucxapp.main(["COM1"]) == 0
    assert steps == steps_expected
    assert "not built" not in capsys.readouterr().out


def test_a_plain_start_runs_the_app_when_the_plugin_does_not_build(monkeypatch, tmp_path, capsys):
    steps = _matter_default(monkeypatch, tmp_path, build=lambda keylog=False: ucxapp.fail("ninja failed"))
    assert ucxapp.main(["COM1"]) == 0
    assert steps == ["start"]
    assert "starting without it" in capsys.readouterr().out


def test_the_tests_own_exe_is_never_built_for(monkeypatch, tmp_path):
    steps = _matter_default(monkeypatch, tmp_path)
    monkeypatch.setenv("UCX_APP_EXE", str(tmp_path / "ucxapp"))
    assert ucxapp.main(["--build"]) == 0
    assert steps == ["app"]


@pytest.mark.parametrize("argv, built", [(["--wasm", "--build"], ["node", "site with node"]),
                                         (["--wasm", "--build", "--no-matter"], ["site without node"])])
def test_the_browser_build_includes_the_matter_node(monkeypatch, tmp_path, argv, built):
    steps = []
    lib = tmp_path / "libucxmatter.a"
    monkeypatch.setattr(ucxapp, "WASM_DIR", tmp_path / "wasm")
    monkeypatch.setattr(ucxapp, "MATTER_WASM_LIB", lib)
    monkeypatch.setattr(ucxapp, "matter_wasm_build", lambda: steps.append("node") or lib.write_text("x"))
    monkeypatch.setattr(ucxapp, "wasm_build", lambda node=True: steps.append(f"site with{'' if node else 'out'} node"))
    assert ucxapp.main(argv) == 0
    assert steps == built


def test_publish_web_refuses_a_branch_pages_does_not_deploy(monkeypatch):
    monkeypatch.setattr(ucxapp, "require", lambda *a: None)
    monkeypatch.setattr(ucxapp, "git_out", lambda *cmd: "feature")
    monkeypatch.setattr(ucxapp, "wasm_build", lambda: pytest.fail("built on the wrong branch"))
    with pytest.raises(SystemExit):
        ucxapp.publish_web("x")


def _matter_tree(tmp_path, monkeypatch, head):
    sdk, patches = tmp_path / "connectedhomeip", tmp_path / "patches"
    (sdk / "src" / "system").mkdir(parents=True)
    (patches / "src" / "system").mkdir(parents=True)
    (sdk / "src" / "system" / "Layer.h").write_text("sdk")
    (sdk / "src" / "system" / "Same.h").write_text("same")
    (patches / "src" / "system" / "Layer.h").write_text("patched")
    (patches / "src" / "system" / "Same.h").write_text("same")
    monkeypatch.setattr(ucxapp, "MATTER_DIR", tmp_path)
    monkeypatch.setattr(ucxapp, "MATTER_SDK", sdk)
    monkeypatch.setattr(ucxapp, "git_out", lambda *cmd: head if "rev-parse" in cmd
                        else "160000 " + "a" * 40 + " 0\tmatter/connectedhomeip")
    return sdk / "src" / "system" / "Layer.h"


def test_matter_patches_are_copied_over_the_pinned_sdk(tmp_path, monkeypatch, capsys):
    target = _matter_tree(tmp_path, monkeypatch, "a" * 40)
    ucxapp.matter_patch()
    assert target.read_text() == "patched"
    assert "Patched 1 Matter SDK file(s)" in capsys.readouterr().out   # the file that already matched is left alone
    ucxapp.matter_patch()
    assert "Patched" not in capsys.readouterr().out


def test_matter_patches_refuse_another_sdk_version(tmp_path, monkeypatch, capsys):
    target = _matter_tree(tmp_path, monkeypatch, "b" * 40)
    with pytest.raises(SystemExit):
        ucxapp.matter_patch()
    assert target.read_text() == "sdk"
    assert "matter/patches is made for aaaaaaaaaa" in capsys.readouterr().err


def test_matter_sdk_fetch_is_one_commit_of_the_folders_the_node_needs(tmp_path, monkeypatch):
    sdk = tmp_path / "connectedhomeip"
    monkeypatch.setattr(ucxapp, "MATTER_SDK", sdk)
    monkeypatch.setattr(ucxapp, "require", lambda *a: None)
    tags = f"{'b' * 40}\trefs/tags/v1.5.0.0\n{'c' * 40}\trefs/tags/v1.6.0.0\n{'a' * 40}\trefs/tags/v1.6.0.0^{{}}"
    monkeypatch.setattr(ucxapp, "git_out", lambda *cmd: "160000 " + "a" * 40 + " 0\tmatter/connectedhomeip"
                        if "ls-files" in cmd else tags if "ls-remote" in cmd else "https://example.invalid/sdk.git")
    calls = []
    monkeypatch.setattr(ucxapp, "check", lambda cmd, what, **kw: calls.append([str(c) for c in cmd]))
    ucxapp.matter_fetch()
    fetch = next(c for c in calls if "fetch" in c)
    assert "--depth" in fetch and "--filter=blob:none" in fetch and fetch[-1] == "a" * 40
    assert next(c for c in calls if "tag" in c)[-2:] == ["v1.6.0.0", "a" * 40]
    sparse = next(c for c in calls if "sparse-checkout" in c)
    assert "examples" not in sparse and "docs" not in sparse and "third_party" not in sparse
    assert "src" in sparse and "third_party/pigweed" in sparse
    parts = [c[-1] for c in calls if "submodule" in c]
    assert parts == [f"third_party/{p}/repo" for p in ucxapp.MATTER_SDK_PARTS]
    assert all("--depth" in c and "--recursive" not in c for c in calls if "submodule" in c)
    assert not any(p in ucxapp.MATTER_SDK_PARTS for p in ("jsoncpp", "mbedtls", "boringssl"))


def test_linux_gn_is_built_from_one_commit_of_its_source_at_the_version_of_the_windows_gn(tmp_path, monkeypatch):
    monkeypatch.setattr(ucxapp, "GN_SOURCE", tmp_path / "gn")
    monkeypatch.setattr(ucxapp, "ROOT", tmp_path)
    monkeypatch.setattr(ucxapp, "require", lambda *a: None)
    monkeypatch.setattr(ucxapp, "matter_tool", lambda name: "ninja")
    calls = []

    def check(cmd, what, **kw):
        calls.append([str(c) for c in cmd])
        (tmp_path / "gn" / "out").mkdir(parents=True, exist_ok=True)

    monkeypatch.setattr(ucxapp, "check", check)
    assert ucxapp.matter_gn_from_source() == tmp_path / "gn" / "out" / "gn"
    fetch = next(c for c in calls if "fetch" in c)
    assert fetch[-3:] == ["1", "origin", ucxapp.GN_COMMIT] and "--depth" in fetch
    assert any("--no-last-commit-position" in c for c in calls)
    header = (tmp_path / "gn" / "out" / "last_commit_position.h").read_text()
    assert f'"{ucxapp.GN_POSITION} ({ucxapp.GN_COMMIT[:12]})"' in header


@pytest.mark.skipif(not ucxapp.IS_WINDOWS, reason="matter/tools/gn.exe runs on Windows")
def test_linux_gn_is_the_version_of_matter_tools_gn():
    out = subprocess.run([ucxapp.MATTER_DIR / "tools" / "gn.exe", "--version"], capture_output=True, text=True)
    assert out.stdout.strip() == f"{ucxapp.GN_POSITION} ({ucxapp.GN_COMMIT[:12]})"


def test_detect_force_probes_unnamed_ports(monkeypatch):
    """--detect-force finds a module behind a plain 'USB Serial Port' and skips ports marked skip."""
    asked = []

    def fake_output(_exe, *args, timeout=30):
        asked.append(args)
        if args == ("COM116", "--exec", "dev.info"):
            return "Model: NORA-B26\nFirmware Version: 3.6.0d\n"
        return ""

    monkeypatch.setattr(ucxapp, "list_ports", lambda _exe: [("COM3", "SOL", "skip"), ("COM4", "USB Serial Port", ""),
                                                            ("COM116", "USB Serial Port", "EVK (FTDI)")])
    monkeypatch.setattr(ucxapp, "app_output", fake_output)
    monkeypatch.setattr(ucxapp, "wifi_summary", lambda: [])
    env = ucxapp.detect("ucxapp.exe", force=True)
    assert env["UCX_PORT_A"] == "COM116"
    assert ("COM3", "--exec", "dev.info") not in asked
    assert ucxapp.detect("ucxapp.exe").get("UCX_PORT_A") is None


def test_detect_drops_a_module_that_does_not_answer(monkeypatch, capsys):
    """A listed NORA that is silent (asleep, hung) gets no UCX_PORT_*, so the tests do not abort on it."""
    monkeypatch.setattr(ucxapp, "list_ports", lambda _exe: [])
    monkeypatch.setattr(ucxapp, "module_ports", lambda _text: ["COM155", "COM13"])
    monkeypatch.setattr(ucxapp, "app_output", lambda _exe, *args, timeout=30:
                        "Model: NORA-W36\n" if args[:1] == ("COM13",) else "")
    monkeypatch.setattr(ucxapp, "wifi_summary", lambda: [])
    env = ucxapp.detect("ucxapp.exe")
    assert env["UCX_PORT_A"] == "COM13" and "UCX_PORT_B" not in env
    assert "COM155         (no answer to dev.info" in capsys.readouterr().out
