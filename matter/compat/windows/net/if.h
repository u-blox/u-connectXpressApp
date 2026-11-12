// Stub net/if.h for Windows compilation
// Provides minimal POSIX network interface compatibility
#pragma once

#include <cstdint>
#include <winsock2.h>
#include <ws2tcpip.h>

// Note: Windows netioapi.h provides if_nametoindex and if_indextoname
// We include it but don't redeclare those functions
#include <iphlpapi.h>

// Define IF_NAMESIZE for BSD-style interface name length
#ifndef IF_NAMESIZE
#define IF_NAMESIZE 256
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Windows provides if_nametoindex and if_indextoname in netioapi.h
// No need to redeclare them here - they're already available

// Prevent Windows headers from defining a fragile `interface` macro that
// breaks C++ code in the connectedhomeip tree (it causes tokens like
// 'PlatformType interface' to be mis-parsed). Undefine it here after
// including Windows headers so downstream C++ files compile normally.
#ifdef interface
#  undef interface
#endif

// MinGW provides if_nameindex as forward declaration, but we need the complete type
// Always provide the full struct definition for chip SDK usage
#ifndef _IF_NAMEINDEX_DEFINED
#define _IF_NAMEINDEX_DEFINED
struct if_nameindex {
    unsigned int if_index;
    char *if_name;
};
#endif

// Windows provides if_nametoindex/if_indextoname in netioapi.h  (iphlpapi.lib)
// We only need to provide if_nameindex/if_freenameindex which Windows doesn't have
// Note: Function name matches struct name per POSIX API design - suppress C++ shadowing warning
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
struct if_nameindex *if_nameindex(void);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
void if_freenameindex(struct if_nameindex *ptr);

// ifreq structure for ioctl operations
struct ifreq {
    char ifr_name[IF_NAMESIZE];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_dstaddr;
        struct sockaddr ifr_broadaddr;
        struct sockaddr ifr_netmask;
        short ifr_flags;
        int ifr_ifindex;
        int ifr_metric;
        int ifr_mtu;
    };
};

// Interface flags (from Linux)
// Check if already defined by Windows headers (ws2ipdef.h) to avoid redefinition warnings
#ifndef IFF_UP
#define IFF_UP          0x1
#endif
#ifndef IFF_BROADCAST
#define IFF_BROADCAST   0x2
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK    0x8
#endif
#ifndef IFF_RUNNING
#define IFF_RUNNING     0x40
#endif
#ifndef IFF_MULTICAST
#define IFF_MULTICAST   0x1000
#endif

// ioctl codes for network interfaces
#ifndef SIOCGIFFLAGS
#define SIOCGIFFLAGS    0x8913
#endif

#ifdef __cplusplus
}
#endif
