// Windows eventfd.h compatibility header
// Provides eventfd() stub using Windows sockets for select() compatibility
#pragma once

#include <winsock2.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// eventfd flags (Linux-compatible, not all used on Windows)
#ifndef EFD_NONBLOCK
#define EFD_NONBLOCK 0x800
#endif

#ifndef EFD_CLOEXEC
#define EFD_CLOEXEC 0x80000
#endif

#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE 0x1
#endif

// eventfd type - on Windows, we use a socket pair for select() compatibility
typedef int eventfd_t;

// eventfd() - Create an event file descriptor
// On Windows, this creates a self-connected UDP socket that works with select()
// Returns -1 on error, socket fd on success
static inline int eventfd(unsigned int initval, int flags) {
    (void)initval;
    (void)flags;
    
    // Create a UDP socket bound to localhost for event signaling
    // This socket can be used with select() on Windows
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return -1;
    }
    
    // Bind to localhost on an ephemeral port
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // Let system choose port
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(sock);
        return -1;
    }
    
    // Get the bound port
    int addrLen = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &addrLen) < 0) {
        closesocket(sock);
        return -1;
    }
    
    // Connect to self so we can use send/recv
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        closesocket(sock);
        return -1;
    }
    
    // Set non-blocking if requested
    if (flags & EFD_NONBLOCK) {
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
    }
    
    return (int)sock;
}

// eventfd_read() - Read from eventfd
static inline int eventfd_read(int fd, eventfd_t *value) {
    char buf[8];
    int n = recv((SOCKET)fd, buf, sizeof(buf), 0);
    if (n > 0 && value) {
        *value = 1;
    }
    return (n > 0) ? 0 : -1;
}

// eventfd_write() - Write to eventfd
static inline int eventfd_write(int fd, eventfd_t value) {
    (void)value;
    char buf = 1;
    int n = send((SOCKET)fd, &buf, 1, 0);
    return (n > 0) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif
