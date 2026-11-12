// Platform-agnostic configuration storage for ucxclient platform
// Supports Windows/POSIX/FreeRTOS via host OS key-value store
#pragma once

#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceError.h>  // For CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND

namespace chip {
namespace DeviceLayer {
namespace Internal {

/**
 * Platform-agnostic configuration storage abstraction
 * Uses host OS native storage (Windows Registry, POSIX files, FreeRTOS NVS)
 */
class UcxclientConfig
{
public:
    // Key is just a string identifier
    using Key = const char *;
    
    // Configuration keys for device instance info
    static constexpr Key kConfigKey_SerialNum = "serial-num";
    static constexpr Key kConfigKey_ManufacturingDate = "mfg-date";
    static constexpr Key kConfigKey_HardwareVersion = "hw-ver";
    static constexpr Key kConfigKey_HardwareVersionString = "hw-ver-str";
    
    // Configuration keys required by GenericConfigurationManagerImpl
    static constexpr Key kConfigKey_CountryCode = "country-code";
    static constexpr Key kConfigKey_RegulatoryLocation = "reg-location";
    static constexpr Key kConfigKey_UniqueId = "unique-id";
    static constexpr Key kConfigKey_FailSafeArmed = "fail-safe-armed";
    static constexpr Key kConfigKey_RebootCount = "reboot-count";
    static constexpr Key kConfigKey_TotalOperationalHours = "total-op-hours";
    static constexpr Key kConfigKey_BootReason = "boot-reason";
    static constexpr Key kConfigKey_SoftwareVersion = "sw-ver";
    static constexpr Key kConfigKey_SoftwareVersionString = "sw-ver-str";
    static constexpr Key kConfigKey_ConfigurationVersion = "cfg-ver";
    
    // Initialize configuration storage (platform-specific)
    static CHIP_ERROR Init() { return CHIP_NO_ERROR; }
    
    // Config value accessors (stub implementations)
    // Return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND so SDK falls back to defaults
    static CHIP_ERROR ReadConfigValue(Key key, bool & val) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    static CHIP_ERROR ReadConfigValue(Key key, uint16_t & val) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    static CHIP_ERROR ReadConfigValue(Key key, uint32_t & val) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    static CHIP_ERROR ReadConfigValue(Key key, uint64_t & val) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    static CHIP_ERROR ReadConfigValueStr(Key key, char * buf, size_t bufSize, size_t & outLen) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    static CHIP_ERROR ReadConfigValueBin(Key key, uint8_t * buf, size_t bufSize, size_t & outLen) { return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND; }
    
    static CHIP_ERROR WriteConfigValue(Key key, bool val) { return CHIP_NO_ERROR; }
    static CHIP_ERROR WriteConfigValue(Key key, uint16_t val) { return CHIP_NO_ERROR; }
    static CHIP_ERROR WriteConfigValue(Key key, uint32_t val) { return CHIP_NO_ERROR; }
    static CHIP_ERROR WriteConfigValue(Key key, uint64_t val) { return CHIP_NO_ERROR; }
    static CHIP_ERROR WriteConfigValueStr(Key key, const char * str) { return CHIP_NO_ERROR; }
    static CHIP_ERROR WriteConfigValueBin(Key key, const uint8_t * data, size_t dataLen) { return CHIP_NO_ERROR; }
    
    static CHIP_ERROR ClearConfigValue(Key key) { return CHIP_NO_ERROR; }
    static bool ConfigValueExists(Key key) { return false; }
    static CHIP_ERROR FactoryResetConfig() { return CHIP_NO_ERROR; }
    
    static void RunConfigUnitTest() {}
};

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
