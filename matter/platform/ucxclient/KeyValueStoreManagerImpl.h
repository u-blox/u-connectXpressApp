/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <platform/KeyValueStoreManager.h>

namespace chip {
namespace DeviceLayer {
namespace PersistedStorage {

/**
 * ucxclient platform implementation of KeyValueStoreManager
 * Provides persistent storage for Matter device configuration
 * Platform-agnostic: uses host OS storage (Windows Registry/POSIX files/FreeRTOS NVS)
 */
class KeyValueStoreManagerImpl final : public KeyValueStoreManager
{
public:
    CHIP_ERROR _Get(const char * key, void * value, size_t value_size, size_t * read_bytes_size, size_t offset);
    CHIP_ERROR _Put(const char * key, const void * value, size_t value_size);
    CHIP_ERROR _Delete(const char * key);
    
    /**
     * Erase all stored key-value pairs (factory reset).
     * Called during Matter factory reset to clear fabrics, ACLs, etc.
     */
    CHIP_ERROR EraseAll();

    /**
     * Boot-time KVS health check: write/read/verify/delete a sentinel key.
     * If the round-trip fails, auto-erases all storage and returns the error.
     * Call BEFORE Server::Init() to prevent hangs on corrupt data.
     */
    CHIP_ERROR HealthCheck();

    /**
     * Debounced flush: call from the main loop every iteration.
     * If the in-RAM map is dirty AND no Put/Delete has happened in the last
     * kFlushDebounceMs (default 500ms), the file is rewritten once and the
     * dirty flag is cleared. Cheap when nothing is dirty (just a flag check).
     *
     * Rationale: ColorControl / LevelControl publish 10+ attribute updates per
     * second during a slider drag. Without coalescing, we rewrite the entire
     * matter_kvs.bin on every tick. Debouncing collapses a slider gesture into
     * one flush after the user lets go.
     */
    static void MaybeFlush();

    /**
     * Force-flush any pending writes immediately. Call before shutdown / exit
     * and after operations that must be durable (e.g., commissioning complete).
     */
    static CHIP_ERROR FlushNow();

    /**
     * u-connectXpressApp: flush, then forget the in-RAM copy, so the next start reads the store again. A node
     * linked into the app (browser, boards) is never unloaded: without this, what matter.reset removed came back.
     */
    static void Unload();

    static KeyValueStoreManagerImpl & GetDefaultInstance();
};

} // namespace PersistedStorage
} // namespace DeviceLayer
} // namespace chip
