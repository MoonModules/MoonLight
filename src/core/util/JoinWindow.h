#pragma once

#include <cstdint>

namespace mm {

/// @defgroup JoinWindow How long a station gets to join a network
/// @{
/// The network cascade and the provisioning answer read the same windows, so a board tells the installer it failed exactly when it gives up itself.

/// How long WiFi gets to join a network, first or again after a dropout.
inline constexpr uint32_t kStaGraceMs = 10000;
/// How long a lease may take: a live Ethernet link or a joined station without an address, or an interface re-leasing after a switch to DHCP.
inline constexpr uint32_t kDhcpWaitMs = 15000;
/// How long a station gets before the next network or the fallback: the join's grace, plus the lease's wait once it has joined, as Ethernet waits on a live link.
inline constexpr uint32_t staWindowMs(bool associated) { return associated ? kStaGraceMs + kDhcpWaitMs : kStaGraceMs; }

/// @}
}  // namespace mm
