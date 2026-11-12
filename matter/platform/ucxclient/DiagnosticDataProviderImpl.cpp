/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "DiagnosticDataProviderImpl.h"
#include "UcxClientManager.h"
#include <platform/DiagnosticDataProvider.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/ConfigurationManager.h>
#include <lib/support/CHIPMemString.h>
#include <app-common/zap-generated/cluster-enums.h>
#include <lib/support/SafeInt.h>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
// CRITICAL: Undefine Windows ERROR macro that conflicts with Matter SDK logging
#ifdef ERROR
#undef ERROR
#endif
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#endif

#if defined(U_PORT_FREERTOS)
extern "C" {
#include "FreeRTOS.h"
#include "task.h"
}
#endif

using namespace chip::app::Clusters::WiFiNetworkDiagnostics;

namespace chip {
namespace DeviceLayer {

static auto sStartTime = std::chrono::steady_clock::now();
static uint16_t sRebootCount = 0;
static constexpr const char * kRebootCountKey = "diag/reboot_cnt";
static constexpr const char * kOperationalHoursKey = "diag/op_hours";
static constexpr const char * kBootReasonKey = "diag/boot_reason";
static uint32_t sPreviousOperationalHours = 0; // Accumulated from previous runs

DiagnosticDataProviderImpl & DiagnosticDataProviderImpl::GetDefaultInstance()
{
    static DiagnosticDataProviderImpl sInstance;
    return sInstance;
}

void DiagnosticDataProviderImpl::IncrementRebootCount()
{
    // Read current reboot count from KVS, increment, and write back
    uint16_t count = 0;
    size_t readSize = 0;
    CHIP_ERROR err = PersistedStorage::KeyValueStoreMgr().Get(kRebootCountKey, &count, sizeof(count), &readSize);
    if (err == CHIP_NO_ERROR && readSize == sizeof(count))
    {
        count++;
    }
    else
    {
        count = 1; // First boot or corrupt data
    }
    sRebootCount = count;
    (void)PersistedStorage::KeyValueStoreMgr().Put(kRebootCountKey, &count, sizeof(count));
    ChipLogProgress(DeviceLayer, "[Diag] Reboot count: %u", static_cast<unsigned>(count));

    // Load accumulated operational hours from previous runs
    uint32_t prevHours = 0;
    readSize = 0;
    err = PersistedStorage::KeyValueStoreMgr().Get(kOperationalHoursKey, &prevHours, sizeof(prevHours), &readSize);
    if (err == CHIP_NO_ERROR && readSize == sizeof(prevHours))
    {
        sPreviousOperationalHours = prevHours;
        ChipLogProgress(DeviceLayer, "[Diag] Previous operational hours: %u", static_cast<unsigned>(prevHours));
    }
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapFree(uint64_t & currentHeapFree)
{
#ifdef _WIN32
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        currentHeapFree = memStatus.ullAvailPhys; // Available physical memory
        return CHIP_NO_ERROR;
    }
#elif defined(U_PORT_FREERTOS)
    currentHeapFree = xPortGetFreeHeapSize();
    return CHIP_NO_ERROR;
#endif
    currentHeapFree = 0;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapUsed(uint64_t & currentHeapUsed)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        currentHeapUsed = pmc.WorkingSetSize; // Current process working set
        return CHIP_NO_ERROR;
    }
#elif defined(U_PORT_FREERTOS)
    // FreeRTOS: total heap - free heap = used heap
    currentHeapUsed = configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize();
    return CHIP_NO_ERROR;
#endif
    currentHeapUsed = 0;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetCurrentHeapHighWatermark(uint64_t & currentHeapHighWatermark)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        currentHeapHighWatermark = pmc.PeakWorkingSetSize; // Peak process working set
        return CHIP_NO_ERROR;
    }
#elif defined(U_PORT_FREERTOS)
    currentHeapHighWatermark = configTOTAL_HEAP_SIZE - xPortGetMinimumEverFreeHeapSize();
    return CHIP_NO_ERROR;
#endif
    currentHeapHighWatermark = 0;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetRebootCount(uint16_t & rebootCount)
{
    rebootCount = sRebootCount;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetUpTime(uint64_t & upTime)
{
    // Calculate uptime in seconds since application start
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - sStartTime);
    upTime = duration.count();
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetTotalOperationalHours(uint32_t & totalOperationalHours)
{
    // Accumulated hours = hours from previous runs + hours from current session
    auto now = std::chrono::steady_clock::now();
    auto sessionHours = std::chrono::duration_cast<std::chrono::hours>(now - sStartTime);
    totalOperationalHours = sPreviousOperationalHours + static_cast<uint32_t>(sessionHours.count());

    // Persist updated total every time it's queried (CSA test may query this)
    uint32_t currentTotal = totalOperationalHours;
    (void)PersistedStorage::KeyValueStoreMgr().Put(kOperationalHoursKey, &currentTotal, sizeof(currentTotal));

    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetBootReason(BootReasonType & bootReason)
{
    // Read boot reason stored at previous shutdown/crash
    uint8_t storedReason = 0;
    size_t readSize = 0;
    CHIP_ERROR err = PersistedStorage::KeyValueStoreMgr().Get(kBootReasonKey, &storedReason, sizeof(storedReason), &readSize);
    if (err == CHIP_NO_ERROR && readSize == sizeof(storedReason) && storedReason <= static_cast<uint8_t>(BootReasonType::kSoftwareReset))
    {
        bootReason = static_cast<BootReasonType>(storedReason);
    }
    else
    {
        bootReason = BootReasonType::kUnspecified;
    }

    // Store "PowerOnReboot" for next boot (overridden by signal handler if crash occurs)
    uint8_t nextReason = static_cast<uint8_t>(BootReasonType::kPowerOnReboot);
    (void)PersistedStorage::KeyValueStoreMgr().Put(kBootReasonKey, &nextReason, sizeof(nextReason));

    return CHIP_NO_ERROR;
}

// ===== WiFi Diagnostics (data from NORA-W36 via UcxClientManager) =====

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBssId(MutableByteSpan & bssId)
{
    auto & ucxMgr = UcxClientManager::GetInstance();
    if (bssId.size() < 6)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    if (!ucxMgr.GetWiFiBssId(bssId.data()))
    {
        return CHIP_ERROR_NOT_CONNECTED;
    }
    bssId.reduce_size(6);
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiSecurityType(SecurityTypeEnum & securityType)
{
    auto & ucxMgr = UcxClientManager::GetInstance();
    if (!ucxMgr.IsWiFiConnected())
    {
        return CHIP_ERROR_NOT_CONNECTED;
    }
    uint8_t secType = ucxMgr.GetWiFiSecurityType();
    // Map from WiFiSecurityBitmap flags to SecurityTypeEnum
    // Bit 5 = WPA3, Bit 4 = WPA2, Bit 3 = WPA, Bit 0 = WEP
    if (secType & 0x20)      // WPA3
        securityType = SecurityTypeEnum::kWpa3;
    else if (secType & 0x10) // WPA2
        securityType = SecurityTypeEnum::kWpa2;
    else if (secType & 0x08) // WPA
        securityType = SecurityTypeEnum::kWpa;
    else if (secType & 0x01) // WEP
        securityType = SecurityTypeEnum::kWep;
    else if (secType == 0)
        securityType = SecurityTypeEnum::kNone;
    else
        securityType = SecurityTypeEnum::kUnspecified;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiVersion(WiFiVersionEnum & wifiVersion)
{
    // NORA-W36 supports WiFi 4 (802.11n) on 2.4GHz
    wifiVersion = WiFiVersionEnum::kN;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiChannelNumber(uint16_t & channelNumber)
{
    auto & ucxMgr = UcxClientManager::GetInstance();
    channelNumber = ucxMgr.GetWiFiChannel();
    if (channelNumber == 0)
    {
        return CHIP_ERROR_NOT_CONNECTED;
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiRssi(int8_t & rssi)
{
    auto & ucxMgr = UcxClientManager::GetInstance();
    return ucxMgr.GetWiFiRssi(rssi);
}

// ===== WiFi Packet Counters =====
// NORA-W36 does not expose packet-level statistics via AT commands.
// These return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE to indicate the data
// is genuinely unavailable (not just unimplemented).

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBeaconLostCount(uint32_t & beaconLostCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiBeaconRxCount(uint32_t & beaconRxCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketMulticastRxCount(uint32_t & packetMulticastRxCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketMulticastTxCount(uint32_t & packetMulticastTxCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketUnicastRxCount(uint32_t & packetUnicastRxCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiPacketUnicastTxCount(uint32_t & packetUnicastTxCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiOverrunCount(uint64_t & overrunCount)
{
    return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
}

CHIP_ERROR DiagnosticDataProviderImpl::GetWiFiCurrentMaxRate(uint64_t & currentMaxRate)
{
    // NORA-W36 supports 802.11n (WiFi 4) on 2.4GHz
    // Max PHY rate: 72.2 Mbps (1 spatial stream, 20MHz, short GI)
    // Report in bits per second as per Matter spec
    currentMaxRate = 72200000ULL;
    return CHIP_NO_ERROR;
}

CHIP_ERROR DiagnosticDataProviderImpl::ResetWiFiNetworkDiagnosticsCounts()
{
    // No counters to reset (NORA-W36 doesn't expose packet stats)
    return CHIP_NO_ERROR;
}

// ===== Thread/Task Metrics (FreeRTOS only, requires configUSE_TRACE_FACILITY) =====

#if defined(U_PORT_FREERTOS) && (configUSE_TRACE_FACILITY == 1)
CHIP_ERROR DiagnosticDataProviderImpl::GetThreadMetrics(ThreadMetrics ** threadMetricsOut)
{
    // FreeRTOS task enumeration for Software Diagnostics cluster
    UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    if (taskCount == 0)
    {
        *threadMetricsOut = nullptr;
        return CHIP_NO_ERROR;
    }

    // Limit to reasonable count to avoid excessive stack usage
    constexpr UBaseType_t kMaxTasks = 20;
    if (taskCount > kMaxTasks)
    {
        taskCount = kMaxTasks;
    }

    TaskStatus_t * taskStatusArray = static_cast<TaskStatus_t *>(
        chip::Platform::MemoryAlloc(taskCount * sizeof(TaskStatus_t)));
    if (taskStatusArray == nullptr)
    {
        return CHIP_ERROR_NO_MEMORY;
    }

    uint32_t totalRunTime = 0;
    UBaseType_t actualCount = uxTaskGetSystemState(taskStatusArray, taskCount, &totalRunTime);

    ThreadMetrics * head = nullptr;
    for (UBaseType_t i = 0; i < actualCount; i++)
    {
        auto * metrics = new ThreadMetrics();
        Platform::CopyString(metrics->NameBuf, sizeof(metrics->NameBuf), taskStatusArray[i].pcTaskName);
        metrics->name.SetValue(CharSpan::fromCharString(metrics->NameBuf));
        metrics->id = taskStatusArray[i].xTaskNumber;
        metrics->stackFreeMinimum.SetValue(taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t));
        metrics->stackFreeCurrent.SetValue(taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t));
        metrics->stackSize.SetValue(0); // Not available from uxTaskGetSystemState

        metrics->Next = head;
        head = metrics;
    }

    chip::Platform::MemoryFree(taskStatusArray);
    *threadMetricsOut = head;
    return CHIP_NO_ERROR;
}

void DiagnosticDataProviderImpl::ReleaseThreadMetrics(ThreadMetrics * threadMetrics)
{
    while (threadMetrics != nullptr)
    {
        ThreadMetrics * next = threadMetrics->Next;
        delete threadMetrics;
        threadMetrics = next;
    }
}
#elif defined(U_PORT_FREERTOS)
// Stub implementations when configUSE_TRACE_FACILITY is disabled
CHIP_ERROR DiagnosticDataProviderImpl::GetThreadMetrics(ThreadMetrics ** threadMetricsOut)
{
    *threadMetricsOut = nullptr;
    return CHIP_NO_ERROR;
}

void DiagnosticDataProviderImpl::ReleaseThreadMetrics(ThreadMetrics * threadMetrics)
{
    (void) threadMetrics;
}
#endif // U_PORT_FREERTOS && configUSE_TRACE_FACILITY

// ===== Network Interface Enumeration =====

CHIP_ERROR DiagnosticDataProviderImpl::GetNetworkInterfaces(NetworkInterface ** netifpp)
{
    using app::Clusters::GeneralDiagnostics::InterfaceTypeEnum;

    auto & ucxMgr = UcxClientManager::GetInstance();

    // NORA-W36 provides a single WiFi interface
    auto * ifp = new NetworkInterface();

    Platform::CopyString(ifp->Name, "wlan0");
    ifp->name          = CharSpan::fromCharString(ifp->Name);
    ifp->isOperational = ucxMgr.IsWiFiConnected();
    ifp->type          = InterfaceTypeEnum::kWiFi;
    ifp->offPremiseServicesReachableIPv4.SetNull();
    ifp->offPremiseServicesReachableIPv6.SetNull();

    // WiFi MAC address (6 bytes)
    uint8_t mac[6] = {};
    if (ConfigurationMgr().GetPrimaryWiFiMACAddress(mac) == CHIP_NO_ERROR)
    {
        memcpy(ifp->MacAddress, mac, 6);
        ifp->hardwareAddress = ByteSpan(ifp->MacAddress, 6);
    }

    // IPv4 address
    const uint8_t * ipv4 = ucxMgr.GetIpv4Address();
    if (ipv4 != nullptr)
    {
        memcpy(ifp->Ipv4AddressesBuffer[0], ipv4, kMaxIPv4AddrSize);
        ifp->Ipv4AddressSpans[0] = ByteSpan(ifp->Ipv4AddressesBuffer[0], kMaxIPv4AddrSize);
        ifp->IPv4Addresses       = app::DataModel::List<ByteSpan>(ifp->Ipv4AddressSpans, 1);
    }

    // IPv6 address
    const uint8_t * ipv6 = ucxMgr.GetIpv6Address();
    if (ipv6 != nullptr)
    {
        memcpy(ifp->Ipv6AddressesBuffer[0], ipv6, kMaxIPv6AddrSize);
        ifp->Ipv6AddressSpans[0] = ByteSpan(ifp->Ipv6AddressesBuffer[0], kMaxIPv6AddrSize);
        ifp->IPv6Addresses       = app::DataModel::List<ByteSpan>(ifp->Ipv6AddressSpans, 1);
    }

    ifp->Next = nullptr;
    *netifpp  = ifp;
    return CHIP_NO_ERROR;
}

void DiagnosticDataProviderImpl::ReleaseNetworkInterfaces(NetworkInterface * netifp)
{
    while (netifp != nullptr)
    {
        NetworkInterface * next = netifp->Next;
        delete netifp;
        netifp = next;
    }
}

DiagnosticDataProvider & GetDiagnosticDataProviderImpl()
{
    return DiagnosticDataProviderImpl::GetDefaultInstance();
}

} // namespace DeviceLayer
} // namespace chip
