#include "NetworkCommissioningDriver.h"

#include "PlatformAbstraction.h"
#include "UcxClientManager.h"
#include "ucxm_host.h"

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KeyValueStoreManager.h>

#include <cstring>

extern "C" {
#include "u_cx_wifi.h"
}

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

namespace {

// In the node's key-value store, which the app seals on disk (ucxapp_matter.h): no plain password in a file
constexpr char kSsidKey[]        = "wifi-ssid";
constexpr char kCredentialsKey[] = "wifi-pass";

// A scan of every channel takes some seconds
constexpr int32_t kScanTimeoutMs = 30000;

// The commissioner is told kWiFiConnectNetworkTimeoutSeconds: the node answers before that
constexpr uint16_t kJoinSeconds = 25;

// +UEWLD reason of an access point that does not take the password (4-way handshake timeout)
constexpr int32_t kReasonHandshake = 15;

class NetworkList final : public NetworkIterator
{
public:
    NetworkList(const UcxWiFiDriver::WiFiNetwork & network, bool connected) : mNetwork(network), mConnected(connected) {}
    size_t Count() override { return mNetwork.ssidLen > 0 ? 1 : 0; }
    bool Next(Network & item) override
    {
        if (mDone || mNetwork.ssidLen == 0)
        {
            return false;
        }
        memcpy(item.networkID, mNetwork.ssid, mNetwork.ssidLen);
        item.networkIDLen = mNetwork.ssidLen;
        item.connected    = mConnected;
        mDone             = true;
        return true;
    }
    void Release() override { delete this; }

private:
    UcxWiFiDriver::WiFiNetwork mNetwork;
    bool mConnected;
    bool mDone = false;
};

class ScanList final : public WiFiScanResponseIterator
{
public:
    ScanList(const UcxScanResult * results, size_t count) : mResults(results), mCount(count) {}
    size_t Count() override { return mCount; }
    bool Next(WiFiScanResponse & item) override
    {
        if (mNext >= mCount)
        {
            return false;
        }
        const UcxScanResult & found = mResults[mNext++];

        item.security = ConvertSecurityType(found.authSuites);
        item.ssidLen  = static_cast<uint8_t>(strlen(found.ssid));
        memcpy(item.ssid, found.ssid, item.ssidLen);
        memcpy(item.bssid, found.bssid, sizeof(item.bssid));
        item.channel = static_cast<uint16_t>(found.channel);
        // A commissioner drops an entry with the wrong band: 2.4 GHz has channels 1 to 14
        item.wiFiBand        = found.channel <= 14 ? WiFiBandEnum::k2g4 : WiFiBandEnum::k5g;
        item.signal.type     = WirelessSignalType::kdBm;
        item.signal.strength = static_cast<int8_t>(found.rssi < -128 ? -128 : found.rssi > 0 ? 0 : found.rssi);
        return true;
    }
    void Release() override { delete this; }

private:
    const UcxScanResult * mResults;
    size_t mCount;
    size_t mNext = 0;
};

class LongTimeout
{
public:
    explicit LongTimeout(int32_t ms) : mPrevious(gUcxmHost->at->setCommandTimeout(gUcxmHost->client, ms, false)) {}
    ~LongTimeout() { (void) gUcxmHost->at->setCommandTimeout(gUcxmHost->client, mPrevious, true); }

private:
    int32_t mPrevious;
};

} // namespace

// Bit 0 = shared secret, 1 = PSK, 2 = EAP, 3 = WPA, 4 = WPA2, 5 = WPA3
BitFlags<WiFiSecurityBitmap> ConvertSecurityType(int32_t authSuites)
{
    BitFlags<WiFiSecurityBitmap> security;

    if (authSuites == 0)
    {
        security.Set(WiFiSecurityBitmap::kUnencrypted);
        return security;
    }
    security.Set(WiFiSecurityBitmap::kWpa3Personal, (authSuites & (1 << 5)) != 0);
    security.Set(WiFiSecurityBitmap::kWpa2Personal, (authSuites & (1 << 4)) != 0);
    security.Set(WiFiSecurityBitmap::kWpaPersonal, (authSuites & (1 << 3)) != 0);
    security.Set(WiFiSecurityBitmap::kWep, (authSuites & (1 << 0)) != 0);
    return security;
}

