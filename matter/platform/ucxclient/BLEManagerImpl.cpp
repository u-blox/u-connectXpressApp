/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/ConnectivityManager.h>
#include <platform/PlatformManager.h>  // For ScheduleWork() - DriveBLEState pattern
#include <platform/ConfigurationManager.h>  // For GetBLEDeviceIdentificationInfo()
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <lib/support/CHIPMemString.h>
#include <ble/BleError.h>  // For BLE_ERROR_REMOTE_DEVICE_DISCONNECTED
#include <ble/BleUUID.h>
#include <ble/CHIPBleServiceData.h>  // For ChipBLEDeviceIdentificationInfo struct

#include "BLEManagerImpl.h"
#include "UcxClientManager.h"
#include "VerboseLogging.h"
#include "support/DeviceConfig.h"

extern "C" {
#include "u_cx.h"
#include "u_cx_bluetooth.h"
#include "u_cx_gatt_server.h"
#include "u_cx_system.h"
#include "u_cx_error_codes.h"  // For uCxGetErrorName() - decode GATT/AT error codes
}

#include "PlatformAbstraction.h"

// BLE commissioning flow counters (used for logging on all platforms).
// uint32_t (was uint16_t): a busy 1-hour soak with frequent Apple Home commands
// can easily exceed 65535 GATT operations on a single connection. uint32_t gives
// us 4.3B headroom, which is "effectively never" for a peripheral.
static uint32_t sBleWriteCount = 0;      // C1 writes received from commissioner
static uint32_t sBleIndicationCount = 0;  // C2 indications sent to commissioner

using namespace ::chip;
using namespace ::chip::Ble;

namespace chip {
namespace DeviceLayer {
namespace Internal {

// Static instance
BLEManagerImpl BLEManagerImpl::sInstance;

// Note: CHIPoBLE UUIDs (CHIP_BLE_SVC_ID, CHIP_BLE_CHAR_1_UUID, etc.) are defined in <ble/BleUUID.h>

// Static callback for DriveBLEState - used with PlatformMgr().ScheduleWork()
void BLEManagerImpl::DriveBLEState(intptr_t arg)
{
    sInstance.DriveBLEStateImpl();
}

// Actual implementation of DriveBLEState (like ESP32 pattern)
void BLEManagerImpl::DriveBLEStateImpl()
{
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] DriveBLEState: serviceMode=%d, advEnabled=%d, advertising=%d",
                   (int)mServiceMode, mAdvertisingEnabled, mAdvertising);

    // If service mode is enabled and advertising is requested, start advertising
    if (mServiceMode == ConnectivityManager::kCHIPoBLEServiceMode_Enabled && mAdvertisingEnabled)
    {
        // Start advertising if not already advertising
        if (!mAdvertising)
        {
            UCX_LOG_VERBOSE(DeviceLayer, "[BLE] DriveBLEState: Starting advertising...");
            CHIP_ERROR err = StartAdvertising();
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(DeviceLayer, "[BLE] DriveBLEState: StartAdvertising failed: 0x%08X", err.AsInteger());
            }
        }
    }
    // Otherwise stop advertising if running
    else if (mAdvertising)
    {
        UCX_LOG_VERBOSE(DeviceLayer, "[BLE] DriveBLEState: Stopping advertising...");
        CHIP_ERROR err = StopAdvertising();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "[BLE] DriveBLEState: StopAdvertising failed: 0x%08X", err.AsInteger());
        }
    }
}

CHIP_ERROR BLEManagerImpl::_Init()
{
    ChipLogProgress(DeviceLayer, "[BLE] Initializing BLE Manager");

    // Get ucxclient handle
    mUcxHandle = UcxClientManager::GetInstance().GetUcxHandle();
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] mUcxHandle = %p", mUcxHandle);
    
    if (mUcxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[BLE] ERROR: ucxclient not initialized");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    // Initialize BleLayer
    CHIP_ERROR err = BleLayer::Init(this, this, &DeviceLayer::SystemLayer());
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] BleLayer init failed: %s", ErrorStr(err));
        return err;
    }

    // Initialize GATT server with CHIPoBLE service
    err = InitGattServer();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] GATT server init failed: %s", ErrorStr(err));
        return err;
    }

    // Set BLE device name from config with MAC suffix for uniqueness
    // (e.g., u-connectMatter-A1B2C3) — same base name as the mDNS DN record.
    char bleName[32];
    snprintf(bleName, sizeof(bleName), "%s-%s",
             ucxmatter::GetDeviceConfig().deviceName.c_str(),
             UcxClientManager::GetInstance().GetMacSuffix());
    Platform::CopyString(mDeviceName, sizeof(mDeviceName), bleName);
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Device name: %s", mDeviceName);

    ChipLogProgress(DeviceLayer, "[BLE] BLE Manager initialized");
    return CHIP_NO_ERROR;
}

void BLEManagerImpl::_Shutdown()
{
    ChipLogProgress(DeviceLayer, "[BLE] Shutting down BLE Manager");
    
    (void)StopAdvertising();  // Explicitly ignore return value
    
    // Close all connections
    for (auto & conn : mConnections)
    {
        if (conn.connectionHandle != 0xFF)
        {
            ReleaseConnection(conn.connectionHandle);
        }
    }

    BleLayer::Shutdown();
}

CHIP_ERROR BLEManagerImpl::InitGattServer()
{
    ChipLogProgress(DeviceLayer, "[BLE] ==== InitGattServer ENTRY ====");

    // u-connectXpressApp: the module is the user's, so its bonds and its Bluetooth mode stay as they are
    // (matter.start checks that the mode has the peripheral role)
    int32_t result;

    // Configure BLE security for Matter commissioning
    // Matter uses PASE/CASE at application layer - NO BLE encryption needed!
    // Set security mode to NONE so characteristics can be accessed without bonding
    
    // AT+UBTPM=1 - Accept pairing if initiated by remote device (for Service Changed CCCD)
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Enabling pairing mode...");
    result = uCxBluetoothSetPairingMode(mUcxHandle, U_BT_PAIRING_MODE_ENABLE);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to enable pairing mode: %d", result);
    }
    
    // AT+UBTBSM=0 - Security mode NONE - no encryption required
    // Matter handles security at application layer via PASE/CASE
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Setting security mode to NONE...");
    result = uCxBluetoothSetSecurityMode(mUcxHandle, U_BT_SECURITY_MODE_NONE);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to set security mode: %d", result);
    }
    
    // AT+UBTIOC=0 - I/O capabilities = NoInputNoOutput (enables Just Works pairing)
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Setting I/O capabilities to NoInputNoOutput...");
    result = uCxBluetoothSetIoCapabilities(mUcxHandle, U_BT_IO_CAP_NO_INPUT_NO_OUTPUT);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to set I/O capabilities: %d", result);
    }

    // Define CHIPoBLE service (0xFFF6)
    // NORA-W36 expects 16-bit UUID in big-endian (network byte order)
    uint8_t serviceUuid[] = {0xFF, 0xF6}; // 16-bit UUID 0xFFF6 in big-endian
    mGattFull = false;
    result = uCxGattServerServiceDefine(mUcxHandle, serviceUuid, 2, &mCHIPoBLEServiceHandle);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to define CHIPoBLE service: %d", result);
        mGattFull = true;
        return CHIP_ERROR_INTERNAL;
    }
    ChipLogProgress(DeviceLayer, "[BLE] CHIPoBLE service defined (handle: %d)", mCHIPoBLEServiceHandle);

    // Add C1 characteristic (client writes - device RX)
    // Matter UUID: 18EE2EF5-263D-4559-959F-4F9C429F9D11
    // NORA-W36 expects 128-bit UUID in big-endian (UUID string order, not reversed)
    // Properties: Write (0x08) - CSA compliant (same as ESP32, Linux, TI implementations)
    // Using HOST CONTROLLED characteristic so we get +UEBTGCW URCs for writes
    uint8_t c1Uuid[] = {0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59,
                        0x95, 0x9F, 0x4F, 0x9C, 0x42, 0x9F, 0x9D, 0x11}; // CHIPoBLE RX (big-endian)
    uint8_t propsWrite[] = {0x08}; // Write (0x08) - CSA standard
    
    uCxGattServerHostCharDefine_t c1Response;
    // Use HostCharDefine so host handles all read/write operations via URCs
    // CRITICAL: Use SECURITY_NONE (value=1) so no BLE encryption is required!
    // Matter uses PASE/CASE for security at application layer, not BLE-level encryption
    result = uCxGattServerHostCharDefine(mUcxHandle, c1Uuid, 16,
                                         propsWrite, 1,
                                         U_GATT_SERVER_READ_SECURITY_NONE,
                                         U_GATT_SERVER_WRITE_SECURITY_NONE,
                                         &c1Response);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to add C1 characteristic: %d", result);
        return CHIP_ERROR_INTERNAL;
    }
    mC1CharHandle = c1Response.value_handle;
    mDefinedC1Handle = mC1CharHandle;
    ChipLogProgress(DeviceLayer, "[BLE] C1 (RX/Write): value_handle=%d, cccd_handle=%d", 
                   mC1CharHandle, c1Response.cccd_handle);

    // Add C2 characteristic (server indicates - device TX)
    // Matter UUID: 18EE2EF5-263D-4559-959F-4F9C429F9D12
    // NORA-W36 expects 128-bit UUID in big-endian (UUID string order, not reversed)
    // Properties: Read (0x02) + Indicate (0x20) = 0x22
    // Note: Matter spec requires BOTH Read and Indicate on C2!
    // Using HOST CONTROLLED characteristic so we get +UEBTGRR URCs for reads
    uint8_t c2Uuid[] = {0x18, 0xEE, 0x2E, 0xF5, 0x26, 0x3D, 0x45, 0x59,
                        0x95, 0x9F, 0x4F, 0x9C, 0x42, 0x9F, 0x9D, 0x12}; // CHIPoBLE TX (big-endian)
    uint8_t propsReadIndicate[] = {0x22}; // Read (0x02) | Indicate (0x20)
    
    uCxGattServerHostCharDefine_t c2Response;
    // Use HostCharDefine so host handles all read/write operations via URCs
    // CRITICAL: Use SECURITY_NONE (value=1) so no BLE encryption is required!
    // Matter uses PASE/CASE for security at application layer, not BLE-level encryption
    result = uCxGattServerHostCharDefine(mUcxHandle, c2Uuid, 16,
                                         propsReadIndicate, 1,
                                         U_GATT_SERVER_READ_SECURITY_NONE,
                                         U_GATT_SERVER_WRITE_SECURITY_NONE,
                                         &c2Response);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to add C2 characteristic: %d", result);
        return CHIP_ERROR_INTERNAL;
    }
    mC2CharHandle = c2Response.value_handle;
    mC2CccdHandle = c2Response.cccd_handle;
    ChipLogProgress(DeviceLayer, "[BLE] C2 (TX/Indicate): value_handle=%d, cccd_handle=%d", 
                   mC2CharHandle, mC2CccdHandle);

    // u-connectXpressApp: URCs about these handles are the node's alone, the app's GATT commands do not see them
    UcxClientManager::GetInstance().OwnGattHandles(mC1CharHandle, mC2CharHandle, mC2CccdHandle);

    // Activate the service
    result = uCxGattServerServiceActivate(mUcxHandle);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to activate service: %d", result);
        return CHIP_ERROR_INTERNAL;
    }
    ChipLogProgress(DeviceLayer, "[BLE] CHIPoBLE service activated");

    // Register BLE connection/disconnection/pairing URC handlers
    ChipLogProgress(DeviceLayer, "[BLE] Registering URC callbacks...");
    uCxGattServerRegisterNotification(mUcxHandle, GattServerWriteCallback);
    uCxGattServerRegisterReadAttribute(mUcxHandle, GattServerReadCallback);
    uCxGattServerRegisterIndicationAck(mUcxHandle, GattServerIndicationAckCallback);
    uCxBluetoothRegisterConnect(mUcxHandle, BleConnectCallback);
    uCxBluetoothRegisterDisconnect(mUcxHandle, BleDisconnectCallback);
    uCxBluetoothRegisterBondStatus(mUcxHandle, BleBondStatusCallback);
    uCxBluetoothRegisterPhyUpdate(mUcxHandle, BlePhyUpdateCallback);

    return CHIP_NO_ERROR;
}

