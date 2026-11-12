/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 */

/**
 * @file
 *   UDP endpoint for NORA-W36 u_cx_socket API.
 *   Windows BSD sockets can't access NORA-W36 network (COM port device).
 *   This endpoint uses u_cx_socket API directly.
 */

#pragma once

#include <inet/UDPEndPoint.h>
#include <system/SystemLayer.h>

namespace chip {
namespace Inet {

class UDPEndPointImplUcx : public UDPEndPoint
{
public:
    UDPEndPointImplUcx(EndPointManager<UDPEndPoint> & endPointManager);

    // UDPEndPoint overrides
    CHIP_ERROR SetMulticastLoopback(IPVersion aIPVersion, bool aLoopback) override;
    InterfaceId GetBoundInterface() const override;
    uint16_t GetBoundPort() const override;

    /**
     * Deliver received UDP message to the endpoint's callback.
     * Called by UcxClientManager when +UUSORD URC arrives.
     */
    void DeliverReceivedMessage(System::PacketBufferHandle && msg, const IPPacketInfo * pktInfo);

    /**
     * Enable/disable verbose UDP logging (for reducing log spam during commissioning wait).
     * @param quiet  true = suppress verbose logs, false = full logging
     */
    static void SetQuietUdpLogging(bool quiet);

    /**
     * Drain pending outgoing UDP packets to NORA-W36 via uCxSocketSendTo.
     *
     * Called from the main loop AFTER ProcessQueuedPackets() so that:
     *   1. Incoming packets are delivered to the SDK without blocking on UART.
     *      SDK responses are enqueued by SendMsgImpl in microseconds.
     *   2. After the deliver burst finishes, all queued TX packets are flushed
     *      in one shot — far less interleaving of send and receive on the UART.
     *
     * Bounded per-call: drains at most kTxDrainPerTick packets to keep the
     * main loop responsive (so new RX URCs get a chance to dispatch).
     */
    static void ProcessTxQueue();

private:
    // UDPEndPoint overrides
#if INET_CONFIG_ENABLE_IPV4
    CHIP_ERROR IPv4JoinLeaveMulticastGroupImpl(InterfaceId aInterfaceId, const IPAddress & aAddress, bool join) override;
#endif // INET_CONFIG_ENABLE_IPV4
    CHIP_ERROR IPv6JoinLeaveMulticastGroupImpl(InterfaceId aInterfaceId, const IPAddress & aAddress, bool join) override;
    CHIP_ERROR BindImpl(IPAddressType addressType, const IPAddress & address, uint16_t port, InterfaceId interfaceId) override;
    CHIP_ERROR BindInterfaceImpl(IPAddressType addressType, InterfaceId interfaceId) override;
    CHIP_ERROR ListenImpl() override;
    CHIP_ERROR SendMsgImpl(const IPPacketInfo * pktInfo, System::PacketBufferHandle && msg) override;
    void CloseImpl() override;

    int32_t mSocket = -1;
    uint16_t mBoundPort = 0;
    InterfaceId mBoundIntfId = InterfaceId::Null();
    IPAddressType mAddressType = IPAddressType::kUnknown;
    bool mSocketOwned = true;  // false = shared dual-stack socket, don't close in CloseImpl
};

// CRITICAL: This typedef is required for EndPointManagerImplPool<UDPEndPointImpl>
// Must be defined when using chip_system_config_use_ucx = true
using UDPEndPointImpl = UDPEndPointImplUcx;

} // namespace Inet

namespace DeviceLayer {

/// Lifetime UDP transport counters exposed for the [alive] vitals one-liner.
/// All values are cumulative since boot; queue values reflect the async TX
/// queue inside UDPEndPointImplUcx.cpp (back-pressure decoupling).
struct UcxUdpStats
{
    uint64_t txPackets;      ///< Successful uCxSocketSendTo calls
    uint64_t rxPackets;      ///< Datagrams delivered to the SDK
    uint64_t txBytes;
    uint64_t rxBytes;
    uint32_t txErrors;       ///< Sends that failed after all retries
    uint32_t txQueueDepth;   ///< Current depth of async TX queue
    uint32_t txQueueHighWater;
    uint32_t txQueueOverflows;
};

void UcxUdpStatsSnapshot(UcxUdpStats & out);

} // namespace DeviceLayer
} // namespace chip
