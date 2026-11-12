/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <platform/DiagnosticDataProvider.h>
#include <platform/KeyValueStoreManager.h>

namespace chip {
namespace DeviceLayer {

class DiagnosticDataProviderImpl : public DiagnosticDataProvider
{
public:
    static DiagnosticDataProviderImpl & GetDefaultInstance();

    // General Diagnostics
    CHIP_ERROR GetCurrentHeapFree(uint64_t & currentHeapFree) override;
    CHIP_ERROR GetCurrentHeapUsed(uint64_t & currentHeapUsed) override;
    CHIP_ERROR GetCurrentHeapHighWatermark(uint64_t & currentHeapHighWatermark) override;
    CHIP_ERROR GetRebootCount(uint16_t & rebootCount) override;
    CHIP_ERROR GetUpTime(uint64_t & upTime) override;
    CHIP_ERROR GetTotalOperationalHours(uint32_t & totalOperationalHours) override;
    CHIP_ERROR GetBootReason(BootReasonType & bootReason) override;

    // WiFi Diagnostics (queries NORA-W36 via UcxClientManager)
    CHIP_ERROR GetWiFiBssId(MutableByteSpan & bssId) override;
    CHIP_ERROR GetWiFiSecurityType(app::Clusters::WiFiNetworkDiagnostics::SecurityTypeEnum & securityType) override;
    CHIP_ERROR GetWiFiVersion(app::Clusters::WiFiNetworkDiagnostics::WiFiVersionEnum & wifiVersion) override;
    CHIP_ERROR GetWiFiChannelNumber(uint16_t & channelNumber) override;
    CHIP_ERROR GetWiFiRssi(int8_t & rssi) override;
    CHIP_ERROR GetWiFiBeaconLostCount(uint32_t & beaconLostCount) override;
    CHIP_ERROR GetWiFiBeaconRxCount(uint32_t & beaconRxCount) override;
    CHIP_ERROR GetWiFiPacketMulticastRxCount(uint32_t & packetMulticastRxCount) override;
    CHIP_ERROR GetWiFiPacketMulticastTxCount(uint32_t & packetMulticastTxCount) override;
    CHIP_ERROR GetWiFiPacketUnicastRxCount(uint32_t & packetUnicastRxCount) override;
    CHIP_ERROR GetWiFiPacketUnicastTxCount(uint32_t & packetUnicastTxCount) override;
    CHIP_ERROR GetWiFiOverrunCount(uint64_t & overrunCount) override;
    CHIP_ERROR GetWiFiCurrentMaxRate(uint64_t & currentMaxRate) override;
    CHIP_ERROR ResetWiFiNetworkDiagnosticsCounts() override;

    // Thread/Task metrics (FreeRTOS only - provides task list for Software Diagnostics cluster)
#if defined(U_PORT_FREERTOS)
    CHIP_ERROR GetThreadMetrics(ThreadMetrics ** threadMetricsOut) override;
    void ReleaseThreadMetrics(ThreadMetrics * threadMetrics) override;
#endif

    // Network Interface Enumeration (General Diagnostics cluster)
    CHIP_ERROR GetNetworkInterfaces(NetworkInterface ** netifpp) override;
    void ReleaseNetworkInterfaces(NetworkInterface * netifp) override;

    // Reboot count persistence helper (call at startup)
    static void IncrementRebootCount();

    // Boot reason persistence helper (call from signal/crash handlers)
    // Stores reason in KVS so GetBootReason() returns correct value on next boot
    static void SetBootReason(BootReasonType reason)
    {
        uint8_t val = static_cast<uint8_t>(reason);
        (void)chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("diag/boot_reason", &val, sizeof(val));
    }

private:
    DiagnosticDataProviderImpl() = default;
};

} // namespace DeviceLayer
} // namespace chip
