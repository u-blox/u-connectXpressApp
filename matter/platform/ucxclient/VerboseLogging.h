/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 */

/**
 * @file
 *   Centralized verbose logging control for ucxclient platform.
 *   
 *   Usage:
 *     - Use UCX_LOG_VERBOSE() for detailed debug messages
 *     - Use ChipLogProgress() for important status messages (always shown)
 *     - Toggle with 'l' key in VirtualHardware
 */

#pragma once

#include <lib/support/logging/CHIPLogging.h>
#include <atomic>

namespace chip {
namespace DeviceLayer {

/**
 * Global verbose logging flag.
 * When false (default), UCX_LOG_VERBOSE() calls are suppressed.
 * Toggle with 'l' key or SetVerboseLogging(true/false).
 */
extern std::atomic<bool> gVerboseLogging;

/**
 * Enable/disable verbose logging for all ucxclient components.
 */
inline void SetVerboseLogging(bool enabled) { gVerboseLogging.store(enabled); }

/**
 * Check if verbose logging is enabled.
 */
inline bool IsVerboseLogging() { return gVerboseLogging.load(); }

} // namespace DeviceLayer
} // namespace chip

/**
 * Log only when verbose mode is enabled.
 * Use for detailed debugging that creates log spam.
 */
#define UCX_LOG_VERBOSE(module, fmt, ...) \
    do { \
        if (chip::DeviceLayer::gVerboseLogging.load()) { \
            ChipLogProgress(module, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

/**
 * Log a banner (========) only in verbose mode.
 */
#define UCX_LOG_VERBOSE_BANNER(module) \
    UCX_LOG_VERBOSE(module, "========================================")

