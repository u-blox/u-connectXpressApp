"""fw.* without a module: the store listing, image checks before anything is flashed, and usage errors."""

import subprocess
import zipfile

import pytest

pytestmark = pytest.mark.app


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_fw_list_shows_only_firmware_in_the_store(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "fw.list")
    assert out.returncode == 0 and "no firmware downloaded" in out.stdout, out.stdout
    store = tmp_path / "storage"
    store.mkdir(exist_ok=True)
    (store / "NORA-W36X-SW-3.4.1.bin").write_bytes(b"\xff" * 100)
    (store / "NORA-W36X-SW-3.4.1.zip").write_bytes(b"PK")
    (store / "notes.txt").write_text("x")
    out = _run(app_exe, tmp_path, "fw.list")
    assert out.returncode == 0, out.stdout
    assert "NORA-W36X-SW-3.4.1.bin" in out.stdout and "100 bytes" in out.stdout, out.stdout
    assert "NORA-W36X-SW-3.4.1.zip" in out.stdout and "notes.txt" not in out.stdout, out.stdout


def test_fw_bootloader_checks_the_image_before_the_port(app_exe, tmp_path):
    (tmp_path / "image.hex").write_text("x")
    (tmp_path / "empty.bin").write_bytes(b"")
    with zipfile.ZipFile(tmp_path / "nobin.zip", "w") as z:
        z.writestr("readme.txt", "no image here")
    (tmp_path / "bad.zip").write_bytes(b"not a zip")
    cases = {"image.hex": "not a .bin file", "missing.bin": "cannot read", "empty.bin": "cannot read",
             "nobin.zip": "no .bin file found", "bad.zip": "cannot extract"}
    for image, error in cases.items():
        out = _run(app_exe, tmp_path, f"fw.bootloader COM250 {image}")
        assert out.returncode != 0 and error in out.stdout, (image, out.stdout)


def test_fw_bootloader_reports_a_port_it_cannot_open(app_exe, tmp_path):
    (tmp_path / "fw.bin").write_bytes(b"\xff" * 64)
    out = _run(app_exe, tmp_path, "fw.bootloader /dev/nonexistent-ucx fw.bin")
    assert out.returncode != 0 and "ERROR: cannot open" in out.stdout, out.stdout


@pytest.mark.parametrize("command", ["fw.list x", "fw.flash", "fw.flash a.bin 0", "fw.flash a.bin x", "fw.bootloader",
                                     "fw.bootloader COM1", "fw.bootloader COM1 a.bin 0", "fw.github.list W36",
                                     "fw.github.list NORA-W36 x", "fw.download a", 'fw.download "a b',
                                     "fw.download a b c d"])
def test_fw_usage_errors(app_exe, tmp_path, command):
    out = _run(app_exe, tmp_path, command)
    assert out.returncode != 0, out.stdout


def test_fw_github_list_needs_a_family_without_a_module(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "fw.github.list")
    assert out.returncode != 0 and "give the family" in out.stdout, out.stdout
