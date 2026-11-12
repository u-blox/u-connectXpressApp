/*
 *    Copyright (c) 2024 Project CHIP Authors
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
 *    @file
 *          Platform-specific configuration overrides for the CHIP BLE
 *          Layer on ucxclient (NORA-W36) platforms.
 *
 */

#pragma once

// ==================== Platform Adaptations ====================

// Use uint16_t as connection object to avoid nullptr issues with handle 0
// NORA-W36 uses connection handles starting from 0, so we can't use nullptr
// as the uninitialized value. Instead, use 0xFFFF as invalid/uninitialized.
#define BLE_CONNECTION_OBJECT uint16_t
#define BLE_CONNECTION_UNINITIALIZED (0xFFFF)

// BLE receive window size for flow control
#define BLE_MAX_RECEIVE_WINDOW_SIZE 5

// Error range for BLE config errors
#define BLE_CONFIG_ERROR_MIN 6000000
#define BLE_CONFIG_ERROR_MAX 6000999

// ========== Platform-specific Configuration Overrides =========

/* none so far */
