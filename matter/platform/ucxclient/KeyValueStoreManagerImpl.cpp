/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/**
 * @file KeyValueStoreManagerImpl.cpp
 * @brief File-based Key-Value Store implementation for persistent storage.
 * 
 * This implementation persists all KVS data to "matter_kvs.bin" file.
 * This includes:
 * - Fabric credentials and metadata
 * - Access Control Lists (ACLs)
 * - Session resumption data
 * - Subscriptions
 * - Operational keys
 * 
 * Without file-based persistence, the device must be re-commissioned
 * on every restart. This is the CRITICAL component for fabric persistence.
 */

// C++ STL includes FIRST - before any platform headers
#include <fstream>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <chrono>

// Platform includes
#include <platform/internal/CHIPDeviceLayerInternal.h>
#include <platform/KeyValueStoreManager.h>
#include "KeyValueStoreManagerImpl.h"
#include "UcxStore.h"

namespace chip {
namespace DeviceLayer {
namespace PersistedStorage {

// File-based storage for fabric persistence
// u-connectXpressApp: one blob in the app's secure store instead of matter_kvs.bin
static const char * kKvsFileName = "kvs";
static std::map<std::string, std::vector<uint8_t>> sStorage;
static std::mutex sStorageMutex;
static bool sStorageLoaded = false;
static KeyValueStoreManagerImpl sInstance;

// =============================================================================
// Debounced flush state
// =============================================================================
// ColorControl/LevelControl publish 10+ attribute updates per second during
// a slider drag. Without coalescing, we rewrite the entire matter_kvs.bin on
// every tick. Debouncing collapses a full slider gesture into ONE flush after
// the user lets go (or after kFlushDebounceMs of inactivity).
//
// Safety: FlushNow() is called on shutdown and after factory reset. All
// fabric/ACL writes get persisted within kFlushDebounceMs of the last write.
static constexpr int64_t kFlushDebounceMs = 500;
static bool      sDirty             = false;
static int64_t   sLastDirtyTimeMs   = 0;
static uint32_t  sCoalescedWrites   = 0;  // counter for diagnostics

static int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Load KVS data from file
static void LoadStorageFromFile()
{
    std::lock_guard<std::mutex> lock(sStorageMutex);
    
    if (sStorageLoaded) return;
    
    if (!ucxmatter::StoreLoad(kKvsFileName, sStorage))
    {
        ChipLogProgress(DeviceLayer, "[KVS] No existing KVS file '%s' - starting fresh", kKvsFileName);
        sStorageLoaded = true;
        return;
    }
    
    sStorageLoaded = true;
    ChipLogProgress(DeviceLayer, "[KVS] Loaded %u entries from '%s'", 
                   static_cast<unsigned>(sStorage.size()), kKvsFileName);
}

// Save KVS data to file
static CHIP_ERROR SaveStorageToFile()
{
    // Note: Caller must hold sStorageMutex
    
    if (!ucxmatter::StoreSave(kKvsFileName, sStorage))
    {
        ChipLogError(DeviceLayer, "[KVS] Failed to open '%s' for writing", kKvsFileName);
        return CHIP_ERROR_WRITE_FAILED;
    }
    
    ChipLogDetail(DeviceLayer, "[KVS] Saved %u entries to '%s'", 
                 static_cast<unsigned>(sStorage.size()), kKvsFileName);
    return CHIP_NO_ERROR;
}

KeyValueStoreManagerImpl & KeyValueStoreManagerImpl::GetDefaultInstance()
{
    return sInstance;
}

CHIP_ERROR KeyValueStoreManagerImpl::_Get(const char * key, void * value, size_t value_size, 
                                          size_t * read_bytes_size, size_t offset)
{
    LoadStorageFromFile();  // Ensure storage is loaded
    
    std::lock_guard<std::mutex> lock(sStorageMutex);
    
    auto it = sStorage.find(key);
    if (it == sStorage.end())
    {
        return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    
    const auto & data = it->second;
    if (offset >= data.size())
    {
        // Handle edge case where offset equals size (empty read)
        if (offset == data.size() && data.size() == 0)
        {
            if (read_bytes_size != nullptr)
            {
                *read_bytes_size = 0;
            }
            return CHIP_NO_ERROR;
        }
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
    
    size_t total_size_to_read = data.size() - offset;
    size_t bytes_to_read = std::min(value_size, total_size_to_read);
    
    if (value != nullptr && bytes_to_read > 0)
    {
        memcpy(value, data.data() + offset, bytes_to_read);
    }
    
    if (read_bytes_size != nullptr)
    {
        *read_bytes_size = bytes_to_read;
    }
    
    // Return buffer too small if we couldn't read everything
    return (value_size < total_size_to_read) ? CHIP_ERROR_BUFFER_TOO_SMALL : CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::_Put(const char * key, const void * value, size_t value_size)
{
    LoadStorageFromFile();  // Ensure storage is loaded
    
    std::lock_guard<std::mutex> lock(sStorageMutex);
    
    std::vector<uint8_t> data;
    if (value != nullptr && value_size > 0)
    {
        data.assign(static_cast<const uint8_t *>(value), 
                    static_cast<const uint8_t *>(value) + value_size);
    }
    sStorage[key] = std::move(data);
    
    // Mark dirty — actual file write is debounced via MaybeFlush() in main loop.
    // This collapses bursty writes (slider drags, attribute storms) into a
    // single flush after kFlushDebounceMs of inactivity.
    if (sDirty) sCoalescedWrites++;
    sDirty           = true;
    sLastDirtyTimeMs = NowMs();
    
    return CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::_Delete(const char * key)
{
    LoadStorageFromFile();  // Ensure storage is loaded
    
    std::lock_guard<std::mutex> lock(sStorageMutex);
    
    auto it = sStorage.find(key);
    if (it == sStorage.end())
    {
        return CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    
    sStorage.erase(it);
    
    // Mark dirty — debounced flush handles persistence. See _Put for rationale.
    if (sDirty) sCoalescedWrites++;
    sDirty           = true;
    sLastDirtyTimeMs = NowMs();
    
    return CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::EraseAll()
{
    std::lock_guard<std::mutex> lock(sStorageMutex);
    
    ChipLogProgress(DeviceLayer, "[KVS] EraseAll - clearing all stored data");
    
    sStorage.clear();
    sStorageLoaded = true;  // Mark as loaded (now empty)
    
    // Drop any pending flush — file is being removed
    sDirty = false;
    
    // Delete the file
    ucxmatter::StoreRemove(kKvsFileName);
    
    ChipLogProgress(DeviceLayer, "[KVS] All data erased and file deleted");
    return CHIP_NO_ERROR;
}

CHIP_ERROR KeyValueStoreManagerImpl::HealthCheck()
{
    static constexpr char kHealthKey[] = "_kvs_health_check_";
    static constexpr uint32_t kSentinel = 0xDEADBEEF;

    ChipLogProgress(DeviceLayer, "[KVS] Health check: write/read/verify sentinel...");

    // Step 1: Write sentinel
    CHIP_ERROR err = _Put(kHealthKey, &kSentinel, sizeof(kSentinel));
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[KVS] Health check FAILED: write error: %s", chip::ErrorStr(err));
        ChipLogError(DeviceLayer, "[KVS] Auto-erasing corrupt storage...");
        (void)EraseAll();
        return err;
    }

    // Step 2: Read back
    uint32_t readBack = 0;
    size_t readSize = 0;
    err = _Get(kHealthKey, &readBack, sizeof(readBack), &readSize, 0);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "[KVS] Health check FAILED: read error: %s", chip::ErrorStr(err));
        ChipLogError(DeviceLayer, "[KVS] Auto-erasing corrupt storage...");
        (void)EraseAll();
        return err;
    }

    // Step 3: Verify
    if (readSize != sizeof(kSentinel) || readBack != kSentinel)
    {
        ChipLogError(DeviceLayer, "[KVS] Health check FAILED: data mismatch (wrote 0x%08lX, read 0x%08lX, size %u)",
                     static_cast<unsigned long>(kSentinel), static_cast<unsigned long>(readBack),
                     static_cast<unsigned>(readSize));
        ChipLogError(DeviceLayer, "[KVS] Auto-erasing corrupt storage...");
        (void)EraseAll();
        return CHIP_ERROR_INTEGRITY_CHECK_FAILED;
    }

    // Step 4: Clean up sentinel
    (void)_Delete(kHealthKey);

    ChipLogProgress(DeviceLayer, "[KVS] Health check PASSED");
    return CHIP_NO_ERROR;
}

// =============================================================================
// Debounced flush API — call MaybeFlush() from main loop, FlushNow() on exit
// =============================================================================

void KeyValueStoreManagerImpl::MaybeFlush()
{
    // Cheap fast-path: untaken lock + single bool check
    if (!sDirty) return;

    std::lock_guard<std::mutex> lock(sStorageMutex);
    if (!sDirty) return;  // re-check under lock

    int64_t idleMs = NowMs() - sLastDirtyTimeMs;
    if (idleMs < kFlushDebounceMs) return;

    uint32_t coalesced = sCoalescedWrites;
    CHIP_ERROR err = SaveStorageToFile();
    if (err == CHIP_NO_ERROR)
    {
        sDirty           = false;
        sCoalescedWrites = 0;
        if (coalesced > 0)
        {
            ChipLogProgress(DeviceLayer, "[KVS] Flushed (coalesced %u writes, %lld ms idle)",
                           static_cast<unsigned>(coalesced + 1),
                           static_cast<long long>(idleMs));
        }
    }
    else
    {
        ChipLogError(DeviceLayer, "[KVS] Debounced flush failed: %s — will retry next tick",
                     ErrorStr(err));
        // Leave dirty=true so we retry. Bump the timestamp slightly so we
        // don't hammer the disk if the failure is persistent.
        sLastDirtyTimeMs = NowMs();
    }
}

CHIP_ERROR KeyValueStoreManagerImpl::FlushNow()
{
    std::lock_guard<std::mutex> lock(sStorageMutex);
    if (!sDirty) return CHIP_NO_ERROR;

    uint32_t coalesced = sCoalescedWrites;
    CHIP_ERROR err = SaveStorageToFile();
    if (err == CHIP_NO_ERROR)
    {
        sDirty           = false;
        sCoalescedWrites = 0;
        ChipLogProgress(DeviceLayer, "[KVS] FlushNow (coalesced %u writes)",
                       static_cast<unsigned>(coalesced + 1));
    }
    else
    {
        ChipLogError(DeviceLayer, "[KVS] FlushNow failed: %s", ErrorStr(err));
    }
    return err;
}

// Global accessor function required by Matter SDK
KeyValueStoreManager & KeyValueStoreMgr()
{
    return KeyValueStoreManagerImpl::GetDefaultInstance();
}

} // namespace PersistedStorage
} // namespace DeviceLayer
} // namespace chip
