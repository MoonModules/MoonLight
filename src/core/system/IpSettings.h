#pragma once

#include "core/util/fnv.h"
#include "core/util/Ipv4.h"   // the rule a static setting is checked by, shared with MoonBase
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

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
// In ipv4::Fault's order, with no sentence for None; the app's alone, so MoonBase carries none of the text.
/// What the card says about each reason a static setting is not used.
inline constexpr const char* kWhy[] = {
    nullptr,
    "static IP not used: the subnet mask is not a valid mask",
    "static IP not used: no address is set",
    "static IP not used: the address is not a host address",
    "static IP not used: the address is the subnet's network or broadcast address",
    "static IP not used: the gateway is outside the subnet",
    "static IP not used: the gateway is the device's own address",
    "static IP not used: the gateway is the subnet's network or broadcast address",
    "static IP not used: the DNS server is not a host address",
};
static_assert(sizeof(kWhy) / sizeof(kWhy[0]) == static_cast<size_t>(ipv4::Fault::Dns) + 1, "a sentence for every fault");

}  // namespace mm::ipsettings

namespace mm {

/// One interface's IP settings: DHCP or Static, and the four addresses Static pins.
struct IpSettings {
    uint8_t mode = ipsettings::kDhcp;   ///< DHCP or Static
    uint8_t ip[4] = {};                 ///< the address
    uint8_t gateway[4] = {};            ///< the router
    uint8_t subnet[4] = {255, 255, 255, 0};   ///< the netmask
    uint8_t dns[4] = {};                ///< the name server

    /// Whether the user pinned a static address.
    bool isStatic() const MM_NONBLOCKING { return mode == ipsettings::kStatic; }

    /// The address at `k` in `kFields` order.
    constexpr uint8_t* address(uint8_t k) MM_NONBLOCKING { return k == 0 ? ip : k == 1 ? gateway : k == 2 ? subnet : dns; }
    /// The same, read-only.
    constexpr const uint8_t* address(uint8_t k) const MM_NONBLOCKING { return const_cast<IpSettings*>(this)->address(k); }
    /// The address a field name names, or null for any other name.
    uint8_t* address(const char* name) {
        for (uint8_t k = 0; k < 4; k++)
            if (std::strcmp(name, ipsettings::kFields[k]) == 0) return address(k);
        return nullptr;
    }

    /// A fingerprint over the mode and the four addresses, so an edit to any of them re-applies.
    constexpr uint32_t sig() const MM_NONBLOCKING {
        Fnv1a f;
        f.add(mode);
        for (uint8_t k = 0; k < 4; k++) f.add(address(k), 4);
        return f.h;
    }

    /// Why a Static setting cannot be used, a sentence for the card, or null when it can.
    const char* problem() const MM_NONBLOCKING {
        return ipsettings::kWhy[static_cast<uint8_t>(ipv4::staticFault(ip, gateway, subnet, dns))];
    }

    /// Whether a Static setting is chosen and can be used; an unusable one leaves the DHCP client running.
    bool usable() const MM_NONBLOCKING { return isStatic() && !problem(); }

    /// A fingerprint of what is in effect: the Static setting when usable, else DHCP, whatever the unused fields hold.
    uint32_t effectiveSig() const MM_NONBLOCKING { return usable() ? sig() : IpSettings{}.sig(); }   // the DHCP side folds to a constant

    /// What `iface` runs with now, as Static settings: each address the lease carries, the defaults for the rest, and the gateway standing in for a missing DNS server.
    static IpSettings leased(platform::NetIface iface) {
        uint8_t lease[4][4];
        platform::netGetIPv4(iface, lease[0], lease[1], lease[2], lease[3]);
        if (!ipv4::word(lease[3])) std::memcpy(lease[3], lease[1], 4);
        IpSettings s;
        s.mode = ipsettings::kStatic;
        for (uint8_t k = 0; k < 4; k++)
            if (ipv4::word(lease[k])) std::memcpy(s.address(k), lease[k], 4);
        return s;
    }

    /// Start a Static setting from what `iface` runs with now, when it has an address and no address is set yet.
    void prefillFrom(platform::NetIface iface) {
        if (!isStatic() || ipv4::word(ip)) return;
        const IpSettings lease = leased(iface);
        if (!ipv4::word(lease.ip)) return;
        for (uint8_t k = 0; k < 4; k++) std::memcpy(address(k), lease.address(k), 4);
    }

    /// Start a Static setting's gateway, mask and DNS server from the network `iface` is on, leaving the address to choose, when no address is set yet.
    void prefillNetworkFrom(platform::NetIface iface) {
        if (!isStatic() || ipv4::word(ip)) return;
        const IpSettings lease = leased(iface);
        if (!ipv4::word(lease.ip)) return;
        for (uint8_t k = 1; k < 4; k++) std::memcpy(address(k), lease.address(k), 4);
    }

    /// Pin the static address onto `iface`, or leave the DHCP client running.
    void applyStatic(platform::NetIface iface) const {
        if (usable()) platform::netSetStaticIPv4(iface, ip, gateway, subnet, dns);
    }

    /// Apply a change to a running interface: pin the static address, or lease again otherwise.
    void applyLive(platform::NetIface iface) const {
        if (usable()) applyStatic(iface);
        else platform::netSetDhcp(iface);
    }

};

/// The IP settings an interface last applied, so only an edit applies again.
struct IpApplied {
    uint32_t sig = 0;      ///< the fingerprint applied
    bool valid = false;    ///< false until the first apply, since any value is a valid fingerprint

    /// Record what `s` puts in effect as applied.
    void mark(const IpSettings& s) MM_NONBLOCKING { sig = s.effectiveSig(); valid = true; }
    /// Record what `s` puts in effect, and say whether that differs from before, so an edit that leaves the addressing as it is touches nothing.
    bool changedTo(const IpSettings& s) MM_NONBLOCKING {
        const uint32_t now = s.effectiveSig();
        if (valid && now == sig) return false;
        sig = now;
        valid = true;
        return true;
    }
};

}  // namespace mm

/// @}
