"""Single module: identity, AT passthrough, certificates, GNSS and IMU detection."""

import re
import subprocess
import time

import pytest
from hwutil import _run


@pytest.mark.hardware
def test_connects_and_reports_model(app_exe, port_a):
    out = _run(app_exe, port_a, "dev.info")
    assert out.returncode == 0, out.stdout
    assert re.search(r"NORA-(W36|B2\d)", out.stdout), out.stdout


@pytest.mark.hardware
def test_device_info_and_wifi_status(app_exe, port_a):
    out = _run(app_exe, port_a, "dev.info", "wifi.status", "sock.list")
    assert out.returncode == 0, out.stdout
    assert "Model:" in out.stdout
    assert "NORA-W36" in out.stdout
    assert "wifi:" in out.stdout
    assert "socket(s)" in out.stdout


@pytest.mark.hardware
def test_uptime_counts_from_reboot(app_exe, port_a):
    out = _run(app_exe, port_a, "dev.reboot", "dev.uptime", timeout=60)
    assert out.returncode == 0, out.stdout
    assert re.search(r"^0 days 00:00:\d\d$", out.stdout, re.M), out.stdout


@pytest.mark.hardware
def test_diagnostic_commands_run(app_exe, port_a):
    out = _run(app_exe, port_a, "dev.caps", "dev.stack", "dev.crash", "dev.diagnose", timeout=120)
    assert out.returncode == 0, out.stdout
    assert "ERROR" not in out.stdout, out.stdout
    result = r"^Result:\s+(PASS|PARTIAL) \(\d+ passed, 0 failed, \d+ not run\)"
    assert re.search(result, out.stdout, re.MULTILINE), out.stdout
    assert re.search(r"^Check device\s+PASS", out.stdout, re.MULTILINE), out.stdout


@pytest.mark.app
def test_imu_detect_probes_usb_uart_adapters(app_exe):
    # "USB-SERIAL CH340" was once parsed as an EVK port with a label and silently skipped.
    ports = subprocess.run([str(app_exe), "--list-ports"], capture_output=True, text=True, timeout=30,
                           errors="replace").stdout
    adapters = re.findall(r"^(COM\d+)\s+available\s+.*USB-SERIAL", ports, re.MULTILINE)
    if not adapters:
        pytest.skip("no USB-SERIAL adapter attached")
    out = _run(app_exe, "COM255", "imu.detect", timeout=300)
    for port in adapters:
        assert f"  {port} (" in out.stdout, out.stdout


@pytest.mark.hardware
@pytest.mark.wifi
def test_raw_at_and_cert_list(app_exe, port_a):
    out = _run(app_exe, port_a, "at +GMM", "cert.list")
    assert out.returncode == 0, out.stdout
    assert "NORA-" in out.stdout


@pytest.mark.hardware
@pytest.mark.wifi
def test_certificate_upload_info_and_remove(app_exe, port_a, tmp_path):
    x509 = pytest.importorskip("cryptography.x509")
    import datetime

    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "ucxapp test ca")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).sign(key, hashes.SHA256()))
    pem = tmp_path / "ucxtest.pem"
    pem.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    fingerprint = cert.fingerprint(hashes.SHA256()).hex()

    _run(app_exe, port_a, "cert.remove ucxtest")  # leftover from an aborted run
    out = _run(app_exe, port_a, f"cert.upload ca {pem} ucxtest", "cert.list", "cert.info ucxtest",
               "cert.remove ucxtest", "cert.list")
    assert out.returncode == 0, out.stdout
    assert "uploaded ca ucxtest" in out.stdout, out.stdout
    assert "fingerprint verified" in out.stdout, out.stdout
    assert fingerprint in out.stdout.lower(), out.stdout
    assert "removed ca ucxtest" in out.stdout, out.stdout


@pytest.mark.gnss
def test_gnss_reads_live_data(app_exe, gnss):
    port, baud = gnss
    proc = subprocess.Popen([str(app_exe), "COM255"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            text=True, errors="replace")
    try:
        for command, wait in ((f"gnss.open {port} {baud}", 4), ("gnss.status", 1), ("gnss.sats", 1),
                              ("gnss.close", 1), ("exit", 0)):
            proc.stdin.write(command + "\n")
            proc.stdin.flush()
            time.sleep(wait)
        out, _ = proc.communicate(timeout=10)
    finally:
        proc.kill()
    assert "NMEA" in out and "0 NMEA" not in out, out
    assert "Fix:" in out and "satellite(s)" in out
