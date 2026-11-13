/*
 * UDPEndPointImplUcx.cpp
 *
 * Pure ucxclient UDP implementation for NORA-W36
 * Uses ucxclient socket API directly (no HybridSocketManager)
 */

#include "UDPEndPointImplUcx.h"
#include "UcxRamTier.h"  // Unified RAM-budget tier (UCX_RAM_TIER_TIGHT/MEDIUM/LARGE)
#include <platform/ucxclient/UcxClientManager.h>
#include <platform/ucxclient/VerboseLogging.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <inet/InetError.h>
#include <inet/InetLayer.h>

#include "u_cx.h"
#include "u_cx_urc.h"
#include "u_cx_socket.h"
#include "u_cx_types.h"
#include "u_cx_at_params.h"  // For uCxStringToIpAddress

#include <cstring>
#include "PlatformAbstraction.h"  // Cross-platform SleepMs(), GetTickMs()

// inet_pton() — POSIX address parser. On Windows it comes through matter/compat/windows/arpa/inet.h
// (winsock2.h); on Linux from the real <arpa/inet.h>.
#include <arpa/inet.h>

namespace chip {
namespace Inet {

// Query NORA-W36 socket last error for enhanced diagnostics
static void LogSocketLastError(uCxHandle_t * ucxHandle, const char * context)
{
    int32_t lastErr = 0;
    int32_t ret = uCxSocketGetLastError(ucxHandle, &lastErr);
    if (ret == 0 && lastErr != 0) {
        ChipLogError(DeviceLayer, "[UDPUcx] %s: socket last error = %d", context, static_cast<int>(lastErr));
    }
}

// NORA-W36 AT+USOSTB maximum payload size (UDP send)
// The firmware rejects sends larger than this with "Host is sending more data than allowed"
static constexpr int32_t kMaxUdpSendBytes = 1460;  // NORA-W36 firmware 3.2.0+ supports full MTU

// Dual-stack socket port registry
// With NORA-W36 LWIP 2.2.0 (V6ONLY=0), a single IPv6 socket handles both IPv4 and IPv6 traffic.
// IPv4 arrives as IPv4-mapped addresses (::ffff:x.x.x.y). When the Matter SDK creates both
// IPv4 and IPv6 endpoints for the same port, the second endpoint reuses the first's socket.
static constexpr size_t kMaxDualStackPorts = 8;
static struct {
    uint16_t port;
    int32_t socket;
} sDualStackRegistry[kMaxDualStackPorts] = {};
static size_t sDualStackCount = 0;

// Global flag to suppress verbose UDP logging during commissioning wait
// Set to true when commissioning window opens, false when pairing starts
bool sQuietUdpLogging = true;

// UDP TX/RX statistics for diagnostics
static uint32_t sTxPacketCount = 0;
static uint32_t sRxPacketCount = 0;
static uint64_t sTxByteCount = 0;
static uint64_t sRxByteCount = 0;
static uint32_t sTxErrorCount = 0;
static uint32_t sTxLargeDupCount = 0;   // duplicate TX of large packets for reliability

// =====================================================================
// LARGE-PACKET RELIABILITY DUPLICATE
// ---------------------------------------------------------------------
// On hostile Wi-Fi paths (iPhone hotspot, congested 2.4 GHz at demos /
// trade shows / training rooms), the single 527-byte CASE Sigma2 frame
// is the most failure-prone packet in the entire commissioning flow.
// When it is dropped, MRP waits 300→600ₒ1200ₒ2400 ms before retrying
// — that is the classic 5 s "CASESession timed out" we see in the log.
//
// Cheap insurance: on every successful send of a packet larger than
// kLargePacketThreshold, transmit ONE additional copy after a short gap.
// The controller's Matter stack dedups by message counter (free on the
// receiver), so the only cost is ~30 ms extra UART time and one extra
// Wi-Fi frame per large send. In return, packet-loss probability for the
// critical handshake frames drops from p to p² (e.g. 20 % → 4 %).
//
// Set to 0 to disable (e.g. for CSA conformance runs that count packets).
// =====================================================================
#ifndef UCX_LARGE_PACKET_DUP_TX
#define UCX_LARGE_PACKET_DUP_TX 1
#endif

// Periodic UDP health snapshot — emitted every kUdpStatsPeriodMs while traffic
// is flowing. Lets us see at-a-glance from the log whether sends/receives are
// happening, whether retries/errors are accumulating, and whether the WiFi
// path is healthy. Reset on every emission so each line is a delta.
static constexpr uint32_t kUdpStatsPeriodMs = 5000;
static uint32_t sStatsWindowStartMs = 0;
static uint32_t sStatsTxPackets   = 0;
static uint32_t sStatsRxPackets   = 0;
static uint32_t sStatsTxBytes     = 0;
static uint32_t sStatsRxBytes     = 0;
static uint32_t sStatsTxLarge     = 0;   // packets > kLargePacketThreshold
static uint32_t sStatsTxRetries   = 0;   // ERROR:36 retries (per attempt)
static uint32_t sStatsTxErrors    = 0;   // sends that failed after all retries
static uint32_t sStatsTxPaceWaits = 0;   // times we slept for large-packet pacing
static uint32_t sStatsTxPaceMsTot = 0;   // total ms slept for pacing in window

// ---------------------------------------------------------------------------
// Lifetime cumulative totals (never reset). Surfaced via UcxUdpStatsSnapshot()
// in the [alive] heartbeat one-liner so simple-console mode users see real
// network progress at a glance without re-enabling the SDK firehose.
// ---------------------------------------------------------------------------
static uint64_t sLifeTxPackets = 0;
static uint64_t sLifeRxPackets = 0;
static uint64_t sLifeTxBytes   = 0;
static uint64_t sLifeRxBytes   = 0;
static uint32_t sLifeTxErrors  = 0;

static void UdpStatsMaybeEmit()
{
    uint32_t now = chip::Platform::GetTickMs();
    if (sStatsWindowStartMs == 0)
    {
        sStatsWindowStartMs = now;
        return;
    }
    uint32_t elapsed = now - sStatsWindowStartMs;
    if (elapsed < kUdpStatsPeriodMs)
        return;
    if ((sStatsTxPackets | sStatsRxPackets | sStatsTxErrors | sStatsTxRetries) == 0)
    {
        // Quiet window — don't spam the log
        sStatsWindowStartMs = now;
        return;
    }
    ChipLogProgress(DeviceLayer,
        "[UDP-STATS] %ums | TX %u pkt (%u B, %u large, %u dup) | RX %u pkt (%u B) | retries=%u errors=%u | pacing %ux total=%ums",
        elapsed,
        sStatsTxPackets, sStatsTxBytes, sStatsTxLarge, sTxLargeDupCount,
        sStatsRxPackets, sStatsRxBytes,
        sStatsTxRetries, sStatsTxErrors,
        sStatsTxPaceWaits, sStatsTxPaceMsTot);
    sStatsWindowStartMs = now;
    sStatsTxPackets = sStatsRxPackets = 0;
    sStatsTxBytes   = sStatsRxBytes   = 0;
    sStatsTxLarge = sStatsTxRetries = sStatsTxErrors = 0;
    sStatsTxPaceWaits = sStatsTxPaceMsTot = 0;
}

// =====================================================================
// ASYNC TX QUEUE — Fix B: decouple SendMsgImpl from UART send path
// =====================================================================
//
// Why:
//   Matter SDK's RX path is:
//       ProcessQueuedPackets() → DeliverReceivedMessage()
//           → SDK processes message
//               → SDK calls SendMsgImpl()  (synchronous send response/ACK)
//                   → AT+USOSTB on the UART: the send waits for the
//                       module's answer. During that wait, NO new packets
//                       are delivered, and the iPad/Apple TV's incoming
//                       retransmits stack up in the URC queue.
//
// Fix:
//   SendMsgImpl now parses the destination, builds a queued TX packet,
//   and enqueues it in microseconds — no UART contention. Then the main
//   loop calls ProcessTxQueue() AFTER ProcessQueuedPackets(), draining
//   all pending TX in one batch. Net effect:
//     • Deliver path stays fast: 1 RX → SDK → enqueue → return (≤1ms)
//     • TX flushes happen as a separate phase, less interleaving with RX
//
// Back-pressure:
//   If the queue is full (overload), SendMsgImpl falls back to a
//   synchronous send so the SDK never sees CHIP_ERROR_NO_MEMORY for a
//   transient burst. Overflow is logged so we can size the queue properly.
//
// Lifetime:
//   Each queued packet captures the NORA-W36 socket FD by value. If the
//   endpoint closes between enqueue and drain, uCxSocketSendTo will fail
//   on the closed FD — we just log and discard. No dangling pointers.
// =====================================================================

namespace {

struct QueuedTxPacket
{
    int32_t          socket;
    uint16_t         destPort;
    uint16_t         dataLen;
    uint32_t         enqueueTickMs;                    // For stale-packet drop + diagnostics
    uSockIpAddress_t destAddr;
    char             destAddrStr[INET6_ADDRSTRLEN];   // For diagnostic log only
    uint8_t          data[kMaxUdpSendBytes];
};

// Capacity sized for worst-case Apple Home commissioning burst:
// ~16 RX requests can each generate 1-2 SDK responses → up to ~32 queued.
// 32 × ~1.5KB ≈ 47KB static. Sized via unified RAM tier (UcxRamTier.h):
// peak observed depth in stress testing was ~10, MRP handles overflow via
// retransmit, so tight tier shrinks to 8 (~12KB) saving ~35KB.
#if defined(UCX_RAM_TIER_TIGHT)
static constexpr size_t   kTxQueueCapacity   = 8;   // ~12KB
#elif defined(UCX_RAM_TIER_MEDIUM)
static constexpr size_t   kTxQueueCapacity   = 32;  // ~47KB — bumped from 16 (May 2026)
                                                    // 16 was filling permanently on iPhone hotspot:
                                                    // SDK retransmits + duplicate ACKs out-paced
                                                    // UART drain → every send hit sync fallback,
                                                    // blocking SendMsgImpl 20-50ms per call.
#else
static constexpr size_t   kTxQueueCapacity   = 64;  // ~94KB — bumped from 32 for desktop dev
#endif
// Max packets drained per ProcessTxQueue() call.
//
// Sized to keep main-loop iteration latency under iPad's MRP retransmit
// budget (~300ms). Each AT+USOSTB takes 18-50ms over UART. With drain=6,
// worst-case ProcessTxQueue blocks ~108-300ms — at the edge but still
// inside iPad's ~300ms MRP active interval, and only when the queue is
// genuinely backlogged (otherwise drain stops early when queue empties).
//
// History:
//   drain=16 — held loop 240-400ms, caused iPad retransmit storms.
//   drain=2  — too conservative; on iPhone hotspot the SDK produced
//              TX faster than 2/tick could ship → queue stuck at capacity
//              → every SendMsgImpl fell back to sync (20-50ms blocking),
//              defeating the whole point of the queue.
//   drain=6  — current. Matches steady-state UART throughput
//              (~33 sends/s) at typical main-loop cadence (~5-10 ticks/s),
//              so queue can actually catch up after a burst.
static constexpr size_t   kTxDrainPerTick    = 6;

static QueuedTxPacket     sTxQueue[kTxQueueCapacity];
static size_t             sTxQueueHead       = 0;   // write index
static size_t             sTxQueueTail       = 0;   // read index
static size_t             sTxQueueCount      = 0;
static chip::Platform::Mutex sTxQueueMutex;
static uint32_t           sTxQueueHighWater  = 0;
static uint32_t           sTxQueueOverflows  = 0;
static uint32_t           sTxQueueAsyncSends = 0;
static uint32_t           sTxQueueSyncFallbacks = 0;
// Stuck-queue diagnostic: tracks last time ProcessTxQueue actually drained a
// packet. If the queue stays non-empty for too long without draining, the
// event loop is blocked (HandleEvents, BLE callback, etc.) — surface it so we
// don't repeat the May 2026 "42-second TX silence during iPad commissioning"
// mystery. See ProcessTxQueue() for full rationale.
static uint32_t           sTxLastDrainTickMs    = 0;
static uint32_t           sTxStuckLastLogTickMs = 0;
static uint32_t           sTxStaleDrops         = 0;

// Forward declaration — synchronous send used by both the async drain path
// and the back-pressure fallback inside SendMsgImpl. File-scope static so the
// definition (also static, outside this anonymous namespace) links correctly.
}  // anonymous namespace

static CHIP_ERROR DoSyncSend(uCxHandle_t * ucxHandle,
                             int32_t socket,
                             uSockIpAddress_t destAddr,
                             uint16_t destPort,
                             const char * destAddrStr,
                             const uint8_t * data,
                             int32_t dataLen);

namespace {

// Try to enqueue. Returns true on success, false if queue is full.
static bool TryEnqueueTx(int32_t socket,
                         const uSockIpAddress_t & destAddr,
                         uint16_t destPort,
                         const char * destAddrStr,
                         const uint8_t * data,
                         int32_t dataLen)
{
    size_t depthAfter = 0;
    {
        chip::Platform::ScopedLock lock(sTxQueueMutex);
        if (sTxQueueCount >= kTxQueueCapacity)
        {
            sTxQueueOverflows++;
            return false;
        }
        QueuedTxPacket & slot = sTxQueue[sTxQueueHead];
        slot.socket        = socket;
        slot.destPort      = destPort;
        slot.dataLen       = static_cast<uint16_t>(dataLen);
        slot.enqueueTickMs = chip::Platform::GetTickMs();
        slot.destAddr      = destAddr;
        // Copy address string for logging (bounded, safe).
        size_t alen = strnlen(destAddrStr, INET6_ADDRSTRLEN - 1);
        memcpy(slot.destAddrStr, destAddrStr, alen);
        slot.destAddrStr[alen] = '\0';
        memcpy(slot.data, data, static_cast<size_t>(dataLen));
        sTxQueueHead = (sTxQueueHead + 1) % kTxQueueCapacity;
        sTxQueueCount++;
        if (sTxQueueCount > sTxQueueHighWater)
            sTxQueueHighWater = static_cast<uint32_t>(sTxQueueCount);
        depthAfter = sTxQueueCount;
    }
    // u-connectXpressApp: the Matter thread sends it at once instead of after its wait for the next timer
    chip::System::UcxWake();
    // Surface every enqueue. ChipLogDetail keeps it visible in normal builds
    // but skippable via log level. Critical for diagnosing "SDK emitted <<<
    // but no AT+USOSTB on UART" — pre-May-2026 we had no signal at all.
    ChipLogDetail(DeviceLayer, "[UDP-ENQ] %d bytes -> %s:%u (depth=%u/%u)",
                  dataLen, destAddrStr, destPort,
                  static_cast<unsigned>(depthAfter),
                  static_cast<unsigned>(kTxQueueCapacity));
    return true;
}

// Pop one packet for sending. Returns false if queue empty.
static bool TryDequeueTx(QueuedTxPacket & out)
{
    chip::Platform::ScopedLock lock(sTxQueueMutex);
    if (sTxQueueCount == 0) return false;
    out = sTxQueue[sTxQueueTail];
    sTxQueueTail = (sTxQueueTail + 1) % kTxQueueCapacity;
    sTxQueueCount--;
    return true;
}

// Read queue depth + enqueue tick of the oldest packet WITHOUT removing it.
// Used by the stuck-queue diagnostic in ProcessTxQueue. Returns depth; sets
// *outOldestTickMs only if depth > 0.
static size_t PeekOldestEnqueueTick(uint32_t * outOldestTickMs)
{
    chip::Platform::ScopedLock lock(sTxQueueMutex);
    if (sTxQueueCount == 0) return 0;
    if (outOldestTickMs != nullptr)
        *outOldestTickMs = sTxQueue[sTxQueueTail].enqueueTickMs;
    return sTxQueueCount;
}

}  // anonymous namespace

// Constructor with logging to verify our implementation is being used
UDPEndPointImplUcx::UDPEndPointImplUcx(EndPointManager<UDPEndPoint> & endPointManager) :
    UDPEndPoint(endPointManager), mSocket(-1), mBoundPort(0)
{
    // Verbose logging disabled for commissioning performance
}

CHIP_ERROR UDPEndPointImplUcx::BindImpl(IPAddressType addressType, const IPAddress & address, uint16_t port,
                                        InterfaceId interfaceId)
{
    // Get ucxclient handle
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
    
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] ucxHandle is NULL!");
        return CHIP_ERROR_INTERNAL;
    }

