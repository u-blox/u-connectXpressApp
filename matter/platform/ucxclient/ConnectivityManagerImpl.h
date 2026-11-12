/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file
 *   ConnectivityManager implementation for ucxclient platform.
 */

#pragma once

#include <platform/CHIPDeviceConfig.h>
#include <platform/ConnectivityManager.h>
#include <platform/internal/GenericConnectivityManagerImpl.h>
#include <platform/internal/GenericConnectivityManagerImpl_UDP.h>

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
#include <platform/internal/GenericConnectivityManagerImpl_TCP.h>
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#include <platform/internal/GenericConnectivityManagerImpl_WiFi.h>
#else
#include <platform/internal/GenericConnectivityManagerImpl_NoWiFi.h>
#endif
#include <platform/internal/GenericConnectivityManagerImpl_BLE.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/internal/GenericConnectivityManagerImpl_Thread.h>
#else
#include <platform/internal/GenericConnectivityManagerImpl_NoThread.h>
#endif
#include <inet/InetLayer.h>
#include <inet/UDPEndPoint.h>
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
#include <inet/TCPEndPoint.h>
#endif
#include <ble/BleLayer.h>

#include <lib/support/BitFlags.h>

namespace chip {
namespace DeviceLayer {

class PlatformManagerImpl;

/**
 * Concrete implementation of ConnectivityManager singleton for ucxclient platform.
 *
 * Architecture:
 * - Matter application runs on host OS (Windows/Linux/FreeRTOS)
 * - NORA-W36 (via ucxclient) handles Wi-Fi and BLE connectivity
 * - NORA-B26 (via ucxclient) handles Thread and BLE connectivity
 * - Compile-time selection: WiFi-only, Thread-only, or both
 */
class ConnectivityManagerImpl final : public ConnectivityManager,
                                      public Internal::GenericConnectivityManagerImpl<ConnectivityManagerImpl>,
                                      public Internal::GenericConnectivityManagerImpl_UDP<ConnectivityManagerImpl>,
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
                                      public Internal::GenericConnectivityManagerImpl_TCP<ConnectivityManagerImpl>,
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
                                      public Internal::GenericConnectivityManagerImpl_WiFi<ConnectivityManagerImpl>,
#else
                                      public Internal::GenericConnectivityManagerImpl_NoWiFi<ConnectivityManagerImpl>,
#endif
                                      public Internal::GenericConnectivityManagerImpl_BLE<ConnectivityManagerImpl>,
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
                                      public Internal::GenericConnectivityManagerImpl_Thread<ConnectivityManagerImpl>
#else
                                      public Internal::GenericConnectivityManagerImpl_NoThread<ConnectivityManagerImpl>
#endif
{
    friend class ConnectivityManager;

public:
    // ===== Platform-specific members that may be accessed directly by the application.

    // Singleton instance
    static ConnectivityManagerImpl sInstance;

private:
    // ===== Methods that implement the ConnectivityManager abstract interface.

    CHIP_ERROR _Init();
    void _OnPlatformEvent(const ChipDeviceEvent * event);

    // ===== WiFi-specific members (only when WiFi enabled)

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    WiFiStationMode _GetWiFiStationMode();
    CHIP_ERROR _SetWiFiStationMode(WiFiStationMode val);
    bool _IsWiFiStationEnabled();
    bool _IsWiFiStationConnected();
    bool _IsWiFiStationProvisioned();
    void _ClearWiFiStationProvision();
    WiFiStationState _GetWiFiStationState();
#endif

    // ===== BLE-specific members

    Ble::BleLayer * _GetBleLayer();

    // ===== EndPointManager accessors (required for ConnectivityManager interface)
    
    Inet::EndPointManager<Inet::UDPEndPoint> & _UDPEndPointManager();
    
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    Inet::EndPointManager<Inet::TCPEndPoint> & _TCPEndPointManager();
#endif

    // ===== Private members

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    WiFiStationMode mWiFiStationMode               = kWiFiStationMode_Disabled;
    WiFiStationState mWiFiStationState             = kWiFiStationState_NotConnected;
    bool mIsProvisioned                            = false;

    void DriveStationState();
    static void DriveStationState(chip::System::Layer * systemLayer, void * appState);
    void UpdateInternetConnectivityState();
#endif

    // EndPointManager instances
    Inet::EndPointManagerImplPool<Inet::UDPEndPointImpl> mUDPEndPointManager;
    
#if INET_CONFIG_ENABLE_TCP_ENDPOINT
    Inet::EndPointManagerImplPool<Inet::TCPEndPointImpl> mTCPEndPointManager;
#endif
};

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
inline ConnectivityManager::WiFiStationMode ConnectivityManagerImpl::_GetWiFiStationMode()
{
    return mWiFiStationMode;
}

inline bool ConnectivityManagerImpl::_IsWiFiStationEnabled()
{
    return mWiFiStationMode == kWiFiStationMode_Enabled;
}

inline bool ConnectivityManagerImpl::_IsWiFiStationConnected()
{
    return mWiFiStationState == kWiFiStationState_Connected;
}

inline ConnectivityManager::WiFiStationState ConnectivityManagerImpl::_GetWiFiStationState()
{
    return mWiFiStationState;
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

} // namespace DeviceLayer
} // namespace chip
