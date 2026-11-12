#include "onboarding.h"

#include <cstring>

#include <lib/support/Span.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

namespace ucxm {

bool OnboardingCodes(Onboarding & onboarding)
{
    chip::PayloadContents payload;

    payload.vendorID          = onboarding.vendorId;
    payload.productID         = onboarding.productId;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.rendezvousInformation.SetValue(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    payload.discriminator.SetLongValue(onboarding.discriminator);
    payload.setUpPINCode = onboarding.passcode;
    if (!payload.isValidQRCodePayload())
    {
        return false;
    }

    // The generators give a length, not a terminator
    memset(onboarding.qr, 0, sizeof(onboarding.qr));
    memset(onboarding.manual, 0, sizeof(onboarding.manual));
    chip::MutableCharSpan qr(onboarding.qr, sizeof(onboarding.qr) - 1);
    chip::MutableCharSpan manual(onboarding.manual, sizeof(onboarding.manual) - 1);
    if (chip::QRCodeBasicSetupPayloadGenerator(payload).payloadBase38Representation(qr) != CHIP_NO_ERROR ||
        chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(manual) != CHIP_NO_ERROR)
    {
        return false;
    }
    onboarding.qr[qr.size()]         = '\0';
    onboarding.manual[manual.size()] = '\0';
    return true;
}

} // namespace ucxm