    mAddressType = addressType;

    // NORA-W36 LWIP 2.2.0 dual-stack: A single IPv6 socket (V6ONLY=0) receives both
    // IPv4 (as ::ffff:x.x.x.y) and IPv6 traffic. Binding both address types to the
    // same port would fail. Check if a socket already exists for this port.
    for (size_t i = 0; i < sDualStackCount; i++)
    {
        if (sDualStackRegistry[i].port == port)
        {
            // Reuse existing dual-stack socket
            mSocket = sDualStackRegistry[i].socket;
            mBoundPort = port;
            mState = State::kBound;
            mSocketOwned = false;
            ChipLogProgress(DeviceLayer, "[UDPUcx] Reusing dual-stack socket %d for port %u (%s)",
                           mSocket, port, (addressType == IPAddressType::kIPv6) ? "IPv6" : "IPv4");
            return CHIP_NO_ERROR;
        }
    }

    // Always create IPv6 socket — dual-stack handles IPv4 via V6ONLY=0 (u-connectXpressApp: in buffered read mode)
    int32_t result = ucx.CreateUdpSocket(&mSocket);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] FATAL: uCxSocketCreate2() failed with error: %d", result);
        LogSocketLastError(ucxHandle, "SocketCreate2");
        return CHIP_ERROR_INTERNAL;
    }

    // Bind to port
    result = uCxSocketBind(ucxHandle, mSocket, port);
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] FATAL: uCxSocketBind() failed with error: %d", result);
        LogSocketLastError(ucxHandle, "SocketBind");
        uCxSocketClose(ucxHandle, mSocket);
        return CHIP_ERROR_INTERNAL;
    }

    // Register in dual-stack port registry
    if (sDualStackCount < kMaxDualStackPorts)
    {
        sDualStackRegistry[sDualStackCount].port = port;
        sDualStackRegistry[sDualStackCount].socket = mSocket;
        sDualStackCount++;
    }

    // URCs for this socket are handled on this thread after the bind, so the endpoint is registered in time
    mBoundPort = port;
    mSocketOwned = true;
    mState = State::kBound;
    UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] Registering endpoint with UcxClientManager...");
    DeviceLayer::UcxClientManager::GetInstance().RegisterUdpEndpoint(mSocket, this);
    
    // Important: always log successful bind (not verbose)
    ChipLogProgress(DeviceLayer, "[UDPUcx] Socket %d bound to port %u (IPv6 dual-stack)", mSocket, port);

    return CHIP_NO_ERROR;
}

