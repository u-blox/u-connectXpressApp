/* The node's own setup codes: discriminator, passcode, SPAKE2+ salt and the Basic Information UniqueID.
 *
 * Drawn at random the first time the node starts and kept in the app's secure store, as the i.MX 93 kit does
 * for its end nodes (u-connectMatter/companion/endnode/tools/gen-commissioning-id.py): two demos in one room
 * never share the SDK's test codes (3840, 20202021) and no code can be worked out from a module's address.
 * u-connectMatter's provider of the same name derived all three from the last three bytes of the address. */
#pragma once

#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>
#include <platform/CommissionableDataProvider.h>

#include <cstddef>
#include <cstdint>

namespace ucxmatter {

class UbloxCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    static constexpr uint32_t kIterationCount = 1000;
    static constexpr size_t kSaltLen          = 32;
    static constexpr size_t kUniqueIdLen      = 16;

    // The stored codes, or new ones when there are none (renew: new ones in any case). With the SDK's memory up.
    CHIP_ERROR Init();

    uint16_t Discriminator() const { return mDiscriminator; }
    uint32_t Passcode() const { return mPasscode; }
    void UniqueId(char * out, size_t size) const; // 32 hex digits

    CHIP_ERROR GetSetupDiscriminator(uint16_t & out) override;
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t & out) override;
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan & out) override;
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan & out, size_t & length) override;
    CHIP_ERROR GetSetupPasscode(uint32_t & out) override;
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    bool Load();
    CHIP_ERROR Draw();

    bool mReady             = false;
    uint16_t mDiscriminator = 0;
    uint32_t mPasscode      = 0;
    uint8_t mSalt[kSaltLen] = {};
    uint8_t mUniqueId[kUniqueIdLen] = {};
    uint8_t mVerifier[97]   = {}; // chip::Crypto::kSpake2p_VerifierSerialized_Length
    size_t mVerifierLen     = 0;
};

} // namespace ucxmatter
