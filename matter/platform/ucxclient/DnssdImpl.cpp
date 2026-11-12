/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 */

#include <platform/CHIPDeviceLayer.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <lib/dnssd/platform/Dnssd.h>
#include <system/SystemClock.h>
#include "UcxClientManager.h"
#include "VerboseLogging.h"
#include <string.h>

namespace chip {
namespace Dnssd {

// mDNS via NORA-W36 using firmware 3.4.0+ official AT+UMD* commands
// ALL network traffic goes through NORA-W36 exclusively
// NO Windows Winsock2 or hybrid implementations
//
// TWO-PHASE COMMIT (CRITICAL for proper gratuitous announcement):
//   Phase 1 — staging (this file, in PublishService):
//     - MdnsStart()              — set hostname only, responder stays DISABLED
//     - MdnsConfigureService()   — add service + TXT records to firmware
//     - MdnsUpdateTxtRecords()   — update TXT records on existing service
//     None of these enable the responder, so no premature/empty announcement.
//   Phase 2 — commit (FinalizeServiceUpdate):
//     - MdnsEnable(true)         — enable the responder. NORA-W36 then fires a
//                                  gratuitous mDNS announcement carrying the
//                                  hostname + all services + TXT records in one
//                                  burst. Idempotent: no-op when already enabled.

// Track configured services to avoid redundant AT commands
// NORA-W36 FW supports up to 3 mDNS services (Matter needs max 2: commissioning + operational)
static constexpr size_t kMaxServices = 3;
static constexpr size_t kMaxServiceNameLen = 64;  // FW: MAX_MDNS_INSTANCE_NAME_LEN=63
static constexpr size_t kMaxServiceTypeLen = 64;  // FW: MAX_MDNS_SERVICE_TYPE_LEN=64
static constexpr size_t kMaxTxtRecords    = 10;   // Max TXT records per service
static constexpr size_t kMaxTxtLen        = 64;   // Max "key=value" length

// Pending AT operation per service slot — flushed by ChipDnssdFinalizeServiceUpdate.
// This keeps PublishService() off the AT path so it returns instantly when called
// from inside an SDK command handler (e.g. AddNOC during multi-fabric commissioning).
enum class PendingMdnsOp : uint8_t
{
    kNone        = 0,
    kConfigure   = 1,  // Call MdnsConfigureService(slot, name, type, port, txt[])
    kUpdateTxt   = 2,  // Call MdnsUpdateTxtRecords(slot, txt[])
};

// Grace period: don't remove services added less than this many ms ago.
// During multi-admin commissioning, the SDK may trigger a service update cycle
// that only includes committed fabrics. A pending fabric's operational service
// (published during AddNOC) would be removed as "stale" before the new controller
// can discover it via mDNS and establish a CASE session.
static constexpr uint64_t kServiceGracePeriodMs = 90000;  // 90s covers typical fail-safe window

struct ConfiguredService {
    bool inUse;
    bool republished;  // Set true by PublishService during an update cycle
    char name[kMaxServiceNameLen];
    char type[kMaxServiceTypeLen];
    uint16_t port;
    uint32_t txtHash;     // FNV-1a hash of all "key=value" TXT records — used to dedup TXT writes
    uint64_t addedAtMs;   // Monotonic timestamp when service was first added

