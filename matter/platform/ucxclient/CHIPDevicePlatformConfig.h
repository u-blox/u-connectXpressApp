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
 *   Platform-specific configuration overrides for CHIP on the u-blox ucxclient platform.
 */

#pragma once

// ==================== Platform Adaptations ====================

// External platform - explicitly specify platform header paths for SDK
#define EXTERNAL_CHIPDEVICEPLATFORMEVENT_HEADER <platform/ucxclient/CHIPDevicePlatformEvent.h>
#define EXTERNAL_PLATFORMMANAGERIMPL_HEADER <platform/ucxclient/PlatformManagerImpl.h>
#define EXTERNAL_CONNECTIVITYMANAGERIMPL_HEADER <platform/ucxclient/ConnectivityManagerImpl.h>
#define EXTERNAL_CONFIGURATIONMANAGERIMPL_HEADER <platform/ucxclient/ConfigurationManagerImpl.h>
#define EXTERNAL_DIAGNOSTICDATAPROVIDERIMPL_HEADER <platform/ucxclient/DiagnosticDataProviderImpl.h>
#define EXTERNAL_KEYVALUESTOREMANAGERIMPL_HEADER <platform/ucxclient/KeyValueStoreManagerImpl.h>

// Matter with ucxclient integration - NORA-W36 handles Wi-Fi/BLE
// Platform-agnostic: can run on Windows, Linux, POSIX, or FreeRTOS
#define CHIP_DEVICE_PLATFORM_TARGET_UCXCLIENT 1

// ==================== Upstream SDK Warning Suppressions ====================

// Define platform target macros to silence -Wundef warnings from Matter SDK headers
#define CHIP_DEVICE_LAYER_TARGET_DARWIN 0
#define CHIP_DEVICE_LAYER_TARGET_LINUX 0
#define CHIP_STACK_LOCK_TRACKING_ENABLED 0              // Lock tracking disabled (not needed for production)
#define CHIP_DEVICE_CONFIG_ENABLE_DYNAMIC_MRP_CONFIG 0  // Static MRP (Message Reliability Protocol) configuration

// ==================== Shared Endpoint Type Fix ====================
// ZAP codegen counts unique endpoint *types*, not endpoint *instances*.
// The fix is in ClusterEndpointCountOverrides.h, included via patched config.h
// AFTER gen_config.h. See platform/ucxclient/ClusterEndpointCountOverrides.h.

// ==================== Network Protocol Selection ====================
//
// Exactly one (or both) of WiFi and Thread must be enabled:
//   NORA-W36: WiFi only (default)
//   NORA-B26: Thread only (or Thread + WiFi with companion module)
//
// Override from build system (BUILD.gn / CMakeLists.txt) by defining before this header:
//   -DCHIP_DEVICE_CONFIG_ENABLE_WIFI=0 -DCHIP_DEVICE_CONFIG_ENABLE_THREAD=1

#ifndef CHIP_DEVICE_CONFIG_ENABLE_WIFI
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI 1           // Default: WiFi enabled (NORA-W36)
#endif

#ifndef CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD 0         // Default: Thread disabled
#endif

// OpenThread is needed by Matter SDK headers when Thread is enabled
// Note: We don't link OpenThread — it runs on NORA-B26 firmware.
// This flag controls Matter SDK's Thread-related code paths.
#ifndef CHIP_ENABLE_OPENTHREAD
#define CHIP_ENABLE_OPENTHREAD CHIP_DEVICE_CONFIG_ENABLE_THREAD
#endif

// BLE commissioning — always enabled (both NORA-W36 and NORA-B26 have BLE)
#define CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE 1

// ==================== WiFi Configuration ====================

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION 1
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_AP 0
#else
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION 0
#define CHIP_DEVICE_CONFIG_ENABLE_WIFI_AP 0
#endif

// ==================== Thread Configuration ====================

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
// SRP DNS-SD via Border Router (Thread uses SRP instead of mDNS multicast)
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD_SRP_CLIENT 1
// DNS-SD browse/resolve via SRP server on Border Router
#define CHIP_DEVICE_CONFIG_ENABLE_THREAD_DNS_CLIENT 1
// ThreadStackManager header path
#define EXTERNAL_THREADSTACKMANAGERIMPL_HEADER <platform/ucxclient/ThreadStackManagerImpl.h>
#endif

// ==================== BLE Configuration ====================

// (CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE defined above in Network Protocol Selection)

// ==================== Network Configuration ====================

/**
 * Socket Layer Architecture:
 * 
 * ⚠️ CRITICAL: ALL network traffic MUST go through NORA-W36 exclusively
 * 
 * UDPEndPointImplUcx → u_cx_socket API (NORA-W36 only)
 * 
 * - NO Windows Winsock2 for any network operations
 * - NO hybrid implementations (Windows + NORA-W36)
 * - TCP/IP stack runs on NORA-W36 (built-in LWIP)
 * - Host only provides BSD socket type definitions for Matter SDK compilation
 * - mDNS multicast via NORA-W36 ucxclient API (uCxMdns* functions)
 */

// Use host OS native TCP/IP stack (Windows/POSIX sockets)
#define CHIP_SYSTEM_CONFIG_USE_SOCKETS 1
// Note: CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS is set by Windows/SystemPlatformConfig.h

// ==================== Security Configuration ====================

// Use platform certificate store
// DNS-SD: mDNS for WiFi (NORA-W36), SRP for Thread (NORA-B26 via Border Router)
#define CHIP_DEVICE_CONFIG_ENABLE_DNSSD 1
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
// Thread uses SRP Client → Border Router → DNS-SD (not multicast mDNS)
#define CHIP_DEVICE_CONFIG_ENABLE_DNSSD_SRP 1
#else
#define CHIP_DEVICE_CONFIG_ENABLE_DNSSD_SRP 0
#endif

// ==================== Storage Configuration ====================

// Use platform-specific persistent storage (registry/file-based)
#define CHIP_DEVICE_CONFIG_ENABLE_CHIP_TIME_SERVICE_TIME_SYNC 0

// ==================== Feature Flags ====================

// OTA (Over-The-Air) firmware update support
#define CHIP_DEVICE_CONFIG_ENABLE_OTA_REQUESTOR 1

// Disable additional data advertising (not needed for basic Matter device)
#define CHIP_ENABLE_ADDITIONAL_DATA_ADVERTISING 0

// Use standard commissionable data provider (not transitional)
#define CHIP_USE_TRANSITIONAL_COMMISSIONABLE_DATA_PROVIDER 0
