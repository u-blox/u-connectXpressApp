/* The Matter stack of the node. Kept apart from plugin.cpp for the reason given in onboarding.h: that file includes
 * u-connectClient, this side the Matter SDK. */
#ifndef UCXM_NODE_H
#define UCXM_NODE_H

#include <cstddef>
#include <cstdint>

namespace ucxm {

// Filled by NodeStart. Discriminator and passcode are the node's own, drawn at random on its first start and
// kept in the store (UbloxCommissionableDataProvider.h); vendor and product are what the node advertises and
// reports.
struct NodeSetup
{
    uint16_t vendorId;
    uint16_t productId;
    uint16_t discriminator;
    uint32_t passcode;
};

// What the module says about itself, for the Basic Information cluster; "" where it says nothing
struct NodeModule
{
    const char * macSuffix; // the last three bytes of its Bluetooth address as six hex digits
    const char * model;     // "NORA-W36"
    const char * typeCode;  // "NORA-W361-00B"
    const char * serial;
    const char * firmware;  // "3.5.0-120"
};

// One line for the app's Events pane; called on the Matter thread
typedef void (*NodeEvent)(const char * line);

// Brings the Matter server up on the module; without a fabric it opens a commissioning window and advertises.
// wifi: the module has Wi-Fi, the network a commissioner then gives the node. nullptr when the node runs, else
// what failed.
const char * NodeStart(const NodeModule & module, bool wifi, NodeEvent event, NodeSetup & setup);
void NodeStop();

// On the AT client's thread: queues the URC for the Matter thread. True when it is the node's alone.
bool NodeUrc(const char * line, size_t length);

bool NodeAdvertising();
int NodeLinks();   // Bluetooth LE links to the node
int NodeFabrics(); // 0: not commissioned
bool NodeWindowOpen();
bool NodeOpenWindow(unsigned seconds = 900); // commissioning window, 180 to 900 s; false when the node refuses
bool NodeRandomOk();                          // matter.selftest: the SDK's DRBG gives bytes, and not the same twice
// matter.selftest: every crypto primitive the node uses against known answers, one report per primitive
typedef void (*CryptoReport)(void * context, const char * name, bool ok, const char * detail);
void NodeCryptoCheck(CryptoReport report, void * context); // with the stack lock
void CryptoCheck(CryptoReport report, void * context);     // crypto_check.cpp
bool SetupCodesAllowed(uint32_t passcode, uint16_t discriminator); // no test or forbidden code (Matter 5.1.7.1)
void NodeNetwork(char * out, size_t size); // one line for matter.status: the node's network on the module

// The node for the live card, without waiting for the Matter thread: false when that thread is busy (the caller
// keeps what it had)
struct NodeState
{
    int fabrics;
    int links;
    bool advertising;
    bool windowOpen;
    char network[160];
};
bool NodeSnapshot(NodeState & state);

// The data model's callback (callbacks.cpp), on the Matter thread: a controller or the node changed an attribute
void NodeAttributeChanged(uint16_t endpoint, uint32_t cluster, uint32_t attribute, const uint8_t * value,
                          uint16_t size);

} // namespace ucxm

#endif // UCXM_NODE_H