CHIP_ERROR UDPEndPointImplUcx::BindInterfaceImpl(IPAddressType addressType, InterfaceId interfaceId)
{
    UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] BindInterfaceImpl called");
    // NORA-W36 has single Wi-Fi interface, no-op
    return CHIP_NO_ERROR;
}

InterfaceId UDPEndPointImplUcx::GetBoundInterface() const
{
    // Return the same stable virtual interface ID we stamp on inbound packets
    // (see UcxClientManager::ProcessQueuedPackets / kUcxVirtualWifiInterfaceId).
    // Keeps SDK bookkeeping consistent for IPv6 link-local scoping.
    return InterfaceId(static_cast<InterfaceId::PlatformType>(1));
}

uint16_t UDPEndPointImplUcx::GetBoundPort() const
{
    return mBoundPort;
}

CHIP_ERROR UDPEndPointImplUcx::ListenImpl()
{
    UCX_LOG_VERBOSE(DeviceLayer, "[UcxUDP-LISTEN] Socket: %d, Port: %u", mSocket, mBoundPort);
    
    // UDP is connectionless, just set state
    mState = State::kListening;
    
    return CHIP_NO_ERROR;
}

CHIP_ERROR UDPEndPointImplUcx::SendMsgImpl(const IPPacketInfo * pktInfo, System::PacketBufferHandle && msg)
{
    char destAddrStr[INET6_ADDRSTRLEN];
    pktInfo->DestAddress.ToString(destAddrStr, sizeof(destAddrStr));

    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
    
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] ERROR: ucxHandle is NULL!");
        return CHIP_ERROR_INTERNAL;
    }

    // NORA-W36 dual-stack (LWIP 2.2.0, V6ONLY=0): All sockets are IPv6.
    // IPv4 destinations must be sent as IPv4-mapped IPv6: ::ffff:x.x.x.y
    // NORA-W36 requires FULLY EXPANDED IPv6 addresses (no :: compression).
    char expandedAddrStr[40];  // Max IPv6: "xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx"
    
    if (pktInfo->DestAddress.IsIPv4())
    {
        // Convert IPv4 to IPv4-mapped IPv6 format for dual-stack socket
        // e.g. 192.168.1.101 → 0000:0000:0000:0000:0000:ffff:c0a8:0165
        struct in_addr ipv4;
        if (inet_pton(AF_INET, destAddrStr, &ipv4) != 1)
        {
            ChipLogError(DeviceLayer, "[UDPUcx] ERROR: Failed to parse IPv4 address: %s", destAddrStr);
            return CHIP_ERROR_INVALID_ADDRESS;
        }
        uint32_t ip = ntohl(ipv4.s_addr);
        snprintf(expandedAddrStr, sizeof(expandedAddrStr),
                 "0000:0000:0000:0000:0000:ffff:%04x:%04x",
                 (ip >> 16) & 0xFFFF, ip & 0xFFFF);
    }
    else
    {
        // Pure IPv6 — expand compressed format to full
        struct in6_addr ipv6;
        if (inet_pton(AF_INET6, destAddrStr, &ipv6) != 1)
        {
            ChipLogError(DeviceLayer, "[UDPUcx] ERROR: Failed to parse IPv6 address: %s", destAddrStr);
            return CHIP_ERROR_INVALID_ADDRESS;
        }
        snprintf(expandedAddrStr, sizeof(expandedAddrStr),
                 "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                 ntohs(((uint16_t*)&ipv6)[0]),
                 ntohs(((uint16_t*)&ipv6)[1]),
                 ntohs(((uint16_t*)&ipv6)[2]),
                 ntohs(((uint16_t*)&ipv6)[3]),
                 ntohs(((uint16_t*)&ipv6)[4]),
                 ntohs(((uint16_t*)&ipv6)[5]),
                 ntohs(((uint16_t*)&ipv6)[6]),
                 ntohs(((uint16_t*)&ipv6)[7]));
    }

    // Convert expanded address to ucxclient format
    uSockIpAddress_t destAddr;
    int32_t parseResult = uCxStringToIpAddress(expandedAddrStr, &destAddr);
    
    if (parseResult != 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] ERROR: Failed to parse IP address '%s': error %d", expandedAddrStr, parseResult);
        return CHIP_ERROR_INVALID_ADDRESS;
    }

    // Check packet size against NORA-W36 AT+USOSTB limit
    int32_t dataLen = static_cast<int32_t>(msg->DataLength());
    if (dataLen > kMaxUdpSendBytes)
    {
        ChipLogError(DeviceLayer, "[UDP-TX] Packet too large: %d bytes (max %d) to %s:%u",
                    dataLen, kMaxUdpSendBytes, destAddrStr, pktInfo->DestPort);
        return CHIP_ERROR_MESSAGE_TOO_LONG;
    }

    // ============================================================
    // Fix B: ENQUEUE for async drain by main loop, return immediately,
    // instead of waiting for the UART here — see TX queue rationale at the top.
    // ============================================================
    if (TryEnqueueTx(mSocket, destAddr, pktInfo->DestPort, destAddrStr,
                     static_cast<const uint8_t *>(msg->Start()), dataLen))
    {
        sTxQueueAsyncSends++;
        return CHIP_NO_ERROR;
    }

    // ============================================================
    // Back-pressure: queue is full. Old behaviour was to sync-send the
    // NEW packet immediately, but that left the queue at capacity, so
    // the next SendMsgImpl call ALSO found it full → sync-sent again →
    // queue never drained → every send was sync (20-50ms blocking) →
    // ProcessUrcs starved → ACKs late → iPad retransmits → more sends
    // → permanent feedback loop. (Observed: overflows incremented by 1
    // on every TX during a 3-min iPhone-hotspot session, May 2026.)
    //
    // New behaviour: drain the OLDEST queued packet synchronously to
    // make room, then enqueue the new packet. Same total UART time as
    // before (one sync send), but:
    //   - FIFO ordering preserved (older packet ships first).
    //   - Queue depth drops to capacity-1 → next SendMsgImpl can enqueue
    //     normally instead of falling back again.
    //   - The async drain in ProcessTxQueue continues to do its job.
    // ============================================================
    QueuedTxPacket head;
    if (TryDequeueTx(head))
    {
        // Ship the oldest packet sync; ignore its return — already logged
        // inside DoSyncSend. SDK has already accepted it (we returned
        // CHIP_NO_ERROR when it was enqueued); MRP handles any genuine loss.
        (void) DoSyncSend(ucxHandle, head.socket, head.destAddr, head.destPort,
                          head.destAddrStr, head.data,
                          static_cast<int32_t>(head.dataLen));

        // Now enqueue the new packet. Should succeed (we just freed a slot).
        if (TryEnqueueTx(mSocket, destAddr, pktInfo->DestPort, destAddrStr,
                         static_cast<const uint8_t *>(msg->Start()), dataLen))
        {
            sTxQueueAsyncSends++;
            return CHIP_NO_ERROR;
        }
        // Race with ProcessTxQueue (other thread refilled the slot) — fall
        // through to the sync path below for the new packet.
    }

    // Last resort: queue still full (would only happen if dequeue raced) or
    // dequeue failed. Sync-send the new packet so SDK doesn't see a failure.
    sTxQueueSyncFallbacks++;
    ChipLogError(DeviceLayer,
        "[UDP-TX] TX queue FULL (capacity=%u, overflows=%u) — falling back to sync send",
        static_cast<unsigned>(kTxQueueCapacity),
        sTxQueueOverflows);
    return DoSyncSend(ucxHandle, mSocket, destAddr, pktInfo->DestPort, destAddrStr,
                      static_cast<const uint8_t *>(msg->Start()), dataLen);
}

