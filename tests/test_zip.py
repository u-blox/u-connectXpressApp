"""utils.zip / utils.unzip round trips against Python's zipfile, offline."""

import os
import struct
import subprocess
import zipfile
import zlib

import pytest

pytestmark = pytest.mark.app


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "--offline"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def _payloads():
    return {
        "text.txt": b"hello zip\n" * 500,
        "random.bin": os.urandom(200_000),
        "empty.dat": b"",
    }


def test_app_zip_read_by_python(app_exe, tmp_path):
    files = _payloads()
    for name, data in files.items():
        (tmp_path / name).write_bytes(data)
    out = _run(app_exe, tmp_path, "utils.zip out.zip " + " ".join(files))
    assert out.returncode == 0, out.stdout + out.stderr
    with zipfile.ZipFile(tmp_path / "out.zip") as z:
        assert z.testzip() is None
        for name, data in files.items():
            assert z.read(name) == data


def test_python_zip_extracted_by_app(app_exe, tmp_path):
    files = _payloads()
    files["sub/dir/nested.txt"] = b"nested"
    with zipfile.ZipFile(tmp_path / "in.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
        z.writestr("stored.txt", b"stored", compress_type=zipfile.ZIP_STORED)
    out = _run(app_exe, tmp_path, "utils.unzip in.zip ex")
    assert out.returncode == 0, out.stdout + out.stderr
    for name, data in files.items():
        assert (tmp_path / "ex" / name).read_bytes() == data
    assert (tmp_path / "ex" / "stored.txt").read_bytes() == b"stored"


def test_round_trip(app_exe, tmp_path):
    (tmp_path / "a.bin").write_bytes(os.urandom(50_000) + b"A" * 50_000)
    out = _run(app_exe, tmp_path, "utils.zip rt.zip a.bin", "utils.unzip rt.zip back")
    assert out.returncode == 0, out.stdout + out.stderr
    assert (tmp_path / "back" / "a.bin").read_bytes() == (tmp_path / "a.bin").read_bytes()


def test_unzip_rejects_unsafe_names(app_exe, tmp_path):
    with zipfile.ZipFile(tmp_path / "evil.zip", "w") as z:
        z.writestr("../evil.txt", b"x")
        z.writestr("ok.txt", b"ok")
    out = _run(app_exe, tmp_path, "utils.unzip evil.zip ex")
    assert "skipped unsafe name" in out.stdout
    assert not (tmp_path / "evil.txt").exists()
    assert (tmp_path / "ex" / "ok.txt").read_bytes() == b"ok"


def test_unzip_bad_input(app_exe, tmp_path):
    (tmp_path / "junk.zip").write_bytes(b"not a zip at all")
    out = _run(app_exe, tmp_path, "utils.unzip junk.zip ex")
    assert out.returncode != 0
    assert "not a zip file" in out.stdout
    out = _run(app_exe, tmp_path, "utils.unzip missing.zip ex")
    assert out.returncode != 0


class _Bits:
    """A deflate bit stream: values LSB first, Huffman codes MSB first."""

    def __init__(self):
        self.bits = []

    def val(self, v, n):
        self.bits += [(v >> i) & 1 for i in range(n)]
        return self

    def code(self, text):
        self.bits += [int(ch) for ch in text]
        return self

    def bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, bit in enumerate(self.bits):
            out[i // 8] |= bit << (i % 8)
        return bytes(out)


def _zip(entries, count=None, cd_shift=0):
    """A zip of (name, method, payload, usize, overrides) entries; overrides patch the central directory fields."""
    local, central = b"", b""
    for name, method, payload, usize, over in entries:
        raw = name.encode()
        crc = zlib.crc32(payload) if method == 0 else 0
        fields = {"crc": crc, "csize": len(payload), "usize": usize, "name": len(raw), "lho": len(local)}
        fields.update(over)
        local += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0, crc, len(payload), usize, len(raw),
                             0) + raw + payload
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, method, 0, 0, fields["crc"],
                               fields["csize"], fields["usize"], fields["name"], 0, 0, 0, 0, 0, fields["lho"]) + raw
    n = len(entries) if count is None else count
    return local + central + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, n, n, len(central), len(local) + cd_shift, 0)


def _fixed():
    return _Bits().val(1, 1).val(1, 2)


def _dynamic(lengths_16_17_18_0, hlit=0):
    bits = _Bits().val(1, 1).val(2, 2).val(hlit, 5).val(0, 5).val(0, 4)
    for v in lengths_16_17_18_0:
        bits.val(v, 3)
    return bits


