/* Stands in for u-connectMatter's matter.config: what the copied platform files read from it, set by the plug-in
 * before the stack starts. */

#pragma once

#include <cstdint>
#include <string>

namespace ucxmatter {

struct DeviceConfig
{
    std::string deviceName            = "u-connectXpress";
    std::string macSuffix             = "000000"; // last three bytes of the module's Bluetooth address
    std::string manufacturingDate;
    std::string softwareVersionString = "1.0";
    uint32_t softwareVersion          = 1;
    uint16_t vendorId                 = 0xFFF1;
    uint16_t productId                = 0x8000;
    uint16_t discriminator            = 3840;
};

DeviceConfig & GetDeviceConfig();

} // namespace ucxmatter
