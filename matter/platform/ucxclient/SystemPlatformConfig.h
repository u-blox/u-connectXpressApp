/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

// CHIP System Layer platform configuration for ucxclient platform
// NORA-W36 provides TCP/IP stack via u_cx_socket API (not host OS sockets)

// CRITICAL ARCHITECTURE DECISION:
// Why USE_SOCKETS=1 instead of USE_LWIP=1?
// - NORA-W36 has built-in LWIP but headers aren't exposed to host
// - Matter SDK requires either LWIP, SOCKETS, or Network.framework
// - We use SOCKETS mode with ucxclient's own BSD socket compatibility headers
// - ucxclient intercepts socket calls and maps to NORA-W36 u_cx_socket API
//
// Why USE_POSIX_SOCKETS=1?
// - SystemConfig.h line 805 auto-sets USE_POSIX_SOCKETS=1 when USE_SOCKETS=1
// - Cannot override in this file (included AFTER SystemConfig.h processes defaults)
// - ucxclient provides BSD socket compatibility headers (netinet/in.h, sys/socket.h, etc.)
//
// ESP32 comparison:
// - ESP32 uses USE_LWIP=1 because ESP-IDF exposes LWIP headers
// - ucxclient doesn't expose LWIP headers, so we use socket abstraction layer

// BSD socket type compatibility:
// The Matter SDK provides socket type definitions when CHIP_SYSTEM_CONFIG_USE_SOCKETS=1
// Platform-specific includes are handled by InetPlatformConfig.h and UDPEndPointImplUcx.cpp
// Do NOT include any POSIX or Windows socket headers here - they break SDK compilation

#define CHIP_SYSTEM_CONFIG_USE_POSIX_TIME_FUNCTS 0

// SystemClock.h tests these right after this file, before the device layer's configuration is read
#define CHIP_DEVICE_LAYER_TARGET_DARWIN 0
#define CHIP_DEVICE_LAYER_TARGET_LINUX 0

// Check if already defined by Windows platform to avoid redefinition
#ifndef CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS
#define CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS 1  // We provide stub BSD interface functions
#endif

#define CHIP_SYSTEM_CONFIG_USE_ZEPHYR_NET_IF 0
#define CHIP_SYSTEM_CONFIG_USE_LWIP 0  // LWIP on NORA-W36 not exposed to host
#define CHIP_SYSTEM_CONFIG_USE_POSIX_PIPE 0  // Windows select() only works with sockets, not pipes!
#define CHIP_SYSTEM_CONFIG_USE_ZEPHYR_SOCKETS 0

// Use standard eventfd path (our sys/eventfd.h stub provides compatibility)
// Matter SDK 1.5.0.1 changed: ZEPHYR_EVENTFD=1 now includes <zephyr/posix/sys/eventfd.h>
// which doesn't exist on Windows. Setting to 0 makes it use <sys/eventfd.h> instead.
#define CHIP_SYSTEM_CONFIG_USE_ZEPHYR_EVENTFD 0

// ============================================================================
// UCXCLIENT WAKE EVENT CONFIGURATION
// ============================================================================
// ucxclient platform provides its own WakeEvent implementation that uses
// platform-agnostic mutex + condition variable instead of host OS sockets.
// This avoids dependency on eventfd/pipe/sockets for the event loop wake mechanism.
// The actual networking goes through NORA-W36 via ucxclient socket API.
#define CHIP_SYSTEM_CONFIG_USE_UCXCLIENT_WAKE_EVENT 1

// Use socket API (build system requires this)
// We provide custom UDP endpoint that uses NORA-W36 u_cx_socket directly
#define CHIP_SYSTEM_CONFIG_USE_SOCKETS 1
// Note: Custom endpoint is UDPEndPointImplUcx (similar to OpenThread)

// Enable POSIX socket types for SystemLayerImplSelect (fd_set, select, etc.)
// The Windows platform provides winsock2-based stubs for these types.
// NOTE: This only provides the SELECT/event loop mechanics - actual network
// I/O goes through ucxclient/NORA-W36, not through host OS sockets!
#ifdef CHIP_SYSTEM_CONFIG_USE_POSIX_SOCKETS
#undef CHIP_SYSTEM_CONFIG_USE_POSIX_SOCKETS
#endif
#define CHIP_SYSTEM_CONFIG_USE_POSIX_SOCKETS 1

// Locking configuration - undefine build system defaults and set our values
#ifdef CHIP_SYSTEM_CONFIG_POSIX_LOCKING
#undef CHIP_SYSTEM_CONFIG_POSIX_LOCKING
#endif
#define CHIP_SYSTEM_CONFIG_POSIX_LOCKING 0

#ifdef CHIP_SYSTEM_CONFIG_NO_LOCKING
#undef CHIP_SYSTEM_CONFIG_NO_LOCKING
#endif
#define CHIP_SYSTEM_CONFIG_NO_LOCKING 1

// Host OS socket configuration
#define CHIP_SYSTEM_CONFIG_USE_NETWORK_FRAMEWORK 0

// ==================== Packet Buffer Configuration ====================
// Use SDK default (1583). NORA-W36 firmware supports up to 1460-byte UDP payloads.
// Matter spec limits to 1232 bytes (1280 IPv6 MTU - 48 headers), well within range.
// Previously reduced to 1087 which caused "first cluster event too big" errors
// during initial subscription reads after commissioning.
// #define CHIP_SYSTEM_CONFIG_PACKETBUFFER_CAPACITY_MAX 1583  // SDK default, no override needed

// Discovery configuration
#ifndef CHIP_DEVICE_CONFIG_MAX_DISCOVERED_IP_ADDRESSES
#define CHIP_DEVICE_CONFIG_MAX_DISCOVERED_IP_ADDRESSES 5
#endif
