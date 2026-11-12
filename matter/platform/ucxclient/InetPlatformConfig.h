/*
 * Copyright (c) 2024 Project CHIP Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <cstdint>

// CHIP Inet Layer platform configuration for ucxclient platform
// 
// Architecture: Matter SDK uses BSD sockets implementation. 
// NORA-W36 integration is done at application layer via UcxClientManager.
// Future: Custom UDP/TCP endpoints can be added via GN build system override.

#define INET_CONFIG_ENABLE_IPV4 1
#define INET_CONFIG_ENABLE_DNS_RESOLVER 1

// u-connectXpressApp: the build sets which endpoints there are (InetBuildConfig.h): UDP through the module,
// no TCP (the node needs none and the module has six sockets)

// For interface enumeration and other SDK parts
#define CHIP_SYSTEM_CONFIG_USE_SOCKETS 1
#ifdef CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS
#undef CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS
#endif
#define CHIP_SYSTEM_CONFIG_USE_BSD_IFADDRS 1

// NOTE: Custom ucxclient UDP/TCP endpoint implementation is in:
// - platform/ucxclient/UDPEndPointImplUcx.h/cpp
// - platform/ucxclient/TCPEndPointImplUcx.h/cpp
// These can be integrated later via GN build system modification