// =====================================================================
// Drain pending TX packets — called from main loop after RX delivery.
// Bounded to kTxDrainPerTick so we don't hog the loop on bursts.
//
// Also implements two safety nets added May 2026 after the iPhone-hotspot
// + iPad demo failure where a 42-second event-loop stall caused the TX
// queue to drain stale fe80 packets to a destination the iPad had already
// abandoned, breaking the second commissioning attempt:
//   (1) STALE-PACKET DROP: any packet that has sat in the queue > 2000ms
//       is discarded with a [UDP-TX] DROP STALE log. Matter MRP at the
//       protocol layer will retransmit if the loss matters; sending a
//       2-second-old packet is worse than not sending it (controller
//       may have moved to a different IP / session).
//   (2) STUCK-QUEUE DIAGNOSTIC: if the queue is non-empty and we haven't
//       successfully drained anything for > 500ms, emit [TX-STUCK] with
//       depth + oldest-packet-age. Surfaces event-loop blocks (long
//       HandleEvents, BLE callbacks, WaitForEvents stuck on huge timer)
//       that would otherwise be invisible.
// =====================================================================
void UDPEndPointImplUcx::ProcessTxQueue()
{
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
    if (ucxHandle == nullptr) return;

    static constexpr uint32_t kStaleThresholdMs   = 2000;
    static constexpr uint32_t kStuckThresholdMs   = 500;
    static constexpr uint32_t kStuckLogPeriodMs   = 500;
    const uint32_t nowMs = chip::Platform::GetTickMs();

    // ---------------------------------------------------------------
    // [TX-PROC] diagnostic — rate-limited to ~1Hz. Confirms ProcessTxQueue
    // is being called AND surfaces the queue depth as seen from inside
    // sTxQueueMutex (the drain-side view). If this prints depth=N but
    // [UDP-TX] never fires, then TryDequeueTx vs TryEnqueueTx disagree
    // on the queue state (mutex/instance bug).
    // ---------------------------------------------------------------
    {
        chip::Platform::ScopedLock lock(sTxQueueMutex);
        static uint32_t sTxProcLastLogMs = 0;
        // u-connectXpressApp: only with packets waiting, an idle node printed this every second
        if (sTxQueueCount > 0 && (sTxProcLastLogMs == 0 || (nowMs - sTxProcLastLogMs) >= 1000))
        {
            ChipLogDetail(DeviceLayer,
                "[TX-PROC] depth=%u head=%u tail=%u lastDrainMs=%u nowMs=%u",
                static_cast<unsigned>(sTxQueueCount),
                static_cast<unsigned>(sTxQueueHead),
                static_cast<unsigned>(sTxQueueTail),
                static_cast<unsigned>(sTxLastDrainTickMs),
                static_cast<unsigned>(nowMs));
            sTxProcLastLogMs = nowMs;
        }
    }

    bool didDrain = false;
    uint32_t loopNowMs = nowMs;
    for (size_t i = 0; i < kTxDrainPerTick; i++)
    {
        QueuedTxPacket pkt;
        if (!TryDequeueTx(pkt))
            break;

        // Stale-packet guard. Drop if older than kStaleThresholdMs — MRP will
        // retransmit anything that genuinely matters with a fresh dest.
        //
        // CRITICAL: Refresh the clock per iteration. The outer `nowMs` is
        // captured ONCE at function entry, but each DoSyncSend below blocks
        // for ~60–120 ms on AT+USOSTB. Meanwhile MatterTask keeps enqueuing
        // (different mutex), so by iteration 2 the freshly-dequeued packet's
        // enqueueTickMs can be GREATER than the stale outer nowMs, causing
        // `nowMs - enqueueTickMs` to underflow to ~UINT32_MAX. That false
        // positive then drops perfectly valid packets as "stale" — which is
        // exactly the bug the May 2026 iPad commissioning logs showed
        // (depth HW=5/32 but 100+ stale-drops, all reporting ages of
        // ~4 294 967 100 ms = 2^32 − ~150). Use signed comparison so any
        // residual ±1-tick scheduling jitter can't underflow either.
        int32_t ageSignedMs = static_cast<int32_t>(loopNowMs - pkt.enqueueTickMs);
        if (ageSignedMs > static_cast<int32_t>(kStaleThresholdMs))
        {
            sTxStaleDrops++;
            ChipLogError(DeviceLayer,
                "[UDP-TX] DROP STALE: %u bytes -> %s:%u (queued %dms ago, total dropped: %u)",
                static_cast<unsigned>(pkt.dataLen), pkt.destAddrStr, pkt.destPort,
                ageSignedMs, sTxStaleDrops);
            didDrain = true;  // We did make queue progress — don't fire stuck warning.
            continue;
        }

        // Discard return — error path already logged inside DoSyncSend.
        // SDK considers the send done (CHIP_NO_ERROR was returned by
        // SendMsgImpl). MRP at the protocol layer handles retransmits
        // for any packet that the controller doesn't ACK.
        (void) DoSyncSend(ucxHandle, pkt.socket, pkt.destAddr, pkt.destPort,
                          pkt.destAddrStr, pkt.data, static_cast<int32_t>(pkt.dataLen));
        didDrain = true;

        // DoSyncSend just blocked on UART for ~60-120 ms; refresh the clock so
        // the next iteration's stale check sees the true current time, not the
        // pre-blocking snapshot. Without this, fresh enqueues during the
        // AT+USOSTB look like they're "from the future" (see comment above).
        loopNowMs = chip::Platform::GetTickMs();
    }

    if (didDrain)
    {
        sTxLastDrainTickMs = loopNowMs;
    }
    else
    {
        // No drain this tick. If the queue is non-empty and has been stuck
        // for > kStuckThresholdMs, warn at most once per kStuckLogPeriodMs.
        uint32_t oldestTickMs = 0;
        size_t depth = PeekOldestEnqueueTick(&oldestTickMs);
        if (depth > 0)
        {
            // Compute "time since last successful drain". If we've never
            // drained, fall back to oldest-packet age (initial bootstrap).
            uint32_t sinceDrainMs = (sTxLastDrainTickMs != 0)
                ? (nowMs - sTxLastDrainTickMs)
                : (nowMs - oldestTickMs);
            uint32_t oldestAgeMs = nowMs - oldestTickMs;
            if (sinceDrainMs > kStuckThresholdMs &&
                (sTxStuckLastLogTickMs == 0 ||
                 (nowMs - sTxStuckLastLogTickMs) >= kStuckLogPeriodMs))
            {
                ChipLogError(DeviceLayer,
                    "[TX-STUCK] depth=%u/%u oldest_age=%ums since_drain=%ums "
                    "(event loop blocked? HandleEvents stall?)",
                    static_cast<unsigned>(depth),
                    static_cast<unsigned>(kTxQueueCapacity),
                    static_cast<unsigned>(oldestAgeMs),
                    static_cast<unsigned>(sinceDrainMs));
                sTxStuckLastLogTickMs = nowMs;
            }
        }
    }

    // Periodic TXQ stats — surface at the same cadence as UDP-STATS so we can
    // confirm Fix B is working: high async count + low sync fallbacks = good.
    static uint32_t sTxqStatsLastMs = 0;
    if (sTxqStatsLastMs == 0) { sTxqStatsLastMs = nowMs; return; }
    if ((nowMs - sTxqStatsLastMs) < kUdpStatsPeriodMs) return;
    if ((sTxQueueAsyncSends | sTxQueueSyncFallbacks | sTxQueueOverflows | sTxStaleDrops) != 0)
    {
        ChipLogProgress(DeviceLayer,
            "[TXQ-STATS] %ums | async=%u sync-fallback=%u overflows=%u stale-drops=%u | depth HW=%u/%u",
            (nowMs - sTxqStatsLastMs),
            sTxQueueAsyncSends, sTxQueueSyncFallbacks, sTxQueueOverflows, sTxStaleDrops,
            sTxQueueHighWater, static_cast<unsigned>(kTxQueueCapacity));
    }
    sTxqStatsLastMs = nowMs;
    // Window-reset async/sync; keep cumulative HighWater + Overflows for visibility.
    sTxQueueAsyncSends = 0;
    sTxQueueSyncFallbacks = 0;
}

