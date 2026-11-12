/* Stub fcntl.h for Windows - WakeEvent compatibility */
#pragma once

#include <io.h>

// Undefine Windows socket event macros that conflict with Matter SDK code
// WakeEvent.cpp uses FD_READ/FD_WRITE as enum names
#ifdef FD_READ
#undef FD_READ
#endif
#ifdef FD_WRITE
#undef FD_WRITE
#endif

// POSIX fcntl command codes
#define F_GETFL 3
#define F_SETFL 4
#define F_SETFD 2  // Set file descriptor flags
#define F_GETFD 1  // Get file descriptor flags

// POSIX file status flags  
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000  // Close-on-exec flag (stub for Windows)

// Windows _O_BINARY flag for binary mode pipes
#ifndef _O_BINARY
#define _O_BINARY 0x8000
#endif

// Stub fcntl function for Windows
// Note: Windows doesn't have true fcntl support, this is a minimal stub
inline int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    return 0; // Stub: always succeeds
}

// Map POSIX pipe to Windows _pipe
inline int pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_BINARY);
}
