/* Setup codes of the node, made by the Matter SDK. Kept apart from the files that include u-connectClient: its
 * Windows port pulls in windows.h, whose ERROR, IN and OUT macros break the SDK's headers. */
#ifndef UCXM_ONBOARDING_H
#define UCXM_ONBOARDING_H

#include <cstdint>

namespace ucxm {

struct Onboarding
{
    uint16_t vendorId;
    uint16_t productId;
    uint16_t discriminator; // 12 bits
    uint32_t passcode;
    char qr[136];    // "MT:..." for the QR code
    char manual[24]; // digits of the manual pairing code
};

// Fills qr and manual from the other fields; false when the SDK refuses them (e.g. a passcode it does not allow).
bool OnboardingCodes(Onboarding & onboarding);

} // namespace ucxm

#endif // UCXM_ONBOARDING_H
