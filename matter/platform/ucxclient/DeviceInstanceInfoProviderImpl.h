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
    CHIP_ERROR GetVendorName(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().vendorName, buf, bufSize); }
    CHIP_ERROR GetProductName(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().deviceName, buf, bufSize); }

    // u-connectXpressApp: from the module the node runs on (DeviceConfig.h), not from stored configuration keys,
    // which this platform never writes: a read of HardwareVersion failed with "value not found"
    CHIP_ERROR GetPartNumber(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().partNumber, buf, bufSize); }
    CHIP_ERROR GetProductURL(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().productUrl, buf, bufSize); }
    CHIP_ERROR GetProductLabel(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().productLabel, buf, bufSize); }
    CHIP_ERROR GetSerialNumber(char * buf, size_t bufSize) override { return Text(ucxmatter::GetDeviceConfig().serialNumber, buf, bufSize); }
    CHIP_ERROR GetHardwareVersionString(char * buf, size_t bufSize) override
    {
        return Text(ucxmatter::GetDeviceConfig().hardwareVersionString, buf, bufSize);
    }
    CHIP_ERROR GetHardwareVersion(uint16_t & hardwareVersion) override
    {
        hardwareVersion = ucxmatter::GetDeviceConfig().hardwareVersion;
        return CHIP_NO_ERROR;
    }

private:
    // An empty value is one the node does not have: the cluster then answers with an empty string
    static CHIP_ERROR Text(const std::string & value, char * buf, size_t bufSize)
    {
        VerifyOrReturnError(!value.empty(), CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE);
        VerifyOrReturnError(bufSize > value.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
        Platform::CopyString(buf, bufSize, value.c_str());
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
