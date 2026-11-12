// Implementation of if_nameindex/if_freenameindex for Windows
// (Windows provides if_nametoindex/if_indextoname in netioapi.h)
#include "if.h"

extern "C" {

// Get list of network interfaces (stub - returns nullptr)
// Windows doesn't provide this BSD function - return empty list
struct if_nameindex *if_nameindex(void) {
    return nullptr;  // No interfaces available in stub
}

// Free interface list (stub - does nothing)
void if_freenameindex(struct if_nameindex *ptr) {
    (void)ptr;
    // Nothing to free in stub implementation
}

} // extern "C"
