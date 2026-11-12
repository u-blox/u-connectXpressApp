// Stub arpa/inet.h for Windows compilation
// Maps POSIX network functions to Windows equivalents
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

// Windows already provides htonl, htons, ntohl, ntohs via winsock2.h
// inet_addr, inet_ntoa, inet_pton, inet_ntop are in ws2tcpip.h

#ifdef __cplusplus
extern "C" {
#endif

// All required functions are already provided by Windows headers

#ifdef __cplusplus
}
#endif
