"""Every target builds the app as C11."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_cmake_sets_c11():
    for rel in ("app/CMakeLists.txt", "app/stm32_freertos/CMakeLists.txt"):
        text = (ROOT / rel).read_text(encoding="utf-8")
        assert "set(CMAKE_C_STANDARD 11)" in text, rel
        assert "set(CMAKE_C_STANDARD_REQUIRED ON)" in text, rel
    assert "/std:c11" in (ROOT / "app/CMakeLists.txt").read_text(encoding="utf-8")


def test_zephyr_keeps_c11_or_newer():
    # Zephyr defaults to C17 (its Kconfig only offers C17 and C23); never pick an older standard.
    text = (ROOT / "app/zephyr/prj.conf").read_text(encoding="utf-8")
    for old in ("CONFIG_STD_C90=y", "CONFIG_STD_C99=y", "CONFIG_STD_C11=y"):
        assert old not in text


def test_header_guards_c11():
    assert "__STDC_VERSION__ < 201112L" in (ROOT / "app/src/ucxapp.h").read_text(encoding="utf-8")
