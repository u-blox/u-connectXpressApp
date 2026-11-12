// Stub fcntl.h for Windows compilation
// Provides minimal POSIX fcntl compatibility
#pragma once

#include <io.h>
#include <fcntl.h>

// Windows doesn't have fcntl(), provide stubs
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 04000

// Place in global namespace so ::fcntl and ::pipe work
static inline int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    // Windows doesn't support fcntl properly
    // Return success for now
    return 0;
}

// Map pipe to _pipe
static inline int pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_BINARY);
}
