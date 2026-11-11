#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

/// The 44-byte header a WLED device broadcasts for discovery.
///
/// @attribution WLED, Moustachauve
///
/// @technical
///
/// It goes to the broadcast address on UDP 65506 and is discovery, not sync: a WLED that receives one lists the sender rather than mirroring its state.
///
/// MoonLight parses inbound packets to find WLED devices, and builds outbound ones so MoonLight devices and the WLED apps find it.
///
/// ## The wire layout
///
/// | Offset | Size | Field, packed, little-endian |
/// |-------:|-----:|-------|
/// | 0 | 2 | token 255 and id 1, the magic bytes |
/// | 2 | 4 | the sender's IPv4 octets |
/// | 6 | 32 | the hostname, null-padded |
/// | 38 | 1 | the board kind in the low seven bits, the top bit meaning the lights are on |
/// | 39 | 1 | the last IP octet, which WLED uses as an instance index |
/// | 40 | 4 | a numeric build date, informational only |
/// | 44 | 2 | MoonLight and MoonBase only: the web page's TCP port |
/// | 46 | 6 | MoonLight and MoonBase only: the board's base MAC, its identity |
///
/// ## What WLED checks
///
/// WLED validates the token, the id, and that the first octet is in its own subnet.
///
/// ## How a MoonLight peer is recognized
///
/// A sentinel in the version field, which no WLED validator reads, marks a MoonLight device while the packet stays WLED-valid.
///
/// Its top three bytes are ASCII `MM` and a protocol version; the low byte says which copy this is, multicast or broadcast.
///
/// MoonBase, MoonLight's installer, sends the same packet with `MB` in place of `MM`, so a board keeps its place on a peer's map while MoonBase holds it, and no WLED or MoonLight recognizer claims it.
///
/// ## The web page's port and the MAC
///
/// A marked packet appends the page's port, so a peer links it at the right address, and the board's base MAC, which a peer keys its row by: a board moving from WiFi to Ethernet changes its address but not its base MAC, the same on every interface. A 44-byte packet means port 80 and no MAC, as WLED's.
///
/// WLED reads any packet of 40 bytes or more and ignores the rest, so the longer packet stays WLED-valid.
///
/// The layout is observed from a live WLED; the fields the native app requires come from Christophe Gagnier's [WLED-Android client](https://github.com/Moustachauve/WLED-Android).
struct WledPacket {
    static constexpr uint16_t kPort = 65506;   ///< the UDP port presence is broadcast on
    static constexpr uint8_t  kDiscoveryGroup[4] = {239, 255, 77, 77};   ///< the multicast copy's group, organization-local scope, never routed off the network
    static constexpr size_t   kSize = 44;      ///< the fixed header size: @xref{the-wire-layout}
    static constexpr uint8_t  kToken = 255;    ///< the first magic byte
    static constexpr uint8_t  kId = 1;         ///< the second magic byte
    static constexpr size_t   kNameOff = 6;    ///< where the hostname starts
    static constexpr size_t   kNameMax = 32;   ///< how many bytes the hostname may fill
    static constexpr size_t   kTypeOff = 38;   ///< where the board kind and lights-on bit sit
    static constexpr size_t   kPortOff = 44;   ///< where a MoonLight device's web port follows the header: @xref{the-web-pages-port-and-the-mac}
    static constexpr size_t   kMacOff = kPortOff + 2;         ///< where the base MAC follows the port
    static constexpr size_t   kSizeWithMac = kMacOff + 6;     ///< the packet a MoonLight or MoonBase board sends: @xref{the-web-pages-port-and-the-mac}
    static constexpr uint16_t kDefaultHttpPort = 80;   ///< the port an absent field means

    /// Whether this is a valid presence header, any other datagram reading as false: @xref{what-wled-checks}.
    static bool isValid(const uint8_t* data, size_t len) {
        // The header, alone or with MoonLight's tail: any other length on this port is something else, not presence.
        return data && (len == kSize || len == kSizeWithMac) && data[0] == kToken && data[1] == kId;
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
    static constexpr uint32_t kMmMarker = 0x014d4d00u;   ///< the sentinel itself: @xref{how-a-moonlight-peer-is-recognized}
    static constexpr uint32_t kMbMarker = 0x01424d00u;   ///< MoonBase's: `MB` and the same protocol version

    /// Stamp the sentinel that marks this packet as a MoonLight peer's, or with `marker` MoonBase's, as the multicast copy.
    static void stampMarker(uint8_t* out, uint32_t marker = kMmMarker) {
        out[kMarkerOff + 0] = (marker >> 0) & 0xff;
        out[kMarkerOff + 1] = (marker >> 8) & 0xff;
        out[kMarkerOff + 2] = (marker >> 16) & 0xff;
        out[kMarkerOff + 3] = (marker >> 24) & 0xff;
    }

    /// Mark which copy this is, the one sent to the broadcast address or the one sent to the group.
    static void stampVia(uint8_t* out, bool broadcast) { out[kMarkerOff] = broadcast ? 1 : 0; }

    /// Append the web page's port and the board's base MAC, into a buffer of kSizeWithMac; answers the packet's length.
    static size_t stampTail(uint8_t* out, uint16_t port, const uint8_t mac[6]) {
        if (port == 0) port = kDefaultHttpPort;
        out[kPortOff] = static_cast<uint8_t>(port & 0xff);
        out[kPortOff + 1] = static_cast<uint8_t>(port >> 8);
        std::memcpy(out + kMacOff, mac, 6);
        return kSizeWithMac;
    }

    /// Copy the base MAC a valid packet carries into `out`; false when it carries none.
    static bool readMac(const uint8_t* data, size_t len, uint8_t out[6]) {
        if (len < kSizeWithMac) return false;
        std::memcpy(out, data + kMacOff, 6);
        return true;
    }

    /// The web page's port a valid packet carries, 80 when it carries none.
    static uint16_t httpPort(const uint8_t* data, size_t len) {
        if (len < kSizeWithMac) return kDefaultHttpPort;
        const uint16_t port = static_cast<uint16_t>(data[kPortOff] | (data[kPortOff + 1] << 8));
        return port ? port : kDefaultHttpPort;
    }

    /// Whether a marked packet is the broadcast copy.
    static bool viaBroadcast(const uint8_t* data) { return data[kMarkerOff] == 1; }

    /// Whether this packet carries the MoonLight sentinel, or with `marker` MoonBase's, whichever copy it is.
    static bool hasMarker(const uint8_t* data, size_t len, uint32_t marker = kMmMarker) {
        if (len < kSize) return false;
        const uint32_t v = (uint32_t(data[kMarkerOff + 1]) << 8)
                         | (uint32_t(data[kMarkerOff + 2]) << 16) | (uint32_t(data[kMarkerOff + 3]) << 24);
        return v == (marker & 0xffffff00u);
    }
};

}  // namespace mm
