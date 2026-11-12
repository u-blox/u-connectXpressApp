"""The test commissioner's PASE (tests/harness/pase.py) against the SDK's SPAKE2+ test vector, so a failing
end-to-end pairing points at the node, not at the test."""

import pytest

pytest.importorskip("cryptography")

from harness import pase  # noqa: E402

# SPAKE2+-P256-SHA256-HKDF draft-01, vector 1 (connectedhomeip src/crypto/tests/SPAKE2P_RFC_test_vectors.h)
W0 = "e6887cf9bdfb7579c69bf47928a84514b5e355ac034863f7ffaf4390e67d798c"
W1 = "24b5ae4abda868ec9336ffc3b78ee31c5755bef1759227ef5372ca139b94e512"
X_SCALAR = "8b0f3f383905cf3a3bb955ef8fb62e24849dd349a05ca79aafb18041d30cbdb6"
X = ("04af09987a593d3bac8694b123839422c3cc87e37d6b41c1d630f000dd64980e53"
     "7ae704bcede04ea3bec9b7475b32fa2ca3b684be14d11645e38ea6609eb39e7e")
Y = ("04417592620aebf9fd203616bbb9f121b730c258b286f890c5f19fea833a9c900c"
     "be9057bc549a3e19975be9927f0e7614f08d1f0a108eede5fd7eb5624584a4f4")
Z = ("0471a35282d2026f36bf3ceb38fcf87e3112a4452f46e9f7b47fd769cfb570145b"
     "62589c76b7aa1eb6080a832e5332c36898426912e29c40ef9e9c742eee82bf30")
V = ("046718981bf15bc4db538fc1f1c1d058cb0eececf1dbe1b1ea08a4e25275d382e8"
     "2b348c8131d8ed669d169c2e03a858db7cf6ca2853a4071251a39fbe8cfc39bc")
EXPECT = {"X": X, "Z": Z, "V": V, "Ka": "f9cab9adcc0ed8e5a4db11a8505914b2", "Ke": "801db297654816eb4f02868129b9dc89",
          "KcA": "0d248d7d19234f1486b2efba5179c52d", "KcB": "556291df26d705a2caedd6474dd0079b",
          "cA": "d4376f2da9c72226dd151b77c2919071155fc22a2068d90b5faa6c78c11e77dd",
          "cB": "0660a680663e8c5695956fb22dff298b1d07a526cf3cc591adfecd1f6ef6e02e"}


def test_spake2p_prover_gives_the_sdk_vector():
    k = pase.prover(b"SPAKE2+-P256-SHA256-HKDF draft-01", int(W0, 16), int(W1, 16), int(X_SCALAR, 16),
                    bytes.fromhex(Y), b"client", b"server")
    assert {name: value.hex() for name, value in k.items()} == EXPECT


def test_hkdf_gives_rfc5869_case_1():
    okm = pase.hkdf(bytes([0x0B] * 22), bytes(range(13)), bytes(range(0xF0, 0xFA)), 42)
    assert okm.hex() == ("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                         "34007208d5b887185865")


def test_tlv_reads_a_pbkdf_param_request_and_a_read_request():
    commissioner = pase.Commissioner(20202021)
    tag, fields, end = pase.tlv(commissioner.request)
    assert tag is None and end == len(commissioner.request)
    assert len(fields[1]) == 32 and fields[2] == 0x2345 and fields[3] == 0 and fields[4] is False
    _, read, _ = pase.tlv(pase.READ_VENDOR_ID)
    assert read == {0: [{2: 0, 3: 0x28, 4: 2}], 3: False, 0xFF: 12}


def test_a_commissioner_message_has_the_matter_headers():
    first = pase.Commissioner(20202021)(None)[0]
    session, header, _, source, rest = pase.parse(first)
    assert session == 0 and source == pase.Commissioner.NODE and len(header) == 16
    flags, opcode, exchange, protocol, ack, payload = pase.exchange_header(rest)
    assert (flags, opcode, exchange, protocol, ack) == (0x05, pase.PBKDF_REQUEST, 0x3456, 0, None)