BLEManager::CHIPoBLEServiceMode BLEManagerImpl::_GetCHIPoBLEServiceMode()
{
    return mServiceMode;
}

CHIP_ERROR BLEManagerImpl::_SetCHIPoBLEServiceMode(BLEManager::CHIPoBLEServiceMode mode)
{
    ChipLogProgress(DeviceLayer, "[BLE] SetCHIPoBLEServiceMode: %d -> %d", (int)mServiceMode, (int)mode);
    mServiceMode = mode;
    // Schedule DriveBLEState to handle mode change
    CHIP_ERROR err = PlatformMgr().ScheduleWork(DriveBLEState, 0);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to schedule DriveBLEState: %" CHIP_ERROR_FORMAT, err.Format());
    }
    return CHIP_NO_ERROR;
}

bool BLEManagerImpl::_IsAdvertisingEnabled()
{
    return mAdvertisingEnabled;
}

CHIP_ERROR BLEManagerImpl::_SetAdvertisingEnabled(bool enabled)
{
    ChipLogProgress(DeviceLayer, "[BLE] ==== _SetAdvertisingEnabled(%s) ====", enabled ? "true" : "false");
    ChipLogProgress(DeviceLayer, "[BLE] Current state: mAdvertisingEnabled=%d, mAdvertising=%d, serviceMode=%d",
                   mAdvertisingEnabled, mAdvertising, (int)mServiceMode);

    // Always update the flag
    mAdvertisingEnabled = enabled;
    
    // Schedule DriveBLEState to handle the change (like ESP32 pattern)
    // This ensures proper thread synchronization and deferred execution
    ChipLogProgress(DeviceLayer, "[BLE] Scheduling DriveBLEState to handle advertising change...");
    CHIP_ERROR err = PlatformMgr().ScheduleWork(DriveBLEState, 0);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to schedule DriveBLEState: %" CHIP_ERROR_FORMAT, err.Format());
    }
    
    ChipLogProgress(DeviceLayer, "[BLE] ==== _SetAdvertisingEnabled EXIT (scheduled) ====");
    return CHIP_NO_ERROR;
}

bool BLEManagerImpl::_IsAdvertising()
{
    return mAdvertising;
}

