// The node has no network interface on the PC: its sockets and its mDNS are on the module.
#include "ifaddrs.h"

int getifaddrs(struct ifaddrs ** ifap)
{
    *ifap = nullptr;
    return 0;
}

void freeifaddrs(struct ifaddrs *) {}
