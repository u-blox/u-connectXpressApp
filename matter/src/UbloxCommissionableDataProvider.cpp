#include "UbloxCommissionableDataProvider.h"

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include <platform/ucxclient/UcxStore.h>

#include <cstring>

namespace ucxmatter {
namespace {

static_assert(chip::Crypto::kSpake2p_VerifierSerialized_Length == 97, "mVerifier has the size of a verifier");

// "factory": matter.reset keeps this blob (ucxapp_matter.h)
constexpr char kStoreName[]     = "factory";
constexpr char kDiscriminator[] = "discriminator";
constexpr char kPasscode[]      = "passcode";
constexpr char kSalt[]          = "salt";
constexpr char kUniqueId[]      = "uniqueid";

// Matter 5.1.7.1: passcodes no device may have; 20202021 is the SDK's test one
bool PasscodeOk(uint32_t p)
{
    return p >= 1 && p <= 99999998 && p % 11111111 != 0 && p != 12345678 && p != 87654321 && p != 20202021;
}

// 3840 to 3842 are what the SDK's examples advertise
bool DiscriminatorOk(uint16_t d)
{
    return d <= 0xFFF && (d < 0xF00 || d > 0xF02);
}

template <typename T>
bool Get(const BlobMap & map, const char * key, T & value)
{
    auto it = map.find(key);

    if (it == map.end() || it->second.size() != sizeof(T))
    {
        return false;
    }
    memcpy(&value, it->second.data(), sizeof(T));
    return true;
}

template <typename T>
void Put(BlobMap & map, const char * key, const T & value)
{
    const uint8_t * bytes = reinterpret_cast<const uint8_t *>(&value);

    map[key].assign(bytes, bytes + sizeof(T));
}

} // namespace

bool UbloxCommissionableDataProvider::Load()
{
    BlobMap map;

    return StoreLoad(kStoreName, map) && Get(map, kDiscriminator, mDiscriminator) && Get(map, kPasscode, mPasscode) &&
        Get(map, kSalt, mSalt) && Get(map, kUniqueId, mUniqueId) && DiscriminatorOk(mDiscriminator) &&
        PasscodeOk(mPasscode);
}

CHIP_ERROR UbloxCommissionableDataProvider::Draw()
{
    do
    {
        ReturnErrorOnFailure(chip::Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&mPasscode), sizeof(mPasscode)));
        mPasscode &= 0x07FFFFFF; // 27 bits
    } while (!PasscodeOk(mPasscode));
    do
    {
        ReturnErrorOnFailure(
            chip::Crypto::DRBG_get_bytes(reinterpret_cast<uint8_t *>(&mDiscriminator), sizeof(mDiscriminator)));
        mDiscriminator &= 0x0FFF;
    } while (!DiscriminatorOk(mDiscriminator));
    ReturnErrorOnFailure(chip::Crypto::DRBG_get_bytes(mSalt, sizeof(mSalt)));
    return chip::Crypto::DRBG_get_bytes(mUniqueId, sizeof(mUniqueId));
}

CHIP_ERROR UbloxCommissionableDataProvider::Init(bool renew)
{
    chip::Crypto::Spake2pVerifier verifier;
    chip::MutableByteSpan serialized(mVerifier, sizeof(mVerifier));

    mReady = false;
    if (renew || !Load())
    {
        BlobMap map;

        ReturnErrorOnFailure(Draw());
        Put(map, kDiscriminator, mDiscriminator);
        Put(map, kPasscode, mPasscode);
        Put(map, kSalt, mSalt);
        Put(map, kUniqueId, mUniqueId);
        if (!StoreSave(kStoreName, map))
        {
            // It still commissions; the next start then has other codes
            ChipLogError(DeviceLayer, "the setup codes were not stored");
        }
    }
    ReturnErrorOnFailure(verifier.Generate(kIterationCount, chip::ByteSpan(mSalt, kSaltLen), mPasscode));
    ReturnErrorOnFailure(verifier.Serialize(serialized));
    mVerifierLen = serialized.size();
    mReady       = true;
    return CHIP_NO_ERROR;
}

void UbloxCommissionableDataProvider::UniqueId(char * out, size_t size) const
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t n                 = 0;

    for (size_t i = 0; i < kUniqueIdLen && n + 2 < size; i++)
    {
        out[n++] = kHex[mUniqueId[i] >> 4];
        out[n++] = kHex[mUniqueId[i] & 0x0F];
    }
    if (size > 0)
    {
        out[n] = '\0';
    }
}

CHIP_ERROR UbloxCommissionableDataProvider::GetSetupDiscriminator(uint16_t & out)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    out = mDiscriminator;
    return CHIP_NO_ERROR;
}

CHIP_ERROR UbloxCommissionableDataProvider::GetSpake2pIterationCount(uint32_t & out)
{
    out = kIterationCount;
    return CHIP_NO_ERROR;
}

CHIP_ERROR UbloxCommissionableDataProvider::GetSpake2pSalt(chip::MutableByteSpan & out)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    return chip::CopySpanToMutableSpan(chip::ByteSpan(mSalt, kSaltLen), out);
}

CHIP_ERROR UbloxCommissionableDataProvider::GetSpake2pVerifier(chip::MutableByteSpan & out, size_t & length)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    length = mVerifierLen;
    return chip::CopySpanToMutableSpan(chip::ByteSpan(mVerifier, mVerifierLen), out);
}

CHIP_ERROR UbloxCommissionableDataProvider::GetSetupPasscode(uint32_t & out)
{
    VerifyOrReturnError(mReady, CHIP_ERROR_INCORRECT_STATE);
    out = mPasscode;
    return CHIP_NO_ERROR;
}

} // namespace ucxmatter
