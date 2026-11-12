/* mDNS on the module: the AT+UMD* wrappers of u-connectMatter's UcxClientManager, on the app's AT client. The
 * module answers the queries itself, so the host has no multicast socket and is not woken by the network. */

#include "UcxClientManager.h"

#include "PlatformAbstraction.h"
#include "ucxm_host.h"

#include <lib/support/logging/CHIPLogging.h>

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "u_cx_general.h"
#include "u_cx_mdns.h"
}

namespace chip {
namespace DeviceLayer {

namespace {

// Setting the host name and enabling the responder can take seconds right after Wi-Fi came up
constexpr int32_t kMdnsTimeoutMs = 10000;

// Our index to the service id the module gave
int32_t sServiceIds[UcxClientManager::kMaxMdnsServices] = { -1, -1, -1 };
bool sEnabled;                            // mirrors AT+UMDE
std::atomic<bool> sResponderDown{ true }; // set by +UEMDD
char sHostname[40];                       // last one sent with AT+UMDHN

// What the module had before the node touched its mDNS (the app's mdns.* commands use it too): MdnsStop puts it back
bool sFound;
bool sFoundEnabled;
char sFoundHostname[sizeof(sHostname)];

void NoteFound(uCxHandle_t * ucx)
{
    uEnabled_t enabled = U_ENABLED_NO;
    const char * name  = nullptr;

    if (sFound)
    {
        return;
    }
    sFound        = true;
    sFoundEnabled = uCxMdnsGetEnabled(ucx, &enabled) == 0 && enabled == U_ENABLED_YES;
    if (uCxMdnsGetHostnameBegin(ucx, &name) && name != nullptr)
    {
        snprintf(sFoundHostname, sizeof(sFoundHostname), "%s", name);
    }
    (void) uCxEnd(ucx);
    sEnabled = sFoundEnabled;
}

class LongTimeout
{
public:
    LongTimeout() : mPrevious(gUcxmHost->at->setCommandTimeout(gUcxmHost->client, kMdnsTimeoutMs, false)) {}
    ~LongTimeout() { (void) gUcxmHost->at->setCommandTimeout(gUcxmHost->client, mPrevious, true); }

private:
    int32_t mPrevious;
};

// "key=value", or a key alone
void SplitTxt(const char * record, char * key, size_t keySize, const char *& value)
{
    const char * eq = strchr(record, '=');
    size_t length   = eq != nullptr ? static_cast<size_t>(eq - record) : strlen(record);

    if (length >= keySize)
    {
        length = keySize - 1;
    }
    memcpy(key, record, length);
    key[length] = '\0';
    value       = eq != nullptr ? eq + 1 : "";
}

} // namespace

bool UcxClientManager::MdnsIsEnabled() const
{
    return sEnabled;
}

void UcxClientManager::MdnsNoteUrc(const char * line, size_t length)
{
    if (length >= 6 && strncmp(line, "+UEMDD", 6) == 0)
    {
        sResponderDown = true;
    }
}

CHIP_ERROR UcxClientManager::MdnsStart(const char * hostname)
{
    uCxHandle_t * ucx = GetUcxHandle();
    char name[sizeof(sHostname)];

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    NoteFound(ucx);

    // The SDK passes the canonical name (12 hex digits of the MAC), the target of its SRV records
    if (hostname != nullptr && hostname[0] != '\0')
    {
        snprintf(name, sizeof(name), "%s", hostname);
    }
    else
    {
        snprintf(name, sizeof(name), "ucx-matter-%s", GetMacSuffix());
    }
    if (sHostname[0] != '\0' && strcmp(sHostname, name) == 0)
    {
        return CHIP_NO_ERROR;
    }
    ChipLogProgress(DeviceLayer, "[mDNS] Setting hostname to %s.local", name);

    // The module may be busy right after Wi-Fi connects (DHCP, multicast join)
    if (uCxGeneralAttention(ucx) != 0)
    {
        Platform::SleepMs(2000);
        VerifyOrReturnError(uCxGeneralAttention(ucx) == 0, CHIP_ERROR_INTERNAL);
    }

    int32_t ret;
    {
        LongTimeout timeout;

        ret = uCxMdnsSetHostname(ucx, name);
    }
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Failed to set hostname: error %d", static_cast<int>(ret));
        sHostname[0] = '\0';
        return CHIP_ERROR_INTERNAL;
    }
    snprintf(sHostname, sizeof(sHostname), "%s", name);

