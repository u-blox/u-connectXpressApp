/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <lib/support/CHIPMemString.h>
#include <lib/support/CodeUtils.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>
#include <platform/ucxclient/ConfigurationManagerImpl.h>
#include "support/DeviceConfig.h"

namespace chip {
namespace DeviceLayer {

class DeviceInstanceInfoProviderImpl : public Internal::GenericDeviceInstanceInfoProvider<Internal::UcxclientConfig>
{
public:
    DeviceInstanceInfoProviderImpl(ConfigurationManagerImpl & configManager) :
        Internal::GenericDeviceInstanceInfoProvider<Internal::UcxclientConfig>(configManager)
    {}

    // Without these overrides the SDK defaults ("TEST_VENDOR"/"TEST_PRODUCT")
    // are reported in Basic Information, and controllers fall back to a
    // generic "Matter Accessory" display name.
    CHIP_ERROR GetVendorName(char * buf, size_t bufSize) override
    {
        VerifyOrReturnError(bufSize > strlen("u-blox"), CHIP_ERROR_BUFFER_TOO_SMALL);
        Platform::CopyString(buf, bufSize, "u-blox");
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductName(char * buf, size_t bufSize) override
    {
        const std::string & name = ucxmatter::GetDeviceConfig().deviceName;
        VerifyOrReturnError(bufSize > name.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
        Platform::CopyString(buf, bufSize, name.c_str());
        return CHIP_NO_ERROR;
    }
};

inline DeviceInstanceInfoProviderImpl & DeviceInstanceInfoProviderMgrImpl()
{
    static DeviceInstanceInfoProviderImpl sInstance(ConfigurationManagerImpl::GetDefaultInstance());
    return sInstance;
}

} // namespace DeviceLayer
} // namespace chip
