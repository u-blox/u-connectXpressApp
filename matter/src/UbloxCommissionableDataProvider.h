/*
 * UbloxCommissionableDataProvider.h
 *
 * Per-unit Matter commissioning identity for u-blox NORA-W36 reference units.
 *
 * Why this exists:
 *   The Matter SDK ships a TestOnlyCommissionableDataProvider that always
 *   returns discriminator=3840 and passcode=20202021 with a pre-baked Spake2+
 *   verifier. When multiple demo units are powered on in BLE range (e.g. at a
 *   trade-show booth), commissioning controllers (Apple/Google Home) match
 *   candidates by *discriminator only* (Matter spec section 5.4.2) and pick
 *   the strongest RSSI — which is NOT necessarily the unit whose QR code the
 *   user just scanned. Result: "commissioned the wrong device" failures.
 *
 * What this provider does:
 *   1. Derives a 12-bit discriminator from the NORA-W36 MAC (4096 values).
 *   2. Derives a 27-bit setup passcode from the MAC, filtering the Matter
 *      spec-forbidden values (11111111, 12345678, 20202021, ...).
 *   3. Derives a deterministic 16-byte Spake2+ salt from the MAC.
 *   4. Computes the Spake2+ verifier on demand from (passcode, salt, iter).
 *
 * Result: every NORA-W36 unit ships with a unique commissioning identity tied
 * to its MAC address. No two units in the wild collide on discriminator with
 * meaningful probability for booth-scale deployments, and the setup passcode
 * is never the test value 20202021 (required for CSA certification).
 */
#pragma once

#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/Span.h>
#include <platform/CommissionableDataProvider.h>

#include <cstdint>
#include <cstring>

namespace ucxmatter {

class UbloxCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    UbloxCommissionableDataProvider() = default;

    // Pass-through fallbacks used if Init() is never called or MAC unknown.
    static constexpr uint16_t kFallbackDiscriminator = 2736;   // 0xAB0
    static constexpr uint32_t kFallbackPasscode      = 36363636;
    static constexpr uint32_t kIterationCount        = 1000;
    static constexpr size_t   kSaltLen               = 16;

    /**
     * Initialize from the 6-character MAC suffix (last 3 MAC bytes as hex,
     * e.g. "A1B2C3") returned by UcxClientManager::GetMacSuffix().
     * If macSuffix is null/empty/all-zero, the fallback values are used.
     * Returns CHIP_NO_ERROR on success (always succeeds — fallback is OK).
     */
    CHIP_ERROR Init(const char * macSuffix)
    {
        uint8_t macBytes[3] = { 0, 0, 0 };
        bool macValid       = ParseHexBytes(macSuffix, macBytes);

        if (macValid)
        {
            DeriveIdentity(macBytes);
        }
        else
        {
            mDiscriminator = kFallbackDiscriminator;
            mPasscode      = kFallbackPasscode;
            DeriveSalt(macBytes); // zero seed → fixed salt
        }

        // Compute Spake2+ verifier from passcode + salt + iter count.
        chip::Crypto::Spake2pVerifier verifier;
        CHIP_ERROR err = verifier.Generate(kIterationCount,
                                           chip::ByteSpan(mSalt, kSaltLen),
                                           mPasscode);
        if (err != CHIP_NO_ERROR)
        {
            return err;
        }

        chip::MutableByteSpan serialized(mVerifier, sizeof(mVerifier));
        ReturnErrorOnFailure(verifier.Serialize(serialized));
        mVerifierLen = serialized.size();
        mReady       = true;
        return CHIP_NO_ERROR;
    }

    uint16_t Discriminator() const { return mDiscriminator; }
    uint32_t Passcode() const      { return mPasscode; }

    // ===== CommissionableDataProvider overrides =====

