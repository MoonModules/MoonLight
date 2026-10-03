#pragma once

#include "core/util/fnv.h"

#include <cstdint>

/// @defgroup IpSettings An interface's IP settings, DHCP or Static, in the words routers and phones use
/// @{
/// Shared by every interface that keeps its own addressing: Ethernet's card and each known WiFi network.

namespace mm::ipsettings {

inline constexpr uint8_t kDhcp = 0;     ///< an address leased from the network
inline constexpr uint8_t kStatic = 1;   ///< the address the user set
/// The select's options, in the order of the values above.
inline constexpr const char* kOptions[] = {"DHCP", "Static"};
/// The four addresses a static setting holds, in the order they are written.
inline constexpr const char* kFields[4] = {"ip", "gateway", "subnet", "dns"};

/// A fingerprint over the mode and the four addresses, so an edit to any of them re-applies.
inline uint32_t sig(uint8_t mode, const uint8_t ip[4], const uint8_t gateway[4], const uint8_t subnet[4], const uint8_t dns[4]) {
    Fnv1a f;
    f.add(mode);
    f.add(ip, 4);
    f.add(gateway, 4);
    f.add(subnet, 4);
    f.add(dns, 4);
    return f.h;
}

}  // namespace mm::ipsettings

/// @}
