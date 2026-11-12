/* Stub sys/time.h for Windows */
#pragma once

#if defined(_WIN32)

#include <time.h>
#include <winsock2.h> // For struct timeval

// POSIX microseconds type
typedef long suseconds_t;

// timeval already defined in winsock2.h on Windows
// struct timeval { long tv_sec; long tv_usec; };

#else

// Non-Windows targets (e.g. ARM cross-compiles) share this connectedhomeip
// checkout with the Windows build; fall through to the real toolchain header.
#include_next <sys/time.h>

#endif // defined(_WIN32)