void UcxWiFiDriver::TellStep(Step step, const char * text)
{
    if (mStepHook != nullptr)
    {
        mStepHook(step, text);
    }
}

CHIP_ERROR UcxWiFiDriver::Init(NetworkStatusChangeCallback * networkStatusChangeCallback)
{
    mpConnectCallback      = nullptr;
    mpStatusChangeCallback = networkStatusChangeCallback;

    // The network of an earlier commissioning; a node that has one joins it again
    (void) RevertConfiguration();
    if (mStagingNetwork.ssidLen > 0 && !UcxClientManager::GetInstance().IsWiFiConnected())
    {
        ChipLogProgress(NetworkProvisioning, "Joining the stored Wi-Fi network %.*s", mStagingNetwork.ssidLen,
                        reinterpret_cast<const char *>(mStagingNetwork.ssid));
        if (StartJoin(mStagingNetwork) != CHIP_NO_ERROR)
        {
            ChipLogError(NetworkProvisioning, "The module did not start to join the stored Wi-Fi network");
        }
    }
    return CHIP_NO_ERROR;
}

void UcxWiFiDriver::Shutdown()
{
    (void) DeviceLayer::SystemLayer().CancelTimer(OnJoinTimeout, this);
    mpConnectCallback      = nullptr;
    mpStatusChangeCallback = nullptr;
}

NetworkIterator * UcxWiFiDriver::GetNetworks()
{
    return new NetworkList(mStagingNetwork, UcxClientManager::GetInstance().IsWiFiConnected());
}

CHIP_ERROR UcxWiFiDriver::CommitConfiguration()
{
    VerifyOrReturnError(mStagingNetwork.ssidLen > 0, CHIP_NO_ERROR);
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Put(kSsidKey, mStagingNetwork.ssid, mStagingNetwork.ssidLen));
    ReturnErrorOnFailure(
        PersistedStorage::KeyValueStoreMgr().Put(kCredentialsKey, mStagingNetwork.credentials, mStagingNetwork.credentialsLen));
    ChipLogProgress(NetworkProvisioning, "Wi-Fi network %.*s stored", mStagingNetwork.ssidLen,
                    reinterpret_cast<const char *>(mStagingNetwork.ssid));
    return CHIP_NO_ERROR;
}

// Back to the stored network: none when the node has never finished a commissioning
CHIP_ERROR UcxWiFiDriver::RevertConfiguration()
{
    WiFiNetwork network;
    size_t ssidLen        = 0;
    size_t credentialsLen = 0;

    mStagingNetwork = WiFiNetwork{};
    CHIP_ERROR err  = PersistedStorage::KeyValueStoreMgr().Get(kSsidKey, network.ssid, sizeof(network.ssid), &ssidLen);
    VerifyOrReturnError(err != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND, CHIP_NO_ERROR);
    ReturnErrorOnFailure(err);
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Get(kCredentialsKey, network.credentials,
                                                                  sizeof(network.credentials), &credentialsLen));
    network.ssidLen        = static_cast<uint8_t>(ssidLen);
    network.credentialsLen = static_cast<uint8_t>(credentialsLen);
    mStagingNetwork        = network;
    return CHIP_NO_ERROR;
}

Status UcxWiFiDriver::AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                                         uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(!ssid.empty() && ssid.size() <= sizeof(mStagingNetwork.ssid), Status::kOutOfRange);
    VerifyOrReturnError(credentials.size() <= sizeof(mStagingNetwork.credentials), Status::kOutOfRange);

    memcpy(mStagingNetwork.ssid, ssid.data(), ssid.size());
    mStagingNetwork.ssidLen = static_cast<uint8_t>(ssid.size());
    memcpy(mStagingNetwork.credentials, credentials.data(), credentials.size());
    mStagingNetwork.credentialsLen = static_cast<uint8_t>(credentials.size());
    TellStep(Step::kCredentials);
    return Status::kSuccess;
}

Status UcxWiFiDriver::RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    mStagingNetwork = WiFiNetwork{};
    return Status::kSuccess;
}

Status UcxWiFiDriver::ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText)
{
    outDebugText.reduce_size(0);
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    return index == 0 ? Status::kSuccess : Status::kOutOfRange; // there is one network
}