CHIP_ERROR BLEManagerImpl::_SetAdvertisingMode(BLEAdvertisingMode mode)
{
    if (mAdvertisingMode != mode)
    {
        ChipLogProgress(DeviceLayer, "[BLE] Advertising mode changed: %s -> %s",
                       (mAdvertisingMode == BLEAdvertisingMode::kFastAdvertising) ? "Fast" : "Slow",
                       (mode == BLEAdvertisingMode::kFastAdvertising) ? "Fast" : "Slow");
        mAdvertisingMode = mode;

        // If currently advertising, restart with new intervals
        if (mAdvertising)
        {
            mRestarting = true;   // the app sees only the new start, not a stop in between
            (void)StopAdvertising();
            mRestarting = false;
            return StartAdvertising();
        }
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::StartAdvertising()
{
    ChipLogProgress(DeviceLayer, "[BLE] ==== StartAdvertising ENTRY ====");
    ChipLogProgress(DeviceLayer, "[BLE] mUcxHandle = %p", mUcxHandle);
    ChipLogProgress(DeviceLayer, "[BLE] mDeviceName = %s", mDeviceName);
    ChipLogProgress(DeviceLayer, "[BLE] mAdvertising = %d", mAdvertising);
    
    if (mUcxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[BLE] ERROR: mUcxHandle is NULL in StartAdvertising!");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    // u-connectXpressApp: the module keeps the local name the user gave it
    int32_t result;

    ChipLogProgress(DeviceLayer, "[BLE] Step 2: Configuring advertising data...");
    
    // Get the BLE Device Identification Info from ConfigurationManager
    // This is the STANDARD Matter BLE advertising format used by all platforms!
    chip::Ble::ChipBLEDeviceIdentificationInfo deviceIdInfo;
    CHIP_ERROR err = ConfigurationMgr().GetBLEDeviceIdentificationInfo(deviceIdInfo);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] GetBLEDeviceIdentificationInfo failed: %" CHIP_ERROR_FORMAT, err.Format());
        return err;
    }
    
    ChipLogProgress(DeviceLayer, "[BLE] DeviceIdInfo: opcode=%d, discriminator=%d, vendorId=%d, productId=%d",
                   deviceIdInfo.OpCode,
                   deviceIdInfo.GetDeviceDiscriminator(),
                   deviceIdInfo.GetVendorId(),
                   deviceIdInfo.GetProductId());

    // Build advertising data in standard Matter BLE format
    // Format: Complete 16-bit UUID + Service Data (UUID + ChipBLEDeviceIdentificationInfo)
    // NOTE: NORA-W36 automatically adds AD Flags (0x02 0x01 0x06), so we don't include them
    uint8_t advData[31];
    size_t advLen = 0;

    // Flags (0x06 = General Discoverable + BR/EDR Not Supported)
    // Note: NORA-W36 sets these automatically, so we don't need to include them
    // advData[advLen++] = 0x02; // Length
    // advData[advLen++] = 0x01; // Type: Flags  
    // advData[advLen++] = 0x06; // Value

    // AD Element 1: Complete List of 16-bit Service UUIDs (CHIPoBLE 0xFFF6)
    advData[advLen++] = 0x03;  // Length
    advData[advLen++] = 0x03;  // AD type: Complete List of 16-bit Service UUIDs
    advData[advLen++] = 0xF6;  // CHIPoBLE UUID LSB (0xFFF6)
    advData[advLen++] = 0xFF;  // CHIPoBLE UUID MSB

    // AD Element 2: Service Data (16-bit UUID) with ChipBLEDeviceIdentificationInfo
    // This is what iPhone Home app and other commissioners scan for!
    static_assert(sizeof(deviceIdInfo) == 8, "ChipBLEDeviceIdentificationInfo size changed!");
    advData[advLen++] = static_cast<uint8_t>(sizeof(deviceIdInfo) + 2 + 1);  // Length: data + UUID + type
    advData[advLen++] = 0x16;  // AD type: Service Data - 16-bit UUID
    advData[advLen++] = 0xF6;  // Service UUID LSB (0xFFF6)
    advData[advLen++] = 0xFF;  // Service UUID MSB
    
    // Copy the ChipBLEDeviceIdentificationInfo structure (8 bytes)
    memcpy(&advData[advLen], &deviceIdInfo, sizeof(deviceIdInfo));
    advLen += sizeof(deviceIdInfo);

    ChipLogProgress(DeviceLayer, "[BLE] Advertising data prepared (%u bytes)", (unsigned int)advLen);
    // ucxclient 3.4.0 renamed this API; the 1-arg variant has the same signature.
    result = uCxBluetoothSetLegacyAdvertiseDataAndType1(mUcxHandle, advData, (int32_t)advLen);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to set advertising data: %d", result);
        return CHIP_ERROR_INTERNAL;
    }

    // Configure BLE connection intervals for Matter commissioning.
    // Must be compatible with all phones (iOS requires min >= 15ms, Android varies).
    // Values in 1.25ms units. BLE spec allows 7.5ms-4000ms.
    // CRITICAL: Set max BEFORE min (constraint: min <= max, setting min > current max errors).
    {
        // Matter spec recommends 15-30ms for active commissioning (faster GATT throughput).
        // Apple Accessory Design Guidelines: min ≥ 15ms, max ≥ min + 15ms. iPad/iPhone
        // will renegotiate via L2CAP CPU Request if it doesn't like our params, so this
        // is safe across vendors. Matches ESP32/nRF52 reference Matter implementations.
        // Tighter intervals reduce CHIPoBLE indication round-trip during PASE/CASE handshake.
        const int32_t connIntervalMin = 12;  // 15ms (Matter/Apple optimal floor)
        const int32_t connIntervalMax = 24;  // 30ms (good throughput, low power impact)
        // Set max first to avoid constraint violation (min must be <= max)
        result = uCxBluetoothSetConnectionIntervalMax(mUcxHandle, connIntervalMax);
        if (result < 0) {
            ChipLogError(DeviceLayer, "[BLE] Failed to set connection interval max: %d", result);
        }
        result = uCxBluetoothSetConnectionIntervalMin(mUcxHandle, connIntervalMin);
        if (result < 0) {
            ChipLogError(DeviceLayer, "[BLE] Failed to set connection interval min: %d", result);
        }
        ChipLogProgress(DeviceLayer, "[BLE] Connection intervals: min=%ldms max=%ldms",
                        static_cast<long>(connIntervalMin * 125 / 100),
                        static_cast<long>(connIntervalMax * 125 / 100));
    }

    // Matter spec: Fast advertising = 20-60ms (32-96 units), Slow = 1285ms (2056 units)
    // Units are 0.625ms each
    int32_t minInterval, maxInterval;
    if (mAdvertisingMode == BLEAdvertisingMode::kFastAdvertising)
    {
        minInterval = 48;   // 30ms (Matter spec recommends 20-60ms fast window)
        maxInterval = 48;
    }
    else
    {
        minInterval = 2056; // 1285ms (Matter spec slow advertising)
        maxInterval = 2056;
    }
    result = uCxBluetoothSetLegacyAdvertisementConfig(mUcxHandle, minInterval, maxInterval);
    if (result < 0) {
        ChipLogError(DeviceLayer, "[BLE] Failed to set advertising intervals: %d", result);
    }
    ChipLogProgress(DeviceLayer, "[BLE] Advertising interval: %s (%ldms)",
                    mAdvertisingMode == BLEAdvertisingMode::kFastAdvertising ? "FAST" : "SLOW",
                    static_cast<long>(minInterval * 625 / 1000));

    ChipLogProgress(DeviceLayer, "[BLE] Step 4: Starting legacy advertising...");
    result = uCxBluetoothLegacyAdvertisementStart(mUcxHandle);
    ChipLogProgress(DeviceLayer, "[BLE] uCxBluetoothLegacyAdvertisementStart returned: %d", result);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to start advertising: %d", result);
        return CHIP_ERROR_INTERNAL;
    }

    mAdvertising = true;
    ChipLogProgress(DeviceLayer, "[BLE] BLE advertising started successfully!");

    ChipLogProgress(DeviceLayer, "[BLE-DBG] GATT: svc=%d, C1=%d, C2=%d, C2_CCCD=%d",
                   mCHIPoBLEServiceHandle, mC1CharHandle, mC2CharHandle, mC2CccdHandle);

    ChipLogProgress(DeviceLayer, "[BLE] ==== StartAdvertising EXIT (success) ====");

    // Notify the application that BLE advertising has started
    {
        ChipDeviceEvent advEvent;
        advEvent.Type = DeviceEventType::kCHIPoBLEAdvertisingChange;
        advEvent.CHIPoBLEAdvertisingChange.Result = kActivity_Started;
        (void) PlatformMgr().PostEvent(&advEvent);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::StopAdvertising()
{
    ChipLogProgress(DeviceLayer, "[BLE] ==== StopAdvertising ENTRY ====");
    ChipLogProgress(DeviceLayer, "[BLE] mAdvertising=%d", mAdvertising);
    
    if (!mAdvertising)
    {
        ChipLogProgress(DeviceLayer, "[BLE] Already not advertising - nothing to do");
        ChipLogProgress(DeviceLayer, "[BLE] ==== StopAdvertising EXIT (already stopped) ====");
        return CHIP_NO_ERROR;
    }

    ChipLogProgress(DeviceLayer, "[BLE] Stopping BLE advertising");
    ChipLogProgress(DeviceLayer, "[BLE] mUcxHandle=%p", mUcxHandle);
    
    if (mUcxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[BLE] ERROR: mUcxHandle is NULL!");
        return CHIP_ERROR_INCORRECT_STATE;
    }

    ChipLogProgress(DeviceLayer, "[BLE] Calling uCxBluetoothLegacyAdvertisementStop()...");
    int32_t result = uCxBluetoothLegacyAdvertisementStop(mUcxHandle);
    ChipLogProgress(DeviceLayer, "[BLE] uCxBluetoothLegacyAdvertisementStop() returned: %d", result);
    
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to stop advertising: %d", result);
        return CHIP_ERROR_INTERNAL;
    }

    mAdvertising = false;
    ChipLogProgress(DeviceLayer, "[BLE] Advertising stopped successfully");
    if (!mRestarting)
    {
        ChipDeviceEvent advEvent;
        advEvent.Type = DeviceEventType::kCHIPoBLEAdvertisingChange;
        advEvent.CHIPoBLEAdvertisingChange.Result = kActivity_Stopped;
        (void) PlatformMgr().PostEvent(&advEvent);
    }
    ChipLogProgress(DeviceLayer, "[BLE] ==== StopAdvertising EXIT (success) ====");
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::_GetDeviceName(char * buf, size_t bufSize)
{
    Platform::CopyString(buf, bufSize, mDeviceName);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::_SetDeviceName(const char * name)
{
    Platform::CopyString(mDeviceName, sizeof(mDeviceName), name);
    return CHIP_NO_ERROR;
}

uint16_t BLEManagerImpl::_NumConnections()
{
    uint16_t count = 0;
    for (const auto & conn : mConnections)
    {
        if (conn.connectionHandle != 0xFF)
        {
            count++;
        }
    }
    return count;
}

void BLEManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
    switch (event->Type)
    {
    case DeviceEventType::kCHIPoBLESubscribe:
        ChipLogProgress(DeviceLayer, "[BLE] Processing kCHIPoBLESubscribe for conn %u", event->CHIPoBLESubscribe.ConId);
        {
            bool subOk = HandleSubscribeReceived(event->CHIPoBLESubscribe.ConId, 
                                   &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
            if (subOk)
            {
                // Post connection established event (like ESP32)
                ChipDeviceEvent connectionEvent;
                connectionEvent.Type = DeviceEventType::kCHIPoBLEConnectionEstablished;
                PlatformMgr().PostEventOrDie(&connectionEvent);
            }
            else
            {
                ChipLogError(DeviceLayer, "[BLE] HandleSubscribeReceived FAILED - no endpoint!");
            }
        }
        break;

    case DeviceEventType::kCHIPoBLEUnsubscribe:
        ChipLogProgress(DeviceLayer, "[BLE] Processing kCHIPoBLEUnsubscribe for conn %u", event->CHIPoBLEUnsubscribe.ConId);
        HandleUnsubscribeReceived(event->CHIPoBLEUnsubscribe.ConId,
                                 &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        break;

    case DeviceEventType::kCHIPoBLEWriteReceived:
        ChipLogProgress(DeviceLayer, "[BLE] Processing kCHIPoBLEWriteReceived for conn %u", event->CHIPoBLEWriteReceived.ConId);
        if (!HandleWriteReceived(event->CHIPoBLEWriteReceived.ConId,
                                 &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_1_UUID,
                                 PacketBufferHandle::Adopt(event->CHIPoBLEWriteReceived.Data)))
        {
            ChipLogError(DeviceLayer, "[BLE] HandleWriteReceived FAILED!");
        }
        break;

    case DeviceEventType::kCHIPoBLEIndicateConfirm:
        ChipLogProgress(DeviceLayer, "[BLE] Processing kCHIPoBLEIndicateConfirm for conn %u", event->CHIPoBLEIndicateConfirm.ConId);
        HandleIndicationConfirmation(event->CHIPoBLEIndicateConfirm.ConId,
                                    &CHIP_BLE_SVC_ID, &Ble::CHIP_BLE_CHAR_2_UUID);
        break;

    case DeviceEventType::kCHIPoBLEConnectionError:
        ChipLogProgress(DeviceLayer, "[BLE] Processing kCHIPoBLEConnectionError for conn %u err=0x%08X",
                       event->CHIPoBLEConnectionError.ConId,
                       static_cast<unsigned>(event->CHIPoBLEConnectionError.Reason.AsInteger()));
        HandleConnectionError(event->CHIPoBLEConnectionError.ConId,
                             event->CHIPoBLEConnectionError.Reason);
        break;

    case DeviceEventType::kCommissioningComplete:
        ChipLogProgress(DeviceLayer, "[BLE] Commissioning complete - ensuring BLE advertising is stopped");
        mAdvertisingEnabled = false;
        (void)StopAdvertising();
        break;

    default:
        break;
    }
}

Ble::BleLayer * BLEManagerImpl::_GetBleLayer()
{
    return this;
}

// BlePlatformDelegate implementation

// Helper to convert BLE_CONNECTION_OBJECT to uint8_t handle
// On Windows: BLE_CONNECTION_OBJECT is uint16_t, use static_cast
// On FreeRTOS: BLE_CONNECTION_OBJECT is void*, use reinterpret_cast
// CRITICAL: On embedded (FreeRTOS/Zephyr), BLE_CONNECTION_OBJECT is void*
//   BLE_CONNECTION_UNINITIALIZED == nullptr == (void*)0
//   NORA-W36 connection handle 0 would map to nullptr and be rejected by BleLayer!
//   Solution: offset by +1 so handle 0 → (void*)1, handle 1 → (void*)2, etc.
static inline uint8_t ConIdToHandle(BLE_CONNECTION_OBJECT conId) {
#if defined(U_PORT_FREERTOS) || defined(__ZEPHYR__)
    return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(conId) - 1);
#else
    return static_cast<uint8_t>(conId);
#endif
}

static inline uintptr_t ConIdToUInt(BLE_CONNECTION_OBJECT conId) {
#if defined(U_PORT_FREERTOS) || defined(__ZEPHYR__)
    return reinterpret_cast<uintptr_t>(conId) - 1;
#else
    return static_cast<uintptr_t>(conId);
#endif
}

// Helper to convert int32_t (from AT callbacks) to BLE_CONNECTION_OBJECT
// On embedded (FreeRTOS/Zephyr): adds +1 offset to avoid nullptr (BLE_CONNECTION_UNINITIALIZED)
static inline BLE_CONNECTION_OBJECT HandleToConId(int32_t conn_handle) {
#if defined(U_PORT_FREERTOS) || defined(__ZEPHYR__)
    return reinterpret_cast<BLE_CONNECTION_OBJECT>(static_cast<uintptr_t>(conn_handle) + 1);
#else
    return static_cast<BLE_CONNECTION_OBJECT>(conn_handle);
#endif
}

CHIP_ERROR BLEManagerImpl::SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId)
{
    ChipLogProgress(DeviceLayer, "[BLE] Subscribe requested (conn: %u)", static_cast<unsigned>(ConIdToUInt(conId)));
    
    BLEConnection * conn = FindConnection(ConIdToHandle(conId));
    if (conn != nullptr)
    {
        conn->subscribed = true;
        return CHIP_NO_ERROR;
    }
    
    return CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR BLEManagerImpl::UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId)
{
    ChipLogProgress(DeviceLayer, "[BLE] Unsubscribe requested (conn: %u)", static_cast<unsigned>(ConIdToUInt(conId)));
    
    BLEConnection * conn = FindConnection(ConIdToHandle(conId));
    if (conn != nullptr)
    {
        conn->subscribed = false;
        return CHIP_NO_ERROR;
    }
    
    return CHIP_ERROR_INVALID_ARGUMENT;
}

CHIP_ERROR BLEManagerImpl::CloseConnection(BLE_CONNECTION_OBJECT conId)
{
    ChipLogProgress(DeviceLayer, "[BLE] Closing connection (conn: %u)", static_cast<unsigned>(ConIdToUInt(conId)));
    
    uint8_t connHandle = ConIdToHandle(conId);
    int32_t result = uCxBluetoothDisconnect(mUcxHandle, connHandle);
    if (result < 0)
    {
        // -16 = U_ERROR_COMMON_NOT_CONNECTED: peer already disconnected (normal during commissioning teardown)
        if (result == -16)
        {
            ChipLogProgress(DeviceLayer, "[BLE] Connection already closed by peer (expected)");
        }
        else
        {
            ChipLogError(DeviceLayer, "[BLE] Disconnect failed: %d", result);
        }
        // Always clean up local state regardless of disconnect result
    }
    
    ReleaseConnection(connHandle);
    return CHIP_NO_ERROR;
}

uint16_t BLEManagerImpl::GetMTU(BLE_CONNECTION_OBJECT conId) const
{
    const BLEConnection * conn = const_cast<BLEManagerImpl*>(this)->FindConnection(ConIdToHandle(conId));
    return conn ? conn->mtu : 23;
}

CHIP_ERROR BLEManagerImpl::SendIndication(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, 
                                    const Ble::ChipBleUUID * charId, System::PacketBufferHandle pBuf)
{
    uint8_t connHandle = ConIdToHandle(conId);
    BLEConnection * conn = FindConnection(connHandle);
    uint16_t currentMtu = (conn != nullptr) ? conn->mtu : 0;
    size_t dataLen = pBuf->DataLength();
    
    sBleIndicationCount++;

    ChipLogProgress(DeviceLayer, "[BLE] SendInd #%u: conn=%u len=%llu MTU=%u sub=%d",
                   static_cast<unsigned>(sBleIndicationCount), static_cast<unsigned>(connHandle),
                   (unsigned long long)dataLen, currentMtu,
                   (conn != nullptr) ? conn->subscribed : -1);
    
    if (conn == nullptr)
    {
        ChipLogError(DeviceLayer, "[BLE] SendInd ABORT: connection %u not found!", static_cast<unsigned>(connHandle));
        return CHIP_ERROR_INCORRECT_STATE;
    }
    
    // Log BTP flags compactly
    if (dataLen >= 1)
    {
        uint8_t flags = pBuf->Start()[0];
        ChipLogProgress(DeviceLayer, "[BLE] TX BTP: flags=0x%02X [%s%s%s%s]",
                       flags,
                       (flags & 0x01) ? "S" : "",
                       (flags & 0x02) ? "C" : "",
                       (flags & 0x04) ? "E" : "",
                       (flags & 0x08) ? "A" : "");
    }
    
    // === RETRY LOOP ===
    // NORA-W36 firmware returns ERROR:1 if the phone hasn't subscribed to indications yet
    // (CCCD write to handle 26). The phone may subscribe AFTER writing the BTP handshake
    // to C1. We retry with short gaps to wait for the subscription.
    //
    // CRITICAL: Stop immediately if the connection is closed by remote (+UEBTDC). Retrying
    // on a dead connection returns ERROR:16 and wastes hundreds of ms.
    //
    // Retry timing: 10 attempts × 50ms = 500ms max wait (fast enough for BTP timing,
    // long enough for slow phones). Previous 8 × 100ms = 800ms was too slow —
    // could cause BTP fragment delivery timeouts on aggressive commissioners.
    static constexpr int kMaxIndicationAttempts = 10;  // 500ms total max wait
    static constexpr int kRetryDelayMs = 50;           // 50ms between retries (was 100ms)
    int32_t result = -1;
    
    for (int attempt = 1; attempt <= kMaxIndicationAttempts; attempt++)
    {
        // Check if connection was closed by remote before attempting
        if (conn->closedByRemote)
        {
            ChipLogError(DeviceLayer, "[BLE] SendInd ABORT: connection %u closed by remote (+UEBTDC)",
                        static_cast<unsigned>(connHandle));
            return CHIP_ERROR_CONNECTION_CLOSED_UNEXPECTEDLY;
        }
        
        result = uCxGattServerSendIndication(mUcxHandle, connHandle, mC2CharHandle,
                                              pBuf->Start(), static_cast<int32_t>(dataLen));
        
        if (result >= 0)
        {
            if (attempt > 1)
            {
                ChipLogProgress(DeviceLayer, "[BLE] Indication succeeded on attempt %d/%d (waited %dms for CCCD subscribe)",
                               attempt, kMaxIndicationAttempts, (attempt - 1) * kRetryDelayMs);
            }
            break;  // Success!
        }
        
        const char * errName = uCxGetErrorName(result);
        
        // ERROR:16 = no connection — remote already disconnected, stop immediately
        if (result == -16)
        {
            ChipLogError(DeviceLayer, "[BLE] SendInd ABORT: ERROR:16 (no connection) - remote disconnected");
            conn->closedByRemote = true;  // Mark so subsequent calls also bail out
            return CHIP_ERROR_CONNECTION_CLOSED_UNEXPECTEDLY;
        }
        
        // u-connectXpressApp: expected until the commissioner subscribes to C2, so not an error (the last
        // attempt's failure below is)
        ChipLogProgress(DeviceLayer, "[BLE] SendInd attempt %d/%d not yet: err=%d (%s)",
                    attempt, kMaxIndicationAttempts, result, errName ? errName : "?");
        
        if (attempt < kMaxIndicationAttempts)
        {
            // Wait between retries - gives time for phone's CCCD subscription
            // to arrive at NORA-W36 over the BLE air interface
            chip::Platform::SleepMs(kRetryDelayMs);
        }
    }
    
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[BLE] === INDICATION FAILED after %d attempts (%dms) ===",
                    kMaxIndicationAttempts, kMaxIndicationAttempts * kRetryDelayMs);
        ChipLogError(DeviceLayer, "[BLE]   conn=%u char=%d cccd=%d len=%llu MTU=%u",
                    static_cast<unsigned>(connHandle), mC2CharHandle, mC2CccdHandle,
                    (unsigned long long)dataLen, currentMtu);
        return CHIP_ERROR_INTERNAL;
    }
    
    ChipLogProgress(DeviceLayer, "[BLE] Indication #%u OK (%llu bytes)",
                   static_cast<unsigned>(sBleIndicationCount), (unsigned long long)dataLen);
    return CHIP_NO_ERROR;
}

