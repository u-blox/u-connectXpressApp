// Windows implementation of ifaddrs.h (BSD-style interface address structures)
// Provides getifaddrs()/freeifaddrs() using Windows IP Helper API
#pragma once

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interface flags (from BSD net/if.h)
#define IFF_UP          0x1     /* Interface is up */
#define IFF_BROADCAST   0x2     /* Broadcast address valid */
#define IFF_DEBUG       0x4     /* Turn on debugging */
#define IFF_LOOPBACK    0x8     /* Is a loopback net */
#define IFF_POINTOPOINT 0x10    /* Interface is point-to-point link */
#define IFF_NOTRAILERS  0x20    /* Avoid use of trailers */
#define IFF_RUNNING     0x40    /* Resources allocated */
#define IFF_NOARP       0x80    /* No address resolution protocol */
#define IFF_PROMISC     0x100   /* Receive all packets */
#define IFF_ALLMULTI    0x200   /* Receive all multicast packets */
#define IFF_MULTICAST   0x8000  /* Supports multicast */

struct ifaddrs {
    struct ifaddrs  *ifa_next;    /* Next item in list */
    char            *ifa_name;    /* Name of interface */
    unsigned int     ifa_flags;   /* Flags from SIOCGIFFLAGS */
    struct sockaddr *ifa_addr;    /* Address of interface */
    struct sockaddr *ifa_netmask; /* Netmask of interface */
    union {
        struct sockaddr *ifu_broadaddr; /* Broadcast address */
        struct sockaddr *ifu_dstaddr;   /* Point-to-point destination address */
    } ifa_ifu;
    void            *ifa_data;    /* Address-specific data */
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

// getifaddrs: Enumerate network interfaces using Windows IP Helper API
// freeifaddrs: Free the ifaddrs list
int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif
