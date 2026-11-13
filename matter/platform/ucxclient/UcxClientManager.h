/* What u-connectMatter's platform files ask of its UcxClientManager, on u-connectXpressApp's plug-in host
 * (app/src/ucxapp_matter.h). The original brings the module up, owns its UART and keeps Wi-Fi state; here the
 * app does all of that, so this is the small rest: the handle, the URC queue and what the node owns on the module. */

#pragma once

#include <lib/core/CHIPError.h>

#include <cstddef>
#include <cstdint>

extern "C" {
#include "u_cx.h"
}

namespace chip {
namespace Inet {
class UDPEndPointImplUcx;
} // namespace Inet

namespace System {
void UcxWake(); // WakeEventUcxclient.cpp: ends the Matter thread's wait
} // namespace System

namespace DeviceLayer {

class UcxClientManager
{
public:
    static UcxClientManager & GetInstance();

    static bool IsTestMode() { return false; }

    uCxHandle_t * GetUcxHandle();
    bool IsInitialized() { return GetUcxHandle() != nullptr; }

    // On the AT client's thread: keeps a copy for ProcessUrcs(). True when the URC is the node's alone.
    bool QueueUrc(const char * line, size_t length);
    // On the Matter thread, once per turn of the event loop: the queued URCs go to the uCx* callbacks.
    void ProcessUrcs();

    // GATT handles of the CHIPoBLE service: URCs about them are the node's alone.
    void OwnGattHandles(int32_t c1, int32_t c2, int32_t c2Cccd);

    const char * GetMacSuffix();

    // Wi-Fi as the module reports it (UcxNetwork.cpp): the app may have it up already (wifi.connect), else the
    // commissioner brings it up through the Wi-Fi driver (NetworkCommissioningDriver.cpp).
    void NetworkStart(); // with the stack: the URC handlers, and the state the module is in now
    bool IsWiFiConnected() const;
    bool IsNetworkReady() const; // it has an address
    void ClearNetworkReady();
    bool IsDnssdRestartPending() const { return false; }
    void ClearDnssdRestartPending() {}
    const uint8_t * GetIpv4Address() const; // 4 bytes, nullptr without one
    const uint8_t * GetIpv6Address() const; // 16 bytes, link-local
    CHIP_ERROR GetWiFiRssi(int8_t & rssi);
    bool GetWiFiBssId(uint8_t * bssid) const;
    uint16_t GetWiFiChannel() const;
    uint8_t GetWiFiSecurityType() const;
    void SetWiFiSecurityType(uint8_t security);

    // mDNS runs on the module (AT+UMD*), never on the host (UcxMdns.cpp; DnssdImpl.cpp is the SDK's side of it).
    // Order: MdnsStart (host name), MdnsConfigureService for each service, then MdnsEnable(true) announces all.
    static constexpr size_t kMaxMdnsServices = 3;
    CHIP_ERROR MdnsStart(const char * hostname = nullptr);
    CHIP_ERROR MdnsEnable(bool enable);
    bool MdnsIsEnabled() const;
    CHIP_ERROR MdnsForceReannounce();
    CHIP_ERROR MdnsSuspend(); // responder off for an update, the services stay
    CHIP_ERROR MdnsStop();
    CHIP_ERROR MdnsConfigureService(int32_t index, const char * serviceName, const char * serviceType, uint16_t port,
                                    const char * const * txtRecords, size_t txtCount);
    CHIP_ERROR MdnsClearServices();
    void MdnsRemoveService(int32_t index);
    CHIP_ERROR MdnsUpdateTxtRecords(int32_t index, const char * const * txtRecords, size_t txtCount);
    void MdnsNoteUrc(const char * line, size_t length); // on the AT client's thread

    // The Matter stack's UDP sockets on the module (UcxNetwork.cpp): their data goes to the endpoint, and their
    // URCs are the node's alone.
    int32_t CreateUdpSocket(int32_t * socket); // u-connectClient's result
    void CloseUdpSocket(int32_t socket);
    int CloseUdpSockets(); // at the node's stop: closes what the stack left open, returns how many
    void RegisterUdpEndpoint(int32_t socketHandle, Inet::UDPEndPointImplUcx * endpoint);
    void UnregisterUdpEndpoint(int32_t socketHandle);
    bool OwnsSocketUrc(const char * line, size_t length) const; // on the AT client's thread
    void TouchAtActivity() {}

    // The module is the user's: a Matter factory reset clears what the node stored, never the module.
    CHIP_ERROR FactoryResetNoraW36() { return CHIP_NO_ERROR; }
};

} // namespace DeviceLayer
} // namespace chip