CHIP_ERROR BLEManagerImpl::SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId,
                                      const Ble::ChipBleUUID * charId, System::PacketBufferHandle pBuf)
{
    // Not used in peripheral role
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void BLEManagerImpl::NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId)
{
    ChipLogProgress(DeviceLayer, "[BLE] CHIP connection closed (conn: %u) writes=%u ind=%u",
                   static_cast<unsigned>(ConIdToUInt(conId)), sBleWriteCount, sBleIndicationCount);
    
    uint8_t connHandle = ConIdToHandle(conId);
    BLEConnection * conn = FindConnection(connHandle);
    
    // Only disconnect if connection exists AND wasn't already closed by remote
    if (conn != nullptr && !conn->closedByRemote)
    {
        // CRITICAL: Defer the disconnect to avoid race condition with indication ACK processing.
        // When NotifyChipConnectionClosed is called from URC callback (GattServerIndicationAckCallback),
        // we're still inside processUrcs() which runs after cmdEnd() releases the mutex.
        // If we send AT+UBTDC immediately, it can collide with pending AT response processing.
        // Use a small delay (50ms) to allow current AT command cycle to fully complete.
        ChipLogProgress(DeviceLayer, "[BLE] Deferring disconnect to avoid AT command race condition...");
        
        // Mark connection as pending disconnect and schedule deferred close
        conn->pendingDisconnect = true;
        
        // Use ScheduleWork to run disconnect on the main Matter thread after current event processing
        (void) PlatformMgr().ScheduleWork([](intptr_t arg) {
#if defined(U_PORT_FREERTOS) || defined(__ZEPHYR__)
            BLE_CONNECTION_OBJECT connId = reinterpret_cast<BLE_CONNECTION_OBJECT>(arg);
#else
            BLE_CONNECTION_OBJECT connId = static_cast<BLE_CONNECTION_OBJECT>(arg);
#endif
            ChipLogProgress(DeviceLayer, "[BLE] Executing deferred disconnect for conn %u", static_cast<unsigned>(ConIdToUInt(connId)));
            
            // Small delay to ensure AT command buffer is clear
            chip::Platform::SleepMs(50);
            
            (void)BLEMgrImpl().CloseConnection(connId);
#if defined(U_PORT_FREERTOS) || defined(__ZEPHYR__)
        }, reinterpret_cast<intptr_t>(conId));
#else
        }, static_cast<intptr_t>(conId));
