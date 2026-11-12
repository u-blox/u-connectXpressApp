"""EU Cyber Resilience Act (2024/2847, Annex I) and RED Delegated Act (EN 18031-1/-2/-3) items the app can show.

Each item points to the tests that prove it; this file fails when one of them is renamed or removed. Items that
belong to the module (secure boot, signed module firmware) or the process (vulnerability handling, support
period) belong to the module and the u-blox release process, not tested here.
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
    "Keys and tokens from a true random source, no weak fallback (EN 18031 CCK; CRA I(2)(e))": [
        "test_cra.py::test_boards_take_entropy_only_from_the_hardware_trng",
        "test_cra.py::test_matter_setup_codes_come_only_from_the_random_generator"],
    # The Matter node (the optional plug-in ucxmatter, docs/SECURITY.md "Matter node")
    "Matter: SBOM of the plug-in's components (CRA I part II(1))": [
        "test_matter.py::test_sbom_lists_the_components_of_an_installed_plugin",
        "test_matter_plugin.py::test_sbom_lists_the_matter_sdk_and_its_parts"],
    "Matter: no default setup codes, a code of its own for each node (EN 18031 AUM; CRA I(2)(b)(d))": [
        "test_matter_plugin.py::test_setup_codes_are_random_for_each_node_and_kept_sealed_in_its_store",
        "test_matter_plugin.py::test_open_shows_the_codes_again_and_keeps_the_window_open"],
    "Matter: fabric keys, Wi-Fi password and setup codes sealed in the store (EN 18031 SSM; CRA I(2)(e))": [
        "test_matter.py::test_state_is_sealed_on_disk_and_survives_a_restart",
        "test_matter_plugin.py::test_setup_codes_are_random_for_each_node_and_kept_sealed_in_its_store",
        "test_matter_plugin.py::test_plugin_has_no_code_that_writes_session_keys_to_a_file"],
    "Matter: secrets kept out of the log and the AT trace (EN 18031 SSM; CRA I(2)(e))": [
        "test_matter_plugin.py::test_log_and_at_trace_do_not_have_the_passcode"],
    "Matter: commissioning and operation over authenticated, encrypted sessions (EN 18031 SCM; CRA I(2)(e)(f))": [
        "test_matter_plugin.py::test_a_pairing_request_over_udp_reaches_the_matter_server_and_is_answered",
        "test_matter_plugin.py::test_a_commissioner_opens_the_matter_transport_over_bluetooth"],
    "Matter: robust against malformed input (EN 18031 RLM; CRA I(2)(h)(i))": [
        "test_matter_plugin.py::test_malformed_datagrams_do_not_stop_the_node",
        "test_matter.py::test_files_that_are_no_plugin_are_refused",
        "test_matter.py::test_plugin_for_another_app_version_is_refused"],
    "Matter: nothing left open on the module when the node stops (CRA I(2)(j))": [
        "test_matter_plugin.py::test_a_node_on_wifi_is_announced_by_the_modules_own_mdns",
        "test_matter_plugin.py::test_node_stops_and_starts_again_in_one_session",
        "test_matter_plugin.py::test_the_modules_bonds_mode_and_name_stay_as_the_user_set_them"],
    "Matter: the user can remove all data and settings (CRA I(2)(m))": [
        "test_matter.py::test_reset_removes_what_the_node_stored_and_reboots_the_module",
        "test_matter_plugin.py::test_reset_keeps_the_setup_codes_and_reset_all_draws_new_ones"],
}


def _functions(path):
    return {n.name for n in ast.walk(ast.parse(path.read_text(encoding="utf-8"))) if isinstance(n, ast.FunctionDef)}


def test_boards_take_entropy_only_from_the_hardware_trng():
    app = TESTS.parent / "app"
    cfg = (app / "mbedtls_embedded_crypto.h").read_text(encoding="utf-8")
    assert "#undef MBEDTLS_PSA_BUILTIN_GET_ENTROPY" in cfg and "#define MBEDTLS_PSA_DRIVER_GET_ENTROPY" in cfg
    for drv in (app / "stm32_freertos" / "ucxapp_rng_stm32.c", app / "zephyr" / "ucxapp_rng_zephyr.c"):
        src = drv.read_text(encoding="utf-8")
        assert "mbedtls_platform_get_entropy" in src and "PSA_ERROR_INSUFFICIENT_ENTROPY" in src, drv.name
        assert not re.search(r"\b(rand|srand|HAL_GetTick|xTaskGetTickCount|k_cycle_get_32)\s*\(",
                             src.split("mbedtls_ms_time")[0]), f"{drv.name}: entropy must not use a weak source"
    for overlay in ("nucleo_h753zi", "nucleo_f767zi"):
        assert "zephyr,entropy = &rng" in (app / "zephyr" / "boards" / f"{overlay}.overlay").read_text()
    doc = (TESTS.parent / "docs" / "SECURITY.md").read_text(encoding="utf-8")
    for platform in ("Windows", "Linux", "WebAssembly", "STM32 FreeRTOS", "STM32 Zephyr", "Boards without TLS"):
        assert platform in doc, platform


@pytest.mark.parametrize("item", EVIDENCE)
def test_every_item_has_its_tests(item):
    missing = [ref for ref in EVIDENCE[item]
               if ref.split("::")[1] not in _functions(TESTS / ref.split("::")[0])]
    assert not missing, f"{item}: evidence tests are gone: {missing}"


def test_matter_setup_codes_come_only_from_the_random_generator():
    matter = TESTS.parent / "matter"
    provider = (matter / "src" / "UbloxCommissionableDataProvider.cpp").read_text(encoding="utf-8")
    draw = provider.split("::Draw()")[1].split("::Init(")[0]
    # every value of a new node is drawn, and a failed draw is an error: no stand-in codes
    assert draw.count("chip::Crypto::DRBG_get_bytes(") == 4 and draw.count("ReturnErrorOnFailure(") == 3, draw
    assert "ReturnErrorOnFailure(Draw());" in provider, provider
    assert "mPasscode      = 0;" in (matter / "src" / "UbloxCommissionableDataProvider.h").read_text(encoding="utf-8")
    sources = [f for d in ("src", "platform") for f in (matter / d).rglob("*") if f.suffix in (".cpp", ".h", ".c")]
    assert len(sources) > 40, sources
    for source in sources:
        text = source.read_text(encoding="utf-8", errors="replace")
        assert not re.search(r"\b(rand|srand|random)\s*\(", text), f"{source.name}: a weak random source"
        # the code every SDK example ships with (the provider names it, to refuse it), and u-connectMatter's
        # stand-ins for a module without an address
        if not source.name.startswith("UbloxCommissionableDataProvider"):
            assert "20202021" not in text, source.name
        assert "36363636" not in text and "kFallback" not in text, source.name
    # without codes the node does not start, and it never advertises with another discriminator
    assert "the Matter SDK made no setup codes" in (matter / "src" / "node.cpp").read_text(encoding="utf-8")
    config = (matter / "platform" / "ucxclient" / "ConfigurationManagerImpl.cpp").read_text(encoding="utf-8")
    assert "SetDeviceDiscriminator(config.discriminator)" not in config, "a stand-in discriminator"
    # the SDK's generator is seeded by the system; its build may not turn that off
    for name in ("args.gni", "BUILD.gn"):
        assert not re.search(r"NO_PLATFORM_ENTROPY|TEST_NULL_ENTROPY|ENTROPY_NV_SEED",
                             (matter / name).read_text(encoding="utf-8")), name
    doc = (TESTS.parent / "docs" / "SECURITY.md").read_text(encoding="utf-8")
    assert "## Matter node" in doc and "matter.factory.key" in doc and "--keylog" in doc, "docs/SECURITY.md"


def test_matter_node_uses_the_apps_mbedtls():
    # one supported mbedTLS for app and node, never the SDK's end-of-life 2.28 (its submodule is not even fetched)
    matter = TESTS.parent / "matter"
    args = (matter / "args.gni").read_text(encoding="utf-8")
    assert re.search(r'^chip_crypto = "psa"$', args, re.M), "args.gni: chip_crypto"
    assert re.search(r'^\s*mbedtls_root = "//mbedtls"$',
                     (matter / "build_overrides" / "mbedtls.gni").read_text(encoding="utf-8"), re.M)
    build = (matter / "mbedtls" / "BUILD.gn").read_text(encoding="utf-8")
    assert '"../../third-party/mbedtls"' in build and "app/mbedtls_user_config.h" in build, build
    launcher = (TESTS.parent / "ucxapp.py").read_text(encoding="utf-8")
    parts = re.search(r"^MATTER_SDK_PARTS = \((.*)\)$", launcher, re.M).group(1)
    assert "mbedtls" not in parts, parts


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
