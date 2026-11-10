"""utils.cert.info / utils.cert.convert: PKCS#12, PKCS#7, DER and PEM to the module's PEM files (offline)."""

import datetime
import subprocess

import pytest

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
    from cryptography.hazmat.primitives.serialization import pkcs7, pkcs12
    from cryptography.x509.oid import NameOID
except ImportError as e:   # not installed, or a broken build (no _rust DLL)
    pytest.skip(f"cryptography is not usable: {e}", allow_module_level=True)

pytestmark = pytest.mark.app

PW = "s3cret-pw"


def _run(app_exe, *commands):
    args = [str(app_exe), "COM255"]
    for command in commands:
        args += ["--exec", command]
    return subprocess.run(args, capture_output=True, text=True, timeout=60, errors="replace",
                          stdin=subprocess.DEVNULL)


def _cert(subject, key, issuer=None, issuer_key=None, ca=False, days=365):
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject)])
    issuer_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, issuer or subject)])
    return (x509.CertificateBuilder().subject_name(name).issuer_name(issuer_name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=days))
            .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True)
            .sign(issuer_key or key, hashes.SHA256()))


@pytest.fixture(scope="module")
def pki():
    root_key = ec.generate_private_key(ec.SECP256R1())
    inter_key = ec.generate_private_key(ec.SECP256R1())
    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    root = _cert("Test Root", root_key, ca=True)
    inter = _cert("Test Intermediate", inter_key, "Test Root", root_key, ca=True)
    leaf = _cert("device-01", leaf_key, "Test Intermediate", inter_key)
    return {"root": root, "inter": inter, "leaf": leaf, "key": leaf_key}


def _pem_certs(path):
    return x509.load_pem_x509_certificates(path.read_bytes())


def _check_converted(tmp_path, pki, stem, with_ca=True):
    chain = _pem_certs(tmp_path / f"{stem}.crt")
    assert chain[0] == pki["leaf"]
    assert pki["inter"] in chain and pki["root"] not in chain
    key = serialization.load_pem_private_key((tmp_path / f"{stem}.key").read_bytes(), None)
    assert key.public_key().public_numbers() == pki["leaf"].public_key().public_numbers()
    if with_ca:
        assert _pem_certs(tmp_path / f"{stem}-ca.crt") == [pki["root"]]


def test_pkcs12_aes_convert(app_exe, tmp_path, pki):
    p12 = tmp_path / "client.p12"
    p12.write_bytes(pkcs12.serialize_key_and_certificates(
        b"device", pki["key"], pki["leaf"], [pki["root"], pki["inter"]],
        serialization.BestAvailableEncryption(PW.encode())))
    out = _run(app_exe, f"utils.cert.convert {p12} {tmp_path / 'out'} {PW}")
    assert out.returncode == 0, out.stdout
    assert "PKCS#12: 3 certificates and a private key" in out.stdout
    _check_converted(tmp_path, pki, "out")

    out = _run(app_exe, f"utils.cert.info {p12} {PW}")
    assert out.returncode == 0, out.stdout
    assert "format   PKCS#12, 3 certificates, private key" in out.stdout
    assert "CN=device-01  - client, matches the key" in out.stdout
    assert "intermediate" in out.stdout and "(self-signed)" in out.stdout
    assert "key      RSA 2048 bits" in out.stdout


def test_pkcs12_sha256_mac_and_default_out_name(app_exe, tmp_path, pki):
    enc = (serialization.PrivateFormat.PKCS12.encryption_builder()
           .kdf_rounds(5000).key_cert_algorithm(pkcs12.PBES.PBESv2SHA256AndAES256CBC)
           .hmac_hash(hashes.SHA256()).build(PW.encode()))
    p12 = tmp_path / "dev.pfx"
    p12.write_bytes(pkcs12.serialize_key_and_certificates(
        b"d", pki["key"], pki["leaf"], [pki["inter"], pki["root"]], enc))
    out = _run(app_exe, f"utils.cert.convert {p12} {tmp_path / 'dev'} {PW}")
    assert out.returncode == 0, out.stdout
    _check_converted(tmp_path, pki, "dev")


def test_pkcs12_without_password(app_exe, tmp_path, pki):
    p12 = tmp_path / "open.p12"
    p12.write_bytes(pkcs12.serialize_key_and_certificates(
        b"x", pki["key"], pki["leaf"], [pki["inter"]], serialization.NoEncryption()))
    out = _run(app_exe, f"utils.cert.convert {p12} {tmp_path / 'open'}")
    assert out.returncode == 0, out.stdout
    _check_converted(tmp_path, pki, "open", with_ca=False)
    assert not (tmp_path / "open-ca.crt").exists()