#endif
    }
    else if (conn != nullptr && conn->closedByRemote)
    {
        ChipLogProgress(DeviceLayer, "[BLE] Connection already closed by remote (+UEBTDC URC) - skipping AT+UBTDC");
        ReleaseConnection(connHandle);  // Clean up since CloseConnection won't be called
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[BLE] Connection already released - skipping disconnect");
    }
}

// Connection management

BLEManagerImpl::BLEConnection * BLEManagerImpl::AllocateConnection(uint8_t connHandle)
{
    for (auto & conn : mConnections)
    {
        if (conn.connectionHandle == 0xFF)
        {
            conn.connectionHandle = connHandle;
            conn.mtu = 23;
            memset(&conn.bdAddress, 0, sizeof(conn.bdAddress));
            conn.bdAddressValid = false;
            conn.subscribed = false;
            conn.pendingSubscribe = false;
            conn.firstWriteReceived = false;
            conn.closedByRemote = false;
            conn.pendingDisconnect = false;
            conn.cccdExplicitlySet = false;
            ChipLogProgress(DeviceLayer, "[BLE] Allocated connection slot (handle: %d)", connHandle);
            return &conn;
        }
    }
    
    ChipLogError(DeviceLayer, "[BLE] No free connection slots!");
    return nullptr;
}

BLEManagerImpl::BLEConnection * BLEManagerImpl::FindConnection(uint8_t connHandle)
{
    for (auto & conn : mConnections)
    {
        if (conn.connectionHandle == connHandle)
        {
            return &conn;
        }
    }
    return nullptr;
}

void BLEManagerImpl::ReleaseConnection(uint8_t connHandle)
{
    BLEConnection * conn = FindConnection(connHandle);
    if (conn != nullptr)
    {
        ChipLogProgress(DeviceLayer, "[BLE] Released connection slot (handle: %d)", connHandle);
        conn->connectionHandle = 0xFF;
        memset(&conn->bdAddress, 0, sizeof(conn->bdAddress));
        conn->bdAddressValid = false;
        conn->subscribed = false;
        conn->pendingSubscribe = false;
        conn->firstWriteReceived = false;
        conn->closedByRemote = false;
        conn->pendingDisconnect = false;
        conn->cccdExplicitlySet = false;
    }
}

const char * BLEManagerImpl::FormatBdAddress(const uBtLeAddress_t & addr, char * buf, size_t bufSize)
{
    if (bufSize < 18) // "XX:XX:XX:XX:XX:XX" + null = 18
    {
        if (bufSize > 0) buf[0] = '\0';
        return buf;
    }
    snprintf(buf, bufSize, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr.address[5], addr.address[4], addr.address[3],
             addr.address[2], addr.address[1], addr.address[0]);
    return buf;
}

void BLEManagerImpl::HandleConnectionEvent(uint8_t connHandle, const uBtLeAddress_t * bdAddr)
{
    ChipLogProgress(DeviceLayer, "[BLE] Connection established (handle: %d)", connHandle);
    sBleWriteCount = 0;
    sBleIndicationCount = 0;
    
    BLEConnection * conn = AllocateConnection(connHandle);
    if (conn == nullptr)
    {
        // Reject connection if no slots available
        ChipLogError(DeviceLayer, "[BLE] No slots - disconnecting handle %d", connHandle);
        uCxBluetoothDisconnect(mUcxHandle, connHandle);
        return;
    }

    // Store peer BD address for diagnostics and connection tracking
    if (bdAddr != nullptr)
    {
        conn->bdAddress = *bdAddr;
        conn->bdAddressValid = true;
        char addrStr[24];
        ChipLogProgress(DeviceLayer, "[BLE] Peer address: %s (%s)", 
                       FormatBdAddress(*bdAddr, addrStr, sizeof(addrStr)),
                       bdAddr->type == U_BD_ADDRESS_TYPE_PUBLIC ? "public" : 
                       bdAddr->type == U_BD_ADDRESS_TYPE_RANDOM ? "random" : "unknown");
    }

    // Query actual negotiated MTU from NORA-W36
    // Default MTU is 23 (minimum BLE ATT MTU), but modern devices like Apple 
    // negotiate higher MTUs (185, 247, 512). Using the correct MTU is critical
    // for proper BTP fragmentation after PASE completes.
    ChipLogProgress(DeviceLayer, "[BLE] Querying actual MTU for connection %d...", connHandle);
    uCxBluetoothListConnectionStatusBegin(mUcxHandle, static_cast<int32_t>(connHandle));
    uCxBtListConnectionStatus_t status;
    while (uCxBluetoothListConnectionStatusGetNext(mUcxHandle, &status))
    {
        if (status.prop_id == U_BT_PROP_ID_MTU_SIZE)
        {
            // MTU value includes the 3-byte ATT header, so actual payload MTU is value
            uint16_t mtuValue = static_cast<uint16_t>(status.status_val);
            if (mtuValue >= 23 && mtuValue <= 517) // Valid BLE MTU range
            {
                conn->mtu = mtuValue;
                ChipLogProgress(DeviceLayer, "[BLE] Negotiated MTU: %u (was default 23)", mtuValue);
            }
            else
            {
                ChipLogProgress(DeviceLayer, "[BLE] Invalid MTU value %u, using default 23", mtuValue);
            }
        }
    }
    uCxEnd(mUcxHandle);
    ChipLogProgress(DeviceLayer, "[BLE] Using MTU: %u for connection %d", conn->mtu, connHandle);

    // Stop advertising after connection (single connection mode)
    (void)StopAdvertising();  // Explicitly ignore return value

    // Note: CHIPoBLE connection established later when client subscribes to C2 (TX characteristic)
}

