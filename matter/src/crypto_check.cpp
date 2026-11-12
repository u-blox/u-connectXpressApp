/* matter.selftest: every cryptographic primitive the node uses, checked against known answers through the SDK's
 * own crypto layer (CHIPCryptoPAL on the app's mbedTLS 4, PSA keystore). The vectors are the SDK's
 * (src/crypto/tests): RFC and NIST answers, the same ones its unit tests use. */
#include "node.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <crypto/CHIPCryptoPAL.h>
#include <crypto/DefaultSessionKeystore.h>
#include <lib/core/CHIPSafeCasts.h>
#include <lib/support/ScopedMemoryBuffer.h>
#include <psa/crypto.h>

#include <crypto/tests/AES_CCM_128_test_vectors.h>
#include <crypto/tests/ECDSA_det_test_vectors.h>
#include <crypto/tests/PBKDF2_SHA256_test_vectors.h>
#include <crypto/tests/SPAKE2P_RFC_test_vectors.h>

// These three define writable globals with short names (v1, data01): keep them out of the app's symbols
namespace {
#include <crypto/tests/HKDF_SHA256_test_vectors.h>
#include <crypto/tests/HMAC_SHA256_test_vectors.h>
#include <crypto/tests/Hash_SHA256_test_vectors.h>
} // namespace

using namespace chip;
using namespace chip::Crypto;

namespace ucxm {

namespace {

class Report
{
public:
    Report(CryptoReport report, void * context) : mReport(report), mContext(context) {}

