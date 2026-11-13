"""Release: the CI workflow builds every board on FreeRTOS and Zephyr plus the browser app, and
release.py refuses an incomplete tree."""

import importlib.util
import zipfile
from pathlib import Path

import pytest

import ucxapp

ROOT = Path(__file__).resolve().parent.parent


def release_py():
    spec = importlib.util.spec_from_file_location("release", ROOT / "scripts" / "release.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def jobs():
    yaml = pytest.importorskip("yaml")
    return yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))["jobs"]


def test_every_board_is_released_on_freertos_and_zephyr(jobs):
    assert {i["board"] for i in jobs["stm32"]["strategy"]["matrix"]["include"]} == \
        {ucxapp.BOARDS[b][0] for b in ucxapp.ACTIVE_BOARDS}
    assert set(jobs["zephyr"]["strategy"]["matrix"]["board"]) == set(ucxapp.ZEPHYR_BOARDS)
    assert set(release_py().STM32_BOARDS) == set(ucxapp.ACTIVE_BOARDS)
    assert set(release_py().ZEPHYR_BOARDS) == set(ucxapp.ZEPHYR_BOARDS)


def test_bundle_collects_every_build(jobs):
    bundle = jobs["bundle"]
    assert set(bundle["needs"]) == {"windows", "linux", "stm32", "zephyr", "wasm", "matter"}
    assert jobs["matter"]["uses"] == "./.github/workflows/matter.yml"
    pattern = bundle["steps"][0]["with"]["pattern"]
    for name in ("linux", "matter-linux", "stm32-*", "zephyr-*", "wasm"):
        assert name in pattern


def test_every_part_is_required(jobs):
    assert all(job.get("continue-on-error") is not True for job in jobs.values())


def test_everything_builds_freertos_zephyr_and_the_browser(monkeypatch):
    seen = {}
    monkeypatch.setattr(ucxapp, "stm32_tools", lambda: None)
    monkeypatch.setattr(ucxapp, "wsl_works", lambda: True)
    monkeypatch.setattr(ucxapp, "zephyr_available", lambda: True)
    monkeypatch.setattr(ucxapp, "emsdk_env", lambda required=True: True)
    monkeypatch.setattr(ucxapp, "build_many", lambda _args, jobs: seen.update(jobs) or 0)
    args = ucxapp.parse(["--everything", "--clean"])[0]
    assert ucxapp.everything(args) == 0
    for b in ucxapp.ACTIVE_BOARDS:
        assert seen[b][:3] == ["--stm32", "--board", b]
        assert (f"z-{b}" in seen) == (b in ucxapp.ZEPHYR_BOARDS)
    for b in ucxapp.ZEPHYR_BOARDS:
        assert seen[f"z-{b}"][:3] == ["--stm32-zephyr", "--board", b]
    assert not any(b in seen or f"z-{b}" in seen for b in ucxapp.PAUSED_BOARDS)
    assert seen["wasm"][0] == "--wasm"
    assert "linux" in seen


@pytest.mark.skipif(not ucxapp.IS_WINDOWS, reason="WSL is the Linux build of a Windows PC")
def test_everything_skips_linux_when_wsl_does_not_start(monkeypatch, capsys):
    """An installed WSL that cannot start a distribution fails every Linux step the same way: the Linux build is
    left out and said so, as when WSL is not installed (CI builds the Linux release)."""
    seen = {}
    monkeypatch.setattr(ucxapp, "stm32_tools", lambda: None)
    monkeypatch.setattr(ucxapp, "zephyr_available", lambda: False)
    monkeypatch.setattr(ucxapp, "emsdk_env", lambda required=True: False)
    monkeypatch.setattr(ucxapp.shutil, "which", lambda name: "wsl.exe")
    monkeypatch.setattr(ucxapp.subprocess, "run",
                        lambda cmd, **kw: ucxapp.subprocess.CompletedProcess(cmd, 0xFFFFFFFF, b"", b""))
    monkeypatch.setattr(ucxapp, "build_many", lambda _args, jobs: seen.update(jobs) or 0)
    assert ucxapp.everything(ucxapp.parse(["--everything", "--clean"])[0]) == 0
    assert "linux" not in seen and "win" in seen
    assert "does not start" in capsys.readouterr().out


@pytest.mark.parametrize("no_matter", [False, True])
def test_everything_fetches_the_matter_sdk_once_before_its_parallel_builds(monkeypatch, no_matter):
    """The host, Linux and browser jobs each build a Matter node from the one SDK checkout: it is fetched and
    patched before they start; --no-matter reaches every one of them instead."""
    steps, seen = [], {}
    monkeypatch.setattr(ucxapp, "stm32_tools", lambda: None)
    monkeypatch.setattr(ucxapp, "wsl_works", lambda: True)
    monkeypatch.setattr(ucxapp, "zephyr_available", lambda: False)
    monkeypatch.setattr(ucxapp, "emsdk_env", lambda required=True: True)
    monkeypatch.setattr(ucxapp, "matter_gcc", lambda: True)
    for name in ("matter_fetch", "matter_patch", "matter_python"):
        monkeypatch.setattr(ucxapp, name, lambda name=name: steps.append(name))

    class Stop(Exception):
        """Enough seen: the tests that follow the builds are not run from a test."""

    def build_many(_args, jobs):
        steps.append("jobs")
        seen.update(jobs)
        raise Stop

    monkeypatch.setattr(ucxapp, "build_many", build_many)
    args = ucxapp.parse(["--everything", "--build", *(["--no-matter"] if no_matter else [])])[0]
    with pytest.raises(Stop):
        ucxapp.everything(args)
    assert steps == (["jobs"] if no_matter else ["matter_fetch", "matter_patch", "matter_python", "jobs"])
    assert all(("--no-matter" in seen[job]) == no_matter for job in ("linux", "wasm")), seen
    assert not any("--no-matter" in job for name, job in seen.items() if name in ucxapp.ACTIVE_BOARDS), seen


