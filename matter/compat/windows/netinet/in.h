// Stub netinet/in.h for Windows compilation
// Maps POSIX network headers to Windows equivalents
#pragma once

// Include Windows socket headers if not already included
#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif

#ifndef _WS2TCPIP_H_
#include <ws2tcpip.h>
#endif

// suseconds_t is in sys/time.h on POSIX, define for Windows
#ifndef _SUSECONDS_T_DEFINED
#define _SUSECONDS_T_DEFINED
typedef long suseconds_t;
#endif

// Address string length constants (provided by POSIX netinet/in.h)
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

// Ensure Windows socket library is linked
#ifndef _WINSOCKAPI_
#pragma comment(lib, "ws2_32.lib")
#endif

// Linux/BSD in_pktinfo has ipi_spec_dst which Windows lacks
// We override the Windows definition with a compatible one
// IMPORTANT: Must come after ws2tcpip.h include but provide BSD-compatible struct
#ifdef _WS2IPDEF_
// Windows already defined in_pktinfo, we need to add ipi_spec_dst compatibility
// Use a different approach - define the missing member as a macro
#ifndef ipi_spec_dst
// Map ipi_spec_dst to ipi_addr (best effort compatibility)
// This works for most use cases where source and dest are the same
#define ipi_spec_dst ipi_addr
#endif
#endif
