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
    assert set(jobs["zephyr"]["strategy"]["matrix"]["board"]) == set(ucxapp.ACTIVE_BOARDS)
    assert set(release_py().STM32_BOARDS) == set(ucxapp.ACTIVE_BOARDS)


def test_bundle_collects_every_build(jobs):
    bundle = jobs["bundle"]
    assert set(bundle["needs"]) == {"windows", "linux", "stm32", "zephyr", "wasm"}
    pattern = bundle["steps"][0]["with"]["pattern"]
    for name in ("linux", "stm32-*", "zephyr-*", "wasm"):
        assert name in pattern


def test_every_part_is_required(jobs):
    assert all(job.get("continue-on-error") is not True for job in jobs.values())


def test_everything_builds_freertos_zephyr_and_the_browser(monkeypatch):
    seen = {}
    monkeypatch.setattr(ucxapp, "stm32_tools", lambda: None)
    monkeypatch.setattr(ucxapp, "zephyr_available", lambda: True)
    monkeypatch.setattr(ucxapp, "emsdk_env", lambda required=True: True)
    monkeypatch.setattr(ucxapp, "build_many", lambda _args, jobs: seen.update(jobs) or 0)
    args = ucxapp.parse(["--everything", "--clean"])[0]
    assert ucxapp.everything(args) == 0
    for b in ucxapp.ACTIVE_BOARDS:
        assert seen[b][:3] == ["--stm32", "--board", b]
        assert seen[f"z-{b}"][:3] == ["--stm32-zephyr", "--board", b]
    assert not any(b in seen or f"z-{b}" in seen for b in ucxapp.PAUSED_BOARDS)
    assert seen["wasm"][0] == "--wasm"


def test_launcher_skips_the_browser_build_without_emsdk(monkeypatch, tmp_path):
    monkeypatch.setattr(ucxapp.shutil, "which", lambda _name: None)
    monkeypatch.setenv("EMSDK", str(tmp_path))
    monkeypatch.setattr(ucxapp, "EMSDK_DIRS", (tmp_path / "emsdk",))
    assert ucxapp.emsdk_env(required=False) is False
    with pytest.raises(SystemExit):
        ucxapp.emsdk_env()


def make_tree(tree, boards, wasm=True):
    for f in ("windows/ucxapp.exe", "linux/ucxapp", *(["wasm/ucxapp.wasm", "wasm/index.html"] if wasm else [])):
        (tree / f).parent.mkdir(parents=True, exist_ok=True)
        (tree / f).write_bytes(b"x")
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
