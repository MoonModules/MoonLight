/// @module DevicesModule
/// @also DevicePlugin
///
/// Drives the whole UDP discovery pipeline on the host, from a synthetic presence packet to the device list it produces.
///
/// @moreinfo
///
/// ## The seat is claimed only while enabled
///
/// applyState() calls prepare(), which claims the seat, only when the module is effectively enabled, and release() otherwise.
/// A disabled DevicesModule claiming it would point the presence pipeline and Hue-bridge routing at a module the user turned off.
/// active_ is a process-wide static, so each case brackets applyState() and release() to leave it clean, the discipline the AudioService cases use.
///
/// ## What it asserts
///
/// Classification, MoonLight against WLED typing, and that one device's packet never contaminates another's name or type.
/// Live rename too, since a device that changes its name mid-session must not appear twice.
/// Packets go in through injectPacketForTest(), the same entry the live recvFrom loop uses, so the private upsert path is reachable with no network.

#include "doctest.h"
#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace mm;

namespace {

// Inject a presence packet from `a.b.c.d` with `name`; `mm` marks it a MoonLight peer.
void inject(DevicesModule& dev, const char* name, bool mm,
            uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t ip[4] = {a, b, c, d};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    if (mm) WledPacket::stampMmMarker(pkt);
    dev.injectPacketForTest(pkt, sizeof(pkt), ip);
}

// Find the serialized row for an IP; return its full JSON (or "" if absent).
std::string rowFor(DevicesModule& dev, const char* ip) {
    for (uint8_t i = 0; i < dev.listRowCount(); i++) {
        mm::JsonSink sink;
        dev.writeListRow(sink, i);
        if (std::strstr(sink.data(), ip)) return sink.data();
    }
    return "";
}

}  // namespace

TEST_CASE("DevicesModule: a plain WLED packet lists a WLED device with its name") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 168, 1, 50);
    std::string row = rowFor(dev, "192.168.1.50");
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "wled-desk") != nullptr);
}

TEST_CASE("DevicesModule: a MoonLight-marked packet lists a MoonLight device") {
    DevicesModule dev;
    inject(dev, "MM-Bench", /*mm=*/true, 192, 168, 1, 60);
    std::string row = rowFor(dev, "192.168.1.60");
    CHECK(std::strstr(row.c_str(), "\"type\":\"MoonLight\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-Bench") != nullptr);
}

TEST_CASE("DevicesModule: a short / garbage datagram is ignored, never listed") {
    DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 70};
    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    dev.injectPacketForTest(garbage, sizeof(garbage), ip);
    CHECK(dev.listRowCount() == 0);
}

// The P4-bench bug: two DIFFERENT devices (a WLED and a MoonLight peer) must each keep their OWN name + type, no cross-contamination between packets.
TEST_CASE("DevicesModule: distinct devices don't cross-contaminate name or type") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 168, 1, 186);  // a WLED
    inject(dev, "MM-S3",     /*mm=*/true,  192, 168, 1, 157);  // a MoonLight peer

    std::string wled = rowFor(dev, "192.168.1.186");
    std::string mm   = rowFor(dev, "192.168.1.157");
    CHECK(std::strstr(wled.c_str(), "wled-desk") != nullptr);
    CHECK(std::strstr(wled.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(wled.c_str(), "MM-S3") == nullptr);   // the contamination bug
    CHECK(std::strstr(mm.c_str(), "MM-S3") != nullptr);
    CHECK(std::strstr(mm.c_str(), "\"type\":\"MoonLight\"") != nullptr);
}

// A peer RENAME must propagate: a later packet from the same IP with a new name updates the row in place, the live-update requirement (the name rides the presence packet).
TEST_CASE("DevicesModule: a peer rename updates the existing row's name") {
    DevicesModule dev;
    inject(dev, "MM-OldName", /*mm=*/true, 192, 168, 1, 100);
    REQUIRE(std::strstr(rowFor(dev, "192.168.1.100").c_str(), "MM-OldName") != nullptr);
    inject(dev, "MM-NewName", /*mm=*/true, 192, 168, 1, 100);   // same device, new name
    std::string row = rowFor(dev, "192.168.1.100");
    CHECK(std::strstr(row.c_str(), "MM-NewName") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-OldName") == nullptr);
}

// A MoonLight device stays MoonLight even when a later plain-WLED packet arrives from the same address, the type only RAISES toward MoonLight, never downgrades. (A MoonLight peer could be seen via an unmarked packet too; that must not relabel it WLED.)
TEST_CASE("DevicesModule: a MoonLight device is not downgraded by a later WLED packet") {
    DevicesModule dev;
    inject(dev, "MM-Peer", /*mm=*/true,  192, 168, 1, 90);   // first: a MoonLight-marked packet
    inject(dev, "MM-Peer", /*mm=*/false, 192, 168, 1, 90);   // later: a plain WLED packet, same IP
    std::string row = rowFor(dev, "192.168.1.90");
    CHECK(std::strstr(row.c_str(), "\"type\":\"MoonLight\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") == nullptr);
}

TEST_CASE("DevicesModule: a DISABLED module does not claim the active_ seat at boot") {
    // A persisted DISABLED module must not claim the singleton seat at boot: @xref{the-seat-is-claimed-only-while-enabled}.
    DevicesModule dis;
    dis.setEnabled(false);
    dis.setup();                           // Phase 3: pure wiring
    dis.applyState();                      // Phase 4: disabled → routes to release, not the seat claim
    CHECK(DevicesModule::active() != &dis); // the disabled module did not claim it

    DevicesModule live;
    live.setup();
    live.applyState();                     // enabled by default → build claims the seat
    CHECK(DevicesModule::active() == &live);

    live.release();                       // vacates
    dis.release();                        // was never active → still a clean no-op
    CHECK(DevicesModule::active() != &dis);
    CHECK(DevicesModule::active() != &live);
}

namespace {
// Inject one marked MoonLight presence copy from 192.168.1.d, as the broadcast or the multicast copy.
void injectCopy(DevicesModule& dev, uint8_t d, bool broadcast) {
    const uint8_t ip[4] = {192, 168, 1, d};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "peer", /*boardType=*/34, /*lightsOn=*/true);
    WledPacket::stampMmMarker(pkt);
    WledPacket::stampVia(pkt, broadcast);
    dev.injectPacketForTest(pkt, sizeof(pkt), ip);
}
}  // namespace