// =====================================================================
// Synchronous send to NORA-W36: pacing + retry + stats + logging.
// Used by ProcessTxQueue (normal path) and SendMsgImpl back-pressure
// fallback.
// =====================================================================
static CHIP_ERROR DoSyncSend(uCxHandle_t * ucxHandle,
                             int32_t socket,
                             uSockIpAddress_t destAddr,
                             uint16_t destPort,
                             const char * destAddrStr,
                             const uint8_t * data,
                             int32_t dataLen)
{
    // Inter-packet pacing for NORA-W36 — TARGETED LARGE-PACKET ONLY
    //
    // Problem: After a large packet (e.g. 527-byte CASE Sigma2 or 1062-byte
    // ReportData chunk), NORA-W36 still has the frame queued for WiFi TX. If
    // the next outgoing packet is ALSO large and arrives at AT+USOSTB before
    // the previous frame has cleared the WiFi MAC, the host-side buffer can
    // congest (ERROR:36) or — worse — the WiFi NIC may drop the new frame
    // silently because its TX descriptor ring is full.
    //
    // Solution: pace ONLY when both the previous AND the current packet are
    // "large" (> kLargePacketThreshold). This is the only case where back-to-
    // back AT+USOSTB risks NORA-W36 buffer pressure. Small packets (StandaloneAck
    // 26B/34B, StatusResponse 42B, InvokeResponse <200B) ALWAYS bypass — they
    // are MRP-critical and any added latency triggers controller retransmits.
    static constexpr int32_t kLargePacketThreshold = 400;
    static constexpr int32_t kPaceMaxMs           = 25;
    static constexpr int32_t kPacePerByteDivisor  = 100;  // ~10ms per 1KB
    static uint32_t sLastSendTickMs   = 0;
    static int32_t  sLastSendSizeB    = 0;

    // Retry constants for transient NORA-W36 send failures (ERROR:36 = LWIP congestion)
    static constexpr int32_t kMaxSendRetries = 3;
    static constexpr int32_t kRetryDelayMs = 25;
    static uint32_t sTotalRetries = 0;

    // Apply pacing ONLY for large-after-large back-to-back sends.
    if (dataLen > kLargePacketThreshold && sLastSendSizeB > kLargePacketThreshold)
    {
        uint32_t nowMs = chip::Platform::GetTickMs();
        int32_t  elapsedMs = static_cast<int32_t>(nowMs - sLastSendTickMs);
        int32_t  requiredMs = sLastSendSizeB / kPacePerByteDivisor;
        if (requiredMs > kPaceMaxMs) requiredMs = kPaceMaxMs;
        if (elapsedMs >= 0 && elapsedMs < requiredMs)
        {
            int32_t waitMs = requiredMs - elapsedMs;
            sStatsTxPaceWaits++;
            sStatsTxPaceMsTot += static_cast<uint32_t>(waitMs);
            chip::Platform::SleepMs(static_cast<uint32_t>(waitMs));
        }
    }

    // Send via ucxclient with retry on transient NORA-W36 errors.
    int32_t result = -1;
    uint32_t sendStartMs = 0;
    for (int32_t attempt = 0; attempt <= kMaxSendRetries; attempt++)
    {
        if (attempt > 0)
        {
            sTotalRetries++;
            sStatsTxRetries++;
            ChipLogProgress(DeviceLayer, "[UDP-TX] Retry %d/%d after %dms: %d bytes to %s:%u (prev err=%d, total retries: %u)",
                           attempt, kMaxSendRetries, kRetryDelayMs, dataLen, destAddrStr, destPort,
                           result, sTotalRetries);
            chip::Platform::SleepMs(kRetryDelayMs);
        }

        sendStartMs = chip::Platform::GetTickMs();
        result = uCxSocketSendTo(ucxHandle, socket, &destAddr, destPort,
                                 data, dataLen);

        if (result >= 0)
            break;  // Success
    }
    const uint32_t sendTookMs = chip::Platform::GetTickMs() - sendStartMs;

    sTxPacketCount++;

    if (result < 0)
    {
        sTxErrorCount++;
        sStatsTxErrors++;
        sLifeTxErrors++;
        ChipLogError(DeviceLayer, "[UDP-TX] SEND FAILED #%u: %d bytes to %s:%u sock=%d err=%d after %d retries (total errors: %u)",
                    sTxPacketCount, dataLen, destAddrStr, destPort, socket, result,
                    kMaxSendRetries, sTxErrorCount);
        LogSocketLastError(ucxHandle, "SocketSendTo");
        UdpStatsMaybeEmit();
        return CHIP_ERROR_SENDING_BLOCKED;
    }

    sTxByteCount += static_cast<uint64_t>(dataLen);
    sLastSendTickMs = chip::Platform::GetTickMs();
    sLastSendSizeB  = dataLen;
    sStatsTxPackets++;
    sStatsTxBytes += static_cast<uint32_t>(dataLen);
    sLifeTxPackets++;
    sLifeTxBytes += static_cast<uint64_t>(dataLen);
    if (dataLen > kLargePacketThreshold) sStatsTxLarge++;

#if UCX_LARGE_PACKET_DUP_TX
    // Reliability duplicate for large packets (Sigma2/Sigma3/large ReportData).
    // See the big comment block at the top of this file. Failure of the
    // duplicate is non-fatal — the original send already succeeded.
    // u-connectXpressApp: only where it is the cheap insurance that comment counts on. At 115200 baud a 1.2 kB
    // report takes 110 ms on the UART; its copy kept the AT client from the app's own commands for as long
    // again, and the peer's acknowledgement waited behind it. MRP repeats a lost message there.
    static constexpr uint32_t kDupMaxSendMs = 40;
    if (dataLen > kLargePacketThreshold && sendTookMs <= kDupMaxSendMs)
    {
        // Brief gap so NORA-W36's WiFi MAC has time to push the first frame
        // out of its TX descriptor ring before the duplicate hits it.
        chip::Platform::SleepMs(8);

        int32_t dupResult = uCxSocketSendTo(ucxHandle, socket, &destAddr, destPort,
                                            data, dataLen);

        if (dupResult >= 0)
        {
            sTxLargeDupCount++;
        }
        else
        {
            ChipLogDetail(DeviceLayer,
                "[UDP-TX] Large-packet duplicate failed err=%d (original send OK)",
                dupResult);
        }
    }
#endif // UCX_LARGE_PACKET_DUP_TX

    // Successful UDP send proves NORA-W36 is alive — touch activity tracker so
    // heartbeat watchdog doesn't fire an AT command mid-burst.
    DeviceLayer::UcxClientManager::GetInstance().TouchAtActivity();

    // Always log sends for operational diagnostics (critical for debugging MRP retransmissions)
    ChipLogProgress(DeviceLayer, "[UDP-TX] #%u: %d bytes -> %s:%u (sock=%d, total: %llu bytes)",
                   sTxPacketCount, dataLen, destAddrStr, destPort, socket,
                   static_cast<unsigned long long>(sTxByteCount));

    UdpStatsMaybeEmit();
    return CHIP_NO_ERROR;
}

