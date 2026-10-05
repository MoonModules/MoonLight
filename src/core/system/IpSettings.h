#pragma once

#include "core/util/fnv.h"
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

    /// Why a Static setting cannot be used, a sentence for the card, or null when it can; gateway and DNS may be left at 0.0.0.0 for none.
    const char* problem() const MM_NONBLOCKING {
        const uint32_t a = word(ip), m = word(subnet), d = word(dns);
        if (!isMask(m)) return "static IP not used: the subnet mask is not a valid mask";
        if (const char* why = addressProblem(a, m)) return why;
        if (const char* why = gatewayProblem(word(gateway), a, m)) return why;
        if (d && !isHost(d)) return "static IP not used: the DNS server is not a host address";
        return nullptr;
    }

    /// Whether a Static setting is chosen and can be used; an unusable one leaves the DHCP client running.
    bool usable() const MM_NONBLOCKING { return isStatic() && !problem(); }

    /// A fingerprint of what is in effect: the Static setting when usable, else DHCP, whatever the unused fields hold.
    uint32_t effectiveSig() const MM_NONBLOCKING { return usable() ? sig() : IpSettings{}.sig(); }   // the DHCP side folds to a constant

    /// What `iface` runs with now, as Static settings: each address the lease carries, the defaults for the rest, and the gateway standing in for a missing DNS server.
    static IpSettings leased(platform::NetIface iface) {
        uint8_t lease[4][4];
        platform::netGetIPv4(iface, lease[0], lease[1], lease[2], lease[3]);
        if (!word(lease[3])) std::memcpy(lease[3], lease[1], 4);
        IpSettings s;
        s.mode = ipsettings::kStatic;
        for (uint8_t k = 0; k < 4; k++)
            if (word(lease[k])) std::memcpy(s.address(k), lease[k], 4);
        return s;
    }

    /// Start a Static setting from what `iface` runs with now, when it has an address and no address is set yet.
    void prefillFrom(platform::NetIface iface) {
        if (!isStatic() || word(ip)) return;
        const IpSettings lease = leased(iface);
        if (!word(lease.ip)) return;
        for (uint8_t k = 0; k < 4; k++) std::memcpy(address(k), lease.address(k), 4);
    }

    /// Start a Static setting's gateway, mask and DNS server from the network `iface` is on, leaving the address to choose, when no address is set yet.
    void prefillNetworkFrom(platform::NetIface iface) {
        if (!isStatic() || word(ip)) return;
        const IpSettings lease = leased(iface);
        if (!word(lease.ip)) return;
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

private:
    static uint32_t word(const uint8_t q[4]) MM_NONBLOCKING {
        return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
    }
    // Not 0.x, loopback 127.x, or multicast and reserved from 224.x on.
    static bool isHost(uint32_t a) MM_NONBLOCKING {
        const uint32_t first = a >> 24;
        return first != 0 && first != 127 && first < 224;
    }
    // Ones then zeros, with at least one of each.
    static bool isMask(uint32_t m) MM_NONBLOCKING { return m != 0 && m != 0xFFFFFFFFu && (~m & (~m + 1)) == 0; }
    // A /31 has no network or broadcast address (RFC 3021), so both of its addresses are hosts.
    static bool isNetworkOrBroadcast(uint32_t a, uint32_t m) MM_NONBLOCKING {
        return ~m > 1 && ((a & ~m) == 0 || (a | m) == 0xFFFFFFFFu);
    }
    static const char* addressProblem(uint32_t a, uint32_t m) MM_NONBLOCKING {
        if (a == 0) return "static IP not used: no address is set";
        if (!isHost(a)) return "static IP not used: the address is not a host address";
        if (isNetworkOrBroadcast(a, m)) return "static IP not used: the address is the subnet's network or broadcast address";
        return nullptr;
    }
    // A gateway of 0.0.0.0 is none, which a local-only device runs without.
    static const char* gatewayProblem(uint32_t g, uint32_t a, uint32_t m) MM_NONBLOCKING {
        if (!g) return nullptr;
        if ((g & m) != (a & m)) return "static IP not used: the gateway is outside the subnet";
        if (g == a) return "static IP not used: the gateway is the device's own address";
        if (isNetworkOrBroadcast(g, m)) return "static IP not used: the gateway is the subnet's network or broadcast address";
        return nullptr;
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