void BLEManagerImpl::HandleDisconnectionEvent(uint8_t connHandle)
{
    // Look up connection first so we can log its BD address
    BLEConnection * conn = FindConnection(connHandle);
    
    if (conn != nullptr && conn->bdAddressValid)
    {
        char addrStr[24];
        ChipLogProgress(DeviceLayer, "[BLE] Connection closed (handle: %d, peer: %s) after %u writes, %u indications",
                       connHandle, FormatBdAddress(conn->bdAddress, addrStr, sizeof(addrStr)),
                       sBleWriteCount, sBleIndicationCount);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[BLE] Connection closed (handle: %d) after %u writes, %u indications",
                       connHandle, sBleWriteCount, sBleIndicationCount);
    }
    
    // Mark connection as closed by remote (prevents redundant AT+UBTDC in NotifyChipConnectionClosed)
    if (conn != nullptr)
    {
        conn->closedByRemote = true;
        
        // Decide whether to post a connection error event or release immediately:
        //
        // If a C1 write was ever received (firstWriteReceived), this was a real CHIPoBLE
        // session. The Matter BLE layer created an endpoint and needs to be notified of
        // the disconnection via kCHIPoBLEConnectionError. This triggers HandleConnectionError
        // which calls NotifyChipConnectionClosed, which sees closedByRemote=true and skips
        // the redundant AT+UBTDC, then releases the connection slot.
        //
        // NOTE: We cannot use conn->subscribed because the SDK calls 
        // UnsubscribeCharacteristic() during graceful teardown, clearing subscribed
        // BEFORE the disconnect URC arrives.
        //
        // If no write was ever received, this was a rogue/incomplete connection (bond
        // failure, scan, early disconnect). No BLE endpoint was created, so we must
        // release the slot immediately to avoid permanently leaking it.
        if (conn->firstWriteReceived)
        {
            ChipLogProgress(DeviceLayer, "[BLE] Posting kCHIPoBLEConnectionError for teardown");
            
            ChipDeviceEvent event;
            event.Type                          = DeviceEventType::kCHIPoBLEConnectionError;
            event.CHIPoBLEConnectionError.ConId = HandleToConId(connHandle);
            event.CHIPoBLEConnectionError.Reason = BLE_ERROR_REMOTE_DEVICE_DISCONNECTED;
            CHIP_ERROR err = PlatformMgr().PostEvent(&event);
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(DeviceLayer, "[BLE] Failed to post connection error event: %" CHIP_ERROR_FORMAT, err.Format());
                // Fallback: release slot directly to avoid permanent leak
                ReleaseConnection(connHandle);
            }
        }
        else
        {
            ChipLogProgress(DeviceLayer, "[BLE] Connection closed before any CHIPoBLE activity - releasing slot");
            ReleaseConnection(connHandle);
        }
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[BLE] Connection not found - already released");
    }

    // Always post kCHIPoBLEConnectionClosed (matches ESP32 pattern)
    // This notifies the application layer that a BLE connection ended,
    // regardless of whether it was a real CHIPoBLE session or a rogue connection.
    {
        ChipDeviceEvent disconnectEvent;
        disconnectEvent.Type = DeviceEventType::kCHIPoBLEConnectionClosed;
        (void) PlatformMgr().PostEvent(&disconnectEvent);
    }

    // Restart advertising
    if (mAdvertisingEnabled)
    {
        (void)StartAdvertising();  // Explicitly ignore return value
    }
}

void BLEManagerImpl::HandleWriteEvent(uint8_t connHandle, int32_t charHandle, const uint8_t * data, size_t length)
{
    ChipLogProgress(DeviceLayer, "[BLE] Write received (handle: %d, char: %d, len: %llu)",
                   connHandle, charHandle, (unsigned long long)length);
    
    if (charHandle == mC1CharHandle)
    {
        // Client wrote to C1 - pass to BleLayer
        System::PacketBufferHandle buf = System::PacketBufferHandle::NewWithData(data, length);
        if (!buf.IsNull())
        {
            HandleWriteReceived(HandleToConId(connHandle),
                               &CHIP_BLE_SVC_ID, &CHIP_BLE_CHAR_1_UUID, std::move(buf));
        }
    }
}

// Static URC callback - BLE connection event
void BLEManagerImpl::BleConnectCallback(uCxHandle_t * puCxHandle, int32_t conn_handle, uBtLeAddress_t * bd_addr)
{
    // DEBUG: Log which FreeRTOS task dispatched this URC callback
#if defined(U_PORT_FREERTOS)
    const char * taskName = pcTaskGetName(nullptr);
    UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    ChipLogProgress(DeviceLayer, "[BLE-DBG] +UEBTC callback on task '%s', stack free: %lu, handle=%ld",
                   taskName ? taskName : "?",
                   static_cast<unsigned long>(stackFree),
                   static_cast<long>(conn_handle));
#else
    ChipLogProgress(DeviceLayer, "[BLE-DBG] +UEBTC callback fired, handle=%ld", static_cast<long>(conn_handle));
#endif
    
    ChipLogProgress(DeviceLayer, "[BLE] Connected: handle=%d addr=%02X:%02X:%02X:%02X:%02X:%02X",
                   conn_handle, bd_addr->address[5], bd_addr->address[4], bd_addr->address[3],
                   bd_addr->address[2], bd_addr->address[1], bd_addr->address[0]);
    
    // NOTE: We do NOT initiate BLE bonding here!
    // Matter uses application-layer security (PASE) over BLE, not BLE pairing/bonding.
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Skipping BLE bonding - Matter uses app-layer security (PASE)");
    
    // Call directly on URC thread (similar to GattServerWriteCallback)
    BLEMgrImpl().HandleConnectionEvent(static_cast<uint8_t>(conn_handle), bd_addr);
}

// Static URC callback - BLE disconnection event
void BLEManagerImpl::BleDisconnectCallback(uCxHandle_t * puCxHandle, int32_t conn_handle)
{
    (void)puCxHandle;
    
    ChipLogProgress(DeviceLayer, "[BLE] Disconnected: handle=%d", conn_handle);
    
    // Call directly on URC thread
    BLEMgrImpl().HandleDisconnectionEvent(static_cast<uint8_t>(conn_handle));
}

// Static URC callback - BLE bond/pairing status
void BLEManagerImpl::BleBondStatusCallback(uCxHandle_t * puCxHandle, uBtLeAddress_t * bd_addr, uBtBondStatus_t bond_status)
{
    (void)puCxHandle;
    
    const char* statusStr = "UNKNOWN";
    switch (bond_status)
    {
        case U_BT_BOND_STATUS_BONDING_SUCCEEDED:
            statusStr = "SUCCESS";
            break;
        case U_BT_BOND_STATUS_BONDING_FAILED_TIMEOUT:
            statusStr = "FAILED (Timeout)";
            break;
        case U_BT_BOND_STATUS_BONDING_FAILED_AUTH:
            statusStr = "FAILED (Auth)";
            break;
        case U_BT_BOND_STATUS_BONDING_FAILED_MITM:
            statusStr = "FAILED (MITM)";
            break;
        case U_BT_BOND_STATUS_BONDING_FAILED_PEER_LOST_BOND:
            statusStr = "FAILED (Peer Lost Bond)";
            break;
    }
    
    ChipLogProgress(DeviceLayer, "[BLE] Bond status: %02X:%02X:%02X:%02X:%02X:%02X - %s",
                   bd_addr->address[5], bd_addr->address[4], bd_addr->address[3],
                   bd_addr->address[2], bd_addr->address[1], bd_addr->address[0], statusStr);
}

// Static URC callback - BLE PHY update event (+UEBTPHYU)
// Called when the BLE link PHY changes (e.g., 1M → 2M). After PHY update, the phone
// typically renegotiates MTU and connection parameters. We re-query MTU here because
// at initial connection time the MTU may still be the default 23.
void BLEManagerImpl::BlePhyUpdateCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                          int32_t phy_status, int32_t tx_phy, int32_t rx_phy)
{
    const char * phyName[] = { "?", "1M", "2M", "Coded" };
    const char * txName = (tx_phy >= 1 && tx_phy <= 3) ? phyName[tx_phy] : phyName[0];
    const char * rxName = (rx_phy >= 1 && rx_phy <= 3) ? phyName[rx_phy] : phyName[0];

    ChipLogProgress(DeviceLayer, "[BLE] PHY Update: conn=%d status=%d TX=%s RX=%s",
                   conn_handle, phy_status, txName, rxName);

    // Re-query MTU — the phone may have negotiated a higher MTU after PHY update
    BLEManagerImpl & manager = BLEMgrImpl();
    BLEConnection * conn = manager.FindConnection(static_cast<uint8_t>(conn_handle));
    if (conn == nullptr)
    {
        ChipLogProgress(DeviceLayer, "[BLE] PHY update for unknown connection %d", conn_handle);
        return;
    }

    uint16_t oldMtu = conn->mtu;
    uCxBluetoothListConnectionStatusBegin(puCxHandle, conn_handle);
    uCxBtListConnectionStatus_t status;
    while (uCxBluetoothListConnectionStatusGetNext(puCxHandle, &status))
    {
        if (status.prop_id == U_BT_PROP_ID_MTU_SIZE)
        {
            uint16_t mtuValue = static_cast<uint16_t>(status.status_val);
            if (mtuValue >= 23 && mtuValue <= 517)
            {
                conn->mtu = mtuValue;
            }
        }
    }
    uCxEnd(puCxHandle);

    if (conn->mtu != oldMtu)
    {
        ChipLogProgress(DeviceLayer, "[BLE] MTU updated after PHY change: %u → %u", oldMtu, conn->mtu);
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[BLE] MTU unchanged after PHY update: %u", conn->mtu);
    }
}