void UDPEndPointImplUcx::CloseImpl()
{
    if (mSocket >= 0)
    {
        if (!mSocketOwned)
        {
            // Shared dual-stack socket — don't close, just detach
            ChipLogProgress(DeviceLayer, "[UDPUcx] Detaching from shared dual-stack socket %d (port %u)", mSocket, mBoundPort);
            mSocket = -1;
        }
        else
        {
            UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] Closing socket %d", mSocket);
            
            // Unregister from receive callbacks BEFORE closing socket
            DeviceLayer::UcxClientManager::GetInstance().UnregisterUdpEndpoint(mSocket);
            
            auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
            uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
            
            if (ucxHandle != nullptr)
            {
                uCxSocketClose(ucxHandle, mSocket);
            }
            
            // Remove from dual-stack registry
            for (size_t i = 0; i < sDualStackCount; i++)
            {
                if (sDualStackRegistry[i].socket == mSocket)
                {
                    // Shift remaining entries down
                    for (size_t j = i; j + 1 < sDualStackCount; j++)
                    {
                        sDualStackRegistry[j] = sDualStackRegistry[j + 1];
                    }
                    sDualStackCount--;
                    break;
                }
            }
            
            mSocket = -1;
        }
    }

    mState = State::kClosed;
}

CHIP_ERROR UDPEndPointImplUcx::IPv4JoinLeaveMulticastGroupImpl(InterfaceId interfaceId, const IPAddress & address, bool join)
{
    char addrStr[INET6_ADDRSTRLEN];
    address.ToString(addrStr, sizeof(addrStr));
    
    UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] IPv4 Multicast %s: %s socket=%d", join ? "JOIN" : "LEAVE", addrStr, mSocket);
    
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
    
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] ERROR: ucxHandle is NULL!");
        return CHIP_ERROR_INTERNAL;
    }
    
    // ucxclient 3.4.0 API: single function with action enum + uSockIpAddress_t.
    uSockIpAddress_t mcastIp;
    if (uCxStringToIpAddress(addrStr, &mcastIp) < 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] Failed to parse IPv4 multicast address: %s", addrStr);
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    int32_t result = uCxSocketMulticastGroup(ucxHandle, mSocket,
        join ? U_SOCKET_GROUP_ACTION_JOIN : U_SOCKET_GROUP_ACTION_LEAVE,
        &mcastIp);
    
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] Multicast %s failed: %s (error=%d)", join ? "join" : "leave", addrStr, result);
        return CHIP_ERROR_INTERNAL;
    }
    
    ChipLogProgress(DeviceLayer, "[UDPUcx] Multicast %s: %s", join ? "joined" : "left", addrStr);
    return CHIP_NO_ERROR;
}

