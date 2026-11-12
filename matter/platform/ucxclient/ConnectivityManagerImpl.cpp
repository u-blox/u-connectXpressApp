/*
 *    Copyright (c) 2025 u-blox
 *    Licensed under the Apache License, Version 2.0
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/ConnectivityManager.h>
#include <platform/PlatformManager.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

#include "ConnectivityManagerImpl.h"
#include "UcxClientManager.h"
#include "BLEManagerImpl.h"

// Include generic implementation - like Linux does
#include <platform/internal/GenericConnectivityManagerImpl_UDP.ipp>

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
#include <platform/internal/GenericConnectivityManagerImpl_TCP.ipp>
#endif

#include <platform/internal/GenericConnectivityManagerImpl_BLE.ipp>

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
extern "C" {
#include "u_cx_wifi.h"
}
#endif

namespace chip {
namespace DeviceLayer {

CHIP_ERROR ConnectivityManagerImpl::_Init()
{
    ChipLogProgress(DeviceLayer, "ConnectivityManagerImpl::Init");

    CHIP_ERROR err = CHIP_NO_ERROR;

#if !defined(U_PORT_FREERTOS)
    // Initialize UDP/TCP EndPointManagers - Only on non-FreeRTOS platforms
    // On FreeRTOS, GenericPlatformManagerImpl::_InitChipStack() already initializes these
    // via the base class call in PlatformManagerImpl::_InitChipStack()
    err = _UDPEndPointManager().Init(DeviceLayer::SystemLayer());
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "UDP EndPointManager Init failed: %08X", err.AsInteger());
        return err;
    }
    ChipLogProgress(DeviceLayer, "UDP EndPointManager initialized");

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    err = _TCPEndPointManager().Init(DeviceLayer::SystemLayer());
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "TCP EndPointManager Init failed: %08X", err.AsInteger());
        return err;
    }
    ChipLogProgress(DeviceLayer, "TCP EndPointManager initialized");
#endif
#else
    // On FreeRTOS: UDP/TCP already initialized by GenericPlatformManagerImpl
    ChipLogProgress(DeviceLayer, "UDP/TCP EndPointManagers already initialized by PlatformMgr");
#endif

    // u-connectXpressApp: the module is open already, on the app's AT client (UcxClientManager.h)
    VerifyOrExit(UcxClientManager::GetInstance().IsInitialized(), err = CHIP_ERROR_INCORRECT_STATE);

#if !defined(U_PORT_FREERTOS)
    // Initialize BLE Manager for Matter commissioning
    // On FreeRTOS: BLE already initialized by GenericPlatformManagerImpl::_InitChipStack()
    ChipLogProgress(DeviceLayer, "Initializing BLE Manager...");
    err = Internal::BLEMgr().Init();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "BLE Manager Init failed: %08X", err.AsInteger());
        goto exit;
    }
    ChipLogProgress(DeviceLayer, "BLE Manager initialized successfully");
#else
    ChipLogProgress(DeviceLayer, "BLE Manager already initialized by PlatformMgr");
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    mWiFiStationMode  = kWiFiStationMode_Disabled;
    mWiFiStationState = kWiFiStationState_NotConnected;
    // u-connectXpressApp: the module's Wi-Fi as it is now, and its URCs from here on
    UcxClientManager::GetInstance().NetworkStart();
#endif

exit:
    return err;
}

void ConnectivityManagerImpl::_OnPlatformEvent(const ChipDeviceEvent * event)
{
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    // Update WiFi state when IP address changes (posted by UcxClientManager callbacks)
    if (event->Type == DeviceEventType::kInterfaceIpAddressChanged)
    {
        auto & ucxMgr = UcxClientManager::GetInstance();
        if (ucxMgr.IsWiFiConnected())
        {
            mWiFiStationMode = kWiFiStationMode_Enabled;
            mWiFiStationState = kWiFiStationState_Connected;
            ChipLogProgress(DeviceLayer, "[ConnMgr] WiFi state updated: Connected");
        }
        else
        {
            mWiFiStationState = kWiFiStationState_NotConnected;
            // Quieted to Detail (Vegas 2026): redundant with [WiFi] Disconnected summary
            // that UcxClientManager already prints once per network-down event.
            ChipLogDetail(DeviceLayer, "[ConnMgr] WiFi state updated: Disconnected");
        }
    }
#else
    (void) event;
#endif
}

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
CHIP_ERROR ConnectivityManagerImpl::_SetWiFiStationMode(WiFiStationMode val)
{
    mWiFiStationMode = val;
    return CHIP_NO_ERROR;
}

bool ConnectivityManagerImpl::_IsWiFiStationProvisioned()
{
    return mIsProvisioned;
}

void ConnectivityManagerImpl::_ClearWiFiStationProvision()
{
    mIsProvisioned = false;
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

Ble::BleLayer * ConnectivityManagerImpl::_GetBleLayer()
{
    return static_cast<chip::DeviceLayer::Internal::BLEManagerImpl&>(chip::DeviceLayer::Internal::BLEMgr())._GetBleLayer();
}

Inet::EndPointManager<Inet::UDPEndPoint> & ConnectivityManagerImpl::_UDPEndPointManager()
{
    return mUDPEndPointManager;
}

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
Inet::EndPointManager<Inet::TCPEndPoint> & ConnectivityManagerImpl::_TCPEndPointManager()
{
    return mTCPEndPointManager;
}
#endif

} // namespace DeviceLayer
} // namespace chip

// Note: Template instantiation happens in GenericConnectivityManagerImpl_UDP.cpp
// to avoid duplicate instantiation when this file is compiled
