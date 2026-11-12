/*
 *    Copyright (c) 2025 u-blox
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * @file
 *   Platform-specific configuration for CHIP on ucxclient.
 */

#pragma once

// ==================== Platform Configuration ====================

#define CHIP_PLATFORM_CONFIG_ENABLE_WIFI 1
#define CHIP_PLATFORM_CONFIG_ENABLE_BLE 1
#define CHIP_PLATFORM_CONFIG_ENABLE_THREAD 0

// ==================== Logging Configuration ====================

#ifndef CHIP_CONFIG_LOG_MESSAGE_MAX_SIZE
#define CHIP_CONFIG_LOG_MESSAGE_MAX_SIZE 256
#endif

// ==================== MRP (Message Reliability Protocol) Configuration ====================
// Increase max retransmissions for noisy Wi-Fi environments.
// Default is 4 retries (~3s total window). With 5 retries (~5.2s window),
// the extra retry at max backoff (~2.2s) significantly improves reliability.
// Backoff is exponential: baseInterval * 1.1 * 1.6^(max(0, n-1)) with 25% jitter.
#ifndef CHIP_CONFIG_RMP_DEFAULT_MAX_RETRANS
#define CHIP_CONFIG_RMP_DEFAULT_MAX_RETRANS 5
#endif

// ==================== Memory Configuration ====================

#ifndef CHIP_CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS
#define CHIP_CONFIG_MAX_UNSOLICITED_MESSAGE_HANDLERS 8
#endif

#ifndef CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS
#define CHIP_CONFIG_MAX_EXCHANGE_CONTEXTS 8
#endif

// Use native OS threading primitives (not FreeRTOS static allocation)
#define CHIP_CONFIG_FREERTOS_USE_STATIC_TASK 0
#define CHIP_CONFIG_FREERTOS_USE_STATIC_QUEUE 0
#define CHIP_CONFIG_FREERTOS_USE_STATIC_SEMAPHORE 0