CHIP_ERROR UDPEndPointImplUcx::IPv6JoinLeaveMulticastGroupImpl(InterfaceId interfaceId, const IPAddress & address, bool join)
{
    char addrStr[INET6_ADDRSTRLEN];
    address.ToString(addrStr, sizeof(addrStr));
    
    // NORA-W36 AT command requires FULLY EXPANDED IPv6 addresses (no :: compression)
    // Convert "ff02::fb" → "ff02:0000:0000:0000:0000:0000:0000:00fb"
    char expandedAddr[40];  // Max IPv6 format: "xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx"
    struct in6_addr ipv6;
    
    // Parse compressed IPv6 address
    if (inet_pton(AF_INET6, addrStr, &ipv6) != 1)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] Failed to parse IPv6 address: %s", addrStr);
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    // Expand to full format (8 groups of 4 hex digits)
    snprintf(expandedAddr, sizeof(expandedAddr),
             "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
             ntohs(((uint16_t*)&ipv6)[0]),
             ntohs(((uint16_t*)&ipv6)[1]),
             ntohs(((uint16_t*)&ipv6)[2]),
             ntohs(((uint16_t*)&ipv6)[3]),
             ntohs(((uint16_t*)&ipv6)[4]),
             ntohs(((uint16_t*)&ipv6)[5]),
             ntohs(((uint16_t*)&ipv6)[6]),
             ntohs(((uint16_t*)&ipv6)[7]));
    
    UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] IPv6 Multicast %s: %s socket=%d", join ? "JOIN" : "LEAVE", addrStr, mSocket);
    
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uCxHandle_t * ucxHandle = ucx.GetUcxHandle();
    
    if (ucxHandle == nullptr)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] ERROR: ucxHandle is NULL!");
        return CHIP_ERROR_INTERNAL;
    }
    
    // Retry logic: mDNS port 5353 can flood with URCs during multicast join,
    // causing AT command timeouts. Retry up to 3 times with increasing delays.
    static constexpr int kMaxRetries = 3;
    static constexpr int kRetryDelayMs[] = { 100, 500, 1000 };
    
    int32_t result = -1;
    for (int attempt = 0; attempt < kMaxRetries; attempt++)
    {
        if (attempt > 0)
        {
            UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] Retry %d/%d after %dms delay...", 
                           attempt + 1, kMaxRetries, kRetryDelayMs[attempt - 1]);
            // Brief delay to let URC flood settle.
            // NOTE: Do NOT call ucx.ProcessUrcs() here. This function may be called
            // from a SDK send/callback path (e.g. during commissioning when a new
            // session triggers a multicast join). Re-entering ProcessUrcs from a
            // send path can recursively deliver received messages back into the SDK
            // and corrupt the MRP retransmission table — historic SIGABRT class.
            // The background RX thread keeps reading the UART regardless, and any
            // queued URCs will be dispatched on the next main-loop ProcessUrcs tick.
            chip::Platform::SleepMs(static_cast<uint32_t>(kRetryDelayMs[attempt - 1]));
        }
        
        // ucxclient 3.4.0 API: single function with action enum + uSockIpAddress_t.
        uSockIpAddress_t mcastIp;
        if (uCxStringToIpAddress(expandedAddr, &mcastIp) < 0)
        {
            ChipLogError(DeviceLayer, "[UDPUcx] Failed to parse IPv6 multicast address: %s", expandedAddr);
            return CHIP_ERROR_INVALID_ARGUMENT;
        }

        result = uCxSocketMulticastGroup(ucxHandle, mSocket,
            join ? U_SOCKET_GROUP_ACTION_JOIN : U_SOCKET_GROUP_ACTION_LEAVE,
            &mcastIp);
        
        if (result >= 0)
        {
            // Success!
            break;
        }
        
        ChipLogError(DeviceLayer, "[UDPUcx] Multicast %s attempt %d failed (error=%d)", 
                    join ? "join" : "leave", attempt + 1, result);
    }
    
    if (result < 0)
    {
        ChipLogError(DeviceLayer, "[UDPUcx] Multicast %s failed after %d retries: %s", 
                    join ? "join" : "leave", kMaxRetries, addrStr);
        return CHIP_ERROR_INTERNAL;
    }
    
    ChipLogProgress(DeviceLayer, "[UDPUcx] Multicast %s: %s", join ? "joined" : "left", addrStr);
    return CHIP_NO_ERROR;
}

