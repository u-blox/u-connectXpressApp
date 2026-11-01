"""Release: the CI workflow builds every board on FreeRTOS and Zephyr plus the browser app, and release.py
refuses an incomplete tree."""

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
        {b[0] for b in ucxapp.BOARDS.values()}
    assert set(jobs["zephyr"]["strategy"]["matrix"]["board"]) == set(ucxapp.BOARDS)
    assert set(release_py().STM32_BOARDS) == set(ucxapp.BOARDS)


def test_bundle_collects_every_build(jobs):
    bundle = jobs["bundle"]
    assert set(bundle["needs"]) == {"windows", "linux", "stm32", "zephyr", "wasm"}
    pattern = bundle["steps"][0]["with"]["pattern"]
    for name in ("linux", "stm32-*", "zephyr-*", "wasm"):
        assert name in pattern


def make_tree(tree, boards):
    for f in ("windows/ucxapp.exe", "linux/ucxapp", "wasm/ucxapp.wasm", "wasm/index.html"):
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
