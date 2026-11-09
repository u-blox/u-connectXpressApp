"""utils.encrypt / utils.decrypt and the AES-256-GCM sealed Wi-Fi passwords (offline)."""

import re
import subprocess
import sys

import pytest

pytestmark = pytest.mark.app

PASS = "correct horse battery"


def _run(app_exe, cwd, *commands):
    args = [str(app_exe), "COM255"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=120, errors="replace", cwd=cwd,
                          stdin=subprocess.DEVNULL)


def test_encrypt_round_trip(app_exe, tmp_path):
    plain = bytes(range(256)) * 40
    (tmp_path / "data.bin").write_bytes(plain)
    enc, dec = tmp_path / "data.enc", tmp_path / "data.out"
    out = _run(app_exe, tmp_path, f'utils.encrypt data.bin {enc} "{PASS}"', f'utils.decrypt {enc} {dec} "{PASS}"')
    assert out.returncode == 0, out.stdout
    blob = enc.read_bytes()
    assert blob[:5] == b"UCXE\x01" and len(blob) == 37 + len(plain) + 16
    assert plain[:64] not in blob
    assert dec.read_bytes() == plain


def test_encrypt_is_standard_aes_gcm(app_exe, tmp_path):
    pytest.importorskip("cryptography.hazmat.primitives.ciphers.aead", exc_type=ImportError)
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

    (tmp_path / "msg.txt").write_text("hello NORA")
    enc = tmp_path / "msg.enc"
    assert _run(app_exe, tmp_path, f'utils.encrypt msg.txt {enc} "{PASS}"').returncode == 0
    blob = enc.read_bytes()
    iterations = int.from_bytes(blob[5:9], "big")
    assert iterations == 600000
    key = PBKDF2HMAC(hashes.SHA256(), 32, blob[9:25], iterations).derive(PASS.encode())
    assert AESGCM(key).decrypt(blob[25:37], blob[37:], blob[:37]) == b"hello NORA"


def test_decrypt_rejects_wrong_passphrase_and_changes(app_exe, tmp_path):
    (tmp_path / "a.txt").write_text("secret")
    enc = tmp_path / "a.enc"
    assert _run(app_exe, tmp_path, f"utils.encrypt a.txt {enc} pw1").returncode == 0
    out = _run(app_exe, tmp_path, f"utils.decrypt {enc} {tmp_path / 'x'} pw2")
    assert out.returncode != 0 and "wrong passphrase" in out.stdout
    assert not (tmp_path / "x").exists()

    blob = bytearray(enc.read_bytes())
    blob[-20] ^= 1
    enc.write_bytes(bytes(blob))
    out = _run(app_exe, tmp_path, f"utils.decrypt {enc} {tmp_path / 'x'} pw1")
    assert out.returncode != 0 and "was changed" in out.stdout

    blob[6] ^= 1   # the iteration count is authenticated too
    (tmp_path / "b.enc").write_bytes(bytes(blob))
    assert _run(app_exe, tmp_path, f"utils.decrypt b.enc {tmp_path / 'x'} pw1").returncode != 0


def test_decrypt_bad_input_and_usage(app_exe, tmp_path):
    (tmp_path / "plain.txt").write_text("not encrypted at all, just some text here")
    out = _run(app_exe, tmp_path, "utils.decrypt plain.txt out pw")
    assert out.returncode != 0 and "not a utils.encrypt file" in out.stdout
    out = _run(app_exe, tmp_path, "utils.decrypt missing.enc out pw")
    assert out.returncode != 0 and "cannot read" in out.stdout
    out = _run(app_exe, tmp_path, "utils.encrypt missing.txt out pw")
    assert out.returncode != 0 and "cannot read" in out.stdout
    for cmd in ("utils.encrypt plain.txt out", 'utils.encrypt plain.txt out ""', "utils.decrypt plain.txt"):
        assert _run(app_exe, tmp_path, cmd).returncode != 0


def test_encrypt_empty_file(app_exe, tmp_path):
    (tmp_path / "empty").write_bytes(b"")
    enc, dec = tmp_path / "e.enc", tmp_path / "e.out"
    out = _run(app_exe, tmp_path, f"utils.encrypt empty {enc} pw", f"utils.decrypt {enc} {dec} pw")
    assert out.returncode == 0, out.stdout
    assert dec.read_bytes() == b""


def test_wifi_passwords_are_sealed_with_the_install_key(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "wifi.profile.add home ubx s3cret!", "wifi.profile.add cafe CafeNet")
    assert out.returncode == 0, out.stdout
    assert "only obfuscated" not in out.stdout     # boards without UCXAPP_CRYPTO print this note instead
    storage = tmp_path / "storage"
    lines = dict(line.split("\t", 1) for line in (storage / "ucxapp.wifi").read_text().splitlines())
    assert lines["home"].startswith("ubx\ta1:")
    assert lines["cafe"] == "CafeNet\t"
    key_file = (storage / "ucxapp.key").read_bytes()
    if sys.platform == "win32":
        assert key_file.startswith(b"UCXK\x01") and len(key_file) > 37   # wrapped with DPAPI
    else:
        assert len(key_file) == 32

    # A changed key cannot open the passwords: the profile loads without one and says why.
    key = (storage / "ucxapp.key").read_bytes()
    (storage / "ucxapp.key").write_bytes(bytes(32))
    out = _run(app_exe, tmp_path, "wifi.profile.list")
    assert "cannot decrypt the password of profile home" in out.stdout
    assert "cafe: ssid=CafeNet open" in out.stdout
    (storage / "ucxapp.key").write_bytes(key)
    out = _run(app_exe, tmp_path, "wifi.profile.list")
    assert "home: ssid=ubx WPA" in out.stdout and "cannot decrypt" not in out.stdout