bool UcxWiFiDriver::NetworkMatch(const WiFiNetwork & network, ByteSpan networkId) const
{
    return network.ssidLen > 0 && networkId.size() == network.ssidLen &&
        memcmp(networkId.data(), network.ssid, network.ssidLen) == 0;
}

// The AT commands of the app's wifi.connect; the module then reports the link and the network with URCs
CHIP_ERROR UcxWiFiDriver::StartJoin(const WiFiNetwork & network)
{
    uCxHandle_t * ucx = UcxClientManager::GetInstance().GetUcxHandle();
    char ssid[sizeof(network.ssid) + 1];
    char key[sizeof(network.credentials) + 1];
    uCxWifiStationStatus_t status;
    bool connected = false;
    int32_t result;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    memcpy(ssid, network.ssid, network.ssidLen);
    ssid[network.ssidLen] = '\0';
    memcpy(key, network.credentials, network.credentialsLen);
    key[network.credentialsLen] = '\0';

    if (uCxWifiStationStatusBegin(ucx, U_WIFI_STATUS_ID_CONNECTION, &status))
    {
        connected = status.type == U_CX_WIFI_STATION_STATUS_RSP_TYPE_STATUS_ID_INT && status.rsp.StatusIdInt.int_val == 2;
    }
    (void) uCxEnd(ucx);
    if (connected)
    {
        // The settings of a connected station cannot be changed
        (void) uCxWifiStationDisconnect(ucx);
        Platform::SleepMs(500);
    }
    UcxClientManager::GetInstance().ClearNetworkReady();

    result = uCxWifiStationSetConnectionParams(ucx, 0, ssid);
    if (result == 0)
    {
        result = network.credentialsLen > 0 ? uCxWifiStationSetSecurityWpa(ucx, 0, key, U_WIFI_WPA_THRESHOLD_WPA2)
                                            : uCxWifiStationSetSecurityOpen(ucx, 0);
    }
    memset(key, 0, sizeof(key));
    if (result == 0)
    {
        result = uCxWifiStationConnect(ucx, 0);
        if (result != 0)
        {
            // An earlier attempt can still be pending in the module: stop it and try once more
            (void) uCxWifiStationDisconnect(ucx);
            Platform::SleepMs(1000);
            result = uCxWifiStationConnect(ucx, 0);
        }
    }
    if (result != 0)
    {
        ChipLogError(NetworkProvisioning, "The module refused the Wi-Fi network: error %d", static_cast<int>(result));
        return CHIP_ERROR_INTERNAL;
    }
    // WPA2 or better, as the threshold asks
    UcxClientManager::GetInstance().SetWiFiSecurityType(network.credentialsLen > 0 ? 0x10 : 0);
    return CHIP_NO_ERROR;
}

void UcxWiFiDriver::ConnectNetwork(ByteSpan networkId, ConnectCallback * callback)
{
    Status status = Status::kSuccess;

    VerifyOrReturn(callback != nullptr);
    TellStep(Step::kJoining);
    if (mpConnectCallback != nullptr)
    {
        status = Status::kUnknownError; // the answer to an earlier request is still due
    }
    else if (!NetworkMatch(mStagingNetwork, networkId))
    {
        status = Status::kNetworkIDNotFound;
    }
    else if (StartJoin(mStagingNetwork) != CHIP_NO_ERROR)
    {
        status = Status::kOtherConnectionFailure;
    }
    if (status != Status::kSuccess)
    {
        TellStep(Step::kFailed, status == Status::kNetworkIDNotFound ? "the commissioner asked for a network it did not give"
                                                                     : "the module did not start to join the network");
        callback->OnResult(status, CharSpan(), 0);
        return;
    }
    mpConnectCallback = callback;
    (void) DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds16(kJoinSeconds), OnJoinTimeout, this);
}

void UcxWiFiDriver::Joined()
{
    ConnectCallback * callback = mpConnectCallback;

    VerifyOrReturn(callback != nullptr); // the app's own wifi.connect, or the module came back by itself
    (void) DeviceLayer::SystemLayer().CancelTimer(OnJoinTimeout, this);
    mpConnectCallback = nullptr;
    TellStep(Step::kJoined);
    callback->OnResult(Status::kSuccess, CharSpan(), 0);
    if (mpStatusChangeCallback != nullptr)
    {
        mpStatusChangeCallback->OnNetworkingStatusChange(
            Status::kSuccess, MakeOptional(ByteSpan(mStagingNetwork.ssid, mStagingNetwork.ssidLen)), NullOptional);
    }
}