    // A new host name is only announced when the responder is enabled again: off now (if it runs), and the next
    // MdnsEnable(true) announces the name with the services
    return MdnsSuspend();
}

CHIP_ERROR UcxClientManager::MdnsEnable(bool enable)
{
    uCxHandle_t * ucx = GetUcxHandle();
    int32_t ret       = -1;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    NoteFound(ucx);
    if (sEnabled == enable)
    {
        return CHIP_NO_ERROR;
    }
    ChipLogProgress(DeviceLayer, "[mDNS] %s responder (AT+UMDE=%d)", enable ? "Enabling" : "Disabling", enable ? 1 : 0);
    {
        LongTimeout timeout;

        // Once more on failure: the module may need a moment after Wi-Fi up
        for (int attempt = 0; attempt < 2 && ret < 0; attempt++)
        {
            if (attempt > 0)
            {
                Platform::SleepMs(500);
            }
            ret = uCxMdnsSetEnabled(ucx, enable ? U_ENABLED_YES : U_ENABLED_NO);
        }
    }
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Failed to %s: error %d", enable ? "enable" : "disable", static_cast<int>(ret));
        return CHIP_ERROR_INTERNAL;
    }
    sEnabled = enable;
    if (!enable)
    {
        // A disabled responder has no services left on the module
        for (int32_t & id : sServiceIds)
        {
            id = -1;
        }
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR UcxClientManager::MdnsSuspend()
{
    uCxHandle_t * ucx = GetUcxHandle();
    int32_t ret;

    if (ucx == nullptr || !sEnabled)
    {
        return CHIP_NO_ERROR;
    }
    {
        LongTimeout timeout;

        ret = uCxMdnsSetEnabled(ucx, U_ENABLED_NO);
    }
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Suspend failed: error %d", static_cast<int>(ret));
        return CHIP_ERROR_INTERNAL;
    }

    // The service ids stay: the module keeps the services, and MdnsEnable(true) announces them again
    sEnabled = false;
    return CHIP_NO_ERROR;
}

CHIP_ERROR UcxClientManager::MdnsForceReannounce()
{
    uCxHandle_t * ucx = GetUcxHandle();
    int32_t ret;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    if (!sEnabled)
    {
        return MdnsEnable(true);
    }
    ChipLogProgress(DeviceLayer, "[mDNS] Forcing re-announce (AT+UMDE=0 then AT+UMDE=1)");

    LongTimeout timeout;

    sResponderDown = false;
    ret            = uCxMdnsSetEnabled(ucx, U_ENABLED_NO);
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Force re-announce: disable failed (error %d)", static_cast<int>(ret));
        return CHIP_ERROR_INTERNAL;
    }

    // +UEMDD comes after the OK; enabling before it looks like the responder went down by itself
    const uint32_t start = Platform::GetTickMs();
    while (!sResponderDown && Platform::ElapsedMs(start) <= 2000)
    {
        Platform::SleepMs(10);
    }
    ret = uCxMdnsSetEnabled(ucx, U_ENABLED_YES);
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Force re-announce: re-enable failed (error %d)", static_cast<int>(ret));
        sEnabled = false;
        return CHIP_ERROR_INTERNAL;
    }
    return CHIP_NO_ERROR;
}

// The node's services go; the responder and the host name are left as the node found them
CHIP_ERROR UcxClientManager::MdnsStop()
{
    uCxHandle_t * ucx = GetUcxHandle();

    if (ucx == nullptr || !sFound)
    {
        return CHIP_NO_ERROR;
    }
    (void) MdnsClearServices();
    if (sHostname[0] != '\0' && sFoundHostname[0] != '\0' && strcmp(sHostname, sFoundHostname) != 0)
    {
        LongTimeout timeout;

        (void) uCxMdnsSetHostname(ucx, sFoundHostname);
    }
    sHostname[0] = '\0';

    const CHIP_ERROR err = MdnsEnable(sFoundEnabled);

    sFound = false;
    return err;
}

