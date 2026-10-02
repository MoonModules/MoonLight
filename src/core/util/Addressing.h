#pragma once

#include "core/util/HostList.h"   // Host: a unicast send goes to each parsed host

#include <cstdint>

namespace mm {

/// @defgroup Addressing How a service reaches many receivers
/// @{
/// One rule for every service that sends one packet to many receivers: unicast to a host list, multicast to a group, or broadcast.
/// Why each, and how discovery proves a network drops multicast: [Multicast and IGMP snooping](../../../explanation/architecture/moonlight.md#multicast-and-igmp-snooping).

/// How a service sends to many receivers; a service offers only the modes its protocol allows.
enum class Addressing : uint8_t {
    Unicast,              ///< a copy to each host in the list
    Multicast,            ///< one copy to the group
    MulticastBroadcast,   ///< the group, plus the broadcast address
    Broadcast,            ///< one copy to every device on the subnet
};

/// The modes' names as a select shows them, indexed by `Addressing`.
inline constexpr const char* kAddressingNames[] = {"unicast", "multicast", "multicast + broadcast", "broadcast"};

/// The limited broadcast address, which every device on the subnet receives.
inline constexpr uint8_t kBroadcastAddress[4] = {255, 255, 255, 255};

/// What the network has proven about multicast, set by discovery's evidence.
struct NetworkPath {
    /// A peer's presence kept arriving over broadcast with no multicast copy, so the group does not reach this device.
    static inline bool multicastDropped = false;
};

/// How often a service sends, which decides whether a network that drops multicast gets a broadcast copy.
enum class Traffic : uint8_t {
    Occasional,   ///< presence every ten seconds, feedback while a control moves: a broadcast copy costs the LAN next to nothing
    FrameRate,    ///< a steady stream, lights or audio: a broadcast copy per packet would load every device on the LAN
};

/// Whether a send in `mode` also goes to the broadcast address, by choice or because the network proved it must.
inline bool alsoBroadcast(Addressing mode, Traffic traffic) {
    return mode == Addressing::MulticastBroadcast
        || (mode == Addressing::Multicast && traffic == Traffic::Occasional && NetworkPath::multicastDropped);
}

/// Send one packet where `mode` says through `send(ip)`, skipping a named host not resolved yet.
template <class Send>
void sendAddressed(Addressing mode, Traffic traffic, const Host* hosts, uint8_t hostCount, const uint8_t group[4], Send&& send) {
    switch (mode) {
        case Addressing::Unicast:
            for (uint8_t i = 0; i < hostCount; i++) {
                const uint8_t* ip = hosts[i].ip;
                if (ip[0] || ip[1] || ip[2] || ip[3]) send(ip);
            }
            return;
        case Addressing::Broadcast:
            send(kBroadcastAddress);
            return;
        case Addressing::Multicast:
        case Addressing::MulticastBroadcast:
            send(group);
            if (alsoBroadcast(mode, traffic)) send(kBroadcastAddress);
            return;
    }
}

/// @}
}  // namespace mm
