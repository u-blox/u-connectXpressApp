// Stub sys/select.h for Windows compilation
// Maps POSIX select API to Windows equivalents
#pragma once

#if defined(_WIN32)

// Windows has select() in winsock2.h
#include <winsock2.h>

// Define fd_set limit if not already defined
#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif

#else

// Non-Windows targets (e.g. ARM cross-compiles) share this connectedhomeip
// checkout with the Windows build; fall through to the real toolchain header.
#include_next <sys/select.h>

#endif // defined(_WIN32)
