// Stub sys/ioctl.h for Windows compilation
// Maps POSIX ioctl to Windows equivalents
#pragma once

// Windows provides ioctl functionality via ioctlsocket in winsock2
#include <winsock2.h>

// Map POSIX ioctl to Windows ioctlsocket with type conversion
// Windows ioctlsocket uses u_long* but POSIX uses int* or struct*
#ifdef __cplusplus
inline int ioctl_compat(SOCKET s, long cmd, int* argp) {
    u_long val = *argp;
    int result = ioctlsocket(s, cmd, &val);
    *argp = (int)val;
    return result;
}
inline int ioctl_compat(SOCKET s, long cmd, u_long* argp) {
    return ioctlsocket(s, cmd, argp);
}
// Generic overload for struct pointers (like ifreq*)
template<typename T>
inline int ioctl_compat(int fd, long cmd, T* argp) {
    // On Windows, most ioctls don't work on file descriptors
    // For network ifreq operations, this is a stub
    (void)fd; (void)cmd; (void)argp;
    return 0;  // Success stub
}
#define ioctl ioctl_compat
#else
#define ioctl ioctlsocket
#endif

// Common ioctl request codes (mapped to Windows equivalents)
#ifndef FIONBIO
#define FIONBIO 0x5421  // Non-blocking I/O
#endif

#ifndef FIONREAD
#define FIONREAD 0x541B // Bytes available to read
#endif

// TCP-specific ioctl codes (Linux compatibility)
#ifndef TIOCOUTQ
#define TIOCOUTQ 0x5411  // Amount of data in send queue (stub value)
#endif

#ifndef TIOCINQ
#define TIOCINQ FIONREAD  // Amount of data in receive queue
#endif