// Static URC callback - ucxclient requires C-style function pointer
// IMPORTANT: This is called from the ucxclient AT client thread (URC processing)
// We must post events to the Matter platform queue, NOT call handlers directly!
void BLEManagerImpl::GattServerWriteCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                             int32_t value_handle, uByteArray_t * value,
                                             uGattServerOptions_t options)
{
    BLEManagerImpl & manager = BLEMgrImpl();
    
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] GATT write: conn=%d, handle=%d, len=%llu, options=%d",
                   conn_handle, value_handle, (unsigned long long)value->length, (int)options);
    
    // Log our expected handles for debugging
    UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Our handles: C1=%d, C2=%d, C2_CCCD=%d",
                   manager.mC1CharHandle, manager.mC2CharHandle, manager.mC2CccdHandle);

    (void) manager.AdoptEarlierService(value_handle, value);
    
    // Check if this handle belongs to our CHIPoBLE service (excluding CCCD which is handled by BLE stack)
    bool isOurCharacteristic = (value_handle == manager.mC1CharHandle ||
                                value_handle == manager.mC2CharHandle);
    
    // Only send write response for OUR characteristics (not CCCD, not standard BLE services)
    // Standard BLE services (handle < 21) are handled by firmware - sending response causes ERROR
    // Our CCCD is also handled by firmware - sending response causes ERROR:11
    if (isOurCharacteristic && options == U_GATT_SERVER_OPTIONS_WRITE_WITH_RESPONSE)
    {
        UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Sending write response for conn=%d, handle=%d", conn_handle, value_handle);
        int32_t result = uCxGattServerWriteReqResponse(puCxHandle, conn_handle);
        if (result < 0)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to send write response: %d", result);
        }
    }
    
    // Check if this is CCCD write for C2 (indications enable/disable)
    if (value_handle == manager.mC2CccdHandle && value->length >= 2)
    {
        uint16_t cccdValue = value->pData[0] | (value->pData[1] << 8);
        bool indicationsEnabled = (cccdValue & 0x0002) != 0; // Bit 1 = Indications
        
        UCX_LOG_VERBOSE(DeviceLayer, "[BLE] CCCD write: value=0x%04X, indications=%s",
                       cccdValue, indicationsEnabled ? "ENABLED" : "DISABLED");
        
        // Update local subscription state
        BLEConnection * conn = manager.FindConnection((uint8_t)conn_handle);
        if (conn != nullptr)
        {
            bool wasAlreadySubscribed = conn->subscribed;
            conn->subscribed = indicationsEnabled;
            conn->cccdExplicitlySet = true;
            
            // CRITICAL: Defer subscribe event until AFTER first write (which creates endpoint)
            // BleLayer::HandleSubscribeReceived() fails if endpoint doesn't exist yet.
            // The first C1 write creates the endpoint via HandleBleTransportConnectionInitiated().
            if (!conn->firstWriteReceived)
            {
                // Mark subscribe as pending - will be sent after first write
                conn->pendingSubscribe = indicationsEnabled;
                UCX_LOG_VERBOSE(DeviceLayer, "[BLE] Deferring subscribe event until first write (endpoint not created yet)");
                return;
            }
            
            // CRITICAL FIX: If we already auto-inferred subscribe on first C1 write, the
            // endpoint is already created and handshake indication already sent/pending.
            // Posting a DUPLICATE kCHIPoBLESubscribe causes BleLayer to call
            // HandleSubscribeReceived() on an endpoint past kState_Connecting, which
            // DESTROYS the endpoint! This kills the entire BLE commissioning session.
            // The real CCCD write from the phone just confirms what we already inferred.
            if (indicationsEnabled && wasAlreadySubscribed)
            {
                ChipLogProgress(DeviceLayer, "[BLE] CCCD subscribe received but already subscribed via auto-infer - skipping duplicate event");
                return;
            }
        }
        
        // Post subscribe/unsubscribe event to Matter platform queue
        ChipDeviceEvent event;
        event.Type = indicationsEnabled ? DeviceEventType::kCHIPoBLESubscribe : DeviceEventType::kCHIPoBLEUnsubscribe;
        event.CHIPoBLESubscribe.ConId = HandleToConId(conn_handle);
        CHIP_ERROR err = PlatformMgr().PostEvent(&event);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to post %s event: %" CHIP_ERROR_FORMAT,
                        indicationsEnabled ? "subscribe" : "unsubscribe", err.Format());
        }
        return;
    }
    
    // Check if this is C1 write (client TX to device - Matter BLE data)
    if (value_handle == manager.mC1CharHandle)
    {
        sBleWriteCount++;
        ChipLogProgress(DeviceLayer, "[BLE] C1 write #%u received (%llu bytes)",
                       sBleWriteCount, (unsigned long long)value->length);
        
        // Log BTP header for debugging (first few bytes reveal protocol state)
        // BTP Header format: 
        //   Byte 0: Flags (0x01=Start, 0x02=Continue, 0x04=End, 0x08=ACK)
        //   If ACK flag: Next byte is ACK sequence number
        //   Next byte: Sequence number
        //   If Start flag: 2 bytes message length
        if (value->length >= 1)
        {
            uint8_t flags = value->pData[0];
            bool hasStart = (flags & 0x01) != 0;
            bool hasContinue = (flags & 0x02) != 0;
            bool hasEnd = (flags & 0x04) != 0;
            bool hasAck = (flags & 0x08) != 0;
            
            ChipLogProgress(DeviceLayer, "[BLE] BTP Header: flags=0x%02X [%s%s%s%s]",
                           flags,
                           hasStart ? "START " : "",
                           hasContinue ? "CONT " : "",
                           hasEnd ? "END " : "",
                           hasAck ? "ACK " : "");
            
            size_t offset = 1;
            if (hasAck && value->length > offset)
            {
                ChipLogProgress(DeviceLayer, "[BLE] BTP ACK seq: %u", value->pData[offset]);
                offset++;
            }
            if (value->length > offset)
            {
                ChipLogProgress(DeviceLayer, "[BLE] BTP RX seq: %u", value->pData[offset]);
                offset++;
            }
            
            // Check if this is a standalone ACK (no data)
            bool isStandaloneAck = hasAck && !hasStart && !hasContinue && !hasEnd;
            if (isStandaloneAck)
            {
                ChipLogProgress(DeviceLayer, "[BLE] >>> STANDALONE ACK (no message payload) <<<");
            }
            else
            {
                ChipLogProgress(DeviceLayer, "[BLE] >>> MESSAGE DATA (payload: %llu bytes after header) <<<",
                               (unsigned long long)(value->length - offset));
            }
        }
        
        // Copy data to a packet buffer (data will be invalid after callback returns)
        System::PacketBufferHandle buf = System::PacketBufferHandle::NewWithData(value->pData, value->length);
        if (buf.IsNull())
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to allocate packet buffer for C1 write");
            return;
        }
        
        // Check if this is the first write (endpoint will be created by HandleWriteReceived)
        BLEConnection * conn = manager.FindConnection((uint8_t)conn_handle);
        bool isFirstWrite = (conn != nullptr && !conn->firstWriteReceived);
        bool hasPendingSubscribe = (conn != nullptr && conn->pendingSubscribe);
        
        if (conn != nullptr)
        {
            conn->firstWriteReceived = true;
            
            // Re-query MTU now (MTU negotiation happens AFTER connection, before first write)
            // At connection time we may have gotten the default 23, but by now the phone
            // should have negotiated a higher MTU (e.g., 247 for Apple devices)
            if (isFirstWrite)
            {
                uCxBluetoothListConnectionStatusBegin(puCxHandle, conn_handle);
                uCxBtListConnectionStatus_t mtuStatus;
                while (uCxBluetoothListConnectionStatusGetNext(puCxHandle, &mtuStatus))
                {
                    if (mtuStatus.prop_id == U_BT_PROP_ID_MTU_SIZE)
                    {
                        uint16_t mtuValue = static_cast<uint16_t>(mtuStatus.status_val);
                        if (mtuValue >= 23 && mtuValue <= 517)
                        {
                            conn->mtu = mtuValue;
                            ChipLogProgress(DeviceLayer, "[BLE] MTU re-query at first write: %u", mtuValue);
                        }
                    }
                }
                uCxEnd(puCxHandle);
            }
        }
        
        // Post write event to Matter platform queue (like ESP32)
        // This MUST be posted BEFORE subscribe - write creates the endpoint!
        ChipDeviceEvent event;
        event.Type = DeviceEventType::kCHIPoBLEWriteReceived;
        event.CHIPoBLEWriteReceived.ConId = HandleToConId(conn_handle);
        event.CHIPoBLEWriteReceived.Data = std::move(buf).UnsafeRelease();
        CHIP_ERROR err = PlatformMgr().PostEvent(&event);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to post write event: %" CHIP_ERROR_FORMAT, err.Format());
        }
        
        // ---- TEST MODE: Echo C1 write back as C2 indication ----
        // When test mode is enabled, echo received BLE data back on C2 with bit 7 set.
        // Skip the BTP Capabilities Request (magic 0x65 0x6C) — that's handled by BleLayer.
        // Only echo BTP data fragments (post-handshake) for full-duplex testing.
        if (UcxClientManager::IsTestMode() && value->length > 2 &&
            !(value->pData[0] == 0x65 && value->pData[1] == 0x6C))
        {
            // Build echo: copy data with response bit set
            uint8_t echoData[256];
            size_t echoLen = static_cast<size_t>(value->length);
            if (echoLen > sizeof(echoData)) echoLen = sizeof(echoData);
            memcpy(echoData, value->pData, echoLen);
            echoData[0] |= 0x80;  // Set response bit in first byte
            
            int32_t echoResult = uCxGattServerSendIndication(puCxHandle,
                static_cast<int32_t>(conn_handle), manager.mC2CharHandle,
                echoData, static_cast<int32_t>(echoLen));
            
            if (echoResult >= 0) {
                ChipLogProgress(DeviceLayer, "[TEST-ECHO] BLE: echoed %u bytes on C2 (conn=%d)",
                               static_cast<unsigned>(echoLen), conn_handle);
            } else {
                ChipLogError(DeviceLayer, "[TEST-ECHO] BLE echo failed: result=%ld (conn=%d)",
                            static_cast<long>(echoResult), conn_handle);
            }
        }
        
        // If subscribe was deferred, post it now (AFTER the write event)
        // The write event will create the endpoint, then subscribe will find it
        // CRITICAL: NORA-W36 handles CCCD writes internally for host-controlled characteristics
        // and does NOT send +UEBTGCW URCs for CCCD. So if no explicit CCCD write was received,
        // auto-infer subscription on first write (per BTP spec, client MUST subscribe before handshake).
        if (isFirstWrite && (hasPendingSubscribe || !conn->subscribed))
        {
            ChipLogProgress(DeviceLayer, "[BLE] Posting %s subscribe event (endpoint will be created by write)",
                           hasPendingSubscribe ? "deferred" : "auto-inferred (NORA-W36 handled CCCD internally)");
            conn->pendingSubscribe = false;
            conn->subscribed = true;
            
            ChipDeviceEvent subEvent;
            subEvent.Type = DeviceEventType::kCHIPoBLESubscribe;
            subEvent.CHIPoBLESubscribe.ConId = HandleToConId(conn_handle);
            err = PlatformMgr().PostEvent(&subEvent);
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(DeviceLayer, "[BLE] Failed to post deferred subscribe event: %" CHIP_ERROR_FORMAT, err.Format());
            }
        }
    }
    else if (!isOurCharacteristic && value_handle != manager.mC2CccdHandle)
    {
        // Not our handle - NORA-W36 handles standard BLE services internally
        ChipLogProgress(DeviceLayer, "[BLE] Write to standard BLE handle %d (ignored - handled by firmware)",
                       value_handle);
    }
}