    // name ok when every vector of it passed; failed says which one did not
    void operator()(const char * name, int failed, int count, const char * what)
    {
        char detail[120];

        if (failed < 0)
        {
            snprintf(detail, sizeof(detail), "%s", what);
        }
        else
        {
            snprintf(detail, sizeof(detail), "vector %d of %d wrong: %s", failed + 1, count, what);
        }
        mReport(mContext, name, failed < 0, detail);
    }

private:
    CryptoReport mReport;
    void * mContext;
};

int Sha256()
{
    const int count = static_cast<int>(MATTER_ARRAY_SIZE(hash_sha256_test_vectors));

    for (int i = 0; i < count; i++)
    {
        const hash_sha256_vector & v = hash_sha256_test_vectors[i];
        uint8_t one[kSHA256_Hash_Length];
        uint8_t streamed[kSHA256_Hash_Length];
        MutableByteSpan out(streamed);
        Hash_SHA256_stream stream;
        const size_t half = v.data_length / 2;

        // in one go, and in two parts as the node hashes a PASE or CASE transcript
        if (Hash_SHA256(v.data, v.data_length, one) != CHIP_NO_ERROR || memcmp(one, v.hash, sizeof(one)) != 0 ||
            stream.Begin() != CHIP_NO_ERROR || stream.AddData(ByteSpan(v.data, half)) != CHIP_NO_ERROR ||
            stream.AddData(ByteSpan(v.data + half, v.data_length - half)) != CHIP_NO_ERROR ||
            stream.Finish(out) != CHIP_NO_ERROR || out.size() != sizeof(streamed) || memcmp(streamed, v.hash, sizeof(streamed)) != 0)
        {
            return i;
        }
    }
    return -1;
}

int Hmac(SessionKeystore & keystore)
{
    const int raw = static_cast<int>(MATTER_ARRAY_SIZE(hmac_sha256_test_vectors_raw_key));
    HMAC_sha mac;

    for (int i = 0; i < raw; i++)
    {
        const hmac_sha256_vector & v = hmac_sha256_test_vectors_raw_key[i];
        uint8_t out[kSHA256_Hash_Length];

        if (v.output_hash_length > sizeof(out) ||
            mac.HMAC_SHA256(v.key, v.key_length, v.message, v.message_length, out, v.output_hash_length) != CHIP_NO_ERROR ||
            memcmp(out, v.output_hash, v.output_hash_length) != 0)
        {
            return i;
        }
    }
    // through a key handle, as group keys and the operational key derivation use it
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(hmac_sha256_test_vectors_key_handle); i++)
    {
        const hmac_sha256_vector & v = hmac_sha256_test_vectors_key_handle[i];
        Symmetric128BitsKeyByteArray material;
        Hmac128KeyHandle key;
        uint8_t out[kSHA256_Hash_Length];
        bool ok = v.key_length == sizeof(material) && v.output_hash_length <= sizeof(out);

        if (ok)
        {
            memcpy(material, v.key, sizeof(material));
            ok = keystore.CreateKey(material, key) == CHIP_NO_ERROR &&
                mac.HMAC_SHA256(key, v.message, v.message_length, out, v.output_hash_length) == CHIP_NO_ERROR &&
                memcmp(out, v.output_hash, v.output_hash_length) == 0;
            keystore.DestroyKey(key);
            ClearSecretData(material);
        }
        if (!ok)
        {
            return raw + static_cast<int>(i);
        }
    }
    return -1;
}

int Hkdf()
{
    HKDF_sha hkdf;

    for (size_t i = 0; i < MATTER_ARRAY_SIZE(hkdf_sha256_test_vectors); i++)
    {
        const hkdf_sha256_vector & v = hkdf_sha256_test_vectors[i];
        uint8_t out[128];

        if (v.output_key_material_length > sizeof(out) ||
            hkdf.HKDF_SHA256(v.initial_key_material, v.initial_key_material_length, v.salt, v.salt_length, v.info,
                             v.info_length, out, v.output_key_material_length) != CHIP_NO_ERROR ||
            memcmp(out, v.output_key_material, v.output_key_material_length) != 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Pbkdf2()
{
    PBKDF2_sha256 pbkdf2;

    for (size_t i = 0; i < MATTER_ARRAY_SIZE(pbkdf2_sha256_test_vectors); i++)
    {
        const pbkdf2_test_vector * v = pbkdf2_sha256_test_vectors[i];
        uint8_t out[64];

        if (v->plen == 0) // the SDK's tests skip these too
        {
            continue;
        }
        if (v->key_len > sizeof(out) ||
            pbkdf2.pbkdf2_sha256(v->password, v->plen, v->salt, v->slen, v->iter, v->key_len, out) != v->result ||
            (v->result == CHIP_NO_ERROR && memcmp(out, v->key, v->key_len) != 0))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool CcmOne(SessionKeystore & keystore, const ccm_128_test_vector & v)
{
    Platform::ScopedMemoryBuffer<uint8_t> ct;
    Platform::ScopedMemoryBuffer<uint8_t> pt;
    uint8_t tag[16];
    Symmetric128BitsKeyByteArray material;
    Aes128KeyHandle key;
    bool ok = v.key_len == sizeof(material) && v.tag_len <= sizeof(tag) && (v.ct_len == 0 || ct.Alloc(v.ct_len)) &&
        (v.pt_len == 0 || pt.Alloc(v.pt_len));

    if (!ok)
    {
        return false;
    }
    memcpy(material, v.key, sizeof(material));
    ok = keystore.CreateKey(material, key) == CHIP_NO_ERROR;
    ClearSecretData(material);
    // a buffer of length 0 is nullptr, as the SDK's tests give it
    ok = ok &&
        AES_CCM_encrypt(v.pt, v.pt_len, v.aad, v.aad_len, key, v.nonce, v.nonce_len, ct.Get(), tag, v.tag_len) == v.result &&
        (v.result != CHIP_NO_ERROR ||
         ((v.ct_len == 0 || memcmp(ct.Get(), v.ct, v.ct_len) == 0) && memcmp(tag, v.tag, v.tag_len) == 0)) &&
        AES_CCM_decrypt(v.ct, v.ct_len, v.aad, v.aad_len, v.tag, v.tag_len, key, v.nonce, v.nonce_len, pt.Get()) == v.result &&
        (v.result != CHIP_NO_ERROR || v.pt_len == 0 || memcmp(pt.Get(), v.pt, v.pt_len) == 0);

    // a message changed on the way must not decrypt
    if (ok && v.result == CHIP_NO_ERROR && v.tag_len > 0)
    {
        memcpy(tag, v.tag, v.tag_len);
        tag[0] ^= 1;
        ok = AES_CCM_decrypt(v.ct, v.ct_len, v.aad, v.aad_len, tag, v.tag_len, key, v.nonce, v.nonce_len, pt.Get()) !=
            CHIP_NO_ERROR;
    }
    keystore.DestroyKey(key);
    return ok;
}

int Ccm(SessionKeystore & keystore)
{
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(ccm_128_test_vectors); i++)
    {
        if (!CcmOne(keystore, *ccm_128_test_vectors[i]))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// RFC 3686 section 6 with the Matter IV (flags | 13-byte nonce | counter), as the SDK's tests give them; message
// privacy uses it
struct CtrVector
{
    const char * key;
    const char * nonce;
    const char * plain;
    const char * cipher;
    size_t length;
};

const CtrVector kCtr[] = {
    { "\xae\x68\x52\xf8\x12\x10\x67\xcc\x4b\xf7\xa5\x76\x55\x77\xf3\x9e",
      "\x00\x00\x00\x30\x00\x00\x00\x00\x00\x00\x00\x00\x00",
      "\x53\x69\x6e\x67\x6c\x65\x20\x62\x6c\x6f\x63\x6b\x20\x6d\x73\x67",
      "\x0d\x0a\x6b\x6d\xc1\xf6\x9b\x4d\x14\xca\x4c\x15\x42\x22\x42\xc4", 16 },
    { "\x7e\x24\x06\x78\x17\xfa\xe0\xd7\x43\xd6\xce\x1f\x32\x53\x91\x63",
      "\x00\x6c\xb6\xdb\xc0\x54\x3b\x59\xda\x48\xd9\x0b\x00",
      "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
      "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
      "\x4f\x3d\xf9\x49\x15\x88\x4d\xe0\xdc\x0e\x30\x95\x0d\xe7\xa6\xe9"
      "\x5a\x91\x7e\x1d\x06\x42\x22\xdb\x2f\x6e\xc7\x3d\x99\x4a\xd9\x5f",
      32 },
};

int Ctr(SessionKeystore & keystore)
{
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(kCtr); i++)
    {
        const CtrVector & v = kCtr[i];
        Symmetric128BitsKeyByteArray material;
        Aes128KeyHandle key;
        uint8_t out[32];
        uint8_t back[32];

        memcpy(material, v.key, sizeof(material));
        bool ok = keystore.CreateKey(material, key) == CHIP_NO_ERROR &&
            AES_CTR_crypt(Uint8::from_const_char(v.plain), v.length, key, Uint8::from_const_char(v.nonce), 13, out) ==
                CHIP_NO_ERROR &&
            memcmp(out, v.cipher, v.length) == 0 &&
            AES_CTR_crypt(out, v.length, key, Uint8::from_const_char(v.nonce), 13, back) == CHIP_NO_ERROR &&
            memcmp(back, v.plain, v.length) == 0;

        keystore.DestroyKey(key);
        ClearSecretData(material);
        if (!ok)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// RFC 6979 A.2.5 keys and signatures: the node signs attestation, CSRs and CASE Sigma2 with its keys and checks
// certificate chains and Sigma3 against public keys
int Ecdsa()
{
    const int count = static_cast<int>(MATTER_ARRAY_SIZE(ecdsa_det_test_vectors));

    for (int i = 0; i < count; i++)
    {
        const ecdsa_det_test_vector & v = ecdsa_det_test_vectors[i];
        const uint8_t * msg             = Uint8::from_const_char(v.message);
        const size_t length             = strlen(v.message);
        P256SerializedKeypair serialized;
        P256Keypair keypair;
        P256ECDSASignature known;
        P256ECDSASignature fresh;
        uint8_t * p = serialized.Bytes();

        *p++ = 0x04;
        memcpy(p, v.public_key_x, sizeof(v.public_key_x));
        memcpy(p + 32, v.public_key_y, sizeof(v.public_key_y));
        memcpy(p + 64, v.private_key, sizeof(v.private_key));
        memcpy(known.Bytes(), v.r, sizeof(v.r));
        memcpy(known.Bytes() + 32, v.s, sizeof(v.s));
        bool ok = serialized.SetLength(serialized.Capacity()) == CHIP_NO_ERROR && known.SetLength(64) == CHIP_NO_ERROR &&
            keypair.Deserialize(serialized) == CHIP_NO_ERROR &&
            // the RFC's signature checks, and one over another message does not
            keypair.Pubkey().ECDSA_validate_msg_signature(msg, length, known) == CHIP_NO_ERROR &&
            keypair.Pubkey().ECDSA_validate_msg_signature(msg, length - 1, known) != CHIP_NO_ERROR &&
            // what the key signs now checks against the RFC's public key
            keypair.ECDSA_sign_msg(msg, length, fresh) == CHIP_NO_ERROR &&
            keypair.Pubkey().ECDSA_validate_msg_signature(msg, length, fresh) == CHIP_NO_ERROR;

        ClearSecretData(serialized.Bytes(), serialized.Capacity());
        if (!ok)
        {
            return i;
        }
    }

    // a key the node makes, kept and loaded again as the operational keystore does
    P256Keypair made;
    P256Keypair loaded;
    P256SerializedKeypair serialized;
    P256ECDSASignature signature;
    static const char kMessage[] = "u-connectXpressApp";
    const uint8_t * msg          = Uint8::from_const_char(kMessage);
    bool ok = made.Initialize(ECPKeyTarget::ECDSA) == CHIP_NO_ERROR && made.Serialize(serialized) == CHIP_NO_ERROR &&
        loaded.Deserialize(serialized) == CHIP_NO_ERROR && loaded.ECDSA_sign_msg(msg, sizeof(kMessage), signature) == CHIP_NO_ERROR &&
        made.Pubkey().ECDSA_validate_msg_signature(msg, sizeof(kMessage), signature) == CHIP_NO_ERROR;

    ClearSecretData(serialized.Bytes(), serialized.Capacity());
    return ok ? -1 : count;
}

// The SDK's ECDH vectors: the node's CASE secret
const uint8_t kEcdhPrivate[] = { 0x7d, 0x7d, 0xc5, 0xf7, 0x1e, 0xb2, 0x9d, 0xda, 0xf8, 0x0d, 0x62, 0x14, 0x63, 0x2e, 0xea, 0xe0,
                                 0x3d, 0x90, 0x58, 0xaf, 0x1f, 0xb6, 0xd2, 0x2e, 0xd8, 0x0b, 0xad, 0xb6, 0x2b, 0xc1, 0xa5, 0x34 };
const uint8_t kEcdhRemote[]  = { 0x04, 0x70, 0x0c, 0x48, 0xf7, 0x7f, 0x56, 0x58, 0x4c, 0x5c, 0xc6, 0x32, 0xca,
                                 0x65, 0x64, 0x0d, 0xb9, 0x1b, 0x6b, 0xac, 0xce, 0x3a, 0x4d, 0xf6, 0xb4, 0x2c,
                                 0xe7, 0xcc, 0x83, 0x88, 0x33, 0xd2, 0x87, 0xdb, 0x71, 0xe5, 0x09, 0xe3, 0xfd,
                                 0x9b, 0x06, 0x0d, 0xdb, 0x20, 0xba, 0x5c, 0x51, 0xdc, 0xc5, 0x94, 0x8d, 0x46,
                                 0xfb, 0xf6, 0x40, 0xdf, 0xe0, 0x44, 0x17, 0x82, 0xca, 0xb8, 0x5f, 0xa4, 0xac };
const uint8_t kEcdhShared[]  = { 0x46, 0xfc, 0x62, 0x10, 0x64, 0x20, 0xff, 0x01, 0x2e, 0x54, 0xa4, 0x34, 0xfb, 0xdd, 0x2d, 0x25,
                                 0xcc, 0xc5, 0x85, 0x20, 0x60, 0x56, 0x1e, 0x68, 0x04, 0x0d, 0xd7, 0x77, 0x89, 0x97, 0xbd, 0x7b };

int Ecdh()
{
    // The known answer through PSA: the SDK's keypair takes no raw private key on the PSA backend
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id                 = 0;
    uint8_t shared[32]              = {};
    size_t length                   = 0;

    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
    bool ok = psa_import_key(&attributes, kEcdhPrivate, sizeof(kEcdhPrivate), &id) == PSA_SUCCESS &&
        psa_raw_key_agreement(PSA_ALG_ECDH, id, kEcdhRemote, sizeof(kEcdhRemote), shared, sizeof(shared), &length) ==
            PSA_SUCCESS &&
        length == sizeof(kEcdhShared) && memcmp(shared, kEcdhShared, sizeof(shared)) == 0;

    psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    ClearSecretData(shared);
    if (!ok)
    {
        return 0;
    }

    // Two of the SDK's keypairs, as CASE makes them, come to the same secret
    P256Keypair a;
    P256Keypair b;
    P256ECDHDerivedSecret ab;
    P256ECDHDerivedSecret ba;

    ok = a.Initialize(ECPKeyTarget::ECDH) == CHIP_NO_ERROR && b.Initialize(ECPKeyTarget::ECDH) == CHIP_NO_ERROR &&
        a.ECDH_derive_secret(b.Pubkey(), ab) == CHIP_NO_ERROR && b.ECDH_derive_secret(a.Pubkey(), ba) == CHIP_NO_ERROR &&
        ab.Length() == kP256_FE_Length && ba.Length() == ab.Length() && memcmp(ab.ConstBytes(), ba.ConstBytes(), ab.Length()) == 0;
    return ok ? -1 : 1;
}

// The node answers a commissioner's CSRRequest with one: it must carry the key and its signature must hold
int Csr()
{
    P256Keypair keypair;
    P256PublicKey key;
    uint8_t csr[kMIN_CSR_Buffer_Size];
    size_t length = sizeof(csr);

    if (keypair.Initialize(ECPKeyTarget::ECDSA) != CHIP_NO_ERROR || keypair.NewCertificateSigningRequest(csr, length) != CHIP_NO_ERROR ||
        length < 2 || VerifyCertificateSigningRequest(csr, length, key) != CHIP_NO_ERROR ||
        memcmp(key.ConstBytes(), keypair.Pubkey().ConstBytes(), kP256_PublicKey_Length) != 0)
    {
        return 0;
    }
    csr[length - 2]++; // in the signature
    csr[length - 1]++;
    return VerifyCertificateSigningRequest(csr, length, key) != CHIP_NO_ERROR ? -1 : 1;
}

// The SDK's SPAKE2+ with the random scalar of the RFC vectors
class FixedSpake2p : public Spake2p_P256_SHA256_HKDF_HMAC
{
public:
    void Fix(const uint8_t * fe, size_t length)
    {
        mFe     = fe;
        mLength = length;
    }
    CHIP_ERROR FEGenerate(void * fe) override { return FELoad(mFe, mLength, fe); }

private:
    const uint8_t * mFe = nullptr;
    size_t mLength      = 0;
};

// Two HKDF keys are the same when they give the same session keys (handles do not show their bytes)
bool SameKeys(SessionKeystore & keystore, const HkdfKeyHandle & a, const HkdfKeyHandle & b)
{
    static const uint8_t kSalt[] = { 'T', 'E', 'S', 'T', 'S', 'A', 'L', 'T' };
    static const uint8_t kInfo[] = { 'T', 'E', 'S', 'T', 'I', 'N', 'F', 'O' };
    AttestationChallenge challenge[2];
    bool ok = true;

    for (int i = 0; i < 2; i++)
    {
        Aes128KeyHandle i2r;
        Aes128KeyHandle r2i;

        ok = ok && keystore.DeriveSessionKeys(i == 0 ? a : b, ByteSpan(kSalt), ByteSpan(kInfo), i2r, r2i, challenge[i]) == CHIP_NO_ERROR;
        keystore.DestroyKey(i2r);
        keystore.DestroyKey(r2i);
    }
    return ok && memcmp(challenge[0].ConstBytes(), challenge[1].ConstBytes(), AttestationChallenge::Capacity()) == 0;
}

bool Spake2pOne(SessionKeystore & keystore, const spake2p_rfc_tv & v)
{
    FixedSpake2p prover;
    FixedSpake2p verifier;
    uint8_t L[kMAX_Point_Length];
    uint8_t X[kMAX_Point_Length];
    uint8_t Y[kMAX_Point_Length];
    uint8_t macA[kMAX_Hash_Length];
    uint8_t macB[kMAX_Hash_Length];
    size_t lLength = sizeof(L);
    size_t xLength = sizeof(X);
    size_t yLength = sizeof(Y);
    size_t aLength = sizeof(macA);
    size_t bLength = sizeof(macB);
    HkdfKeyHandle known;
    HkdfKeyHandle proverKe;
    HkdfKeyHandle verifierKe;

    prover.Fix(v.x, v.x_len);
    verifier.Fix(v.y, v.y_len);
    bool ok = prover.Init(v.context, v.context_len) == CHIP_NO_ERROR &&
        prover.BeginProver(v.prover_identity, v.prover_identity_len, v.verifier_identity, v.verifier_identity_len, v.w0,
                           v.w0_len, v.w1, v.w1_len) == CHIP_NO_ERROR &&
        prover.ComputeRoundOne(nullptr, 0, X, &xLength) == CHIP_NO_ERROR && xLength == v.X_len && memcmp(X, v.X, v.X_len) == 0 &&
        // the node is the verifier: L is what it keeps instead of the passcode
        verifier.Init(v.context, v.context_len) == CHIP_NO_ERROR &&
        verifier.ComputeL(L, &lLength, v.w1, v.w1_len) == CHIP_NO_ERROR && lLength == v.L_len && memcmp(L, v.L, v.L_len) == 0 &&
        verifier.BeginVerifier(v.verifier_identity, v.verifier_identity_len, v.prover_identity, v.prover_identity_len, v.w0,
                               v.w0_len, L, lLength) == CHIP_NO_ERROR &&
        verifier.ComputeRoundOne(X, xLength, Y, &yLength) == CHIP_NO_ERROR && yLength == v.Y_len && memcmp(Y, v.Y, v.Y_len) == 0 &&
        verifier.ComputeRoundTwo(X, xLength, macB, &bLength) == CHIP_NO_ERROR && bLength == v.MAC_KcB_len &&
        memcmp(macB, v.MAC_KcB, v.MAC_KcB_len) == 0 &&
        prover.ComputeRoundTwo(Y, yLength, macA, &aLength) == CHIP_NO_ERROR && aLength == v.MAC_KcA_len &&
        memcmp(macA, v.MAC_KcA, v.MAC_KcA_len) == 0 && prover.KeyConfirm(macB, bLength) == CHIP_NO_ERROR &&
        verifier.KeyConfirm(macA, aLength) == CHIP_NO_ERROR;

    // the keys both sides now hold are the vector's Ke
    if (ok)
    {
        ok = keystore.CreateKey(ByteSpan(v.Ke, v.Ke_len), known) == CHIP_NO_ERROR &&
            prover.GetKeys(keystore, proverKe) == CHIP_NO_ERROR && verifier.GetKeys(keystore, verifierKe) == CHIP_NO_ERROR &&
            SameKeys(keystore, proverKe, known) && SameKeys(keystore, verifierKe, known);
        keystore.DestroyKey(known);
        keystore.DestroyKey(proverKe);
        keystore.DestroyKey(verifierKe);
    }

    // a wrong confirmation is refused
    if (ok)
    {
        FixedSpake2p again;
        uint8_t X2[kMAX_Point_Length];
        uint8_t Y2[kMAX_Point_Length];
        uint8_t mac[kMAX_Hash_Length];
        size_t x2 = sizeof(X2);
        size_t y2 = sizeof(Y2);
        size_t m  = sizeof(mac);
        FixedSpake2p peer;

        again.Fix(v.y, v.y_len);
        peer.Fix(v.x, v.x_len);
        macA[0] ^= 1;
        ok = peer.Init(v.context, v.context_len) == CHIP_NO_ERROR &&
            peer.BeginProver(v.prover_identity, v.prover_identity_len, v.verifier_identity, v.verifier_identity_len, v.w0,
                             v.w0_len, v.w1, v.w1_len) == CHIP_NO_ERROR &&
            peer.ComputeRoundOne(nullptr, 0, X2, &x2) == CHIP_NO_ERROR && again.Init(v.context, v.context_len) == CHIP_NO_ERROR &&
            again.BeginVerifier(v.verifier_identity, v.verifier_identity_len, v.prover_identity, v.prover_identity_len, v.w0,
                                v.w0_len, L, lLength) == CHIP_NO_ERROR &&
            again.ComputeRoundOne(X2, x2, Y2, &y2) == CHIP_NO_ERROR && again.ComputeRoundTwo(X2, x2, mac, &m) == CHIP_NO_ERROR &&
            again.KeyConfirm(macA, aLength) != CHIP_NO_ERROR;
    }
    return ok;
}

int Spake2p(SessionKeystore & keystore)
{
    for (size_t i = 0; i < MATTER_ARRAY_SIZE(rfc_tvs); i++)
    {
        if (!Spake2pOne(keystore, *rfc_tvs[i]))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// RFC 3610 packet vector 1, the message the session keys below encrypt
const Symmetric128BitsKeyByteArray kCcmKey = { 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
                                               0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf };
const uint8_t kCcmNonce[13] = { 0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5 };
const uint8_t kCcmAad[8]    = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
const uint8_t kCcmPlain[23] = { 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
                                0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e };

// The handle encrypts as a key made from these bytes does
bool SameAes(SessionKeystore & keystore, const Aes128KeyHandle & handle, const uint8_t * bytes)
{
    Symmetric128BitsKeyByteArray material;
    Aes128KeyHandle key;
    uint8_t cipher[2][sizeof(kCcmPlain)];
    uint8_t tag[2][16];

    memcpy(material, bytes, sizeof(material));
    bool ok = keystore.CreateKey(material, key) == CHIP_NO_ERROR;
    for (int i = 0; ok && i < 2; i++)
    {
        ok = AES_CCM_encrypt(kCcmPlain, sizeof(kCcmPlain), kCcmAad, sizeof(kCcmAad), i == 0 ? handle : key, kCcmNonce,
                             sizeof(kCcmNonce), cipher[i], tag[i], 16) == CHIP_NO_ERROR;
    }
    keystore.DestroyKey(key);
    ClearSecretData(material);
    return ok && memcmp(cipher[0], cipher[1], sizeof(cipher[0])) == 0 && memcmp(tag[0], tag[1], sizeof(tag[0])) == 0;
}

// Session keys as PASE and CASE derive them: their handles must encrypt as the HKDF bytes they come from
int SessionKeys(SessionKeystore & keystore)
{
    static const char kInfo[] = "SessionKeys";
    static const uint8_t kSecret[32] = { 0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f,
                                         0x64, 0xd0, 0x36, 0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a,
                                         0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf };
    static const uint8_t kSalt[13] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c };
    const ByteSpan info(Uint8::from_const_char(kInfo), sizeof(kInfo) - 1);
    HKDF_sha hkdf;
    Aes128KeyHandle i2r;
    Aes128KeyHandle r2i;
    AttestationChallenge challenge;
    uint8_t bytes[48] = {};

    bool ok = keystore.DeriveSessionKeys(ByteSpan(kSecret), ByteSpan(kSalt), info, i2r, r2i, challenge) == CHIP_NO_ERROR &&
        hkdf.HKDF_SHA256(kSecret, sizeof(kSecret), kSalt, sizeof(kSalt), info.data(), info.size(), bytes, sizeof(bytes)) ==
            CHIP_NO_ERROR &&
        memcmp(challenge.ConstBytes(), bytes + 32, 16) == 0 && SameAes(keystore, i2r, bytes) && SameAes(keystore, r2i, bytes + 16);

    keystore.DestroyKey(i2r);
    keystore.DestroyKey(r2i);
    ClearSecretData(bytes);
    return ok ? -1 : 0;
}

// The Matter specification's example (Operational Discovery): the ID in the node's operational DNS-SD name
int FabricId()
{
    static const uint8_t kRoot[] = { 0x04, 0x4a, 0x9f, 0x42, 0xb1, 0xca, 0x48, 0x40, 0xd3, 0x72, 0x92, 0xbb, 0xc7, 0xf6, 0xa7,
                                     0xe1, 0x1e, 0x22, 0x20, 0x0c, 0x97, 0x6f, 0xc9, 0x00, 0xdb, 0xc9, 0x8a, 0x7a, 0x38, 0x3a,
                                     0x64, 0x1c, 0xb8, 0x25, 0x4a, 0x2e, 0x56, 0xd4, 0xe2, 0x95, 0xa8, 0x47, 0x94, 0x3b, 0x4e,
                                     0x38, 0x97, 0xc4, 0xa7, 0x73, 0xe9, 0x30, 0x27, 0x7b, 0x4d, 0x9f, 0xbe, 0xde, 0x8a, 0x05,
                                     0x26, 0x86, 0xbf, 0xac, 0xfa };
    static const uint8_t kExpect[] = { 0x87, 0xe1, 0xb0, 0x04, 0xe2, 0x35, 0xa1, 0x30 };
    const P256PublicKey root(kRoot);
    uint8_t id[kCompressedFabricIdentifierSize] = {};
    MutableByteSpan out(id);

    return GenerateCompressedFabricId(root, 0x2906C908D115D362, out) == CHIP_NO_ERROR && out.size() == sizeof(kExpect) &&
            memcmp(id, kExpect, sizeof(kExpect)) == 0
        ? -1
        : 0;
}

} // namespace

void CryptoCheck(CryptoReport report, void * context)
{
    DefaultSessionKeystore keystore;
    Report check(report, context);

    if (psa_crypto_init() != PSA_SUCCESS)
    {
        check("crypto", 0, 1, "psa_crypto_init failed");
        return;
    }
    check("SHA-256", Sha256(), static_cast<int>(MATTER_ARRAY_SIZE(hash_sha256_test_vectors)),
          "FIPS 180-2 hashes, in one go and streamed");
    check("HMAC", Hmac(keystore),
          static_cast<int>(MATTER_ARRAY_SIZE(hmac_sha256_test_vectors_raw_key) + MATTER_ARRAY_SIZE(hmac_sha256_test_vectors_key_handle)),
          "RFC 4231 HMAC-SHA256, raw key and key handle");
    check("HKDF", Hkdf(), static_cast<int>(MATTER_ARRAY_SIZE(hkdf_sha256_test_vectors)), "RFC 5869 HKDF-SHA256");
    check("PBKDF2", Pbkdf2(), static_cast<int>(MATTER_ARRAY_SIZE(pbkdf2_sha256_test_vectors)),
          "RFC 7914 PBKDF2-SHA256 (the passcode verifier)");
    check("AES-CCM", Ccm(keystore), static_cast<int>(MATTER_ARRAY_SIZE(ccm_128_test_vectors)),
          "NIST and RFC 3610 AES-128-CCM through key handles, bad tags refused");
    check("AES-CTR", Ctr(keystore), static_cast<int>(MATTER_ARRAY_SIZE(kCtr)), "RFC 3686 AES-128-CTR (message privacy)");
    check("ECDSA", Ecdsa(), static_cast<int>(MATTER_ARRAY_SIZE(ecdsa_det_test_vectors)) + 1,
          "RFC 6979 P-256 signatures check, signing and stored keys work");
    check("ECDH", Ecdh(), 2, "P-256 shared secret known answer, two keypairs agree");
    check("CSR", Csr(), 2, "the node's CSR carries its key, a changed one is refused");
    check("SPAKE2+", Spake2p(keystore), static_cast<int>(MATTER_ARRAY_SIZE(rfc_tvs)),
          "SPAKE2+ P-256 vectors end to end, a wrong confirmation refused");
    check("session keys", SessionKeys(keystore), 1, "PASE and CASE key handles encrypt as their HKDF bytes");
    check("fabric ID", FabricId(), 1, "the specification's compressed fabric ID");
}

} // namespace ucxm
