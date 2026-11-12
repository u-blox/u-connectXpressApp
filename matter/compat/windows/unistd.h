// Windows unistd.h compatibility header
// Provides POSIX functionality stubs for Windows
#pragma once

#if defined(_WIN32)

#include <io.h>
#include <process.h>
#include <direct.h>

// POSIX type definitions
#ifndef ssize_t
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif

// Map POSIX functions to Windows equivalents
#ifndef close
#define close _close
#endif

#ifndef read
#define read _read
#endif

#ifndef write
#define write _write
#endif

#ifndef getpid
#define getpid _getpid
#endif

#ifndef getcwd
#define getcwd _getcwd
#endif

#ifndef chdir
#define chdir _chdir
#endif

#ifndef dup
#define dup _dup
#endif

#ifndef dup2
#define dup2 _dup2
#endif

#ifndef isatty
#define isatty _isatty
#endif

#ifndef lseek
#define lseek _lseek
#endif

// File access modes (may already be defined)
#ifndef R_OK
#define R_OK 4  // Read permission
#define W_OK 2  // Write permission
#define X_OK 1  // Execute permission (always granted on Windows)
#define F_OK 0  // Existence
#endif

// usleep is not available on Windows, use Sleep from windows.h
#ifndef usleep
#include <windows.h>
// usleep takes microseconds, Sleep takes milliseconds
static inline int usleep(unsigned int usec) {
    Sleep(usec / 1000);
    return 0;
}
#endif

// sleep (seconds)
#ifndef sleep
static inline unsigned int sleep(unsigned int seconds) {
    Sleep(seconds * 1000);
    return 0;
}
#endif

#else

// Non-Windows targets (e.g. ARM cross-compiles) share this connectedhomeip
// checkout with the Windows build; fall through to the real toolchain header.
#include_next <unistd.h>

#endif // defined(_WIN32)

