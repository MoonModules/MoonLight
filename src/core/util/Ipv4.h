#pragma once

#include "platform/nonblocking.h"   // MM_NONBLOCKING, a compiler marker with no platform contract

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mm {

/// @defgroup Ipv4 An IPv4 address as four octets
/// @{
/// Reading one from text, and whether a static setting can be used.
///
/// MoonBase includes it as well as the app, so the recovery image refuses the same static settings the app refuses and comes up where the app does.

/// Parse "A.B.C.D" into four octets, returning false on anything else; `out` is written only on success, as `inet_pton` does.
inline bool parseDottedQuad(const char* s, uint8_t out[4]) {
    if (!s) return false;
    uint8_t q[4];
    const char* p = s;
    for (int idx = 0; idx < 4; idx++) {
        char* end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return false;
        q[idx] = static_cast<uint8_t>(v);
        // Three dots between the octets, and nothing after the last (e.g. "1.2.3.4x" fails).
        if (*end != (idx == 3 ? '\0' : '.')) return false;
        p = end + 1;
    }
    std::memcpy(out, q, 4);
    return true;
}

namespace ipv4 {

/// Four octets as one number, the first octet highest.
inline uint32_t word(const uint8_t q[4]) MM_NONBLOCKING {
    return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
}

// Not 0.x, loopback 127.x, or multicast and reserved from 224.x on.
/// Whether an address can name a host.
inline bool isHost(uint32_t a) MM_NONBLOCKING {
    const uint32_t first = a >> 24;
    return first != 0 && first != 127 && first < 224;
}

/// Whether a netmask is ones then zeros, with at least one of each.
inline bool isMask(uint32_t m) MM_NONBLOCKING { return m != 0 && m != 0xFFFFFFFFu && (~m & (~m + 1)) == 0; }

// A /31 has no network or broadcast address (RFC 3021), so both of its addresses are hosts.
/// Whether an address is its subnet's network or broadcast address.
inline bool isNetworkOrBroadcast(uint32_t a, uint32_t m) MM_NONBLOCKING {
    return ~m > 1 && ((a & ~m) == 0 || (a | m) == 0xFFFFFFFFu);
}

// A code rather than a sentence, so MoonBase, which only asks whether a setting is usable, carries none of the text.
/// Why a static setting cannot be used, or None when it can.
enum class Fault : uint8_t { None, Mask, NoAddress, AddressNotHost, AddressReserved, GatewayOutside, GatewaySelf, GatewayReserved, Dns };

/// Why a static address cannot be used in its subnet.
inline Fault addressFault(uint32_t a, uint32_t m) MM_NONBLOCKING {
    if (a == 0) return Fault::NoAddress;
    if (!isHost(a)) return Fault::AddressNotHost;
    if (isNetworkOrBroadcast(a, m)) return Fault::AddressReserved;
    return Fault::None;
}

// A gateway of 0.0.0.0 is none, which a local-only device runs without.
/// Why a gateway cannot be used for an address.
inline Fault gatewayFault(uint32_t g, uint32_t a, uint32_t m) MM_NONBLOCKING {
    if (!g) return Fault::None;
    if ((g & m) != (a & m)) return Fault::GatewayOutside;
    if (g == a) return Fault::GatewaySelf;
    if (isNetworkOrBroadcast(g, m)) return Fault::GatewayReserved;
    return Fault::None;
}

/// Why a static setting cannot be used; gateway and DNS may be left at 0.0.0.0 for none.
inline Fault staticFault(const uint8_t ip[4], const uint8_t gateway[4], const uint8_t subnet[4],
                         const uint8_t dns[4]) MM_NONBLOCKING {
    const uint32_t a = word(ip), m = word(subnet), d = word(dns);
    if (!isMask(m)) return Fault::Mask;
    if (const Fault f = addressFault(a, m); f != Fault::None) return f;
    if (const Fault f = gatewayFault(word(gateway), a, m); f != Fault::None) return f;
    if (d && !isHost(d)) return Fault::Dns;
    return Fault::None;
}

}  // namespace ipv4

/// @}

}  // namespace mm
