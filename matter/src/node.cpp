#include "node.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/boolean-state-server/CodegenIntegration.h>
#include <app/clusters/temperature-measurement-server/CodegenIntegration.h>
#include <app/server/AppDelegate.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <app/server/Server.h>
#include <app/persistence/String.h>
#include <lib/support/DefaultStorageKeyAllocator.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <crypto/CHIPCryptoPAL.h>
#include <crypto/DefaultSessionKeystore.h>
#include <psa/crypto.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <lib/support/CHIPMem.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/internal/BLEManager.h>

#include <platform/ucxclient/BLEManagerImpl.h>
#include <platform/ucxclient/DeviceInstanceInfoProviderImpl.h>
#include <platform/ucxclient/KeyValueStoreManagerImpl.h>
#include <platform/ucxclient/NetworkCommissioningDriver.h>
#include <platform/ucxclient/PlatformManagerImpl.h>
#include <platform/ucxclient/UcxClientManager.h>
#include <platform/ucxclient/support/DeviceConfig.h>

#include "UbloxCommissionableDataProvider.h"
#include "endpoints.h"
#include "progress.h"
#include "ucxm_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace chip;
using namespace chip::DeviceLayer;

namespace ucxm {

static void NetworkText(char * out, size_t size);

namespace {

// A commissioner has this long to find the node after matter.start or matter.open (the longest Matter allows)
constexpr uint16_t kWindowSeconds = 900;

ucxmatter::UbloxCommissionableDataProvider gCommissionableData;
NodeEvent gEvent;
bool gStackUp;
bool gServerUp;
bool gLoopUp;
bool gWiFi;
bool gWiFiUp;
bool gOwnWrite; // the node writes its own attributes: no event for them, matter.set told already
bool gDemo;
unsigned gDemoStep;

// matter.demo: one step every 3 s; the temperature goes round once in 2 minutes, the contact changes every 30 s
constexpr uint32_t kDemoStepMs       = 3000;
constexpr unsigned kDemoTempSteps    = 40;
constexpr unsigned kDemoContactSteps = 10;
constexpr uint32_t kLight            = 0x0101; // device types of endpoints.cpp
constexpr uint32_t kTemp             = 0x0302;
constexpr uint32_t kContact          = 0x0015;

// With the stack lock: the attributes of an endpoint tied to the data model take its values
void WriteModel(const Endpoint & e)
{
    namespace Clusters  = app::Clusters;
    const EndpointId ep = EndpointModel(e);

    if (!gServerUp || ep == 0)
    {
        return;
    }
    gOwnWrite = true;
    if (e.kind->deviceType == kLight)
    {
        (void) Clusters::OnOff::Attributes::OnOff::Set(ep, e.value[0] != 0);
        // 1 to 254 on the wire
        (void) Clusters::LevelControl::Attributes::CurrentLevel::Set(
            ep, static_cast<uint8_t>(std::max<int32_t>(1, (e.value[1] * 254 + 50) / 100)));
    }
    else if (e.kind->deviceType == kTemp)
    {
        (void) Clusters::TemperatureMeasurement::SetMeasuredValue(ep, chip::app::DataModel::MakeNullable(static_cast<int16_t>(e.value[0])));
    }
    else if (e.kind->deviceType == kContact)
    {
        Clusters::BooleanStateCluster * cluster = Clusters::BooleanState::FindClusterOnEndpoint(ep);

        if (cluster != nullptr)
        {
            (void) cluster->SetStateValue(e.value[0] == 0); // true: in contact, closed
        }
    }
    gOwnWrite = false;
}

// With the stack lock, when the server is up: the light as the data model kept it (a controller's last word), the
// sensors as the node has them
void SyncModel()
{
    namespace Clusters = app::Clusters;

    for (int i = 0; i < EndpointCount(); i++)
    {
        Endpoint & e        = EndpointAt(i);
        const EndpointId ep = EndpointModel(e);
        bool on             = false;
        chip::app::DataModel::Nullable<uint8_t> level;

        if (e.kind->deviceType != kLight)
        {
            WriteModel(e);
        }
        else if (ep != 0 && Clusters::OnOff::Attributes::OnOff::Get(ep, &on) == Protocols::InteractionModel::Status::Success)
        {
            e.value[0] = on ? 1 : 0;
            // a fresh node's level is the minimum: the light keeps its own (100%), or it would come on at 1%
            if (Clusters::LevelControl::Attributes::CurrentLevel::Get(ep, level) == Protocols::InteractionModel::Status::Success &&
                !level.IsNull() && level.Value() > 1)
            {
                e.value[1] = std::max(1, (level.Value() * 100 + 127) / 254);
            }
            else
            {
                WriteModel(e);
            }
        }
    }
}

// On the Matter thread with the stack lock
void DemoTick(System::Layer *, void *)
{
    if (!gDemo)
    {
        return;
    }
    gDemoStep++;
    for (int i = 0; i < EndpointCount(); i++)
    {
        Endpoint & e = EndpointAt(i);

        if (e.kind->deviceType == kTemp)
        {
            const double degrees = 21.5 + 2.5 * sin(6.283185307179586 * gDemoStep / kDemoTempSteps);

            e.value[0] = static_cast<int32_t>(lround(degrees * 10.0) * 10); // 0.1 degrees, as the card shows it
            WriteModel(e);
        }
        else if (e.kind->deviceType == kContact && gDemoStep % kDemoContactSteps == 0)
        {
            char line[64];

            e.value[0] = e.value[0] != 0 ? 0 : 1;
            WriteModel(e);
            snprintf(line, sizeof(line), "matter: %s = %s (demo)", e.name, e.value[0] != 0 ? "open" : "closed");
            gEvent(line);
        }
    }
    (void) SystemLayer().StartTimer(System::Clock::Milliseconds32(kDemoStepMs), DemoTick, nullptr);
}

using WiFiDriver = NetworkCommissioning::UcxWiFiDriver;

// The Network Commissioning cluster of the root endpoint, on the module's Wi-Fi
app::Clusters::NetworkCommissioning::Instance gWiFiCommissioning(0, &WiFiDriver::GetInstance());

// With the stack lock: steps 3 to 5 of a commissioning
void OnWiFiStep(WiFiDriver::Step step, const char * text)
{
    switch (step)
    {
    case WiFiDriver::Step::kCredentials:
        ProgressDone(kStepNetworkGiven);
        break;
    case WiFiDriver::Step::kJoining:
        ProgressDone(kStepNetworkJoin);
        break;
    case WiFiDriver::Step::kJoined:
        ProgressDone(kStepNetworkUp);
        break;
    case WiFiDriver::Step::kFailed:
        ProgressFailed(kStepNetworkUp, text);
        break;
    }
}

struct StackLock
{
    StackLock() { PlatformMgr().LockChipStack(); }
    ~StackLock() { PlatformMgr().UnlockChipStack(); }
};

// Commissioning as the app shows it (progress.h); on the Matter thread
class Commissioning : public AppDelegate, public FabricTable::Delegate
{
public:
    void OnCommissioningWindowOpened() override
    {
        ProgressReset();
        gEvent("matter: commissioning window open");
    }
    void OnCommissioningWindowClosed() override { gEvent("matter: commissioning window closed"); }
    void OnCommissioningSessionEstablishmentStarted() override { gEvent("matter: pairing with the passcode ..."); }
    void OnCommissioningSessionStarted() override { ProgressDone(kStepPaired); }
    void OnCommissioningSessionStopped() override { gEvent("matter: commissioning session closed"); }
    void OnCommissioningSessionEstablishmentError(CHIP_ERROR err) override { ProgressFailed(kStepPaired, ErrorStr(err)); }
    void OnFabricCommitted(const FabricTable &, FabricIndex) override { ProgressDone(kStepFabric); }
    void OnFabricRemoved(const FabricTable & table, FabricIndex) override
    {
        char line[80];

        snprintf(line, sizeof(line), "matter: fabric removed, %u left", static_cast<unsigned>(table.FabricCount()));
        gEvent(line);
    }
};

Commissioning gCommissioning;

void OnDeviceEvent(const ChipDeviceEvent * event, intptr_t)
{
    if (event->Type == DeviceEventType::kCommissioningComplete)
    {
        const FabricTable & fabrics = Server::GetInstance().GetFabricTable();
        const FabricInfo * fabric   = fabrics.FindFabricWithIndex(event->CommissioningComplete.fabricIndex);
        char network[96];
        char line[200];

        ProgressDone(kStepComplete);
        // Runs with the stack lock held, as NetworkText wants
        NetworkText(network, sizeof(network));
        snprintf(line, sizeof(line), "all OK: commissioned by vendor 0x%04X (fabric %u of %u), %s; controllers can use the "
                 "endpoints now", fabric != nullptr ? static_cast<unsigned>(fabric->GetVendorId()) : 0u,
                 static_cast<unsigned>(event->CommissioningComplete.fabricIndex),
                 static_cast<unsigned>(fabrics.FabricCount()), network);
        if (gUcxmHost != nullptr)
        {
            gUcxmHost->log(UCXAPP_MATTER_LOG_INFO, line);
        }
    }
    else if (event->Type == DeviceEventType::kFailSafeTimerExpired)
    {
        ProgressFailed(0, "the commissioner did not finish in time; the node is as before");
    }
}

void OnIdentify(::Identify * identify);
void OnIdentifyStop(::Identify *) {}

// The Identify cluster wants one of these for each endpoint of ucxmatter.zap that has it
using IdentifyType = app::Clusters::Identify::IdentifyTypeEnum;
::Identify gIdentifyNode(0, OnIdentify, OnIdentifyStop, IdentifyType::kNone);
::Identify gIdentifyLight(1, OnIdentify, OnIdentifyStop, IdentifyType::kVisibleIndicator);
::Identify gIdentifyContact(3, OnIdentify, OnIdentifyStop, IdentifyType::kNone);

void OnIdentify(::Identify * identify)
{
    gEvent(identify == &gIdentifyLight ? "matter: identify the light"
               : identify == &gIdentifyContact ? "matter: identify the contact sensor" : "matter: identify the node");
}

// A new node calls itself what the product is: Apple Home names the accessory after NodeLabel, which the SDK leaves
// empty until a controller writes one (a name given in the home app is kept)
void DefaultNodeLabel(PersistentStorageDelegate * storage)
{
    namespace BasicInformation = app::Clusters::BasicInformation;
    const StorageKeyName key   = DefaultStorageKeyAllocator::AttributeValue(kRootEndpointId, BasicInformation::Id,
                                                                            BasicInformation::Attributes::NodeLabel::Id);
    app::Storage::String<BasicInformation::Attributes::NodeLabel::TypeInfo::MaxLength()> label;

    if (storage == nullptr || storage->SyncDoesKeyExist(key.KeyName()) ||
        !label.SetContent(CharSpan::fromCharString(ucxmatter::GetDeviceConfig().deviceName.c_str())))
    {
        return;
    }
    const ByteSpan value = app::Storage::Internal::ShortStringOutputAdapter(label).ContentWithPrefix();
    (void) storage->SyncSetKeyValue(key.KeyName(), value.data(), static_cast<uint16_t>(value.size()));
}

// The Matter server on the stack: the data model, attestation and, without a fabric, an open commissioning window
const char * ServerStart()
{
    static CommonCaseDeviceServerInitParams sParams;
    StackLock lock;

    // The SDK's test certificates, which commissioners accept for the test vendor only
    Credentials::SetDeviceAttestationCredentialsProvider(Credentials::Examples::GetExampleDACProvider());
    if (sParams.InitializeStaticResourcesBeforeServerInit() != CHIP_NO_ERROR)
    {
        return "the Matter server got no resources";
    }
    DefaultNodeLabel(sParams.persistentStorageDelegate);
    sParams.appDelegate                        = &gCommissioning;
    sParams.advertiseCommissionableIfNoFabrics = true;
    sParams.discoveryTimeout                   = System::Clock::Seconds16(kWindowSeconds);
    app::CodegenDataModelProvider::Instance().SetPersistentStorageDelegate(sParams.persistentStorageDelegate);
    sParams.dataModelProvider = &app::CodegenDataModelProvider::Instance();
    if (Server::GetInstance().Init(sParams) != CHIP_NO_ERROR)
    {
        return "the Matter server did not start (matter.log detail shows why)";
    }
    gServerUp = true;
    SyncModel();
    (void) Server::GetInstance().GetFabricTable().AddFabricDelegate(&gCommissioning);
    (void) PlatformMgr().AddEventHandler(OnDeviceEvent, 0);
    if (gWiFi)
    {
        // A node that was commissioned before joins its network again here
        WiFiDriver::GetInstance().SetStepHook(OnWiFiStep);
        if (gWiFiCommissioning.Init() != CHIP_NO_ERROR)
        {
            return "the Matter server got no Wi-Fi driver";
        }
        gWiFiUp = true;
    }
    return nullptr;
}

// The codes of gCommissionableData, for the caller and for what the platform files read (DeviceConfig.h)
void SetupTake(NodeSetup & setup)
{
    ucxmatter::DeviceConfig & config = ucxmatter::GetDeviceConfig();
    char unique[2 * ucxmatter::UbloxCommissionableDataProvider::kUniqueIdLen + 1];

    gCommissionableData.UniqueId(unique, sizeof(unique));
    setup.discriminator  = gCommissionableData.Discriminator();
    setup.passcode       = gCommissionableData.Passcode();
    config.discriminator = setup.discriminator;
    config.uniqueId      = unique;
}

// "3.5.0-120" as 0x03050078: a newer firmware has the larger number
uint32_t FirmwareNumber(const char * text)
{
    unsigned part[4] = { 0, 0, 0, 0 };
    uint32_t number  = 0;

    (void) sscanf(text, "%u.%u.%u-%u", &part[0], &part[1], &part[2], &part[3]);
    for (unsigned p : part)
    {
        number = (number << 8) | (p < 255 ? p : 255);
    }
    return number != 0 ? number : 1;
}

} // namespace

const char * NodeStart(const NodeModule & module, bool wifi, NodeEvent event, NodeSetup & setup)
{
    ucxmatter::DeviceConfig & config = ucxmatter::GetDeviceConfig();
    const char * failed;

    gEvent = event;
    gWiFi  = wifi;
    if (Platform::MemoryInit() != CHIP_NO_ERROR)
    {
        return "the Matter SDK got no memory";
    }
    if (gCommissionableData.Init() != CHIP_NO_ERROR)
    {
        return "the Matter SDK made no setup codes (no random numbers, or no passcode verifier)";
    }
    SetCommissionableDataProvider(&gCommissionableData);
    SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
    SetupTake(setup);
    if (GetDeviceInstanceInfoProvider()->GetVendorId(setup.vendorId) != CHIP_NO_ERROR ||
        GetDeviceInstanceInfoProvider()->GetProductId(setup.productId) != CHIP_NO_ERROR)
    {
        return "the Matter SDK gave no vendor and product";
    }
    config.macSuffix = module.macSuffix;
    config.vendorId  = setup.vendorId;
    config.productId = setup.productId;
    // Basic Information: the module is the node's hardware and its firmware the node's software
    config.serialNumber          = std::string(module.serial).substr(0, 32);
    config.partNumber            = std::string(module.typeCode).substr(0, 32);
    config.hardwareVersionString = module.typeCode[0] == '\0' ? std::string(module.model).substr(0, 64)
                                                              : std::string(module.typeCode).substr(0, 64);
    if (module.firmware[0] != '\0')
    {
        config.softwareVersionString = std::string(module.firmware).substr(0, 64);
        config.softwareVersion       = FirmwareNumber(module.firmware);
    }

    // Defines the Matter service on the module's GATT server. A stack that fails halfway (the module's GATT table
    // full) is shut down by NodeStop as well: its system layer, left up, aborts when the plug-in is unloaded
    gStackUp = true;
    if (PlatformMgr().InitChipStack() != CHIP_NO_ERROR)
    {
        return "the Matter stack did not start on the module (matter.log detail shows why)";
    }
    PlatformMgrImpl().SetAppLoopCallback([]() { PersistedStorage::KeyValueStoreManagerImpl::MaybeFlush(); });
    failed = ServerStart();
    if (failed != nullptr)
    {
        return failed;
    }
    if (PlatformMgr().StartEventLoopTask() != CHIP_NO_ERROR)
    {
        return "the Matter thread did not start";
    }
    gLoopUp = true;
    if (NodeFabrics() > 0)
    {
        return nullptr; // commissioned: it advertises again only after matter.open
    }
    for (int i = 0; i < 500 && !NodeAdvertising(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return NodeAdvertising() ? nullptr : "the module does not advertise the Matter service (matter.log detail shows why)";
}

bool NodeGattFull()
{
    return Internal::BLEMgrImpl().GattFull();
}

void NodeStop()
{
    if (gLoopUp)
    {
        (void) PlatformMgr().StopEventLoopTask();
        gLoopUp = false;
    }
    if (gStackUp)
    {
        {
            StackLock lock;

            gDemo = false;
            SystemLayer().CancelTimer(DemoTick, nullptr);
            if (gWiFiUp)
            {
                gWiFiCommissioning.Shutdown();
                gWiFiUp = false;
            }
            if (gServerUp)
            {
                Server::GetInstance().Shutdown();
                gServerUp = false;
            }
            Internal::BLEMgr().Shutdown(); // takes the advertisement down and drops the link

            // What the stack should have given back already; on the module these outlive the node otherwise
            auto & ucx = UcxClientManager::GetInstance();
            (void) ucx.MdnsStop();
            const int left = ucx.CloseUdpSockets();
            if (left > 0 && gUcxmHost != nullptr)
            {
                char line[80];

                snprintf(line, sizeof(line), "matter: closed %d UDP socket(s) the stack left open", left);
                gUcxmHost->log(UCXAPP_MATTER_LOG_INFO, line);
            }
        }
        (void) PersistedStorage::KeyValueStoreManagerImpl::FlushNow();
        PlatformMgr().Shutdown();
        gStackUp = false;
    }
    Platform::MemoryShutdown();
}

bool NodeUrc(const char * line, size_t length)
{
    return UcxClientManager::GetInstance().QueueUrc(line, length);
}

bool NodeAdvertising()
{
    StackLock lock;

    return gStackUp && ConnectivityMgr().IsBLEAdvertising();
}

int NodeLinks()
{
    StackLock lock;

    return gStackUp ? static_cast<int>(ConnectivityMgr().NumBLEConnections()) : 0;
}

int NodeFabrics()
{
    StackLock lock;

    return gServerUp ? static_cast<int>(Server::GetInstance().GetFabricTable().FabricCount()) : 0;
}

bool NodeWindowOpen()
{
    StackLock lock;

    return gServerUp && Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
}

bool NodeOpenWindow(unsigned seconds)
{
    StackLock lock;

    if (!gServerUp || seconds < 180 || seconds > kWindowSeconds)
    {
        return false;
    }
    auto & window = Server::GetInstance().GetCommissioningWindowManager();

    return window.IsCommissioningWindowOpen() ||
        window.OpenBasicCommissioningWindow(System::Clock::Seconds16(static_cast<uint16_t>(seconds))) == CHIP_NO_ERROR;
}

bool NodeRandomOk()
{
    static const uint8_t kZero[16] = {};
    uint8_t a[16]                  = {};
    uint8_t b[16]                  = {};

    return Crypto::DRBG_get_bytes(a, sizeof(a)) == CHIP_NO_ERROR && Crypto::DRBG_get_bytes(b, sizeof(b)) == CHIP_NO_ERROR &&
        memcmp(a, b, sizeof(a)) != 0 && memcmp(a, kZero, sizeof(a)) != 0;
}

void NodeCryptoCheck(CryptoReport report, void * context)
{
    StackLock lock;

    CryptoCheck(report, context);
}

// With the stack lock
static void NetworkText(char * out, size_t size)
{
    UcxClientManager & module = UcxClientManager::GetInstance();
    const uint8_t * ip        = module.GetIpv4Address();

    if (!gWiFi)
    {
        snprintf(out, size, "Thread is not in this plug-in yet: a commissioner pairs over Bluetooth LE and stops "
                            "where it gives the node its network");
    }
    else if (!gStackUp || !module.IsNetworkReady())
    {
        snprintf(out, size, "Wi-Fi down; a commissioner gives the node its network, or wifi.connect does");
    }
    else if (ip != nullptr)
    {
        snprintf(out, size, "Wi-Fi up, %u.%u.%u.%u, channel %u", ip[0], ip[1], ip[2], ip[3],
                 static_cast<unsigned>(module.GetWiFiChannel()));
    }
    else
    {
        snprintf(out, size, "Wi-Fi up (IPv6 only), channel %u", static_cast<unsigned>(module.GetWiFiChannel()));
    }
}

void NodeNetwork(char * out, size_t size)
{
    StackLock lock;

    NetworkText(out, size);
}

bool NodeSnapshot(NodeState & state)
{
    if (!gStackUp || !PlatformMgr().TryLockChipStack())
    {
        return false;
    }
    state.fabrics     = gServerUp ? static_cast<int>(Server::GetInstance().GetFabricTable().FabricCount()) : 0;
    state.links       = static_cast<int>(ConnectivityMgr().NumBLEConnections());
    state.advertising = ConnectivityMgr().IsBLEAdvertising();
    state.windowOpen  = gServerUp && Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    NetworkText(state.network, sizeof(state.network));
    PlatformMgr().UnlockChipStack();
    return true;
}

void NodeAttributeChanged(uint16_t endpoint, uint32_t cluster, uint32_t attribute, const uint8_t * value, uint16_t size)
{
    char line[64];
    Endpoint * e = EndpointOfModel(endpoint);

    // not while the server sets its start values, nor for what the node writes itself
    if (!gLoopUp || gOwnWrite || value == nullptr || size < 1)
    {
        return;
    }
    if (cluster == app::Clusters::OnOff::Id && attribute == app::Clusters::OnOff::Attributes::OnOff::Id)
    {
        if (e != nullptr)
        {
            e->value[0] = value[0] != 0 ? 1 : 0;
        }
        snprintf(line, sizeof(line), "matter: endpoint %u switched %s", static_cast<unsigned>(endpoint),
                 value[0] != 0 ? "on" : "off");
        gEvent(line);
    }
    else if (cluster == app::Clusters::LevelControl::Id &&
             attribute == app::Clusters::LevelControl::Attributes::CurrentLevel::Id && value[0] != 0xFF)
    {
        // 1 to 254 on the wire, 0xFF is null
        const unsigned percent = (static_cast<unsigned>(value[0]) * 100 + 127) / 254;

        if (e != nullptr)
        {
            e->value[1] = static_cast<int32_t>(std::max(1u, percent));
        }
        snprintf(line, sizeof(line), "matter: endpoint %u level %u%%", static_cast<unsigned>(endpoint), percent);
        gEvent(line);
    }
}

void NodeEndpointChanged(const Endpoint & endpoint)
{
    if (!gStackUp || !gServerUp)
    {
        return;
    }
    StackLock lock;

    WriteModel(endpoint);
}

bool NodeDemo(bool on)
{
    if (!gLoopUp)
    {
        return false;
    }
    StackLock lock;

    SystemLayer().CancelTimer(DemoTick, nullptr);
    gDemo     = on;
    gDemoStep = 0;
    return !on || SystemLayer().StartTimer(System::Clock::Milliseconds32(kDemoStepMs), DemoTick, nullptr) == CHIP_NO_ERROR;
}

bool NodeDemoOn()
{
    return gDemo;
}

} // namespace ucxm
