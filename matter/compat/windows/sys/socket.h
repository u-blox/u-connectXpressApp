// Stub sys/socket.h for Windows compilation
// Maps POSIX socket API to Windows equivalents
#pragma once

// Windows socket headers provide the same functionality
#ifndef _WINSOCK2API_
#include <winsock2.h>
#endif

#ifndef _WS2TCPIP_H_
#include <ws2tcpip.h>
#endif

#include <mswsock.h>  // For WSAMSG

// Undefine Windows SAL annotation macros that conflict with Matter SDK enum values
// minwindef.h defines IN and OUT as empty macros, breaking mdns::Minimal::QClass::IN
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

// Undefine Windows API macros that conflict with Matter SDK method names
// Windows remaps LoadString to LoadStringA/LoadStringW - breaks AttributePersistence::LoadString
#ifdef LoadString
#undef LoadString
#endif

// Undefine Windows socket event macros that conflict with Matter SDK code
// WakeEvent.cpp uses FD_READ/FD_WRITE as enum names
#ifdef FD_READ
#undef FD_READ
#endif
#ifdef FD_WRITE
#undef FD_WRITE
#endif

// Undefine 'interface' macro from Windows headers that conflicts with C++ parameter names
#ifdef interface
#undef interface
#endif

// Map common POSIX socket types that Windows has under different names
// MinGW may already define socklen_t
#ifndef _SOCKLEN_T_DEFINED
#define _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif

// POSIX socket constants
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

// IPv6 multicast group membership (Windows uses IPV6_ADD_MEMBERSHIP)
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP IPV6_ADD_MEMBERSHIP
#endif

#ifndef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

// POSIX sendmsg/recvmsg structures mapped to Windows equivalents
struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct msghdr {
    void *msg_name;           // Socket name
    socklen_t msg_namelen;    // Length of name
    struct iovec *msg_iov;    // Scatter/gather array
    size_t msg_iovlen;        // # elements in msg_iov
    void *msg_control;        // Ancillary data
    size_t msg_controllen;    // Ancillary data buffer len
    int msg_flags;            // Flags on received message
};

// cmsghdr - guard against MinGW/MSYS2 which already defines this via _WSACMSGHDR
#if !defined(_WSACMSGHDR) && !defined(__MINGW32__)
struct cmsghdr {
    size_t cmsg_len;          // Data byte count, including header
    int cmsg_level;           // Originating protocol
    int cmsg_type;            // Protocol-specific type
};
#endif

// Control message macros (minimal stubs)
// wincrypt.h defines CMSG_DATA as numeric 1, but we need function-like macro - undefine it
#ifdef CMSG_DATA
#undef CMSG_DATA
#endif
#ifdef CMSG_FIRSTHDR
#undef CMSG_FIRSTHDR
#endif
#ifdef CMSG_NXTHDR
#undef CMSG_NXTHDR
#endif
#ifdef CMSG_SPACE
#undef CMSG_SPACE
#endif
#ifdef CMSG_LEN
#undef CMSG_LEN
#endif

#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(mhdr)->msg_control : (struct cmsghdr*)0)

#define CMSG_NXTHDR(mhdr, cmsg) ((struct cmsghdr*)0)  // Stub: no next header

#define CMSG_DATA(cmsg) ((unsigned char*)(cmsg) + sizeof(struct cmsghdr))

#define CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))

#define CMSG_LEN(len) (sizeof(struct cmsghdr) + (len))

// Message flags
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0  // Windows socket are non-blocking by default when configured
#endif

// Stub implementations for sendmsg/recvmsg (minimal functionality)
#ifdef __cplusplus
extern "C" {
#endif

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags);

#ifdef __cplusplus
}
#endif

// Windows socket functions use different types than POSIX
// Windows: int* for length parameters, POSIX: socklen_t*
// Windows: char* for buffers, POSIX: void*
#ifndef setsockopt_compat
inline int setsockopt_compat(SOCKET s, int level, int optname, const void* optval, socklen_t optlen) {
    return setsockopt(s, level, optname, (const char*)optval, optlen);
}
inline int getsockopt_compat(SOCKET s, int level, int optname, void* optval, socklen_t* optlen) {
    return getsockopt(s, level, optname, (char*)optval, optlen);
}
inline int getpeername_compat(int s, struct sockaddr* name, socklen_t* namelen) {
    return getpeername((SOCKET)s, name, (int*)namelen);
}
inline int getsockname_compat(int s, struct sockaddr* name, socklen_t* namelen) {
    return getsockname((SOCKET)s, name, (int*)namelen);
}
inline ssize_t send_compat(SOCKET s, const void* buf, size_t len, int flags) {
    return send(s, (const char*)buf, (int)len, flags);
}
inline ssize_t recv_compat(SOCKET s, void* buf, size_t len, int flags) {
    return recv(s, (char*)buf, (int)len, flags);
}
#define setsockopt setsockopt_compat
#define getsockopt getsockopt_compat
#define getpeername getpeername_compat
#define getsockname getsockname_compat
#define send send_compat
#define recv recv_compat
#endif

// TCP keepalive options (Linux compatibility)
#ifndef TCP_KEEPALIVE
#define TCP_KEEPALIVE 9   // Keepalive idle time (Windows uses different API)
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 5   // Keepalive interval
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 6     // Keepalive count
#endif