void UcxWiFiDriver::Failed(Status status, const char * text)
{
    ConnectCallback * callback = mpConnectCallback;
    uCxHandle_t * ucx          = UcxClientManager::GetInstance().GetUcxHandle();

    VerifyOrReturn(callback != nullptr);
    (void) DeviceLayer::SystemLayer().CancelTimer(OnJoinTimeout, this);
    mpConnectCallback = nullptr;
    if (ucx != nullptr)
    {
        (void) uCxWifiStationDisconnect(ucx); // else the module keeps trying, and refuses the next attempt
    }
    TellStep(Step::kFailed, text);
    callback->OnResult(status, CharSpan(), 0);
}

void UcxWiFiDriver::OnJoinTimeout(System::Layer *, void * self)
{
    static_cast<UcxWiFiDriver *>(self)->Failed(Status::kNetworkNotFound,
                                               "the module did not get on the network in time (out of range, or no address)");
}

// The two below come from the URC handlers, which run without the stack lock; the cluster's callbacks need it
void UcxWiFiDriver::NoteNetworkUp()
{
    (void) PlatformMgr().ScheduleWork([](intptr_t) { GetInstance().Joined(); }, 0);
}

void UcxWiFiDriver::NoteLinkDown(int32_t reason)
{
    if (reason == kReasonHandshake)
    {
        (void) PlatformMgr().ScheduleWork(
            [](intptr_t) { GetInstance().Failed(Status::kAuthFailure, "the access point did not take the password"); }, 0);
    }
}

// Up to kMaxScanResults networks, each name once with its strongest access point
size_t UcxWiFiDriver::Scan(ByteSpan ssid)
{
    uCxHandle_t * ucx = UcxClientManager::GetInstance().GetUcxHandle();
    uCxWifiStationScan_t found;
    size_t count = 0;

    VerifyOrReturnValue(ucx != nullptr, 0);

    LongTimeout timeout(kScanTimeoutMs);

    uCxWifiStationScan1Begin(ucx, U_WIFI_SCAN_MODE_ACTIVE);
    while (uCxWifiStationScan1GetNext(ucx, &found))
    {
        const size_t length = found.ssid != nullptr ? strlen(found.ssid) : 0;
        size_t at           = count;

        // No hidden network, and only the one asked for when the commissioner names it
        if (length == 0 || length > DeviceLayer::Internal::kMaxWiFiSSIDLength ||
            (!ssid.empty() && (ssid.size() != length || memcmp(ssid.data(), found.ssid, length) != 0)))
        {
            continue;
        }
        for (size_t i = 0; i < count; i++)
        {
            if (strcmp(mScanResults[i].ssid, found.ssid) == 0)
            {
                at = i;
            }
        }
        if (at == count && count == kMaxScanResults)
        {
            // Full: the weakest one gives way
            at = 0;
            for (size_t i = 1; i < count; i++)
            {
                at = mScanResults[i].rssi < mScanResults[at].rssi ? i : at;
            }
        }
        if (at < count && mScanResults[at].rssi >= found.rssi)
        {
            continue;
        }
        UcxScanResult & result = mScanResults[at];

        memcpy(result.ssid, found.ssid, length + 1);
        memcpy(result.bssid, found.bssid.address, sizeof(result.bssid));
        result.channel    = found.channel;
        result.rssi       = found.rssi;
        result.authSuites = found.authentication_suites;
        count += at == count ? 1 : 0;
    }
    (void) uCxEnd(ucx);
    return count;
}

void UcxWiFiDriver::ScanNetworks(ByteSpan ssid, ScanCallback * callback)
{
    VerifyOrReturn(callback != nullptr);

    const size_t count = Scan(ssid);

    ChipLogProgress(NetworkProvisioning, "Wi-Fi scan: %u network(s)", static_cast<unsigned>(count));
    callback->OnFinished(Status::kSuccess, CharSpan(), new ScanList(mScanResults, count));
}

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
