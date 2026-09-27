/// @module DevicePlugin
/// @also DevicesModule
///
/// Pins the plugin classification: each plugin claims the presence port and types a datagram it receives.
///
/// @moreinfo
///
/// ## How a packet reaches a verdict
///
/// A synthetic presence packet goes in, the 44-byte WLED-compatible header, and a classification comes out.
/// Pure host logic with no network, because the plugins are the second caller that makes the seam testable.

#include "doctest.h"
#include "core/system/DevicePlugin.h"
#include "core/system/WledPacket.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {
const uint8_t kSrcIp[4] = {192, 168, 1, 50};

// Build a WLED-valid presence packet; `mm` stamps the MoonLight marker (a MoonLight peer).
void packet(uint8_t out[WledPacket::kSize], const char* name, bool mm) {
    WledPacket::build(out, kSrcIp, name, /*boardType=*/34, /*lightsOn=*/true);
    if (mm) WledPacket::stampMmMarker(out);
}
}  // namespace

TEST_CASE("MmPlugin claims a presence packet carrying the MoonLight marker") {
    MmPlugin p;
    CHECK(p.discoveryPort() == WledPacket::kPort);

    uint8_t pkt[WledPacket::kSize];
    packet(pkt, "Bench-P4", /*mm=*/true);
    DiscoveredDevice d;
    REQUIRE(p.classifyPacket(pkt, sizeof(pkt), kSrcIp, d));
    CHECK(d.type == DevType::MoonLight);
    CHECK(std::strcmp(d.name, "Bench-P4") == 0);
}

TEST_CASE("MmPlugin declines a plain WLED packet (no MoonLight marker)") {
    MmPlugin p;
    uint8_t pkt[WledPacket::kSize];
    packet(pkt, "wled-desk", /*mm=*/false);
    DiscoveredDevice d;
    CHECK_FALSE(p.classifyPacket(pkt, sizeof(pkt), kSrcIp, d));
}

TEST_CASE("WledPlugin claims a plain WLED packet as WLED") {
    WledPlugin p;
    CHECK(p.discoveryPort() == WledPacket::kPort);

    uint8_t pkt[WledPacket::kSize];
    packet(pkt, "wled-desk", /*mm=*/false);
    DiscoveredDevice d;
    REQUIRE(p.classifyPacket(pkt, sizeof(pkt), kSrcIp, d));
    CHECK(d.type == DevType::Wled);
    CHECK(std::strcmp(d.name, "wled-desk") == 0);
}

TEST_CASE("WledPlugin declines a MoonLight-marked packet (that's a peer, not a WLED)") {
    // A MoonLight peer broadcasts a WLED-VALID packet, so without the marker check WledPlugin would mis-claim it. The marker keeps the MoonLight/WLED kinds distinct.
    WledPlugin p;
    uint8_t pkt[WledPacket::kSize];
    packet(pkt, "Bench-P4", /*mm=*/true);
    DiscoveredDevice d;
    CHECK_FALSE(p.classifyPacket(pkt, sizeof(pkt), kSrcIp, d));
}

TEST_CASE("Plugins decline a short / garbage datagram, never read out of bounds") {
    MmPlugin mmp;
    WledPlugin wp;
    DiscoveredDevice d;
    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};   // too short, wrong magic
    CHECK_FALSE(mmp.classifyPacket(garbage, sizeof(garbage), kSrcIp, d));
    CHECK_FALSE(wp.classifyPacket(garbage, sizeof(garbage), kSrcIp, d));
    CHECK_FALSE(wp.classifyPacket(nullptr, 0, kSrcIp, d));
}

TEST_CASE("WledPlugin tolerates an empty name (the module supplies the IP fallback)") {
    WledPlugin p;
    uint8_t pkt[WledPacket::kSize];
    packet(pkt, "", /*mm=*/false);
    DiscoveredDevice d;
    REQUIRE(p.classifyPacket(pkt, sizeof(pkt), kSrcIp, d));
    CHECK(d.type == DevType::Wled);
    CHECK(d.name[0] == '\0');
}