def test_wifi_old_obfuscated_profiles_are_dropped(app_exe, tmp_path):
    key = b"uBloxUcxClient"
    obf = bytes(c ^ key[i % len(key)] for i, c in enumerate(b"oldpass1")).hex()
    storage = tmp_path / "storage"
    storage.mkdir()
    (storage / "ucxapp.wifi").write_text(f"old\tOldNet\t{obf}\n")
    out = _run(app_exe, tmp_path, "wifi.profile.list")
    assert "profile old has an old password format; add it again" in out.stdout
    assert "ssid=OldNet" not in out.stdout
    out = _run(app_exe, tmp_path, "wifi.profile.add new NewNet newpass9", "wifi.profile.list")
    assert "new: ssid=NewNet WPA" in out.stdout and "ssid=OldNet" not in out.stdout
    text = (storage / "ucxapp.wifi").read_text()
    assert obf not in text and text.count("\ta1:") == 1


PEM = "-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg\n-----END PRIVATE KEY-----\n"


def _export(app_exe, tmp_path, name):
    """A store file as the app reads it, through utils.encrypt/decrypt to an absolute (unsealed) path."""
    enc, out_file = tmp_path / "export.enc", tmp_path / "export.out"
    out_file.unlink(missing_ok=True)
    out = _run(app_exe, tmp_path, f"utils.encrypt {name} {enc} pw", f"utils.decrypt {enc} {out_file} pw")
    return out_file.read_text() if out.returncode == 0 else None


def test_secure_store_seals_private_keys(app_exe, tmp_path):
    out = _run(app_exe, tmp_path, "utils.secure")
    assert out.returncode == 0, out.stdout
    assert "install key: none yet" in out.stdout and "0 private key file(s) sealed" in out.stdout

    (tmp_path / "dev.pem").write_bytes(PEM.encode())
    out = _run(app_exe, tmp_path, f"utils.encrypt dev.pem {tmp_path / 'k.enc'} pw",
               f"utils.decrypt {tmp_path / 'k.enc'} dev.key pw",
               f"utils.decrypt {tmp_path / 'k.enc'} {tmp_path / 'plain.key'} pw", "utils.secure")
    assert out.returncode == 0, out.stdout
    sealed = (tmp_path / "storage" / "dev.key").read_bytes()
    assert sealed.startswith(b"UCXS\x01") and b"PRIVATE" not in sealed
    assert len(sealed) == 4 + 1 + 12 + len(PEM) + 16
    assert (tmp_path / "plain.key").read_text() == PEM      # paths outside the store stay as asked
    assert re.search(r"dev\.key\s+sealed", out.stdout) and "1 private key file(s) sealed" in out.stdout
    if sys.platform == "win32":
        assert "DPAPI" in out.stdout, out.stdout
    assert _export(app_exe, tmp_path, "dev.key") == PEM


def test_secure_store_rejects_changed_or_renamed_files(app_exe, tmp_path):
    storage = tmp_path / "storage"
    storage.mkdir()
    (storage / "a.key").write_text(PEM)
    assert _run(app_exe, tmp_path, "utils.secure").returncode == 0
    sealed = (storage / "a.key").read_bytes()
    assert sealed.startswith(b"UCXS")                         # plain keys are sealed at start
    assert _export(app_exe, tmp_path, "a.key") == PEM

    (storage / "b.key").write_bytes(sealed)                  # the name is authenticated
    assert _export(app_exe, tmp_path, "b.key") is None
    changed = bytearray(sealed)
    changed[-20] ^= 1
    (storage / "a.key").write_bytes(bytes(changed))
    assert _export(app_exe, tmp_path, "a.key") is None
    out = _run(app_exe, tmp_path, "utils.secure")
    assert re.search(r"a\.key\s+sealed", out.stdout), out.stdout   # still sealed, just not openable


def test_secure_store_keeps_an_older_raw_install_key(app_exe, tmp_path):
    pytest.importorskip("cryptography.hazmat.primitives.ciphers.aead", exc_type=ImportError)
    import os

    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    key, iv = os.urandom(32), os.urandom(12)
    sealed = "a1:" + (iv + AESGCM(key).encrypt(iv, b"oldkey99", b"ucxapp-secret-1")).hex()
    storage = tmp_path / "storage"
    storage.mkdir()
    (storage / "ucxapp.key").write_bytes(key)
    (storage / "ucxapp.wifi").write_text(f"home\tHomeNet\t{sealed}\n")
    out = _run(app_exe, tmp_path, "wifi.profile.list", "utils.secure")
    assert "home: ssid=HomeNet WPA" in out.stdout and "cannot decrypt" not in out.stdout, out.stdout
    if sys.platform == "win32":
        assert (storage / "ucxapp.key").read_bytes().startswith(b"UCXK\x01")   # wrapped on first use
        out = _run(app_exe, tmp_path, "wifi.profile.list")
        assert "home: ssid=HomeNet WPA" in out.stdout, out.stdout
    else:
        assert (storage / "ucxapp.key").read_bytes() == key


def test_secure_usage(app_exe, tmp_path):
    assert _run(app_exe, tmp_path, "utils.secure extra").returncode != 0
