/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

// C++ STL includes FIRST - before any platform headers
#include <atomic>
#include <fstream>
#include <sstream>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

// Now platform includes
#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/ConfigurationManager.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>  // For GetDeviceInstanceInfoProvider()
#include <ble/CHIPBleServiceData.h>  // For ChipBLEDeviceIdentificationInfo
#include "ConfigurationManagerImpl.h"
#include "KeyValueStoreManagerImpl.h"  // For EraseAll()
#include "UcxClientManager.h"
#include "UcxStore.h"
#include "support/DeviceConfig.h"

// ucxclient includes
extern "C" {
#include "u_cx_system.h"
#include "u_cx_at_params.h"
}

namespace chip {
namespace DeviceLayer {

// File-based storage implementation
// u-connectXpressApp: one blob in the app's secure store instead of matter_config.bin
static const char * kConfigFileName = "config";
static std::map<std::string, std::vector<uint8_t>> sConfigCache;
static bool sConfigLoaded = false;

// Helper: Convert Key enum to string (Note: Key is actually const char* for ucxclient)
static std::string KeyToString(Internal::UcxclientConfig::Key key)
{
    // Key is const char*, so just wrap it in a string with a prefix
    return std::string("key_") + std::string(key);
}

// Load configuration from file
static void LoadConfigFromFile()
{
    if (sConfigLoaded) return;
    
    if (!ucxmatter::StoreLoad(kConfigFileName, sConfigCache))
    {
        ChipLogProgress(DeviceLayer, "[Config] No existing config file - starting fresh");
        sConfigLoaded = true;
        return;
    }
    
    sConfigLoaded = true;
    ChipLogProgress(DeviceLayer, "[Config] Loaded %u config entries from file", (unsigned)sConfigCache.size());
}

// Save configuration to file
static CHIP_ERROR SaveConfigToFile()
{
    if (!ucxmatter::StoreSave(kConfigFileName, sConfigCache))
    {
        ChipLogError(DeviceLayer, "[Config] Failed to open config file for writing");
        return CHIP_ERROR_WRITE_FAILED;
    }
    
    ChipLogProgress(DeviceLayer, "[Config] Saved %u config entries to file", (unsigned)sConfigCache.size());
    return CHIP_NO_ERROR;
}

// ===== ConfigurationManagerImpl implementation =====

ConfigurationManagerImpl & ConfigurationManagerImpl::GetDefaultInstance()
{
    static ConfigurationManagerImpl sInstance;
    return sInstance;
}

// Public interface stubs
CHIP_ERROR ConfigurationManagerImpl::GetRebootCount(uint32_t & rebootCount) { rebootCount = 0; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::StoreRebootCount(uint32_t) { return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::GetTotalOperationalHours(uint32_t & hours) { hours = 0; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::StoreTotalOperationalHours(uint32_t) { return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::GetBootReason(uint32_t & reason) { reason = 0; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::StoreBootReason(uint32_t) { return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::GetRegulatoryLocation(uint8_t & location) { location = 0; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::GetLocationCapability(uint8_t & location) { location = 0; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::GetConfigurationVersion(uint32_t & version) { version = 1; return CHIP_NO_ERROR; }
CHIP_ERROR ConfigurationManagerImpl::StoreConfigurationVersion(uint32_t) { return CHIP_NO_ERROR; }

// Private interface stubs
CHIP_ERROR ConfigurationManagerImpl::Init() 
{ 
    LoadConfigFromFile();
    
    // Seed manufacturing date from DeviceConfig if not already in persistent store
    const auto & mfgDate = ucxmatter::GetDeviceConfig().manufacturingDate;
    if (!mfgDate.empty())
    {
        size_t outLen = 0;
        char buf[11];
        CHIP_ERROR err = ReadConfigValueStr(Internal::UcxclientConfig::kConfigKey_ManufacturingDate, buf, sizeof(buf), outLen);
        if (err == CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND)
        {
            (void) WriteConfigValueStr(Internal::UcxclientConfig::kConfigKey_ManufacturingDate, mfgDate.c_str(), mfgDate.size());
            ChipLogProgress(DeviceLayer, "[Config] Seeded ManufacturingDate: %s", mfgDate.c_str());
        }
    }
    
    return CHIP_NO_ERROR; 
}

CHIP_ERROR ConfigurationManagerImpl::GetPrimaryWiFiMACAddress(uint8_t * buf)
{
    ChipLogProgress(DeviceLayer, "[MAC] GetPrimaryWiFiMACAddress called");
    uCxHandle_t * ucxHandle = UcxClientManager::GetInstance().GetUcxHandle();
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[MAC] ucxHandle is null - ucxclient not initialized");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    uMacAddress_t macAddr;
    int32_t result = uCxSystemGetLocalAddress(ucxHandle, U_INTERFACE_ID_WIFI_STATION, &macAddr);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "uCxSystemGetLocalAddress failed: %d", (int)result);
        return CHIP_ERROR_INTERNAL;
    }

    // Copy MAC address to output buffer (6 bytes)
    memcpy(buf, macAddr.address, U_MAC_ADDR_LEN);
    
    ChipLogProgress(DeviceLayer, "Got WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                   buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    
    return CHIP_NO_ERROR;
}

bool ConfigurationManagerImpl::CanFactoryReset() { return true; }

void ConfigurationManagerImpl::InitiateFactoryReset() 
{
    ChipLogProgress(DeviceLayer, "[Config] ========================================");
    ChipLogProgress(DeviceLayer, "[Config] FACTORY RESET - clearing all stored data");
    ChipLogProgress(DeviceLayer, "[Config] ========================================");
    
    // Clear config cache and delete config file
    sConfigCache.clear();
    ucxmatter::StoreRemove(kConfigFileName);
    
    // CRITICAL: Also erase KeyValueStore (matter_kvs.bin) which contains:
    // - WiFi credentials (saved by NetworkCommissioning cluster)
    // - Fabric data, ACLs, session resumption, etc.
    CHIP_ERROR kvsErr = PersistedStorage::KeyValueStoreManagerImpl::GetDefaultInstance().EraseAll();
    if (kvsErr != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[Config] Failed to erase KVS: %s", ErrorStr(kvsErr));
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[Config] KeyValueStore (matter_kvs.bin) erased");
    }
    
    // Reset NORA-W36 if not already done (handles remote factory reset from controller)
    // FactoryResetNoraW36() checks mInitialized internally — safe to call multiple times.
    auto & ucxMgr = UcxClientManager::GetInstance();
    if (ucxMgr.IsInitialized())
    {
        ChipLogProgress(DeviceLayer, "[Config] Resetting NORA-W36 to factory defaults...");
        (void)ucxMgr.FactoryResetNoraW36();
    }
    
    // u-connectXpressApp: the app stays up; the node is commissionable again after matter.stop and matter.start
    ChipLogProgress(DeviceLayer, "[Config] ========================================");
    ChipLogProgress(DeviceLayer, "[Config] FACTORY RESET COMPLETE");
    ChipLogProgress(DeviceLayer, "[Config] ========================================");
}

CHIP_ERROR ConfigurationManagerImpl::ReadPersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t & value) 
{ 
    LoadConfigFromFile();
    
    std::string keyStr = std::string("persist_") + std::string(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    
    if (it->second.size() != sizeof(uint32_t))
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    memcpy(&value, it->second.data(), sizeof(uint32_t));
    ChipLogDetail(DeviceLayer, "[Config] Read persisted storage key=%s, value=%u", keyStr.c_str(), value);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::WritePersistedStorageValue(::chip::Platform::PersistedStorage::Key key, uint32_t value) 
{ 
    LoadConfigFromFile();
    
    std::string keyStr = std::string("persist_") + std::string(key);
    
    std::vector<uint8_t> data(sizeof(uint32_t));
    memcpy(data.data(), &value, sizeof(uint32_t));
    
    sConfigCache[keyStr] = data;
    ChipLogDetail(DeviceLayer, "[Config] Write persisted storage key=%s, value=%u", keyStr.c_str(), value);
    
    return SaveConfigToFile();
}

// Config value read/write implementations
CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, bool & val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        ChipLogDetail(DeviceLayer, "[Config] Key %s not found (bool)", keyStr.c_str());
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    
    if (it->second.size() != sizeof(uint8_t))
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    val = (it->second[0] != 0);
    ChipLogDetail(DeviceLayer, "[Config] Read %s = %d (bool)", keyStr.c_str(), val);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint32_t & val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        ChipLogDetail(DeviceLayer, "[Config] Key %s not found (uint32)", keyStr.c_str());
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    
    if (it->second.size() != sizeof(uint32_t))
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    memcpy(&val, it->second.data(), sizeof(uint32_t));
    ChipLogDetail(DeviceLayer, "[Config] Read %s = %u (uint32)", keyStr.c_str(), val);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint64_t & val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        ChipLogDetail(DeviceLayer, "[Config] Key %s not found (uint64)", keyStr.c_str());
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    
    if (it->second.size() != sizeof(uint64_t))
    {
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    memcpy(&val, it->second.data(), sizeof(uint64_t));
    ChipLogDetail(DeviceLayer, "[Config] Read %s = %llu (uint64)", keyStr.c_str(), (unsigned long long)val);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueStr(Key key, char * buf, size_t bufSize, size_t & outLen) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        outLen = 0;
        ChipLogDetail(DeviceLayer, "[Config] Key %s not found (string)", keyStr.c_str());
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    
    outLen = it->second.size();
    if (outLen > bufSize)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    
    memcpy(buf, it->second.data(), outLen);
    ChipLogDetail(DeviceLayer, "[Config] Read %s = \"%.*s\" (string)", keyStr.c_str(), (int)outLen, buf);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueBin(Key key, uint8_t * buf, size_t bufSize, size_t & outLen) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    auto it = sConfigCache.find(keyStr);
    if (it == sConfigCache.end())
    {
        outLen = 0;
        ChipLogDetail(DeviceLayer, "[Config] Key %s not found (binary)", keyStr.c_str());
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    
    outLen = it->second.size();
    if (outLen > bufSize)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    
    memcpy(buf, it->second.data(), outLen);
    ChipLogDetail(DeviceLayer, "[Config] Read %s (%u bytes binary)", keyStr.c_str(), (unsigned)outLen);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, bool val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    std::vector<uint8_t> data(1);
    data[0] = val ? 1 : 0;
    
    sConfigCache[keyStr] = data;
    ChipLogDetail(DeviceLayer, "[Config] Write %s = %d (bool)", keyStr.c_str(), val);
    
    return SaveConfigToFile();
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint32_t val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    std::vector<uint8_t> data(sizeof(uint32_t));
    memcpy(data.data(), &val, sizeof(uint32_t));
    
    sConfigCache[keyStr] = data;
    ChipLogDetail(DeviceLayer, "[Config] Write %s = %u (uint32)", keyStr.c_str(), val);
    
    return SaveConfigToFile();
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint64_t val) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    std::vector<uint8_t> data(sizeof(uint64_t));
    memcpy(data.data(), &val, sizeof(uint64_t));
    
    sConfigCache[keyStr] = data;
    ChipLogDetail(DeviceLayer, "[Config] Write %s = %llu (uint64)", keyStr.c_str(), (unsigned long long)val);
    
    return SaveConfigToFile();
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * str) 
{ 
    return WriteConfigValueStr(key, str, str ? strlen(str) : 0);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * str, size_t strLen) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    std::vector<uint8_t> data(strLen);
    if (strLen > 0 && str != nullptr)
    {
        memcpy(data.data(), str, strLen);
    }
    
    sConfigCache[keyStr] = data;
    ChipLogDetail(DeviceLayer, "[Config] Write %s = \"%.*s\" (string)", keyStr.c_str(), (int)strLen, str);
    
    return SaveConfigToFile();
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueBin(Key key, const uint8_t * data, size_t dataLen) 
{ 
    LoadConfigFromFile();
    std::string keyStr = KeyToString(key);
    
    std::vector<uint8_t> vec(dataLen);
    if (dataLen > 0 && data != nullptr)
    {
        memcpy(vec.data(), data, dataLen);
    }
    
    sConfigCache[keyStr] = vec;
    ChipLogDetail(DeviceLayer, "[Config] Write %s (%u bytes binary)", keyStr.c_str(), (unsigned)dataLen);
    
    return SaveConfigToFile();
}
void ConfigurationManagerImpl::RunConfigUnitTest(void) {}

} // namespace DeviceLayer
} // namespace chip

// Implement all required GenericConfigurationManagerImpl virtual methods
namespace chip {
namespace DeviceLayer {
namespace Internal {

// Cache for MAC address to avoid repeated AT commands
static uint8_t sCachedMAC[6] = {0};
static bool sMACCached = false;

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetPrimaryMACAddress(chip::Span<uint8_t> & buf) {
    if (buf.size() < 6) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    
    // Return cached MAC if available
    if (sMACCached) {
        memcpy(buf.data(), sCachedMAC, 6);
        buf = buf.SubSpan(0, 6);
        ChipLogProgress(DeviceLayer, "[MAC-SPAN] Returning cached MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                       buf.data()[0], buf.data()[1], buf.data()[2], buf.data()[3], buf.data()[4], buf.data()[5]);
        return CHIP_NO_ERROR;
    }
    
    ChipLogProgress(DeviceLayer, "[MAC-SPAN] GetPrimaryMACAddress called (querying NORA-W36)");
    
    uCxHandle_t * ucxHandle = UcxClientManager::GetInstance().GetUcxHandle();
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[MAC-SPAN] ucxHandle is null - ucxclient not initialized");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    uMacAddress_t macAddr;
    int32_t result = uCxSystemGetLocalAddress(ucxHandle, U_INTERFACE_ID_WIFI_STATION, &macAddr);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[MAC-SPAN] uCxSystemGetLocalAddress failed: %d", (int)result);
        return CHIP_ERROR_INTERNAL;
    }

    // Copy MAC address to output buffer (6 bytes)
    memcpy(buf.data(), macAddr.address, U_MAC_ADDR_LEN);
    buf = buf.SubSpan(0, U_MAC_ADDR_LEN);
    
    // Cache for future calls
    memcpy(sCachedMAC, buf.data(), 6);
    sMACCached = true;
    
    ChipLogProgress(DeviceLayer, "[MAC-SPAN] Got WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X (cached)",
                   buf.data()[0], buf.data()[1], buf.data()[2], buf.data()[3], buf.data()[4], buf.data()[5]);
    
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetPrimaryWiFiMACAddress(uint8_t * buf) {
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(buf, mac, 6);
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetPrimary802154MACAddress(uint8_t * buf) {
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetSoftwareVersionString(char * buf, size_t bufSize) {
    const auto & str = ucxmatter::GetDeviceConfig().softwareVersionString;
    size_t len = str.size();
    if (bufSize > len) {
        memcpy(buf, str.c_str(), len + 1);
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_BUFFER_TOO_SMALL;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetSoftwareVersion(uint32_t & version) {
    version = ucxmatter::GetDeviceConfig().softwareVersion;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetFirmwareBuildChipEpochTime(System::Clock::Seconds32 & buildTime) {
    buildTime = System::Clock::Seconds32(0);
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::SetFirmwareBuildChipEpochTime(System::Clock::Seconds32) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetRegulatoryLocation(uint8_t & location) {
    location = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetCountryCode(char * buf, size_t bufSize, size_t & outLen) {
    const char * code = "SE";  // Sweden
    outLen = 2;
    if (bufSize >= 3) {
        memcpy(buf, code, 3);
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_BUFFER_TOO_SMALL;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreSerialNumber(const char *, size_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreManufacturingDate(const char *, size_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreSoftwareVersion(uint32_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreConfigurationVersion(uint32_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreHardwareVersion(uint16_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreRegulatoryLocation(uint8_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreCountryCode(const char *, size_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetUniqueId(char * buf, size_t bufSize) {
    // u-connectXpressApp: random, drawn with the setup codes; it must not give away the address or serial number
    const std::string & unique = ucxmatter::GetDeviceConfig().uniqueId;
    char id[40];
    if (!unique.empty()) {
        snprintf(id, sizeof(id), "%.32s", unique.c_str());
    } else {
        snprintf(id, sizeof(id), "UCX-MATTER-%s", UcxClientManager::GetInstance().GetMacSuffix());
    }
    size_t len = strlen(id);
    if (bufSize > len) {
        memcpy(buf, id, len + 1);
        return CHIP_NO_ERROR;
    }
    return CHIP_ERROR_BUFFER_TOO_SMALL;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreUniqueId(const char *, size_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GenerateUniqueId(char * buf, size_t bufSize) {
    return GetUniqueId(buf, bufSize);
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetFailSafeArmed(bool & armed) {
    armed = false;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::SetFailSafeArmed(bool) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetBLEDeviceIdentificationInfo(Ble::ChipBLEDeviceIdentificationInfo & info) {
    // Initialize the structure properly with Matter commissioning data
    info.Init();
    
    // Get discriminator from commissionable data provider
    uint16_t discriminator = 0;
    CHIP_ERROR err = GetCommissionableDataProvider()->GetSetupDiscriminator(discriminator);
    if (err == CHIP_NO_ERROR)
    {
        info.SetDeviceDiscriminator(discriminator);
        ChipLogProgress(DeviceLayer, "[Config] BLE discriminator: %u", discriminator);
    }
    else
    {
        // Use discriminator from device config (or default)
        const auto& config = ucxmatter::GetDeviceConfig();
        info.SetDeviceDiscriminator(config.discriminator);
        ChipLogProgress(DeviceLayer, "[Config] BLE discriminator: %u (from config)", config.discriminator);
    }
    
    // Get vendor/product IDs from device config (loaded from matter.config)
    const auto& deviceConfig = ucxmatter::GetDeviceConfig();
    
    // Vendor ID from config (or DeviceInstanceInfoProvider if available)
    uint16_t vendorId = 0;
    err = chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetVendorId(vendorId);
    if (err == CHIP_NO_ERROR && vendorId != 0)
    {
        info.SetVendorId(vendorId);
        ChipLogProgress(DeviceLayer, "[Config] BLE vendorId: 0x%04X (from provider)", vendorId);
    }
    else
    {
        info.SetVendorId(deviceConfig.vendorId);
        ChipLogProgress(DeviceLayer, "[Config] BLE vendorId: 0x%04X (from config)", deviceConfig.vendorId);
    }
    
    // Product ID from config (or DeviceInstanceInfoProvider if available)
    uint16_t productId = 0;
    err = chip::DeviceLayer::GetDeviceInstanceInfoProvider()->GetProductId(productId);
    if (err == CHIP_NO_ERROR && productId != 0)
    {
        info.SetProductId(productId);
        ChipLogProgress(DeviceLayer, "[Config] BLE productId: 0x%04X (from provider)", productId);
    }
    else
    {
        info.SetProductId(deviceConfig.productId);
        ChipLogProgress(DeviceLayer, "[Config] BLE productId: 0x%04X (from config)", deviceConfig.productId);
    }
    
    // Set advertisement version (0 = initial version)
    info.SetAdvertisementVersion(0);
    
    ChipLogProgress(DeviceLayer, "[Config] GetBLEDeviceIdentificationInfo complete");
    return CHIP_NO_ERROR;
}

template<>
bool GenericConfigurationManagerImpl<UcxclientConfig>::IsFullyProvisioned() {
    return true;
}

template<>
void GenericConfigurationManagerImpl<UcxclientConfig>::LogDeviceConfig() {}

template<>
bool GenericConfigurationManagerImpl<UcxclientConfig>::IsCommissionableDeviceTypeEnabled() {
    return true;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetDeviceTypeId(uint32_t & deviceType) {
    deviceType = 0x0016; // Root Node
    return CHIP_NO_ERROR;
}

template<>
bool GenericConfigurationManagerImpl<UcxclientConfig>::IsCommissionableDeviceNameEnabled() {
    return true;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetCommissionableDeviceName(char * buf, size_t bufSize) {
    // Per-unit DN (mDNS TXT record): "<config name>-<MAC suffix>"
    // Makes each NORA-W36 booth unit distinguishable in commissioner UIs.
    const auto& config = ucxmatter::GetDeviceConfig();
    const char * base  = config.deviceName.c_str();
    const char * mac   = UcxClientManager::GetInstance().GetMacSuffix();

    int written;
    if (mac != nullptr && mac[0] != '\0' && strcmp(mac, "000000") != 0)
    {
        written = snprintf(buf, bufSize, "%s-%s", base, mac);
    }
    else
    {
        written = snprintf(buf, bufSize, "%s", base);
    }
    if (written < 0 || static_cast<size_t>(written) >= bufSize)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetInitialPairingHint(uint16_t & hint) {
    hint = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetInitialPairingInstruction(char * buf, size_t bufSize) {
    buf[0] = '\0';
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetSecondaryPairingHint(uint16_t & hint) {
    hint = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetSecondaryPairingInstruction(char * buf, size_t bufSize) {
    buf[0] = '\0';
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetRebootCount(uint32_t & rebootCount) {
    rebootCount = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreRebootCount(uint32_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetTotalOperationalHours(uint32_t & hours) {
    hours = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreTotalOperationalHours(uint32_t) {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetBootReason(uint32_t & reason) {
    reason = 0;
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::StoreBootReason(uint32_t) {
    return CHIP_NO_ERROR;
}

template<>
void GenericConfigurationManagerImpl<UcxclientConfig>::InitiateFactoryReset() {
    // Factory reset stub - would erase all persistent data
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::Init() {
    return CHIP_NO_ERROR;
}

template<>
CHIP_ERROR GenericConfigurationManagerImpl<UcxclientConfig>::GetConfigurationVersion(uint32_t & version) {
    version = 1;
    return CHIP_NO_ERROR;
}

} // namespace Internal

// Provide the singleton accessor required by the SDK
ConfigurationManager & ConfigurationMgrImpl()
{
    return ConfigurationManagerImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip
