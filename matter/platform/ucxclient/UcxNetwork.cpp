/* The node's network on the module: Wi-Fi as the module reports it and the UDP sockets of the Matter stack.
 * The app keeps its own Wi-Fi commands and sockets: Wi-Fi URCs go to both, a socket's URCs only to its owner.
 * Everything here runs on the Matter thread without the stack lock (UcxClientManager::ProcessUrcs), except
 * OwnsSocketUrc, which the AT client's thread calls. */

#include "UcxClientManager.h"

#include "NetworkCommissioningDriver.h"

#include <inet/IPAddress.h>
#include <inet/IPPacketInfo.h>
#include <inet/UDPEndPointImplUcx.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemPacketBuffer.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "u_cx_socket.h"
#include "u_cx_wifi.h"
}

namespace chip {
namespace DeviceLayer {

namespace {

constexpr size_t kMaxSockets    = 8;
constexpr int32_t kMaxDatagram  = 1460;
constexpr unsigned kInterfaceId = 1; // what UDPEndPointImplUcx::GetBoundInterface gives: the module's one interface

std::atomic<bool> sLinkUp{ false };
std::atomic<bool> sNetworkUp{ false };
std::atomic<uint16_t> sChannel{ 0 };
std::atomic<uint8_t> sSecurity{ 0 };
bool sHasBssid;
bool sHasIpv4;
bool sHasIpv6;
uint8_t sBssid[6];
uint8_t sIpv4[4];
uint8_t sIpv6[16];

std::atomic<int32_t> sSockets[kMaxSockets]{ { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 } };
Inet::UDPEndPointImplUcx * sEndpoints[kMaxSockets];
// Every socket the node made on the module and has not closed: what the node leaves open is closed at its stop
int32_t sCreated[kMaxSockets]{ -1, -1, -1, -1, -1, -1, -1, -1 };

Inet::UDPEndPointImplUcx * FindEndpoint(int32_t socket)
{
    for (size_t i = 0; i < kMaxSockets; i++)
    {
        if (socket >= 0 && sSockets[i].load() == socket)
        {
            return sEndpoints[i];
        }
    }
    return nullptr;
}

// u-connectClient keeps IPv4 as one word and IPv6 as four, the first of the address in the highest
void Ipv4Bytes(const uSockIpAddress_t & address, uint8_t * out)
{
    for (int i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>(address.address.ipv4 >> (24 - 8 * i));
    }
}

void Ipv6Bytes(const uSockIpAddress_t & address, uint8_t * out)
{
    for (int i = 0; i < 16; i++)
    {
        out[i] = static_cast<uint8_t>(address.address.ipv6[3 - i / 4] >> (24 - 8 * (i % 4)));
    }
}

bool AllZero(const uint8_t * bytes, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (bytes[i] != 0)
        {
            return false;
        }
    }
    return true;
}

Inet::IPAddress Ipv4Address(const uint8_t * bytes)
{
    struct in_addr address;

    memcpy(&address, bytes, 4);
    return Inet::IPAddress(address);
}

Inet::IPAddress Ipv6Address(const uint8_t * bytes)
{
    struct in6_addr address;

    memcpy(&address, bytes, 16);
    return Inet::IPAddress(address);
}

void ReadAddresses(uCxHandle_t * ucx)
{
    uSockIpAddress_t address;

    sHasIpv4 = uCxWifiStationGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_IPV4, &address) == 0 &&
        address.type == U_SOCK_ADDRESS_TYPE_V4;
    if (sHasIpv4)
    {
        Ipv4Bytes(address, sIpv4);
        sHasIpv4 = !AllZero(sIpv4, sizeof(sIpv4));
    }
    sHasIpv6 = uCxWifiStationGetNetworkStatus(ucx, U_WIFI_NET_STATUS_ID_IPV6, &address) == 0 &&
        address.type == U_SOCK_ADDRESS_TYPE_V6;
    if (sHasIpv6)
    {
        Ipv6Bytes(address, sIpv6);
        sHasIpv6 = !AllZero(sIpv6, sizeof(sIpv6));
    }
}

void PostAddressEvent(bool up)
{
    ChipDeviceEvent event = {};

    event.Type                           = DeviceEventType::kInterfaceIpAddressChanged;
    event.InterfaceIpAddressChanged.Type = up ? InterfaceIpChangeType::kIpV6_Assigned : InterfaceIpChangeType::kIpV6_Lost;
    (void) PlatformMgr().PostEvent(&event);
    if (up)
    {
        // The SDK publishes its mDNS services on this one only
        ChipDeviceEvent restart = {};

        restart.Type = DeviceEventType::kDnssdRestartNeeded;
        (void) PlatformMgr().PostEvent(&restart);
    }
}

// With the addresses read
void AnnounceUp()
{
    sLinkUp    = true;
    sNetworkUp = true;
    ChipLogProgress(DeviceLayer, "[WiFi] network up: IPv4 %u.%u.%u.%u%s, channel %u", sIpv4[0], sIpv4[1], sIpv4[2],
                    sIpv4[3], sHasIpv6 ? " and IPv6" : "", static_cast<unsigned>(sChannel.load()));
    PostAddressEvent(true);
    NetworkCommissioning::UcxWiFiDriver::GetInstance().NoteNetworkUp(); // a commissioner may wait for it
}

void OnLinkUp(uCxHandle_t *, int32_t, uMacAddress_t * bssid, int32_t channel)
{
    if (bssid != nullptr)
    {
        memcpy(sBssid, bssid->address, sizeof(sBssid));
        sHasBssid = true;
    }
    sChannel = static_cast<uint16_t>(channel);
    sLinkUp  = true;
}

void OnLinkDown(uCxHandle_t *, int32_t, int32_t reason)
{
    ChipLogProgress(DeviceLayer, "[WiFi] link down, reason %d", static_cast<int>(reason));
    sLinkUp = false;
    NetworkCommissioning::UcxWiFiDriver::GetInstance().NoteLinkDown(reason);
}

void OnNetworkUp(uCxHandle_t * ucx)
{
    ReadAddresses(ucx);
    AnnounceUp();
}

void OnNetworkDown(uCxHandle_t *)
{
    sNetworkUp = false;
    sLinkUp    = false;
    sHasIpv4   = false;
    sHasIpv6   = false;
    sHasBssid  = false;
    sChannel   = 0;
    ChipLogProgress(DeviceLayer, "[WiFi] network down");
    PostAddressEvent(false);
}

// One datagram to the SDK, which answers from inside this call: with the stack lock
void Deliver(int32_t socket, const uCxSocketReceiveFrom_t & from, const uint8_t * data, size_t length)
{
    Inet::IPPacketInfo info;
    uint8_t bytes[16];
    bool ipv4 = from.remote_ip.type == U_SOCK_ADDRESS_TYPE_V4;

    info.Clear();
    if (ipv4)
    {
        Ipv4Bytes(from.remote_ip, bytes);
        info.SrcAddress = Ipv4Address(bytes);
    }
    else
    {
        // An IPv4 sender on the module's dual-stack socket comes as ::ffff:a.b.c.d
        static const uint8_t kMapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

        Ipv6Bytes(from.remote_ip, bytes);
        ipv4            = memcmp(bytes, kMapped, sizeof(kMapped)) == 0;
        info.SrcAddress = ipv4 ? Ipv4Address(bytes + 12) : Ipv6Address(bytes);
    }
    info.SrcPort   = static_cast<uint16_t>(from.remote_port);
    info.Interface = Inet::InterfaceId(static_cast<Inet::InterfaceId::PlatformType>(kInterfaceId));
    if (ipv4 && sHasIpv4)
    {
        info.DestAddress = Ipv4Address(sIpv4);
    }
    else if (!ipv4 && sHasIpv6)
    {
        info.DestAddress = Ipv6Address(sIpv6);
    }

    PlatformMgr().LockChipStack();
    Inet::UDPEndPointImplUcx * endpoint = FindEndpoint(socket);
    System::PacketBufferHandle buffer   = System::PacketBufferHandle::NewWithData(data, length);

    if (endpoint != nullptr && !buffer.IsNull())
    {
        info.DestPort = endpoint->GetBoundPort();
        endpoint->DeliverReceivedMessage(std::move(buffer), &info);
    }
    else if (endpoint != nullptr)
    {
        ChipLogError(DeviceLayer, "[UDP] no buffer for %u bytes on socket %d", static_cast<unsigned>(length),
                     static_cast<int>(socket));
    }
    PlatformMgr().UnlockChipStack();
}

// "+UESODA:<socket>,<bytes>": the module holds the data (buffered read mode), one datagram for each read
void OnSocketData(uCxHandle_t * ucx, int32_t socket, int32_t bytes)
{
    static uint8_t sData[kMaxDatagram]; // the Matter thread only

    if (FindEndpoint(socket) == nullptr)
    {
        return; // a socket of the app
    }
    while (bytes > 0)
    {
        uCxSocketReceiveFrom_t from;
        const int32_t read = uCxSocketReceiveFrom(ucx, socket, bytes < kMaxDatagram ? bytes : kMaxDatagram, sData, &from);

        if (read <= 0)
        {
            break; // an earlier read took it
        }
        bytes -= read;
        Deliver(socket, from, sData, static_cast<size_t>(read));
    }
}

} // namespace

void UcxClientManager::NetworkStart()
{
    uCxHandle_t * ucx = GetUcxHandle();
    uCxWifiStationStatus_t status;
    bool connected = false;

    if (ucx == nullptr)
    {
        return;
    }
    uCxWifiRegisterLinkUp(ucx, OnLinkUp);
    uCxWifiRegisterLinkDown(ucx, OnLinkDown);
    uCxWifiRegisterStationNetworkUp(ucx, OnNetworkUp);
    uCxWifiRegisterStationNetworkDown(ucx, OnNetworkDown);
    uCxSocketRegisterDataAvailable(ucx, OnSocketData);

    // The app may have Wi-Fi up already (wifi.connect): then no URC will tell
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_CONNECTION, &status))
    {
        connected = status.type == U_CX_WIFI_STATION_STATUS_RSP_TYPE_STATUS_ID_INT && status.rsp.StatusIdInt.int_val == 2;
    }
    (void) uCxEnd(ucx);
    if (!connected)
    {
        return;
    }
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_CHANNEL, &status) &&
        status.type == U_CX_WIFI_STATION_STATUS_RSP_TYPE_STATUS_ID_INT)
    {
        sChannel = static_cast<uint16_t>(status.rsp.StatusIdInt.int_val);
    }
    (void) uCxEnd(ucx);
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_BSSID, &status) &&
        status.type == U_CX_WIFI_STATION_STATUS_RSP_TYPE_STATUS_ID_MAC)
    {
        memcpy(sBssid, status.rsp.StatusIdMac.bssid.address, sizeof(sBssid));
        sHasBssid = true;
    }
    (void) uCxEnd(ucx);
    ReadAddresses(ucx);
    if (sHasIpv4 || sHasIpv6)
    {
        AnnounceUp();
    }
}

bool UcxClientManager::IsWiFiConnected() const
{
    return sNetworkUp || sLinkUp;
}

bool UcxClientManager::IsNetworkReady() const
{
    return sNetworkUp;
}

void UcxClientManager::ClearNetworkReady()
{
    sNetworkUp = false;
}

const uint8_t * UcxClientManager::GetIpv4Address() const
{
    return sNetworkUp && sHasIpv4 ? sIpv4 : nullptr;
}

const uint8_t * UcxClientManager::GetIpv6Address() const
{
    return sNetworkUp && sHasIpv6 ? sIpv6 : nullptr;
}

CHIP_ERROR UcxClientManager::GetWiFiRssi(int8_t & rssi)
{
    uCxHandle_t * ucx = GetUcxHandle();
    uCxWifiStationStatus_t status;
    int32_t value = -32768;

    VerifyOrReturnError(ucx != nullptr && IsWiFiConnected(), CHIP_ERROR_NOT_CONNECTED);
    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_RSSI, &status) &&
        status.type == U_CX_WIFI_STATION_STATUS_RSP_TYPE_STATUS_ID_INT)
    {
        value = status.rsp.StatusIdInt.int_val;
    }
    (void) uCxEnd(ucx);
    VerifyOrReturnError(value >= -128 && value <= 0, CHIP_ERROR_NOT_CONNECTED);
    rssi = static_cast<int8_t>(value);
    return CHIP_NO_ERROR;
}