    // Deferred AT-command state — written by PublishService, consumed by
    // FinalizeServiceUpdate (or DeferredAutoFinalize). Keeps the AT-command
    // burst (~3.5s of AT+UMDSA + AT+UMDTRA) off the SDK callback path.
    PendingMdnsOp pendingOp;
    char          pendingTxt[kMaxTxtRecords][kMaxTxtLen];
    size_t        pendingTxtCount;
};
static ConfiguredService sConfiguredServices[kMaxServices] = {};

// True if PublishService modified firmware state during the current SDK update
// cycle (added/removed/changed a service or its TXT records). FinalizeServiceUpdate
// uses this to decide whether to bother calling MdnsEnable(true). The responder
// itself is idempotent so this is purely an optimisation, but it also lets us log
// "no-op transaction" cleanly.
static bool sTransactionDirty = false;

// True when a proper RemoveServices() has been called — indicates the SDK owns
// the transaction lifecycle and will call FinalizeServiceUpdate() itself.
// When false, PublishService() was called outside a RemoveServices→Finalize cycle
// (e.g., from the kOperationalNetworkEnabled handler in the SDK which calls
// AdvertiseOperational() directly without the RemoveServices/Finalize wrapper).
// In that case, we schedule a deferred auto-finalize so the responder is properly
// updated even when the SDK omits the Finalize call.
static bool sInTransaction = false;

// Deferred auto-finalize callback — runs on next event loop iteration if the SDK
// didn't call FinalizeServiceUpdate() after a PublishService() outside a transaction.
static void DeferredAutoFinalize(intptr_t /* unused */)
{
    if (!sTransactionDirty)
    {
        // SDK already finalized (or nothing changed) — no-op.
        return;
    }
    ChipLogProgress(DeviceLayer, "[mDNS] Auto-finalize: SDK did not call FinalizeServiceUpdate() — committing now");
    (void) ChipDnssdFinalizeServiceUpdate();
}

// FNV-1a hash over an array of NUL-terminated "key=value" TXT records.
// Order-sensitive (Matter SDK passes records in stable order), small (~32 bytes/record)
// so collisions are extremely unlikely in our use case.
static uint32_t HashTxtRecords(const char * const * records, size_t count)
{
    uint32_t h = 0x811C9DC5u;  // FNV offset basis
    for (size_t i = 0; i < count; i++)
    {
        const char * p = records[i];
        if (!p) { continue; }
        for (; *p; ++p)
        {
            h ^= static_cast<uint8_t>(*p);
            h *= 0x01000193u;  // FNV prime
        }
        // Record separator so {"a","bc"} hashes differently from {"ab","c"}
        h ^= 0xFFu;
        h *= 0x01000193u;
    }
    return h;
}

// Helper to find a service slot by name
static int FindServiceSlot(const char* name)
{
    for (size_t i = 0; i < kMaxServices; i++)
    {
        if (sConfiguredServices[i].inUse && strcmp(sConfiguredServices[i].name, name) == 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Helper to find a free service slot
static int FindFreeSlot()
{
    for (size_t i = 0; i < kMaxServices; i++)
    {
        if (!sConfiguredServices[i].inUse)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Helper to check if a service matches what's already configured
static bool ServiceMatches(int slot, const char* name, const char* type, uint16_t port)
{
    if (slot < 0 || slot >= static_cast<int>(kMaxServices) || !sConfiguredServices[slot].inUse)
    {
        return false;
    }
    return strcmp(sConfiguredServices[slot].name, name) == 0 &&
           strcmp(sConfiguredServices[slot].type, type) == 0 &&
           sConfiguredServices[slot].port == port;
}

// Stash TXT records ("key=value" strings) into a slot's pendingTxt buffer.
// Returns the count actually copied (capped at kMaxTxtRecords).
static size_t StashPendingTxt(ConfiguredService & slot, const char * const * records, size_t count)
{
    size_t n = (count > kMaxTxtRecords) ? kMaxTxtRecords : count;
    for (size_t i = 0; i < n; i++)
    {
        if (records[i])
        {
            strncpy(slot.pendingTxt[i], records[i], kMaxTxtLen - 1);
            slot.pendingTxt[i][kMaxTxtLen - 1] = '\0';
        }
        else
        {
            slot.pendingTxt[i][0] = '\0';
        }
    }
    slot.pendingTxtCount = n;
    return n;
}

// Flush all pending mDNS AT commands previously staged by PublishService.
// Called from ChipDnssdFinalizeServiceUpdate (either synchronously by the SDK
// or via DeferredAutoFinalize on the next event loop iteration).
//
// Doing the AT burst here — not in PublishService — keeps the SDK callback path
// fast (< 1ms instead of ~3.5s for a fresh fabric publish) and lets InvokeCommandResponse
// for AddNOC reach the controller before its 30s ArmFailSafe timeout fires.
static void FlushPendingMdnsOperations()
{
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();

    // Determine if any slot has work pending.
    bool anyPending = false;
    for (size_t i = 0; i < kMaxServices; i++)
    {
        if (sConfiguredServices[i].pendingOp != PendingMdnsOp::kNone)
        {
            anyPending = true;
            break;
        }
    }
    if (!anyPending) { return; }

    // If the responder is currently enabled, suspend it so we don't serve
    // partial/inconsistent service data while flushing. Inside an SDK transaction
    // RemoveServices() already suspended it — this is a no-op then.
    if (ucx.MdnsIsEnabled())
    {
        (void) ucx.MdnsSuspend();
    }

    for (size_t i = 0; i < kMaxServices; i++)
    {
        ConfiguredService & s = sConfiguredServices[i];
        if (s.pendingOp == PendingMdnsOp::kNone) { continue; }

        // Build const-pointer view of the stashed records.
        const char * txt[kMaxTxtRecords];
        for (size_t j = 0; j < s.pendingTxtCount; j++) { txt[j] = s.pendingTxt[j]; }

        if (s.pendingOp == PendingMdnsOp::kConfigure)
        {
            ChipLogProgress(DeviceLayer, "[mDNS] Flushing deferred Configure: slot %u name=%s type=%s port=%u txt=%u",
                            static_cast<unsigned>(i), s.name, s.type,
                            static_cast<unsigned>(s.port), static_cast<unsigned>(s.pendingTxtCount));
            CHIP_ERROR err = ucx.MdnsConfigureService(static_cast<int32_t>(i), s.name, s.type, s.port,
                                                     s.pendingTxtCount > 0 ? txt : nullptr,
                                                     s.pendingTxtCount);
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(DeviceLayer, "[mDNS] Deferred Configure failed for slot %u (err=%" CHIP_ERROR_FORMAT ")",
                             static_cast<unsigned>(i), err.Format());
                // Mark slot unused so a future PublishService can retry from scratch.
                s.inUse = false;
            }
        }
        else if (s.pendingOp == PendingMdnsOp::kUpdateTxt)
        {
            ChipLogProgress(DeviceLayer, "[mDNS] Flushing deferred UpdateTxt: slot %u txt=%u",
                            static_cast<unsigned>(i), static_cast<unsigned>(s.pendingTxtCount));
            (void) ucx.MdnsUpdateTxtRecords(static_cast<int32_t>(i),
                                     s.pendingTxtCount > 0 ? txt : nullptr,
                                     s.pendingTxtCount);
        }

        s.pendingOp        = PendingMdnsOp::kNone;
        s.pendingTxtCount  = 0;
    }
}

CHIP_ERROR ChipDnssdInit(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback errorCallback, void * context)
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdInit() - Initializing NORA-W36 mDNS");

    // Clear service tracking
    for (size_t i = 0; i < kMaxServices; i++)
    {
        sConfiguredServices[i].inUse = false;
        sConfiguredServices[i].republished = false;
        sConfiguredServices[i].txtHash = 0;
        sConfiguredServices[i].pendingOp = PendingMdnsOp::kNone;
        sConfiguredServices[i].pendingTxtCount = 0;
    }
    sTransactionDirty = false;

    if (initCallback)
    {
        initCallback(context, CHIP_NO_ERROR);
    }

    return CHIP_NO_ERROR;
}

void ChipDnssdShutdown()
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdShutdown()");

    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();

    // Disable responder (idempotent — no-op if already disabled)
    (void) ucx.MdnsStop();

    // Clear all services on NORA-W36
    (void) ucx.MdnsClearServices();

    // Clear tracking state
    for (size_t i = 0; i < kMaxServices; i++)
    {
        sConfiguredServices[i].inUse = false;
        sConfiguredServices[i].republished = false;
        sConfiguredServices[i].txtHash = 0;
        sConfiguredServices[i].pendingOp = PendingMdnsOp::kNone;
        sConfiguredServices[i].pendingTxtCount = 0;
    }
    sTransactionDirty = false;
}

CHIP_ERROR ChipDnssdPublishService(const DnssdService * service, DnssdPublishCallback callback, void * context)
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdPublishService()");
    
    if (!service)
    {
        ChipLogError(DeviceLayer, "[mDNS] ERROR: service is NULL!");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    ChipLogProgress(DeviceLayer, "[mDNS] Service type: %s", service->mType);
    ChipLogProgress(DeviceLayer, "[mDNS] Service name: %s", service->mName);
    ChipLogProgress(DeviceLayer, "[mDNS] Port: %u", service->mPort);
    ChipLogProgress(DeviceLayer, "[mDNS] TXT entry count: %u", (unsigned)service->mTextEntrySize);
    
    // Build TXT records array — passed as "key=value" strings to UcxClientManager,
    // which sends individual AT+UMDTRA commands per record (firmware 3.4.0+).
    // Matter commissioning uses up to ~8 TXT records (D, CM, VP, DT, DN, RI, PH, PI)
    char txtBuffer[kMaxTxtRecords][kMaxTxtLen];
    const char * txtRecords[kMaxTxtRecords];
    size_t txtCount = 0;
    
    if (service->mTextEntrySize > kMaxTxtRecords)
    {
        ChipLogError(DeviceLayer, "[mDNS] WARNING: %u TXT entries requested but firmware supports only %u — extras will be DROPPED",
                     static_cast<unsigned>(service->mTextEntrySize), static_cast<unsigned>(kMaxTxtRecords));
    }
    for (size_t i = 0; i < service->mTextEntrySize && txtCount < kMaxTxtRecords; i++)
    {
        const auto& entry = service->mTextEntries[i];
        if (entry.mKey && entry.mData && entry.mDataSize > 0)
        {
            // Format as "key=value"
            int written = snprintf(txtBuffer[txtCount], kMaxTxtLen, "%s=%.*s",
                                   entry.mKey,
                                   static_cast<int>(entry.mDataSize),
                                   reinterpret_cast<const char*>(entry.mData));
            if (written > 0 && static_cast<size_t>(written) < kMaxTxtLen)
            {
                txtRecords[txtCount] = txtBuffer[txtCount];
                ChipLogProgress(DeviceLayer, "[mDNS]   TXT[%u]: %s", (unsigned)txtCount, txtRecords[txtCount]);
                txtCount++;
            }
        }
        else if (entry.mKey)
        {
            // Key without value (flag)
            int written = snprintf(txtBuffer[txtCount], kMaxTxtLen, "%s", entry.mKey);
            if (written > 0 && static_cast<size_t>(written) < kMaxTxtLen)
            {
                txtRecords[txtCount] = txtBuffer[txtCount];
                ChipLogProgress(DeviceLayer, "[mDNS]   TXT[%u]: %s", (unsigned)txtCount, txtRecords[txtCount]);
                txtCount++;
            }
        }
    }
    
    // Use the SDK-provided protocol (kDnssdProtocolTcp or kDnssdProtocolUdp) — same as
    // ESP32/Linux. Avoids fragile string parsing of mType (which could change naming).
    const bool isUdp = (service->mProtocol == DnssdServiceProtocol::kDnssdProtocolUdp);
    char fullServiceType[64];
    snprintf(fullServiceType, sizeof(fullServiceType), "%s.%s",
             service->mType, isUdp ? "_udp" : "_tcp");

    // SUBTYPES — Matter advertises subtypes for commissioning (_S<short_disc>, _L<long_disc>,
    // _CM, _V<vendor>, _T<devicetype>) and operational (_I<compressed_fabric_id>). Each subtype
    // is a separate PTR record at <subtype>._sub.<service_type>._<proto>.local pointing to
    // the same instance.
    //
    // NORA-W36 firmware 3.4.0+ mDNS API does NOT yet expose subtype registration.
    // Without subtypes, controllers cannot find this device by short discriminator (QR code).
    // TODO(firmware): Request uCxMdnsSubtypeAdd(serviceId, subtype) API in NORA-W36 firmware.
    if (service->mSubTypeSize > 0)
    {
        // One-shot warning (Vegas 2026): the missing-subtype API is a firmware
        // limitation that does not change at runtime, so emit the warning + the
        // dropped-subtype list ONCE per process. Every advertisement refresh
        // afterwards logs at Detail level only.
        static bool sSubtypeWarningEmitted = false;
        if (!sSubtypeWarningEmitted)
        {
            sSubtypeWarningEmitted = true;
            // u-connectXpressApp: a known limit of the module, not a fault of this run: detail, not error
            ChipLogDetail(DeviceLayer, "[mDNS] %u subtypes requested but the module's mDNS has no subtypes: a commissioner cannot browse for the discriminator",
                         static_cast<unsigned>(service->mSubTypeSize));
            for (size_t i = 0; i < service->mSubTypeSize; i++)
            {
                ChipLogDetail(DeviceLayer, "[mDNS]   dropped subtype[%u]: %s._sub.%s",
                             static_cast<unsigned>(i), service->mSubTypes[i], fullServiceType);
            }
        }
        else
        {
            ChipLogDetail(DeviceLayer, "[mDNS] (suppressed) %u subtypes dropped on %s",
                          static_cast<unsigned>(service->mSubTypeSize), fullServiceType);
        }
    }

    ChipLogProgress(DeviceLayer, "[mDNS] Proto: %s, TXT records: %u, Full type: %s",
                    isUdp ? "UDP" : "TCP",
                    (unsigned)txtCount, fullServiceType);
    
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    
    // Check if network is ready - if not, we can't configure mDNS yet
    // The service will be published when kInterfaceIpAddressChanged event is received
    if (!ucx.IsNetworkReady())
    {
        // This is NOT an error - Matter SDK calls this during commissioning before Wi-Fi is connected
        // We just silently skip and the service will be published when network comes up
        ChipLogProgress(DeviceLayer, "[mDNS] Network not ready - service will be published when network is up");
        if (callback)
        {
            callback(context, service->mType, service->mName, CHIP_NO_ERROR);
        }
        return CHIP_NO_ERROR;
    }

    // Phase 1 (staging): set hostname only — DO NOT enable the responder yet.
    // Use the SDK-provided hostname (canonical 12-hex MAC, e.g. "54F82A136310")
    // so the AAAA record matches the SRV record target — same as ESP32/Linux.
    // Idempotent: MdnsStart() skips AT+UMDHN if hostname unchanged.
    // The responder is enabled later by ChipDnssdFinalizeServiceUpdate, so the
    // first gratuitous announcement carries hostname + ALL services + TXT.
    CHIP_ERROR hostErr = ucx.MdnsStart(service->mHostName);
    if (hostErr != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[mDNS] Failed to set hostname (continuing anyway)");
        // Non-fatal — fall through and try to stage the service. Finalize will
        // try MdnsEnable; if firmware really is wedged, that will fail too.
    }

    // Find existing slot for this service or allocate a new one
    int slot = FindServiceSlot(service->mName);
    if (slot < 0)
    {
        slot = FindFreeSlot();
        if (slot < 0)
        {
            ChipLogError(DeviceLayer, "[mDNS] No free service slots available");
            if (callback)
            {
                callback(context, service->mType, service->mName, CHIP_ERROR_NO_MEMORY);
            }
            return CHIP_ERROR_NO_MEMORY;
        }
    }

    // Mark this slot as republished (survives FinalizeServiceUpdate cleanup)
    sConfiguredServices[slot].republished = true;

    // Compute TXT hash to detect "no actual change" — common case during subscribe
    // cycles where the SDK re-publishes identical TXT records.
    uint32_t newTxtHash = HashTxtRecords(txtCount > 0 ? txtRecords : nullptr, txtCount);

    // Check if the base service (name+type+port) is already configured identically.
    if (ServiceMatches(slot, service->mName, fullServiceType, service->mPort))
    {
        if (sConfiguredServices[slot].txtHash == newTxtHash)
        {
            // Fully identical — no AT commands needed at all.
            ChipLogProgress(DeviceLayer, "[mDNS] Service slot %d unchanged (TXT hash 0x%08X) — no AT commands sent",
                            slot, static_cast<unsigned>(newTxtHash));
            if (callback)
            {
                callback(context, service->mType, service->mName, CHIP_NO_ERROR);
            }
            return CHIP_NO_ERROR;
        }

        // Same service, different TXT — update TXT only (cheaper than full re-add).
        ChipLogProgress(DeviceLayer, "[mDNS] Service slot %d TXT changed (hash 0x%08X -> 0x%08X), staging deferred UpdateTxt",
                        slot, static_cast<unsigned>(sConfiguredServices[slot].txtHash),
                        static_cast<unsigned>(newTxtHash));

        // Stage the TXT update — do NOT issue AT commands here. Finalize will flush.
        StashPendingTxt(sConfiguredServices[slot], txtCount > 0 ? txtRecords : nullptr, txtCount);
        // If a kConfigure is already pending for this slot (publish + update before Finalize),
        // keep it as kConfigure so the full record gets pushed in one shot.
        if (sConfiguredServices[slot].pendingOp != PendingMdnsOp::kConfigure)
        {
            sConfiguredServices[slot].pendingOp = PendingMdnsOp::kUpdateTxt;
        }
        sConfiguredServices[slot].txtHash = newTxtHash;
        sTransactionDirty = true;

        // Auto-finalize if outside a proper transaction
        if (!sInTransaction)
        {
            ChipLogProgress(DeviceLayer, "[mDNS] TXT update outside transaction — scheduling deferred auto-finalize");
            (void) DeviceLayer::PlatformMgr().ScheduleWork(DeferredAutoFinalize, 0);
        }

        if (callback)
        {
            callback(context, service->mType, service->mName, CHIP_NO_ERROR);
        }
        return CHIP_NO_ERROR;
    }

    // New or changed base service — stage a full Configure. Do NOT issue AT commands
    // here; FlushPendingMdnsOperations() runs from FinalizeServiceUpdate (possibly
    // via DeferredAutoFinalize on the next event loop iteration), keeping this
    // function fast even when called from inside an SDK command handler.
    sConfiguredServices[slot].inUse = true;
    sConfiguredServices[slot].republished = true;
    strncpy(sConfiguredServices[slot].name, service->mName, kMaxServiceNameLen - 1);
    sConfiguredServices[slot].name[kMaxServiceNameLen - 1] = '\0';
    strncpy(sConfiguredServices[slot].type, fullServiceType, kMaxServiceTypeLen - 1);
    sConfiguredServices[slot].type[kMaxServiceTypeLen - 1] = '\0';
    sConfiguredServices[slot].port = service->mPort;
    sConfiguredServices[slot].txtHash = newTxtHash;
    sConfiguredServices[slot].addedAtMs = chip::System::SystemClock().GetMonotonicMilliseconds64().count();
    StashPendingTxt(sConfiguredServices[slot], txtCount > 0 ? txtRecords : nullptr, txtCount);
    sConfiguredServices[slot].pendingOp = PendingMdnsOp::kConfigure;
    sTransactionDirty = true;

    ChipLogProgress(DeviceLayer, "[mDNS] Service staged in slot %d: %s.%s port=%d (TXT hash 0x%08X) — deferred to Finalize",
                   slot, service->mName, fullServiceType, service->mPort,
                   static_cast<unsigned>(newTxtHash));

    // If PublishService was called outside a RemoveServices→Finalize transaction
    // (e.g., from the SDK's kOperationalNetworkEnabled handler which calls
    // AdvertiseOperational() directly), schedule a deferred auto-finalize so the
    // pending AT burst happens on the next event loop iteration — OFF the SDK
    // callback path. This is the critical fix for AddNOC second-fabric latency.
    if (!sInTransaction)
    {
        ChipLogProgress(DeviceLayer, "[mDNS] PublishService outside transaction — scheduling deferred auto-finalize");
        (void) DeviceLayer::PlatformMgr().ScheduleWork(DeferredAutoFinalize, 0);
    }

    if (callback)
    {
        callback(context, service->mType, service->mName, CHIP_NO_ERROR);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdRemoveServices()
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdRemoveServices() — start of update transaction");

    // NOTE: Matter SDK calls RemoveServices() before every service update cycle.
    // The cycle is: RemoveServices → PublishService × N → FinalizeServiceUpdate.
    //
    // We do NOT remove services from NORA-W36 here. Instead we mark all slots as
    // "not republished". PublishService() will set republished=true for services it
    // re-publishes. FinalizeServiceUpdate() then removes any stale services that were
    // NOT republished (i.e., truly removed by the SDK).
    //
    // This avoids unnecessary remove+add churn on NORA-W36 which causes brief mDNS gaps.

    sInTransaction = true;  // SDK owns this transaction — Finalize will be called

    // LAZY SUSPEND (Vegas 2026 fix): do NOT suspend the responder here.
    //
    // The Matter SDK calls RemoveServices→PublishService×N→FinalizeServiceUpdate
    // on every commissioning-state change AND on every periodic refresh. The vast
    // majority of those transactions re-publish IDENTICAL services (PublishService
    // detects this via TXT-hash match and returns early without setting
    // sTransactionDirty). Sending AT+UMDE=0 / AT+UMDE=1 + URC pair on every such
    // no-op transaction stresses the NORA-W36 firmware — under heavy load (e.g.
    // mid-CommissioningComplete with WiFi association + CASE traffic) it has been
    // observed to crash and emit +STARTUP.
    //
    // Instead, suspend lazily: FlushPendingMdnsOperations() (called from Finalize)
    // already suspends the responder right before issuing AT+UMDSA / AT+UMDTRA.
    // Stale-service removal in Finalize also suspends if it actually has work to
    // do. Pure republish-only transactions therefore send ZERO AT+UMD* commands.
    for (size_t i = 0; i < kMaxServices; i++)
    {
        sConfiguredServices[i].republished = false;
    }
    // Reset for this transaction — PublishService will set it true on any real change.
    sTransactionDirty = false;

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdFinalizeServiceUpdate()
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdFinalizeServiceUpdate() — committing transaction");

    // STEP 1: Flush any AT-command work that PublishService deferred.
    // PublishService now only stashes parameters into the slot's pendingOp/pendingTxt
    // — the actual AT+UMDSA / AT+UMDTRA / AT+UMDTRC burst happens here, off the
    // SDK callback path. This is what keeps AddNOC's InvokeCommandResponse on time.
    FlushPendingMdnsOperations();

    // STEP 2: Remove any services that were NOT re-published during this update cycle.
    // These are services the SDK decided to drop (e.g., switching from commissioning to operational).
    //
    // IMPORTANT: During multi-admin commissioning, a pending fabric's operational service
    // is published during AddNOC, but a subsequent service update cycle (triggered by
    // subscription reports or other events) may NOT re-publish it because the fabric is
    // still pending (fail-safe not committed). Without the grace period, the service would
    // be removed before the new controller discovers it via mDNS.
    auto & ucx = DeviceLayer::UcxClientManager::GetInstance();
    uint64_t nowMs = chip::System::SystemClock().GetMonotonicMilliseconds64().count();

    // First pass: count how many slots actually need removing right now (out-of-grace,
    // unrepublished, in-use). We only suspend the responder if there is real work.
    size_t stalePending = 0;
    for (size_t i = 0; i < kMaxServices; i++)
    {
        if (sConfiguredServices[i].inUse && !sConfiguredServices[i].republished)
        {
            uint64_t ageMs = nowMs - sConfiguredServices[i].addedAtMs;
            if (ageMs >= kServiceGracePeriodMs) { stalePending++; }
        }
    }

    // Lazy suspend for the stale-removal step (matches FlushPendingMdnsOperations'
    // own lazy-suspend behaviour). If a flush already ran above, the responder is
    // already suspended and MdnsSuspend() is a no-op.
    if (stalePending > 0 && ucx.MdnsIsEnabled())
    {
        (void) ucx.MdnsSuspend();
    }

    for (size_t i = 0; i < kMaxServices; i++)
    {
        if (sConfiguredServices[i].inUse && !sConfiguredServices[i].republished)
        {
            uint64_t ageMs = nowMs - sConfiguredServices[i].addedAtMs;
            if (ageMs < kServiceGracePeriodMs)
            {
                // Service was recently added (e.g., pending fabric during commissioning).
                // Keep it alive — the controller needs time to discover and CASE-connect.
                ChipLogProgress(DeviceLayer, "[mDNS] Keeping recent service slot %u: %s (age %llu ms, grace %llu ms)",
                                static_cast<unsigned>(i), sConfiguredServices[i].name,
                                static_cast<unsigned long long>(ageMs),
                                static_cast<unsigned long long>(kServiceGracePeriodMs));
                continue;
            }
            ChipLogProgress(DeviceLayer, "[mDNS] Removing stale service slot %u: %s (age %llu ms)",
                            static_cast<unsigned>(i), sConfiguredServices[i].name,
                            static_cast<unsigned long long>(ageMs));
            ucx.MdnsRemoveService(static_cast<int32_t>(i));
            sConfiguredServices[i].inUse = false;
            sConfiguredServices[i].txtHash = 0;
            sConfiguredServices[i].pendingOp = PendingMdnsOp::kNone;
            sConfiguredServices[i].pendingTxtCount = 0;
            sTransactionDirty = true;
        }
    }

    // Phase 2 — commit. Enable the responder if not already enabled.
    // - First successful transaction after init: triggers gratuitous announcement
    //   carrying hostname + all staged services + TXT records in one burst (the
    //   "fill TXT then enable" trick).
    // - Subsequent transactions: MdnsEnable(true) is a no-op (already enabled),
    //   per-service add/remove already announces incrementally on the firmware side.
    //
    // Vegas 2026 fix: with lazy suspend in RemoveServices, three cases:
    //
    //   A) Responder DISABLED (first-ever Finalize after init, or after Stop):
    //      → MdnsEnable(true). Single AT+UMDE=1, gratuitous announcement carries
    //        hostname + all staged services + TXT.
    //
    //   B) Responder ENABLED, sTransactionDirty=false (pure republish, no change):
    //      → ZERO AT commands. Completely silent. This is the common case during
    //        commissioning state churn and steady-state subscription refreshes.
    //        This is the path that prevents the +STARTUP crash on NORA-W36 3.4.0-120.
    //
    //   C) Responder DISABLED, sTransactionDirty=true:
    //      → FlushPendingMdnsOperations() or stale-removal already suspended
    //        (AT+UMDE=0). One AT+UMDE=1 here to re-enable and announce.
    //
    // Edge case (defensive): dirty + still enabled shouldn't happen because every
    // mutating path lazily suspends. If it does, force a re-announcement.
    // u-connectXpressApp: without a network nothing is staged, and the responder stays as the user has it
    if (!ucx.IsNetworkReady())
    {
        ChipLogDetail(DeviceLayer, "[mDNS] No network yet: the responder is left alone");
    }
    else if (!ucx.MdnsIsEnabled())
    {
        ChipLogProgress(DeviceLayer, "[mDNS] %s responder — gratuitous announcement will fire",
                        sTransactionDirty ? "Re-enabling" : "Enabling");
        CHIP_ERROR enableErr = ucx.MdnsEnable(true);
        if (enableErr != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "[mDNS] Failed to enable responder at Finalize");
            // Don't propagate — services are still staged, next transaction will retry.
        }
    }
    else if (!sTransactionDirty)
    {
        // Common path: responder running, no changes — totally silent.
        ChipLogDetail(DeviceLayer, "[mDNS] No changes this transaction — no AT commands sent");
    }
    else
    {
        ChipLogProgress(DeviceLayer, "[mDNS] Services changed without suspend — forcing gratuitous re-announcement");
        CHIP_ERROR enableErr = ucx.MdnsForceReannounce();
        if (enableErr != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "[mDNS] Failed to force re-announcement");
        }
    }

    sTransactionDirty = false;
    sInTransaction = false;
    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdBrowse(const char * type, DnssdServiceProtocol protocol, Inet::IPAddressType addressType,
                           Inet::InterfaceId interface, DnssdBrowseCallback callback, void * context,
                           intptr_t * browseIdentifier)
{
    // mDNS service discovery (browsing) via NORA-W36
    // NORA-W36 firmware mDNS browse support is planned — when available, this will
    // use ucxclient API to send a PTR query for the requested service type and
    // call the callback with discovered services.
    //
    // CSA CERTIFICATION NOTE:
    // - For DEVICE role: Browse is NOT required. Devices advertise, controllers browse.
    //   CSA test harness performs its own browse — device is passive.
    // - For CONTROLLER role: Browse would be needed (not applicable for this device).
    //
    // When NORA-W36 firmware adds mDNS browse AT command (e.g., AT+UMDB):
    //   1. Send browse query via ucxclient: uCxMdnsBrowse(type, protocol)
    //   2. Register URC callback for browse results
    //   3. Collect results and call callback with discovered services
    //   4. Return browseIdentifier for StopBrowse
    
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdBrowse(type=%s, proto=%d) - not required for device role (CSA compliant)",
                   type ? type : "null", static_cast<int>(protocol));
    
    if (browseIdentifier)
    {
        *browseIdentifier = 0;
    }
    
    // TODO: When NORA-W36 firmware adds mDNS browse AT command (e.g., AT+UMDB):
    //   1. Send browse query via ucxclient: uCxMdnsBrowse(type, protocol)
    //   2. Register URC callback for browse results
    //   3. Collect results and call callback with discovered services
    //   4. Return browseIdentifier for StopBrowse
    
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ChipDnssdStopBrowse(intptr_t browseIdentifier)
{
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdStopBrowse(id=%ld) - awaiting NORA-W36 firmware support",
                   static_cast<long>(browseIdentifier));
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

CHIP_ERROR ChipDnssdResolve(DnssdService * service, Inet::InterfaceId interface, DnssdResolveCallback callback, void * context)
{
    // mDNS service resolution via NORA-W36
    // When NORA-W36 firmware adds mDNS resolve (e.g., AT+UMDQ for SRV+AAAA queries),
    // this will resolve a service instance name to its host, port, TXT records, and IP addresses.
    //
    // CSA CERTIFICATION NOTE:
    // - For DEVICE role: Resolve is NOT required. Devices advertise, controllers resolve.
    //   CSA test harness performs its own resolution — device is passive.
    // - For CONTROLLER role: Resolve would be needed after Browse (not applicable).
    //
    // When NORA-W36 firmware adds mDNS resolve AT command:
    //   1. Send SRV query via ucxclient: uCxMdnsResolve(service->mName, service->mType)
    //   2. Send AAAA query for the resolved hostname
    //   3. Parse TXT records from response
    //   4. Call callback with resolved service + IP addresses
    
    ChipLogProgress(DeviceLayer, "[mDNS] ChipDnssdResolve(name=%s, type=%s) - not required for device role (CSA compliant)",
                   service ? service->mName : "null",
                   service ? service->mType : "null");
    
    // TODO: When NORA-W36 firmware adds mDNS resolve AT command:
    //   1. Send SRV query via ucxclient: uCxMdnsResolve(service->mName, service->mType)
    //   2. Send AAAA query for the resolved hostname
    //   3. Parse TXT records from response
    //   4. Call callback with resolved service + IP addresses
    
    if (callback)
    {
        callback(context, service, Span<Inet::IPAddress>(), CHIP_ERROR_NOT_IMPLEMENTED);
    }

    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void ChipDnssdResolveNoLongerNeeded(const char * instanceName)
{
    // No active resolution to cancel (NORA-W36 browse/resolve not yet implemented)
}

CHIP_ERROR ChipDnssdReconfirmRecord(const char * hostname, Inet::IPAddress address, Inet::InterfaceId interface)
{
    // Record reconfirmation not supported on NORA-W36 mDNS responder
    return CHIP_NO_ERROR;
}

} // namespace Dnssd
} // namespace chip