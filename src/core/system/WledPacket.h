#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

/// The 44-byte header a WLED device broadcasts for discovery.
///
/// @moreinfo
///
/// It goes to the broadcast address on UDP 65506, and it is discovery rather than sync.
/// A WLED that receives one lists the sender rather than mirroring its state; sync and control are a separate protocol on a port WLED never shares.
///
/// MoonLight uses it in both directions, both discovery-only.
/// It parses inbound packets to find real WLED devices on the LAN.
/// It builds outbound ones so MoonLight devices find each other, and so a WLED app browsing that port lists us too.
///
/// ## The wire layout
///
/// Exactly 44 bytes, packed, with little-endian fields.
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 1 | token, always 255, a magic byte |
/// | 1 | 1 | id, always 1, magic and protocol |
/// | 2 | 4 | the sender's IPv4 octets |
/// | 6 | 32 | the hostname, null-padded |
/// | 38 | 1 | the board kind in the low seven bits, the top bit meaning the lights are on |
/// | 39 | 1 | the last IP octet, which WLED uses as an instance index |
/// | 40 | 4 | a numeric build date, informational only |
///
/// ## What WLED checks
///
/// WLED validates the token, the id, and that the first octet is in its own subnet.
///
/// ## How a MoonLight peer is recognized
///
/// A MoonLight device broadcasts a WLED-valid packet so the WLED apps list it, but a peer must be able to tell a MoonLight device from a generic WLED.
/// A sentinel is stamped into the version field, which no WLED validator reads, so the packet stays valid while marking us uniquely.
/// It is ASCII `MM` with a small protocol version, little-endian.
///
/// ## Prior art
///
/// The layout was observed from a live WLED and cross-checked against the field names in MoonLight's device module, then written fresh here.
/// The native-app discovery contract was reverse-engineered from Christophe Gagnier's [WLED-Android client](https://github.com/Moustachauve/WLED-Android), whose discovery and state models told us exactly which fields the app requires.
struct WledPacket {
    static constexpr uint16_t kPort = 65506;   ///< the UDP port presence is broadcast on
    static constexpr size_t   kSize = 44;      ///< the fixed header size
    static constexpr uint8_t  kToken = 255;    ///< the first magic byte
    static constexpr uint8_t  kId = 1;         ///< the second magic byte
    static constexpr size_t   kNameOff = 6;    ///< where the hostname starts
    static constexpr size_t   kNameMax = 32;   ///< how many bytes the hostname may fill
    static constexpr size_t   kTypeOff = 38;   ///< where the board kind and lights-on bit sit

    /// Whether this is a valid presence header, any other datagram reading as false.
    static bool isValid(const uint8_t* data, size_t len) {
        // Exactly kSize: a longer datagram on this port is something else, not presence.
        return data && len == kSize && data[0] == kToken && data[1] == kId;
    }

    /// Copy the null-padded name into `out`, truncating to fit, once `isValid` has passed.
    static void readName(const uint8_t* data, char* out, size_t outCap) {
        if (!out || outCap == 0) return;
        size_t n = 0;
        for (; n < kNameMax && n < outCap - 1; n++) {
            uint8_t c = data[kNameOff + n];
            if (c == 0) break;
            out[n] = static_cast<char>(c);
        }
        out[n] = 0;
    }

    /// Build one presence packet into `out`, whose first IP octet must be real or WLED rejects it.
    static void build(uint8_t* out, const uint8_t ip[4], const char* name,
                      uint8_t boardType, bool lightsOn) {
        std::memset(out, 0, kSize);
        out[0] = kToken;
        out[1] = kId;
        out[2] = ip[0]; out[3] = ip[1]; out[4] = ip[2]; out[5] = ip[3];
        if (name) {
            size_t n = std::strlen(name);
            if (n > kNameMax) n = kNameMax;   // bytes 6..37, null-padded by the memset
            // NOLINTNEXTLINE(bugprone-not-null-terminated-result) zeroed above, so null-padded
            std::memcpy(out + kNameOff, name, n);
        }
        out[kTypeOff] = static_cast<uint8_t>((boardType & 0x7f) | (lightsOn ? 0x80 : 0));
        out[39] = ip[3];   // insId = last IP octet, matching WLED's convention
        // version (40..43) left 0 — informational only, no validator reads it.
    }

    static constexpr size_t  kMarkerOff = 40;            ///< where the MoonLight sentinel is stamped
    static constexpr uint32_t kMmMarker = 0x014d4d00u;   ///< the sentinel itself, ASCII MM and a version

    /// Stamp the sentinel that marks this packet as a MoonLight peer's.
    static void stampMmMarker(uint8_t* out) {
        out[kMarkerOff + 0] = (kMmMarker >> 0) & 0xff;
        out[kMarkerOff + 1] = (kMmMarker >> 8) & 0xff;
        out[kMarkerOff + 2] = (kMmMarker >> 16) & 0xff;
        out[kMarkerOff + 3] = (kMmMarker >> 24) & 0xff;
    }

    /// Whether this packet carries the MoonLight sentinel.
    static bool hasMmMarker(const uint8_t* data, size_t len) {
        if (len < kSize) return false;
        uint32_t v = uint32_t(data[kMarkerOff]) | (uint32_t(data[kMarkerOff + 1]) << 8)
                   | (uint32_t(data[kMarkerOff + 2]) << 16) | (uint32_t(data[kMarkerOff + 3]) << 24);
        return v == kMmMarker;
    }
};

}  // namespace mm