bool UcxClientManager::GetWiFiBssId(uint8_t * bssid) const
{
    if (!IsWiFiConnected() || !sHasBssid)
    {
        return false;
    }
    memcpy(bssid, sBssid, sizeof(sBssid));
    return true;
}

uint16_t UcxClientManager::GetWiFiChannel() const
{
    return IsWiFiConnected() ? sChannel.load() : 0;
}

uint8_t UcxClientManager::GetWiFiSecurityType() const
{
    return sSecurity;
}

void UcxClientManager::SetWiFiSecurityType(uint8_t security)
{
    sSecurity = security;
}

// The read mode is the module's setting for the sockets created after it. The node reads its datagrams when
// +UESODA tells (buffered); the app may have chosen another mode for its own sockets, and gets it back.
int32_t UcxClientManager::CreateUdpSocket(int32_t * socket)
{
    uCxHandle_t * ucx = GetUcxHandle();
    uReadMode_t mode  = U_READ_MODE_BUFFERED;
    int32_t result;

    if (ucx == nullptr)
    {
        return -1;
    }
    if (uCxSocketGetReadMode(ucx, &mode) != 0)
    {
        mode = U_READ_MODE_BUFFERED;
    }
    if (mode != U_READ_MODE_BUFFERED)
    {
        (void) uCxSocketSetReadMode(ucx, U_READ_MODE_BUFFERED);
    }
    result = uCxSocketCreate2(ucx, U_SOCKET_PROTOCOL_UDP, U_SOCKET_PREF_IP_VER_IPV6, socket);
    if (mode != U_READ_MODE_BUFFERED)
    {
        (void) uCxSocketSetReadMode(ucx, mode);
    }
    for (int32_t & created : sCreated)
    {
        if (result >= 0 && created < 0)
        {
            created = *socket;
            break;
        }
    }
    return result;
}

void UcxClientManager::CloseUdpSocket(int32_t socket)
{
    uCxHandle_t * ucx = GetUcxHandle();

    UnregisterUdpEndpoint(socket);
    for (int32_t & created : sCreated)
    {
        if (created == socket)
        {
            created = -1;
        }
    }
    if (ucx != nullptr && socket >= 0)
    {
        (void) uCxSocketClose(ucx, socket);
    }
}

int UcxClientManager::CloseUdpSockets()
{
    int closed = 0;

    for (int32_t created : sCreated)
    {
        if (created >= 0)
        {
            CloseUdpSocket(created);
            closed++;
        }
    }
    return closed;
}

void UcxClientManager::RegisterUdpEndpoint(int32_t socketHandle, Inet::UDPEndPointImplUcx * endpoint)
{
    UnregisterUdpEndpoint(socketHandle);
    for (size_t i = 0; i < kMaxSockets; i++)
    {
        if (sSockets[i].load() < 0)
        {
            sEndpoints[i] = endpoint;
            sSockets[i]   = socketHandle;
            return;
        }
    }
    ChipLogError(DeviceLayer, "[UDP] no room for socket %d: its data will not reach the node", static_cast<int>(socketHandle));
}

void UcxClientManager::UnregisterUdpEndpoint(int32_t socketHandle)
{
    for (size_t i = 0; i < kMaxSockets; i++)
    {
        if (socketHandle >= 0 && sSockets[i].load() == socketHandle)
        {
            sSockets[i]   = -1;
            sEndpoints[i] = nullptr;
        }
    }
}

// "+UESODA:<socket>,<bytes>" and "+UESOCL:<socket>"
bool UcxClientManager::OwnsSocketUrc(const char * line, size_t length) const
{
    static const char * const kNames[] = { "+UESODA:", "+UESOCL:" };

    for (const char * name : kNames)
    {
        const size_t n = strlen(name);

        if (length > n && strncmp(line, name, n) == 0)
        {
            const int32_t socket = static_cast<int32_t>(strtol(line + n, nullptr, 10));

            for (const auto & owned : sSockets)
            {
                if (owned.load() == socket)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace DeviceLayer
} // namespace chip