// A peer heard only over broadcast proves the group does not reach this device, so every multicast send adds a broadcast copy.
TEST_CASE("DevicesModule: a peer heard only over broadcast proves the network drops multicast, and its multicast copy clears it") {
    NetworkPath::multicastDropped = false;
    DevicesModule dev;
    for (int i = 0; i < 3; i++) injectCopy(dev, 61, /*broadcast=*/true);
    const bool proven = NetworkPath::multicastDropped;
    // A multicast copy turning up later means the group arrives after all, a slow join being the likelier story.
    injectCopy(dev, 61, /*broadcast=*/false);
    const bool cleared = !NetworkPath::multicastDropped;
    NetworkPath::multicastDropped = false;   // reset before asserting, so a failure leaks into no later test
    CHECK(proven);
    CHECK(cleared);
}

TEST_CASE("DevicesModule: a peer heard both ways, or not at all, proves nothing") {
    NetworkPath::multicastDropped = false;
    DevicesModule other;
    for (int i = 0; i < 5; i++) {
        injectCopy(other, 63, /*broadcast=*/false);
        injectCopy(other, 63, /*broadcast=*/true);
    }
    const bool bothWays = NetworkPath::multicastDropped;
    // Silence is what a device alone on the network hears too.
    DevicesModule alone;
    const bool silence = NetworkPath::multicastDropped;
    NetworkPath::multicastDropped = false;
    CHECK_FALSE(bothWays);
    CHECK_FALSE(silence);
}

// A switch that forgets the group minutes in is the classic snooping failure, so multicast copies heard earlier must not hide it.
TEST_CASE("DevicesModule: a group that stops arriving after it worked is caught") {
    NetworkPath::multicastDropped = false;
    DevicesModule dev;
    for (int i = 0; i < 10; i++) {
        injectCopy(dev, 65, /*broadcast=*/false);
        injectCopy(dev, 65, /*broadcast=*/true);
    }
    const bool whileWorking = NetworkPath::multicastDropped;
    for (int i = 0; i < 3; i++) injectCopy(dev, 65, /*broadcast=*/true);
    const bool afterOutage = NetworkPath::multicastDropped;
    NetworkPath::multicastDropped = false;
    CHECK_FALSE(whileWorking);
    CHECK(afterOutage);
}

// One peer whose group never arrives must not flip the flag back after every multicast copy from another, a status write each time.
TEST_CASE("DevicesModule: a peer without multicast does not undo another peer's multicast every round") {
    NetworkPath::multicastDropped = false;
    DevicesModule dev;
    for (int i = 0; i < 3; i++) injectCopy(dev, 66, /*broadcast=*/true);
    const bool proven = NetworkPath::multicastDropped;
    injectCopy(dev, 67, /*broadcast=*/false);   // another peer's multicast arrives: the group does reach us
    for (int i = 0; i < 5; i++) injectCopy(dev, 66, /*broadcast=*/true);
    const bool stillCleared = !NetworkPath::multicastDropped;
    NetworkPath::multicastDropped = false;
    CHECK(proven);
    CHECK(stillCleared);
}

// The copy flag sits in the marker's low byte, so a peer still recognizes us whichever copy arrives.
TEST_CASE("WledPacket: either copy of a MoonLight packet carries the marker") {
    const uint8_t ip[4] = {192, 168, 1, 64};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "peer", 34, true);
    WledPacket::stampMmMarker(pkt);
    WledPacket::stampVia(pkt, true);
    CHECK(WledPacket::hasMmMarker(pkt, sizeof(pkt)));
    CHECK(WledPacket::viaBroadcast(pkt));
    WledPacket::stampVia(pkt, false);
    CHECK(WledPacket::hasMmMarker(pkt, sizeof(pkt)));
    CHECK_FALSE(WledPacket::viaBroadcast(pkt));
}