    CHIP_ERROR GetSetupDiscriminator(uint16_t & out) override
    {
        out = mReady ? mDiscriminator : kFallbackDiscriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t & it) override
    {
        it = kIterationCount;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan & buf) override
    {
        if (buf.size() < kSaltLen) return CHIP_ERROR_BUFFER_TOO_SMALL;
        memcpy(buf.data(), mSalt, kSaltLen);
        buf.reduce_size(kSaltLen);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan & buf, size_t & outLen) override
    {
        if (!mReady) return CHIP_ERROR_INCORRECT_STATE;
        outLen = mVerifierLen;
        if (buf.size() < outLen) return CHIP_ERROR_BUFFER_TOO_SMALL;
        memcpy(buf.data(), mVerifier, outLen);
        buf.reduce_size(outLen);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSetupPasscode(uint32_t & out) override
    {
        out = mReady ? mPasscode : kFallbackPasscode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    static bool ParseHexBytes(const char * s, uint8_t out[3])
    {
        if (s == nullptr) return false;
        size_t i = 0;
        for (; i < 6 && s[i] != '\0'; ++i)
        {
            char c = s[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f')))
            {
                return false;
            }
        }
        if (i != 6) return false;

        auto nyb = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return static_cast<uint8_t>(c - 'a' + 10);
        };
        out[0] = static_cast<uint8_t>((nyb(s[0]) << 4) | nyb(s[1]));
        out[1] = static_cast<uint8_t>((nyb(s[2]) << 4) | nyb(s[3]));
        out[2] = static_cast<uint8_t>((nyb(s[4]) << 4) | nyb(s[5]));
        // Reject the all-zero suffix — MAC not yet known.
        return (out[0] | out[1] | out[2]) != 0;
    }

    void DeriveIdentity(const uint8_t mac[3])
    {
        // 24 bits of MAC entropy. Mix with a 32-bit Knuth multiplicative hash
        // so passcode and discriminator are not trivially correlated.
        uint32_t macInt = (static_cast<uint32_t>(mac[0]) << 16) |
                          (static_cast<uint32_t>(mac[1]) << 8)  |
                           static_cast<uint32_t>(mac[2]);

        // ----- Discriminator: 12 bits, avoid 0x000 and 0xFFF -----
        // Fold high/low halves so all 24 bits contribute.
        uint16_t disc = static_cast<uint16_t>((macInt ^ (macInt >> 12)) & 0x0FFF);
        if (disc == 0x000) disc = 0x001;
        if (disc == 0xFFF) disc = 0xFFE;
        // Avoid the SDK default (3840 / 0xF00) so we never look like a stock
        // sample device on the air even if the hash lands there.
        if (disc == 0xF00) disc = 0xF01;
        mDiscriminator = disc;

        // ----- Passcode: 27-bit, in 1..99,999,998, no forbidden values -----
        uint32_t hash = macInt * 2654435761u;            // Knuth golden ratio
        hash ^= (hash >> 16);
        hash *= 2246822519u;
        hash ^= (hash >> 13);

        uint32_t passcode = (hash % 99999998u) + 1;
        while (IsForbiddenPasscode(passcode))
        {
            passcode = (passcode % 99999998u) + 1u; // step deterministically
            if (passcode == 0) passcode = 1;
            // Add a perturbation to escape forbidden-value neighborhoods.
            passcode = (passcode + 7919u) % 99999998u + 1u;
        }
        mPasscode = passcode;

        DeriveSalt(mac);
    }

    void DeriveSalt(const uint8_t mac[3])
    {
        // Deterministic 16-byte salt = "UBLX-NORA-W36X-" + MAC byte, XORed
        // with index to ensure no two positions are equal even with mac=0.
        static const char kPrefix[] = "UBLX-NORA-W36X-";
        constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;       // 15
        static_assert(kPrefixLen < kSaltLen, "prefix must fit in salt");

        for (size_t i = 0; i < kPrefixLen; ++i)
        {
            mSalt[i] = static_cast<uint8_t>(kPrefix[i] ^ mac[i % 3]);
        }
        for (size_t i = kPrefixLen; i < kSaltLen; ++i)
        {
            mSalt[i] = static_cast<uint8_t>(mac[(i - kPrefixLen) % 3] ^ (i * 0x5Au));
        }
    }

    static bool IsForbiddenPasscode(uint32_t p)
    {
        // Matter spec section 5.1.6.1: invalid passcode values.
        switch (p)
        {
        case 0:
        case 11111111u:
        case 22222222u:
        case 33333333u:
        case 44444444u:
        case 55555555u:
        case 66666666u:
        case 77777777u:
        case 88888888u:
        case 99999999u:
        case 12345678u:
        case 87654321u:
        case 20202021u:                 // SDK test passcode — never ship this
            return true;
        default:
            return false;
        }
    }

    bool     mReady          = false;
    uint16_t mDiscriminator  = kFallbackDiscriminator;
    uint32_t mPasscode       = kFallbackPasscode;
    uint8_t  mSalt[kSaltLen] = {};
    uint8_t  mVerifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
    size_t   mVerifierLen    = 0;
};

} // namespace ucxmatter