// The module's GATT server cannot remove a service, so every start in one boot of the module that the app did not
// reboot in between (a page reloaded, an earlier app session) adds one more Matter service with the same layout below
// ours. A phone that cached the module's GATT table writes its BTP handshake to the C1 of such a copy: unanswered,
// it times out and disconnects. The copies are identical, so the node takes the one the phone uses (and the newest
// one again when the next phone uses that).
bool BLEManagerImpl::AdoptEarlierService(int32_t valueHandle, const uByteArray_t * value)
{
    const int32_t size = mC2CccdHandle - mCHIPoBLEServiceHandle + 1;   // handles of one Matter service

    if (value == nullptr || value->length < 2 || value->pData[0] != 0x65 || value->pData[1] != 0x6C ||
        mCHIPoBLEServiceHandle <= 0 || size <= 0 || valueHandle <= 0 || valueHandle > mDefinedC1Handle ||
        valueHandle == mC1CharHandle || (mDefinedC1Handle - valueHandle) % size != 0)
    {
        return false;
    }
    const int32_t shift = mC1CharHandle - valueHandle;

    ChipLogProgress(DeviceLayer, "[BLE] Handshake on C1 %d of %s Matter service of this module boot: using it "
                                 "(C1 %d, C2 %d) instead of C1 %d",
                    valueHandle, valueHandle == mDefinedC1Handle ? "the newest" : "an earlier", valueHandle,
                    mC2CharHandle - shift, mC1CharHandle);
    mCHIPoBLEServiceHandle -= shift;
    mC1CharHandle = valueHandle;
    mC2CharHandle -= shift;
    mC2CccdHandle -= shift;
    UcxClientManager::GetInstance().OwnGattHandles(mC1CharHandle, mC2CharHandle, mC2CccdHandle);
    return true;
}

// Static URC callback for GATT read requests
// Called when a client tries to read a characteristic value
void BLEManagerImpl::GattServerReadCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                            int32_t value_handle)
{
    BLEManagerImpl & manager = BLEMgrImpl();
    
    ChipLogProgress(DeviceLayer, "[BLE] GATT read request: conn=%d, handle=%d", conn_handle, value_handle);
    
    // Check which characteristic is being read
    if (value_handle == manager.mC2CharHandle)
    {
        // C2 read request - respond with empty data (no pending TX data)
        // The actual data will be sent via indications
        ChipLogProgress(DeviceLayer, "[BLE] C2 read - responding with empty data");
        uint8_t emptyData[1] = {0};  // Provide actual buffer even for empty response
        int32_t result = uCxGattServerReadReqResponse(puCxHandle, conn_handle, emptyData, 0);
        if (result < 0)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to send C2 read response: %d", result);
        }
        else
        {
            ChipLogProgress(DeviceLayer, "[BLE] C2 read response sent successfully");
        }
    }
    else if (value_handle == manager.mC1CharHandle)
    {
        // C1 is write-only, but respond anyway
        ChipLogProgress(DeviceLayer, "[BLE] C1 read (write-only) - responding with empty data");
        uint8_t emptyData[1] = {0};
        int32_t result = uCxGattServerReadReqResponse(puCxHandle, conn_handle, emptyData, 0);
        if (result < 0)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to send C1 read response: %d", result);
        }
    }
    else if (value_handle == manager.mC2CccdHandle)
    {
        // CCCD read - return current subscription state
        BLEConnection * conn = manager.FindConnection((uint8_t)conn_handle);
        uint16_t cccdValue = (conn != nullptr && conn->subscribed) ? 0x0002 : 0x0000;
        uint8_t response[2] = { (uint8_t)(cccdValue & 0xFF), (uint8_t)((cccdValue >> 8) & 0xFF) };
        
        ChipLogProgress(DeviceLayer, "[BLE] CCCD read - responding with 0x%04X", cccdValue);
        int32_t result = uCxGattServerReadReqResponse(puCxHandle, conn_handle, response, 2);
        if (result < 0)
        {
            ChipLogError(DeviceLayer, "[BLE] Failed to send CCCD read response: %d", result);
        }
    }
    else
    {
        // Unknown handle - standard BLE services are handled by firmware
        // Only Host Controlled characteristics send +UEBTGRR URCs, and we defined only C1/C2 as host controlled
        ChipLogProgress(DeviceLayer, "[BLE] Read from standard BLE handle %d (handled by firmware)", value_handle);
    }
}

// Static URC callback for indication acknowledgment
// Called when a client acknowledges receipt of an indication
void BLEManagerImpl::GattServerIndicationAckCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                                     int32_t char_handle)
{
    (void)puCxHandle;
    
    ChipLogProgress(DeviceLayer, "[BLE] Indication ACK received: conn=%d, char=%d", conn_handle, char_handle);
    
    // Post kCHIPoBLEIndicateConfirm event to Matter platform queue
    ChipDeviceEvent event;
    event.Type = DeviceEventType::kCHIPoBLEIndicateConfirm;
    event.CHIPoBLEIndicateConfirm.ConId = HandleToConId(conn_handle);
    CHIP_ERROR err = PlatformMgr().PostEvent(&event);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[BLE] Failed to post indicate confirm event: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void BLEManagerImpl::OnBleEvent(uCxHandle_t * puCxHandle, int32_t event_type, void * pParam)
{
    // Legacy event handler - not used with GATT Server URC callbacks
    (void)puCxHandle;
    (void)event_type;
    (void)pParam;
}

BLEManagerImpl & BLEMgrImpl()
{
    return BLEManagerImpl::sInstance;
}

BLEManager & BLEMgr()
{
    return BLEManagerImpl::sInstance;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