CHIP_ERROR UcxClientManager::MdnsConfigureService(int32_t index, const char * serviceName, const char * serviceType,
                                                  uint16_t port, const char * const * txtRecords, size_t txtCount)
{
    uCxHandle_t * ucx = GetUcxHandle();
    char bareType[64];
    const char * suffix;
    uMdnsProtocol_t protocol = U_MDNS_PROTOCOL_TCP;
    int32_t serviceId        = -1;
    int32_t ret;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(index >= 0 && static_cast<size_t>(index) < kMaxMdnsServices, CHIP_ERROR_INVALID_ARGUMENT);

    if (sServiceIds[index] >= 0)
    {
        (void) uCxMdnsServiceRemove(ucx, sServiceIds[index]);
        sServiceIds[index] = -1;
    }

    // "_matterc._udp" is type "_matterc" over UDP for the module
    snprintf(bareType, sizeof(bareType), "%s", serviceType);
    suffix = strstr(serviceType, "._udp");
    if (suffix != nullptr)
    {
        protocol = U_MDNS_PROTOCOL_UDP;
    }
    else
    {
        suffix = strstr(serviceType, "._tcp");
    }
    if (suffix != nullptr && static_cast<size_t>(suffix - serviceType) < sizeof(bareType))
    {
        bareType[suffix - serviceType] = '\0';
    }

    ret = uCxMdnsServiceAdd4(ucx, bareType, protocol, static_cast<int32_t>(port), serviceName, &serviceId);
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Failed to add service %s %s: error %d", serviceName, serviceType,
                     static_cast<int>(ret));
        return CHIP_ERROR_INTERNAL;
    }
    sServiceIds[index] = serviceId;
    ChipLogProgress(DeviceLayer, "[mDNS] Service %s %s port %u: id %d", serviceName, serviceType,
                    static_cast<unsigned>(port), static_cast<int>(serviceId));

    for (size_t i = 0; i < txtCount && txtRecords != nullptr && txtRecords[i] != nullptr; i++)
    {
        char key[65];
        const char * value;

        SplitTxt(txtRecords[i], key, sizeof(key), value);
        ret = uCxMdnsTxtRecordAdd(ucx, serviceId, key, value);
        if (ret < 0)
        {
            // The rest still goes in: some records are better than none
            ChipLogError(DeviceLayer, "[mDNS] Failed to add TXT record \"%s\": error %d", txtRecords[i],
                         static_cast<int>(ret));
        }
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR UcxClientManager::MdnsClearServices()
{
    uCxHandle_t * ucx = GetUcxHandle();
    bool failed       = false;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    for (int32_t & id : sServiceIds)
    {
        if (id >= 0)
        {
            failed = uCxMdnsServiceRemove(ucx, id) < 0 || failed;
            id     = -1;
        }
    }
    return failed ? CHIP_ERROR_INTERNAL : CHIP_NO_ERROR;
}

void UcxClientManager::MdnsRemoveService(int32_t index)
{
    uCxHandle_t * ucx = GetUcxHandle();

    if (ucx != nullptr && index >= 0 && static_cast<size_t>(index) < kMaxMdnsServices && sServiceIds[index] >= 0)
    {
        (void) uCxMdnsServiceRemove(ucx, sServiceIds[index]);
        sServiceIds[index] = -1;
    }
}

CHIP_ERROR UcxClientManager::MdnsUpdateTxtRecords(int32_t index, const char * const * txtRecords, size_t txtCount)
{
    uCxHandle_t * ucx = GetUcxHandle();
    int32_t ret;

    VerifyOrReturnError(ucx != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(index >= 0 && static_cast<size_t>(index) < kMaxMdnsServices && sServiceIds[index] >= 0,
                        CHIP_ERROR_INVALID_ARGUMENT);

    ret = uCxMdnsTxtRecordClear(ucx, sServiceIds[index]);
    if (ret < 0)
    {
        ChipLogError(DeviceLayer, "[mDNS] Failed to clear TXT records: error %d", static_cast<int>(ret));
    }
    for (size_t i = 0; i < txtCount && txtRecords != nullptr && txtRecords[i] != nullptr; i++)
    {
        char key[65];
        const char * value;

        SplitTxt(txtRecords[i], key, sizeof(key), value);
        ret = uCxMdnsTxtRecordAdd(ucx, sServiceIds[index], key, value);
        if (ret < 0)
        {
            ChipLogError(DeviceLayer, "[mDNS] Failed to update TXT \"%s\": error %d", txtRecords[i], static_cast<int>(ret));
        }
    }
    return CHIP_NO_ERROR;
}

} // namespace DeviceLayer
} // namespace chip
