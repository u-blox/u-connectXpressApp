/* Stands in for u-connectMatter's matter.config: what the copied platform files read from it, set by the plug-in
 * before the stack starts. */

#pragma once

#include <cstdint>
#include <string>

namespace ucxmatter {

struct DeviceConfig
{
    // What the Basic Information cluster tells a controller; the plug-in fills in the module's own values
    std::string deviceName            = "u-connectXpressApp"; // ProductName, and with the address the mDNS "DN"
    std::string vendorName            = "u-blox";
    std::string productLabel          = "u-connectXpressApp";
    std::string productUrl            = "https://www.u-blox.com";
    std::string macSuffix             = "000000"; // last three bytes of the module's Bluetooth address
    std::string hardwareVersionString = "u-connectXpress module"; // the module's type code, "NORA-W361-00B"
    std::string partNumber;                       // the same type code; empty: none
    std::string serialNumber;                     // the module's; empty: none
    std::string uniqueId;                         // drawn with the setup codes
    std::string manufacturingDate;
    std::string softwareVersionString = "1.0";    // the module's firmware, "3.5.0-120"
    uint32_t softwareVersion          = 1;
    uint16_t hardwareVersion          = 1;
    uint16_t vendorId                 = 0xFFF1;
    uint16_t productId                = 0x8000;
    uint16_t discriminator            = 3840;
};

DeviceConfig & GetDeviceConfig();

} // namespace ucxmatter
