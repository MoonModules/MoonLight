#pragma once

#include <cstdint>

namespace mm {

/// The web page's service name, which Discovery reads back to announce the page's port.
inline constexpr const char* kHttpService = "HTTP";

/// What a module offers on the network and what it exchanges with whom, reported by the module itself, the way it reports its memory and tick time.
///
/// @technical
///
/// A module already counts its traffic for its status line, so a report reads those counters once a second and adds nothing per packet.
///
/// The protocol and unit names are string literals: the collector keeps the pointers until the next report.
class NetworkUse {
public:
    /// A reporter outlives the report it is handed to, and is destroyed through this base.
    virtual ~NetworkUse() = default;
    /// A service this module offers: a protocol's name and the port it listens on, 0 for one outside IP such as raw Ethernet.
    virtual void service(const char* protocol, uint16_t port) = 0;
    /// Traffic with one peer: the protocol's name, which way, the peer's address, a multicast group or the broadcast address included, and how many `unit` a second, 0 where the module keeps no rate.
    virtual void flow(const char* protocol, bool outbound, const uint8_t peer[4], uint32_t perSecond, const char* unit) = 0;
};

}  // namespace mm
