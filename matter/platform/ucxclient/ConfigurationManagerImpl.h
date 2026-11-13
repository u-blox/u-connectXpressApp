// Platform-agnostic ConfigurationManagerImpl for ucxclient platform
#pragma once

#include <platform/internal/GenericConfigurationManagerImpl.h>
#include "UcxclientConfig.h"

namespace chip {
namespace DeviceLayer {

/**
 * ucxclient platform implementation of ConfigurationManager
 * Uses host OS-agnostic configuration storage (Windows/POSIX/FreeRTOS)
 */
class ConfigurationManagerImpl final : public Internal::GenericConfigurationManagerImpl<Internal::UcxclientConfig>
{
public:
    static ConfigurationManagerImpl & GetDefaultInstance();
    // u-connectXpressApp: forget the in-RAM copy (written at every change), the next start reads the store again
    static void Unload();

    CHIP_ERROR GetRebootCount(uint32_t & rebootCount) override;
    CHIP_ERROR StoreRebootCount(uint32_t rebootCount) override;
    CHIP_ERROR GetTotalOperationalHours(uint32_t & totalOperationalHours) override;
    CHIP_ERROR StoreTotalOperationalHours(uint32_t totalOperationalHours) override;
    CHIP_ERROR GetBootReason(uint32_t & bootReason) override;
    CHIP_ERROR StoreBootReason(uint32_t bootReason) override;
    CHIP_ERROR GetRegulatoryLocation(uint8_t & location) override;
    CHIP_ERROR GetLocationCapability(uint8_t & location) override;
    CHIP_ERROR GetConfigurationVersion(uint32_t & configurationVersion) override;
    CHIP_ERROR StoreConfigurationVersion(uint32_t configurationVersion) override;

private:
    // Implementation of ConfigurationManager public interface
    CHIP_ERROR Init() override;
    CHIP_ERROR GetPrimaryWiFiMACAddress(uint8_t * buf) override;
    bool CanFactoryReset() override;
    void InitiateFactoryReset() override;
    CHIP_ERROR ReadPersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t & value) override;
    CHIP_ERROR WritePersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t value) override;

    // Implementation of GenericConfigurationManagerImpl protected interface
    CHIP_ERROR ReadConfigValue(Key key, bool & val) override;
    CHIP_ERROR ReadConfigValue(Key key, uint32_t & val) override;
    CHIP_ERROR ReadConfigValue(Key key, uint64_t & val) override;
    CHIP_ERROR ReadConfigValueStr(Key key, char * buf, size_t bufSize, size_t & outLen) override;
    CHIP_ERROR ReadConfigValueBin(Key key, uint8_t * buf, size_t bufSize, size_t & outLen) override;
    CHIP_ERROR WriteConfigValue(Key key, bool val) override;
    CHIP_ERROR WriteConfigValue(Key key, uint32_t val) override;
    CHIP_ERROR WriteConfigValue(Key key, uint64_t val) override;
    CHIP_ERROR WriteConfigValueStr(Key key, const char * str) override;
    CHIP_ERROR WriteConfigValueStr(Key key, const char * str, size_t strLen) override;
    CHIP_ERROR WriteConfigValueBin(Key key, const uint8_t * data, size_t dataLen) override;
    void RunConfigUnitTest(void) override;
};

} // namespace DeviceLayer
} // namespace chip
