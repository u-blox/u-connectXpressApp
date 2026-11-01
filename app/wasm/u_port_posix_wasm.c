/* u-connectClient's POSIX port for the browser build. Emscripten's clock_gettime() rejects CLOCK_MONOTONIC_RAW
 * (the tick would be uninitialised memory), so the port gets CLOCK_MONOTONIC instead. */
#include <time.h>
#undef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#include "../../u-connectClient/ports/os/u_port_posix.c"
