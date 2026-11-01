"""EU Cyber Resilience Act (2024/2847, Annex I) and RED Delegated Act (EN 18031-1/-2/-3) items the app can show.

Each item points to the tests that prove it; this file fails when one of them is renamed or removed. Items that
belong to the module (secure boot, signed module firmware) or the process (vulnerability handling, support
period) are listed in README.md, not tested here.
"""

import ast
import re
import subprocess
from pathlib import Path

import pytest

TESTS = Path(__file__).parent

EVIDENCE = {
    "SBOM of every shipped component (CRA I part II(1))": [
        "test_cra.py::test_sbom_lists_every_component"],
    "No default passwords; access control on network interfaces (EN 18031 ACM, AUM; CRA I(2)(d))": [
        "test_webpage.py::test_web_preview_serves_the_real_commands",
        "test_cli.py::test_tls_client_auth_accepts_only_certificates_from_the_ca"],
    "Stored secrets protected (EN 18031 SSM; CRA I(2)(e))": [
        "test_crypt.py::test_wifi_passwords_are_sealed_with_the_install_key",
        "test_crypt.py::test_decrypt_rejects_wrong_passphrase_and_changes",
        "test_usecase.py::test_page_does_not_remember_secrets",
        "test_cli.py::test_tls_pem_prints_the_key_only_when_asked"],
    "Secrets kept out of logs (EN 18031 SSM; CRA I(2)(e))": [
        "test_vucx.py::test_at_trace_hides_wifi_password"],
    "Confidential, authenticated communication (EN 18031 SCM; CRA I(2)(e)(f))": [
        "test_cli.py::test_tls_certificates_form_a_valid_chain",
        "test_cli.py::test_tls_server_certificate_is_valid_for_its_names_only",
        "test_dtls.py::test_dtls_selftest_handshake_and_echo"],
    "Vetted crypto implementation (EN 18031 CCK; CRA I(2)(e))": [
        "test_cli.py::test_mbedtls_selftest_passes",
        "test_crypt.py::test_encrypt_is_standard_aes_gcm"],
    "Robust against malformed input (EN 18031 RLM; CRA I(2)(h)(i))": [
        "test_certfile.py::test_bad_input",
        "test_crypt.py::test_decrypt_bad_input_and_usage"],
}


def _functions(path):
    return {n.name for n in ast.walk(ast.parse(path.read_text(encoding="utf-8"))) if isinstance(n, ast.FunctionDef)}


@pytest.mark.parametrize("item", EVIDENCE)
def test_every_item_has_its_tests(item):
    missing = [ref for ref in EVIDENCE[item]
               if ref.split("::")[1] not in _functions(TESTS / ref.split("::")[0])]
    assert not missing, f"{item}: evidence tests are gone: {missing}"


@pytest.mark.app
def test_sbom_lists_every_component(app_exe, tmp_path):
    out = subprocess.run([str(app_exe), "COM255", "--exec", "app.sbom"], capture_output=True, text=True, timeout=30,
                         errors="replace", cwd=tmp_path, stdin=subprocess.DEVNULL).stdout
    blocks = re.findall(r"^(\S.*?)  \[(.+?)\]\n  .+\n  (https://\S+)$", out, re.MULTILINE)
    names = [b[0] for b in blocks]
    assert len(blocks) >= 5, out
    for name in ("u-connectXpressApp", "u-connectClient", "mbedTLS"):
        assert any(n.startswith(name + " ") for n in names), (name, out)
    for name, licence, _ in blocks:
        assert licence.strip(), name
        if name.startswith(("u-connectXpressApp", "u-connectClient", "mbedTLS", "TF-PSA-Crypto")):
            assert re.search(r" \d+\.\d+\.\d+", name), f"{name}: no version"
    assert "commit " in out, "u-connectClient without its commit"
