/* ucxclient platform CHIPDevicePlatformEvent.h
 * Provides a placeholder `ChipDevicePlatformEvent` union used by
 * CHIPDeviceEvent to hold platform-specific event data. This is a
 * minimal implementation for builds that don't require detailed
 * platform-specific event fields.
 */
#pragma once

#include <stdint.h>

namespace chip {
namespace DeviceLayer {

struct ChipDevicePlatformEvent
{
    // Placeholder fields for platform-specific events. Real platforms
    // can extend this with detailed event payloads.
    union
    {
        uint32_t Dummy;
    } Data;
};

} // namespace DeviceLayer
} // namespace chip
