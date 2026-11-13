/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <platform/internal/BLEManager.h>
#include <ble/Ble.h>

// Include ucxclient types for BLE/GATT
#include "u_cx.h"
#include "u_cx_types.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

/**
 * BLE Manager implementation using u-blox ucxclient for NORA-W36.
 * Provides CHIPoBLE support for Matter commissioning.
 */
class BLEManagerImpl final : public BLEManager,
                              private Ble::BleLayer,
                              private Ble::BlePlatformDelegate,
                              private Ble::BleApplicationDelegate
{
public:
    BLEManagerImpl() = default;

    // Singleton instance
    static BLEManagerImpl sInstance;

    // BLEManager interface (CRTP pattern - no virtual/override needed)
    CHIP_ERROR _Init();
    void _Shutdown();
    BLEManager::CHIPoBLEServiceMode _GetCHIPoBLEServiceMode();
    CHIP_ERROR _SetCHIPoBLEServiceMode(BLEManager::CHIPoBLEServiceMode mode);
    bool _IsAdvertisingEnabled();
    CHIP_ERROR _SetAdvertisingEnabled(bool enabled);
    bool _IsAdvertising();
    CHIP_ERROR _SetAdvertisingMode(BLEManager::BLEAdvertisingMode mode);
    CHIP_ERROR _GetDeviceName(char * buf, size_t bufSize);
    CHIP_ERROR _SetDeviceName(const char * name);
    uint16_t _NumConnections();
    void _OnPlatformEvent(const ChipDeviceEvent * event);
    Ble::BleLayer * _GetBleLayer();

    // BlePlatformDelegate interface
    CHIP_ERROR SubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR UnsubscribeCharacteristic(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId) override;
    CHIP_ERROR CloseConnection(BLE_CONNECTION_OBJECT conId) override;
    uint16_t GetMTU(BLE_CONNECTION_OBJECT conId) const override;
    CHIP_ERROR SendIndication(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId, System::PacketBufferHandle pBuf) override;
    CHIP_ERROR SendWriteRequest(BLE_CONNECTION_OBJECT conId, const Ble::ChipBleUUID * svcId, const Ble::ChipBleUUID * charId, System::PacketBufferHandle pBuf) override;

    // BleApplicationDelegate interface
    void NotifyChipConnectionClosed(BLE_CONNECTION_OBJECT conId) override;

    // u-connectXpressApp: the last Init failed because the module refused the CHIPoBLE service (its GATT server
    // keeps every service defined until it reboots)
    bool GattFull() const { return mGattFull; }

private:
    struct BLEConnection
    {
        uint8_t connectionHandle = 0xFF;
        uint16_t mtu = 23; // Default BLE ATT MTU
        uBtLeAddress_t bdAddress;       // BD address of connected peer (from +UEBTLC URC)
        bool bdAddressValid = false;    // True if bdAddress was set on connect
        bool subscribed = false;
        bool pendingSubscribe = false;  // Defer subscribe event until after first write
        bool firstWriteReceived = false; // Track if endpoint exists yet
        bool closedByRemote = false;  // Track if connection was closed by remote (via +UEBTDC URC)
        bool pendingDisconnect = false; // Track if disconnect is pending (deferred to avoid AT race)
        bool cccdExplicitlySet = false; // Track if CCCD was set on NORA-W36 firmware
    };

    static constexpr uint8_t kMaxConnections = 1; // Matter typically uses 1 BLE connection
    BLEConnection mConnections[kMaxConnections];

    BLEManager::CHIPoBLEServiceMode mServiceMode = ConnectivityManager::kCHIPoBLEServiceMode_Enabled;
    BLEManager::BLEAdvertisingMode mAdvertisingMode = BLEAdvertisingMode::kFastAdvertising;
    bool mAdvertisingEnabled = false;
    bool mAdvertising = false;
    bool mRestarting  = false;   // StopAdvertising only to start again with other intervals
    char mDeviceName[32] = "Matter Device";

    // ucxclient handles
    uCxHandle_t * mUcxHandle = nullptr;
    int32_t mGattServerHandle = -1;
    int32_t mCHIPoBLEServiceHandle = -1;
    int32_t mC1CharHandle = -1; // CHIPoBLE C1 characteristic (write)
    int32_t mC2CharHandle = -1; // CHIPoBLE C2 characteristic (indicate)
    int32_t mC2CccdHandle = -1; // C2 CCCD for indication subscriptions
    bool mGattFull = false;
    int32_t mDefinedC1Handle = -1; // C1 of the service this start defined, the newest on the module

    // A BTP handshake on the C1 of a Matter service an earlier start left on the module: that copy becomes ours
    bool AdoptEarlierService(int32_t valueHandle, const uByteArray_t * value);

    // Helper methods - state machine
    static void DriveBLEState(intptr_t arg);  // Static callback for ScheduleWork
    void DriveBLEStateImpl();                  // Actual state machine implementation
    
    // Helper methods - BLE operations
    CHIP_ERROR InitGattServer();
    CHIP_ERROR StartAdvertising();
    CHIP_ERROR StopAdvertising();
    void HandleConnectionEvent(uint8_t connHandle, const uBtLeAddress_t * bdAddr = nullptr);
    void HandleDisconnectionEvent(uint8_t connHandle);
    void HandleWriteEvent(uint8_t connHandle, int32_t charHandle, const uint8_t * data, size_t length);
    BLEConnection * AllocateConnection(uint8_t connHandle);
    BLEConnection * FindConnection(uint8_t connHandle);
    void ReleaseConnection(uint8_t connHandle);
    static const char * FormatBdAddress(const uBtLeAddress_t & addr, char * buf, size_t bufSize);

    // Static URC callbacks (ucxclient requires C-style functions)
    static void BleConnectCallback(uCxHandle_t * puCxHandle, int32_t conn_handle, uBtLeAddress_t * bd_addr);
    static void BleDisconnectCallback(uCxHandle_t * puCxHandle, int32_t conn_handle);
    static void BleBondStatusCallback(uCxHandle_t * puCxHandle, uBtLeAddress_t * bd_addr, uBtBondStatus_t bond_status);
    static void BlePhyUpdateCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                     int32_t phy_status, int32_t tx_phy, int32_t rx_phy);
    static void GattServerWriteCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                       int32_t value_handle, uByteArray_t * value,
                                       uGattServerOptions_t options);
    static void GattServerReadCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                      int32_t value_handle);
    static void GattServerIndicationAckCallback(uCxHandle_t * puCxHandle, int32_t conn_handle,
                                                int32_t char_handle);
    static void OnBleEvent(uCxHandle_t * puCxHandle, int32_t event_type, void * pParam);
};

BLEManager & BLEMgr();

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
