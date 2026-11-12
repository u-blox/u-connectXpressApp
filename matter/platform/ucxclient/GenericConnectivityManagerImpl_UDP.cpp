/*
 *    Copyright (c) 2025 u-blox
 *    Licensed under the Apache License, Version 2.0
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/ConnectivityManager.h>
#include "ConnectivityManagerImpl.h"

// Include generic template implementations after the class is fully defined
#include <platform/internal/GenericConnectivityManagerImpl_UDP.ipp>

#if INET_CONFIG_ENABLE_TCP_ENDPOINT
#include <platform/internal/GenericConnectivityManagerImpl_TCP.ipp>
#endif

namespace chip {
namespace DeviceLayer {

// Instantiate the ConnectivityMgr singleton
ConnectivityManagerImpl ConnectivityManagerImpl::sInstance;

ConnectivityManager & ConnectivityMgr()
{
    return ConnectivityManagerImpl::sInstance;
}

ConnectivityManager * GetConnectivityManager()
{
    return &ConnectivityManagerImpl::sInstance;
}

} // namespace DeviceLayer
} // namespace chip