def test_pkcs12_wrong_password(app_exe, tmp_path, pki):
    p12 = tmp_path / "client.p12"
    p12.write_bytes(pkcs12.serialize_key_and_certificates(
        b"d", pki["key"], pki["leaf"], None, serialization.BestAvailableEncryption(PW.encode())))
    for args in (f"{p12} wrong", f"{p12}"):
        out = _run(app_exe, f"utils.cert.info {args}")
        assert out.returncode == 1
        assert "wrong password" in out.stdout


def test_pkcs12_legacy_3des_gives_a_hint(app_exe, tmp_path, pki):
    enc = (serialization.PrivateFormat.PKCS12.encryption_builder()
           .key_cert_algorithm(pkcs12.PBES.PBESv1SHA1And3KeyTripleDESCBC).hmac_hash(hashes.SHA1()).build(PW.encode()))
    p12 = tmp_path / "old.p12"
    p12.write_bytes(pkcs12.serialize_key_and_certificates(b"d", pki["key"], pki["leaf"], None, enc))
    out = _run(app_exe, f"utils.cert.info {p12} {PW}")
    assert out.returncode == 1
    assert "legacy PKCS#12 encryption" in out.stdout and "AES" in out.stdout


@pytest.mark.parametrize("encoding", [serialization.Encoding.DER, serialization.Encoding.PEM])
def test_pkcs7_bundle(app_exe, tmp_path, pki, encoding):
    p7b = tmp_path / "chain.p7b"
    p7b.write_bytes(pkcs7.serialize_certificates([pki["root"], pki["inter"]], encoding))
    out = _run(app_exe, f"utils.cert.convert {p7b} {tmp_path / 'chain'}")
    assert out.returncode == 0, out.stdout
    assert "2 certificates" in out.stdout
    assert set(_pem_certs(tmp_path / "chain.crt")) == {pki["root"], pki["inter"]}


def test_der_certificate_and_key(app_exe, tmp_path, pki):
    cer = tmp_path / "root.cer"
    cer.write_bytes(pki["root"].public_bytes(serialization.Encoding.DER))
    out = _run(app_exe, f"utils.cert.info {cer}", f"utils.cert.convert {cer} {tmp_path / 'root'}")
    assert out.returncode == 0, out.stdout
    assert "DER certificate" in out.stdout and "CN=Test Root" in out.stdout
    assert _pem_certs(tmp_path / "root.crt") == [pki["root"]]

    der = tmp_path / "key.der"
    der.write_bytes(pki["key"].private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8,
                                             serialization.NoEncryption()))
    out = _run(app_exe, f"utils.cert.info {der}")
    assert out.returncode == 0, out.stdout
    assert "DER private key" in out.stdout and "RSA 2048 bits" in out.stdout


def test_pem_bundle_with_encrypted_key(app_exe, tmp_path, pki):
    pem = tmp_path / "bundle.pem"
    pem.write_bytes(b"Bag Attributes\n    friendlyName: d\n"
                    + pki["leaf"].public_bytes(serialization.Encoding.PEM)
                    + pki["key"].private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                               serialization.BestAvailableEncryption(PW.encode()))
                    + pki["inter"].public_bytes(serialization.Encoding.PEM)
                    + pki["root"].public_bytes(serialization.Encoding.PEM))
    out = _run(app_exe, f"utils.cert.convert {pem} {tmp_path / 'b'} {PW}")
    assert out.returncode == 0, out.stdout
    _check_converted(tmp_path, pki, "b")
    out = _run(app_exe, f"utils.cert.info {pem}")
    assert out.returncode == 1
    assert "encrypted; give the password" in out.stdout


def test_bad_input(app_exe, tmp_path):
    junk = tmp_path / "junk.bin"
    junk.write_bytes(b"\x30\x03\x02\x01\x05")
    text = tmp_path / "notes.txt"
    text.write_text("hello")
    for name in (junk, text, tmp_path / "missing.p12"):
        out = _run(app_exe, f"utils.cert.info {name}")
        assert out.returncode == 1
        assert "ERROR" in out.stdout
    assert "usage:" in _run(app_exe, "utils.cert.convert").stdout
    long_out = tmp_path / ("x" * 240)   # longer than the output buffer: refused, not silently truncated
    out = _run(app_exe, f"utils.cert.convert {junk} {long_out}")
    assert out.returncode == 1 and "output path longer than" in out.stdout, out.stdout
    assert not list(tmp_path.glob("x*")), list(tmp_path.iterdir())


def test_import_needs_a_device(app_exe, tmp_path):
    out = _run(app_exe, f"cert.import {tmp_path / 'x.p12'}")
    assert out.returncode == 1
    assert "not connected" in out.stdout