def test_launcher_skips_the_browser_build_without_emsdk(monkeypatch, tmp_path):
    monkeypatch.setattr(ucxapp.shutil, "which", lambda _name: None)
    monkeypatch.setenv("EMSDK", str(tmp_path))
    monkeypatch.setattr(ucxapp, "EMSDK_DIRS", (tmp_path / "emsdk",))
    assert ucxapp.emsdk_env(required=False) is False
    with pytest.raises(SystemExit):
        ucxapp.emsdk_env()


def make_tree(tree, boards, wasm=True, matter=True, node=True):
    for f in ("windows/ucxapp.exe", "linux/ucxapp",
              *(["windows/ucxmatter.dll", "linux/libucxmatter.so"] if matter else []),
              *(["wasm/ucxapp.wasm", "wasm/index.html"] if wasm else [])):
        (tree / f).parent.mkdir(parents=True, exist_ok=True)
        (tree / f).write_bytes(b"Matter SDK v1" if node and f.endswith(".wasm") else b"x")
    for b in boards:
        for os_ in ("stm32", "zephyr"):
            (tree / os_ / f"{os_}_{b}").mkdir(parents=True)


def test_package_refuses_an_incomplete_tree(tmp_path):
    rel = release_py()
    make_tree(tmp_path / "t", rel.STM32_BOARDS[1:])
    with pytest.raises(SystemExit):
        rel.package(tmp_path / "t", tmp_path / "a.zip")


def test_package_refuses_a_tree_without_the_browser_app(tmp_path):
    rel = release_py()
    make_tree(tmp_path / "t", rel.STM32_BOARDS, wasm=False)
    with pytest.raises(SystemExit):
        rel.package(tmp_path / "t", tmp_path / "a.zip")


def test_package_refuses_a_tree_without_the_matter_plugin(tmp_path):
    rel = release_py()
    make_tree(tmp_path / "t", rel.STM32_BOARDS, matter=False)
    with pytest.raises(SystemExit):
        rel.package(tmp_path / "t", tmp_path / "a.zip")


@pytest.mark.parametrize("missing", ["windows/ucxmatter.dll", "linux/libucxmatter.so"])
def test_package_needs_the_matter_plugin_of_every_host(tmp_path, missing):
    rel = release_py()
    make_tree(tmp_path / "t", rel.STM32_BOARDS)
    (tmp_path / "t" / missing).unlink()
    with pytest.raises(SystemExit):
        rel.package(tmp_path / "t", tmp_path / "a.zip")


def test_package_refuses_a_browser_app_without_the_matter_node(tmp_path, capsys):
    rel = release_py()
    make_tree(tmp_path / "t", rel.STM32_BOARDS, node=False)
    with pytest.raises(SystemExit):
        rel.package(tmp_path / "t", tmp_path / "a.zip")
    assert "no Matter node" in capsys.readouterr().err


def test_the_windows_exe_and_plugin_are_both_signed(tmp_path, monkeypatch):
    rel = release_py()
    (tmp_path / "app" / "bin").mkdir(parents=True)
    for name in ("ucxapp.exe", "ucxmatter.dll"):
        (tmp_path / "app" / "bin" / name).write_bytes(name.encode())
    ran = []
    monkeypatch.setattr(rel, "ROOT", tmp_path)
    monkeypatch.setattr(rel, "find_signtool", lambda: "signtool")
    monkeypatch.setattr(rel, "run", lambda *cmd, check=True: ran.append([str(c) for c in cmd]))
    exe = rel.build_and_sign("THUMB", tmp_path / "tree")
    signed = [Path(cmd[-1]).name for cmd in ran if cmd[:2] == ["signtool", "sign"]]
    verified = [Path(cmd[-1]).name for cmd in ran if cmd[:2] == ["signtool", "verify"]]
    assert sorted(signed) == sorted(verified) == ["ucxapp.exe", "ucxmatter.dll"]
    assert exe == tmp_path / "tree" / "windows" / "ucxapp.exe"
    assert (tmp_path / "tree" / "windows" / "ucxmatter.dll.sha256").read_text().endswith("  ucxmatter.dll\n")


def test_a_release_without_the_windows_plugin_stops_before_signing(tmp_path, monkeypatch):
    rel = release_py()
    (tmp_path / "app" / "bin").mkdir(parents=True)
    (tmp_path / "app" / "bin" / "ucxapp.exe").write_bytes(b"x")
    ran = []
    monkeypatch.setattr(rel, "ROOT", tmp_path)
    monkeypatch.setattr(rel, "find_signtool", lambda: "signtool")
    monkeypatch.setattr(rel, "run", lambda *cmd, check=True: ran.append(str(cmd[0])))
    with pytest.raises(SystemExit):
        rel.build_and_sign("THUMB", tmp_path / "tree")
    assert "signtool" not in ran


def test_package_writes_the_archive_and_the_browser_zip(tmp_path):
    rel = release_py()
    tree = tmp_path / "t"
    make_tree(tree, rel.STM32_BOARDS)
    rel.package(tree, tmp_path / "a.zip")
    rel.package(tree, tmp_path / "w.zip", tree / "wasm")
    with zipfile.ZipFile(tmp_path / "a.zip") as z:
        info = z.getinfo("linux/ucxapp")
        assert info.external_attr >> 16 == 0o755
    with zipfile.ZipFile(tmp_path / "w.zip") as z:
        assert sorted(z.namelist()) == ["wasm/index.html", "wasm/ucxapp.wasm"]
