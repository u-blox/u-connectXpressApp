/* The Wi-Fi side of the Network Commissioning cluster on the module: scan, the network the commissioner gives,
 * joining it. From u-connectMatter's driver; here the URCs come on the Matter thread, so joining does not wait:
 * the answer to the commissioner goes out when the module reports the network (UcxNetwork.cpp) or the time is up. */

#pragma once

#include <platform/NetworkCommissioning.h>
#include <system/SystemLayer.h>

#include <cstdint>

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

inline constexpr uint8_t kMaxWiFiNetworks                  = 1;
inline constexpr uint8_t kWiFiScanNetworksTimeOutSeconds   = 15;
inline constexpr uint8_t kWiFiConnectNetworkTimeoutSeconds = 30;

// u-connectXpress authentication suites (AT+UWSSC) as the cluster's security bits
BitFlags<WiFiSecurityBitmap> ConvertSecurityType(int32_t authSuites);

struct UcxScanResult
{
    char ssid[DeviceLayer::Internal::kMaxWiFiSSIDLength + 1];
    uint8_t bssid[DeviceLayer::Internal::kWiFiBSSIDLength];
    int32_t channel;
    int32_t rssi;
    int32_t authSuites;
};

class UcxWiFiDriver final : public WiFiDriver
{
public:
    // What the app shows of it (the commissioning steps); called with the stack lock
    enum class Step
    {
        kCredentials, // the commissioner gave a network
        kJoining,
        kJoined,
        kFailed, // with the reason as text
    };
    using StepHook = void (*)(Step step, const char * text);

    struct WiFiNetwork
    {
        uint8_t ssid[DeviceLayer::Internal::kMaxWiFiSSIDLength];
        uint8_t ssidLen = 0;
        uint8_t credentials[DeviceLayer::Internal::kMaxWiFiKeyLength];
        uint8_t credentialsLen = 0;
    };

    static UcxWiFiDriver & GetInstance()
    {
        static UcxWiFiDriver instance;
        return instance;
    }

    void SetStepHook(StepHook hook) { mStepHook = hook; }

    // From UcxNetwork.cpp, on the Matter thread without the stack lock
    void NoteNetworkUp();
    void NoteLinkDown(int32_t reason);

    // BaseDriver
    NetworkIterator * GetNetworks() override;
    CHIP_ERROR Init(NetworkStatusChangeCallback * networkStatusChangeCallback) override;
    void Shutdown() override;

    // WirelessDriver
    uint8_t GetMaxNetworks() override { return kMaxWiFiNetworks; }
    uint8_t GetScanNetworkTimeoutSeconds() override { return kWiFiScanNetworksTimeOutSeconds; }
    uint8_t GetConnectNetworkTimeoutSeconds() override { return kWiFiConnectNetworkTimeoutSeconds; }
    CHIP_ERROR CommitConfiguration() override;
    CHIP_ERROR RevertConfiguration() override;
    Status RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex) override;
    Status ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText) override;
    void ConnectNetwork(ByteSpan networkId, ConnectCallback * callback) override;

    // WiFiDriver
    Status AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                              uint8_t & outNetworkIndex) override;
    void ScanNetworks(ByteSpan ssid, ScanCallback * callback) override;

private:
    static constexpr size_t kMaxScanResults = 10; // more does not get through the Bluetooth LE transport in time

    bool NetworkMatch(const WiFiNetwork & network, ByteSpan networkId) const;
    CHIP_ERROR StartJoin(const WiFiNetwork & network);
    size_t Scan(ByteSpan ssid);
    void Joined();
    void Failed(Status status, const char * text);
    void TellStep(Step step, const char * text = "");
    static void OnJoinTimeout(System::Layer * layer, void * self);

    WiFiNetwork mStagingNetwork;
    ConnectCallback * mpConnectCallback                  = nullptr;
    NetworkStatusChangeCallback * mpStatusChangeCallback = nullptr;
    StepHook mStepHook                                   = nullptr;
    UcxScanResult mScanResults[kMaxScanResults];
};

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
