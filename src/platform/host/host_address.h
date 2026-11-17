#pragma once

/// Which of the host's addresses is its address on the LAN, apart from the interface walk so a test can hand it any host.

#include <cstddef>
#include <cstdint>
#include <cstring>
#ifndef _WIN32
  #include <net/if.h>   // IFF_UP and the other interface flags getifaddrs reports
#endif

namespace mm::platform {

/// One IPv4 address the host holds.
struct HeldAddress {
    uint32_t addr;      ///< the address, in network order
    unsigned flags;     ///< the IFF_ flags of the interface holding it
    bool virtualName;   ///< the interface is one virtualInterface() names
};

/// Whether `name` is a virtual interface: loopback and macOS's tunnels and radios by family and number (`utun3`), Linux's container and VM networks by any suffix (`veth1a2b3c`).
inline bool virtualInterface(const char* name) {
    static constexpr const char* kNumbered[] = {"lo", "utun", "awdl", "llw", "anpi", "bridge", "gif", "stf", "ap", "pktap"};
    static constexpr const char* kAnySuffix[] = {"veth", "docker", "br-", "virbr"};
    if (!name) return false;
    for (const char* p : kAnySuffix)
        if (std::strncmp(name, p, std::strlen(p)) == 0) return true;
    for (const char* p : kNumbered) {
        const size_t l = std::strlen(p);
        if (std::strncmp(name, p, l) == 0 && (name[l] == 0 || (name[l] >= '0' && name[l] <= '9'))) return true;
    }
    return false;
}

#ifndef _WIN32
/// Whether `held` sits on a LAN: up, running and broadcasting, which a VPN tunnel and loopback are not, on an interface virtualInterface() leaves out, as a container's bridge.
inline bool reachesLan(const HeldAddress& held) {
    constexpr unsigned kNeeded = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
    return (held.flags & kNeeded) == kNeeded && !(held.flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) && !held.virtualName;
}
#endif

/// The host's LAN address: the default route's own address `route` when a LAN interface holds it, else the first LAN address, so a VPN holding the default route leaves the device on the LAN it serves; `route` where no interface reaches a LAN.
inline uint32_t lanAddress(uint32_t route, [[maybe_unused]] const HeldAddress* held, [[maybe_unused]] size_t count) {
#ifndef _WIN32
    uint32_t first = 0;
    for (size_t i = 0; i < count; i++) {
        if (!reachesLan(held[i])) continue;
        if (held[i].addr == route) return route;
        if (!first) first = held[i].addr;
    }
    if (first) return first;
#endif
    return route;
}

}  // namespace mm::platform