CHIP_ERROR UDPEndPointImplUcx::SetMulticastLoopback(IPVersion aIPVersion, bool aLoopback)
{
    UCX_LOG_VERBOSE(DeviceLayer, "[UDPUcx] SetMulticastLoopback (NOT SUPPORTED on NORA-W36)");
    // NORA-W36 does not support multicast loopback
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void UDPEndPointImplUcx::DeliverReceivedMessage(System::PacketBufferHandle && msg, const IPPacketInfo * pktInfo)
{
    sRxPacketCount++;
    sRxByteCount += msg->DataLength();
    sStatsRxPackets++;
    sStatsRxBytes += static_cast<uint32_t>(msg->DataLength());
    sLifeRxPackets++;
    sLifeRxBytes += static_cast<uint64_t>(msg->DataLength());
    
    // Only log in verbose mode - ProcessQueuedPackets already logs each delivery
    if (pktInfo != nullptr)
    {
        char srcAddr[INET6_ADDRSTRLEN];
        pktInfo->SrcAddress.ToString(srcAddr, sizeof(srcAddr));
        UCX_LOG_VERBOSE(DeviceLayer, "[UcxUDP-RX] #%u: %llu bytes from %s:%u (total: %llu bytes)", 
                       sRxPacketCount, static_cast<unsigned long long>(msg->DataLength()), 
                       srcAddr, pktInfo->SrcPort, static_cast<unsigned long long>(sRxByteCount));
    }
    
    // Only deliver if we're listening and have a callback registered
    if (mState == State::kListening && OnMessageReceived != nullptr)
    {
        OnMessageReceived(this, std::move(msg), pktInfo);
    }
    else
    {
        ChipLogError(DeviceLayer, "[UcxUDP-RX] Cannot deliver: state=%d callback=%p", (int)mState, (void*)OnMessageReceived);
    }
    if (!sQuietUdpLogging)
    {
        ChipLogProgress(DeviceLayer, "[UcxUDP-RX] ===== END DELIVER =====");
    }
}

void UDPEndPointImplUcx::SetQuietUdpLogging(bool quiet)
{
    sQuietUdpLogging = quiet;
    ChipLogProgress(DeviceLayer, "[UDPUcx] Verbose logging %s", quiet ? "SUPPRESSED (quiet mode)" : "ENABLED");
}

// Snapshot implementation lives inside chip::Inet so it can read both the
// file-scope sLife* counters and the anonymous-namespace sTxQueue* state.
// The public symbol is declared in chip::DeviceLayer; we forward to it.
static void FillUcxUdpStats(chip::DeviceLayer::UcxUdpStats & out)
{
    out.txPackets        = sLifeTxPackets;
    out.rxPackets        = sLifeRxPackets;
    out.txBytes          = sLifeTxBytes;
    out.rxBytes          = sLifeRxBytes;
    out.txErrors         = sLifeTxErrors;
    out.txQueueOverflows = sTxQueueOverflows;
    {
        chip::Platform::ScopedLock lock(sTxQueueMutex);
        out.txQueueDepth     = static_cast<uint32_t>(sTxQueueCount);
        out.txQueueHighWater = sTxQueueHighWater;
    }
}

} // namespace Inet

namespace DeviceLayer {

void UcxUdpStatsSnapshot(UcxUdpStats & out)
{
    chip::Inet::FillUcxUdpStats(out);
}

} // namespace DeviceLayer
} // namespace chip