DEFLATE_ERRORS = {
    "empty": (b"", 1),
    "type3": (_Bits().val(1, 1).val(3, 2).bytes(), 1),
    "stored-short": (_Bits().val(1, 1).val(0, 2).bytes() + b"\x05\x00", 5),
    "stored-nlen": (_Bits().val(1, 1).val(0, 2).bytes() + b"\x05\x00\x00\x00hello", 5),
    "fixed-short": (_fixed().bytes(), 1),
    "fixed-literal-overflow": (_fixed().code("01110001").code("0000000").bytes(), 0),
    "fixed-length-286": (_fixed().code("11000110").bytes(), 4),
    "fixed-dist-30": (_fixed().code("01110001").code("0000001").code("11110").bytes(), 4),
    "fixed-dist-too-far": (_fixed().code("0000001").code("00000").bytes(), 4),
    "fixed-copy-overflow": (_fixed().code("01110001").code("0000001").code("00000").bytes(), 2),
    "dynamic-hlit": (_dynamic([], hlit=30).bytes(), 1),
    "dynamic-short": (_Bits().val(1, 1).val(2, 2).val(0, 5).val(0, 5).val(15, 4).bytes(), 1),
    "dynamic-oversubscribed": (_dynamic([1, 1, 1, 1]).bytes(), 1),
    "dynamic-repeat-first": (_dynamic([1, 0, 0, 1]).code("1").bytes(), 1),
    "dynamic-repeat-overflow": (_dynamic([0, 0, 1, 1]).code("1").val(127, 7).code("1").val(127, 7).bytes(), 1),
    # 0, then 16 repeats it 3 times, then 18 fills zeros up to all 258 lengths: no end-of-block code
    "dynamic-no-end": (_dynamic([2, 0, 2, 1]).code("0").code("10").val(0, 2).code("11").val(127, 7)
                       .code("11").val(105, 7).bytes(), 1),
}


@pytest.mark.parametrize("case", DEFLATE_ERRORS)
def test_unzip_refuses_a_corrupt_deflate_stream(app_exe, tmp_path, case):
    payload, usize = DEFLATE_ERRORS[case]
    (tmp_path / "bad.zip").write_bytes(_zip([("f.bin", 8, payload, usize, {})]))
    out = _run(app_exe, tmp_path, "utils.unzip bad.zip ex")
    assert out.returncode != 0 and "ERROR: f.bin: unsupported method or corrupt data" in out.stdout, out.stdout
    assert not (tmp_path / "ex" / "f.bin").exists()


@pytest.mark.parametrize(("entries", "kwargs", "message"), [
    ([("a.txt", 0, b"abc", 3, {})], {"count": 2}, "ERROR: corrupt central directory"),
    ([("a.txt", 0, b"abc", 3, {"name": 300})], {}, "ERROR: corrupt central directory"),
    ([("a.txt", 0, b"abc", 3, {"lho": 5})], {}, "ERROR: corrupt entry a.txt"),
    ([("a.txt", 0, b"abc", 3, {"csize": 4000})], {}, "ERROR: corrupt entry a.txt"),
    ([("a.txt", 0, b"abc", 4, {})], {}, "ERROR: a.txt: unsupported method or corrupt data"),
    ([("a.txt", 12, b"abc", 3, {})], {}, "ERROR: a.txt: unsupported method or corrupt data"),
    ([("d/", 0, b"", 0, {}), ("d", 0, b"x", 1, {})], {}, "ERROR: cannot write ex/d"),
])
def test_unzip_refuses_a_corrupt_archive(app_exe, tmp_path, entries, kwargs, message):
    (tmp_path / "bad.zip").write_bytes(_zip(entries, **kwargs))
    out = _run(app_exe, tmp_path, "utils.unzip bad.zip ex")
    assert out.returncode != 0 and message in out.stdout, out.stdout


def test_zip_reports_missing_files_and_an_unwritable_archive(app_exe, tmp_path):
    (tmp_path / "a.txt").write_text("a")
    out = _run(app_exe, tmp_path, "utils.zip out.zip a.txt missing.txt")
    assert out.returncode != 0 and "ERROR: cannot read missing.txt" in out.stdout, out.stdout
    out = _run(app_exe, tmp_path, "utils.zip nodir/out.zip a.txt")
    assert out.returncode != 0 and "ERROR: cannot create nodir/out.zip" in out.stdout, out.stdout
