/// @module DevicesModule
/// @also DevicePlugin
///
/// Drives the whole UDP discovery pipeline on the host, from a synthetic presence packet to the device list it produces.
///
/// @technical
///
/// ## The seat is claimed only while enabled
///
/// applyState() calls prepare(), which claims the seat, only when the module is effectively enabled, and release() otherwise: a disabled DevicesModule claiming it would point presence and Hue-bridge routing at a module the user turned off.
///
/// active_ is a process-wide static, so each case brackets applyState() and release() to leave it clean, the discipline the AudioService cases use.
///
/// ## What it asserts
///
/// Classification, MoonLight against WLED typing, and that one device's packet never contaminates another's name or type. Live rename too, since a device that changes its name mid-session must not appear twice.
///
/// Packets go in through injectPacketForTest(), the same entry the live recvFrom loop uses, so the private upsert path is reachable with no network.

#include "doctest.h"
#include "discovery_rows.h"
#include "mdns_replies.h"   // an ESPHome board's real reply to the browse
#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace mm;

namespace {

// Inject a presence packet from `a.b.c.d` with `name`; `mm` marks it a MoonLight peer. Addresses are RFC 5737's 192.0.2.0/24, since one the test machine holds lists as itself.
void inject(DevicesModule& dev, const char* name, bool mm, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t ip[4] = {a, b, c, d};
    discovery_rows::hearPresence(dev, ip, name, mm);
}

// The serialized row for an IP, or "" if absent.
std::string rowFor(DevicesModule& dev, const char* ip) { return discovery_rows::rowAt(dev, ip); }

}  // namespace

TEST_CASE("DevicesModule: a plain WLED packet lists a WLED device with its name") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 0, 2, 50);
    std::string row = rowFor(dev, "192.0.2.50");
    CHECK(std::strstr(row.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "wled-desk") != nullptr);
}

TEST_CASE("DevicesModule: a MoonLight-marked packet lists a MoonLight device") {
    DevicesModule dev;
    inject(dev, "MM-Bench", /*mm=*/true, 192, 0, 2, 60);
    std::string row = rowFor(dev, "192.0.2.60");
    CHECK(std::strstr(row.c_str(), "\"type\":\"MoonLight\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-Bench") != nullptr);
}

TEST_CASE("DevicesModule: a short / garbage datagram is ignored, never listed") {
    DevicesModule dev;
    const uint8_t ip[4] = {192, 0, 2, 70};
    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    dev.injectPacketForTest(garbage, sizeof(garbage), ip);
    CHECK(dev.listRowCount() == 0);
}

// The P4-bench bug: two DIFFERENT devices (a WLED and a MoonLight peer) must each keep their OWN name + type, no cross-contamination between packets.
TEST_CASE("DevicesModule: distinct devices don't cross-contaminate name or type") {
    DevicesModule dev;
    inject(dev, "wled-desk", /*mm=*/false, 192, 0, 2, 186);  // a WLED
    inject(dev, "MM-S3",     /*mm=*/true,  192, 0, 2, 157);  // a MoonLight peer

    std::string wled = rowFor(dev, "192.0.2.186");
    std::string mm   = rowFor(dev, "192.0.2.157");
    CHECK(std::strstr(wled.c_str(), "wled-desk") != nullptr);
    CHECK(std::strstr(wled.c_str(), "\"type\":\"WLED\"") != nullptr);
    CHECK(std::strstr(wled.c_str(), "MM-S3") == nullptr);   // the contamination bug
    CHECK(std::strstr(mm.c_str(), "MM-S3") != nullptr);
    CHECK(std::strstr(mm.c_str(), "\"type\":\"MoonLight\"") != nullptr);
}

// A peer RENAME must propagate: a later packet from the same IP with a new name updates the row in place, the live-update requirement (the name rides the presence packet).
TEST_CASE("DevicesModule: a peer rename updates the existing row's name") {
    DevicesModule dev;
    inject(dev, "MM-OldName", /*mm=*/true, 192, 0, 2, 100);
    REQUIRE(std::strstr(rowFor(dev, "192.0.2.100").c_str(), "MM-OldName") != nullptr);
    inject(dev, "MM-NewName", /*mm=*/true, 192, 0, 2, 100);   // same device, new name
    std::string row = rowFor(dev, "192.0.2.100");
    CHECK(std::strstr(row.c_str(), "MM-NewName") != nullptr);
    CHECK(std::strstr(row.c_str(), "MM-OldName") == nullptr);
}

// A MoonLight device stays MoonLight even when a later plain-WLED packet arrives from the same address, the type only RAISES toward MoonLight, never downgrades. (A MoonLight peer could be seen via an unmarked packet too; that must not relabel it WLED.)
TEST_CASE("DevicesModule: a MoonLight device is not downgraded by a later WLED packet") {
    DevicesModule dev;
    inject(dev, "MM-Peer", /*mm=*/true,  192, 0, 2, 90);   // first: a MoonLight-marked packet
    inject(dev, "MM-Peer", /*mm=*/false, 192, 0, 2, 90);   // later: a plain WLED packet, same IP
    std::string row = rowFor(dev, "192.0.2.90");
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
// Inject one marked MoonLight presence copy from 192.0.2.d, as the broadcast or the multicast copy.
void injectCopy(DevicesModule& dev, uint8_t d, bool broadcast) {
    const uint8_t ip[4] = {192, 0, 2, d};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "peer", /*boardType=*/34, /*lightsOn=*/true);
    WledPacket::stampMarker(pkt);
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

// The device list is not saved, so neither a new device nor a verdict flip arms a save.
TEST_CASE("DevicesModule: neither a new device nor the multicast verdict changing arms a save") {
    NetworkPath::multicastDropped = false;
    DevicesModule dev;
    injectCopy(dev, 62, /*broadcast=*/true);
    const bool newRowSaves = dev.dirty();
    for (int i = 0; i < 2; i++) injectCopy(dev, 62, /*broadcast=*/true);   // the third copy proves the drop
    const bool proven = NetworkPath::multicastDropped;
    const bool dirtyAfterProof = dev.dirty();
    injectCopy(dev, 62, /*broadcast=*/false);                              // and a multicast copy clears it
    const bool dirtyAfterClear = dev.dirty();
    NetworkPath::multicastDropped = false;
    CHECK_FALSE(newRowSaves);
    CHECK(proven);
    CHECK_FALSE(dirtyAfterProof);
    CHECK_FALSE(dirtyAfterClear);
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
    const uint8_t ip[4] = {192, 0, 2, 64};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "peer", 34, true);
    WledPacket::stampMarker(pkt);
    WledPacket::stampVia(pkt, true);
    CHECK(WledPacket::hasMarker(pkt, sizeof(pkt)));
    CHECK(WledPacket::viaBroadcast(pkt));
    WledPacket::stampVia(pkt, false);
    CHECK(WledPacket::hasMarker(pkt, sizeof(pkt)));
    CHECK_FALSE(WledPacket::viaBroadcast(pkt));
}

// A board is known by the name it announces, in both directions, and only from what was heard this session.
TEST_CASE("DevicesModule: a board heard announcing itself answers its name for its address, and its address for its name") {
    DevicesModule dev;
    inject(dev, "MM-S31", /*mm=*/true, 192, 0, 2, 125);
    const uint8_t at[4] = {192, 0, 2, 125}, elsewhere[4] = {192, 0, 2, 126};
    REQUIRE(dev.nameOf(at) != nullptr);
    CHECK(std::strcmp(dev.nameOf(at), "MM-S31") == 0);
    CHECK(dev.nameOf(elsewhere) == nullptr);
    uint8_t ip[4] = {};
    CHECK(dev.addressOf("mm-s31.local", ip));   // as mDNS names it: letter case and .local aside
    CHECK(std::memcmp(ip, at, 4) == 0);
    CHECK_FALSE(dev.addressOf("MM-S3", ip));     // a prefix is another name
    CHECK_FALSE(dev.addressOf("desk.local", ip));
}

// A board that changed its address leaves its old row listed until it ages out, so a host setting naming it must reach where it is now.
TEST_CASE("DevicesModule: a board heard at a new address is looked up there, not at the address it left") {
    DevicesModule dev;
    mm::platform::setTestNowMs(1000);
    inject(dev, "MM-Eye", /*mm=*/true, 192, 0, 2, 31);   // where it was
    mm::platform::setTestNowMs(5000);
    inject(dev, "MM-Eye", /*mm=*/true, 192, 0, 2, 30);   // where it is now, listed after the old row
    uint8_t ip[4] = {};
    REQUIRE(dev.addressOf("MM-Eye", ip));
    CHECK(ip[3] == 30);
}

// A card names its peer the same way everywhere: the board and its address, or the address alone for a desk or a computer.
TEST_CASE("DevicesModule: a status names a known peer by its board and an unknown one by its address") {
    DevicesModule dev;
    dev.setup();
    dev.applyState();   // claims the seat a status reads through
    inject(dev, "MM-S31", /*mm=*/true, 192, 0, 2, 125);
    inject(dev, "MM-a-name-of-23-letters", /*mm=*/true, 255, 255, 255, 254);
    char peer[HostResolver::kPeerChars];
    const uint8_t known[4] = {192, 0, 2, 125}, unknown[4] = {192, 0, 2, 9}, longest[4] = {255, 255, 255, 254};
    HostResolver::formatPeer(peer, sizeof(peer), known);
    CHECK(std::string(peer) == "MM-S31 (192.0.2.125)");
    HostResolver::formatPeer(peer, sizeof(peer), unknown);
    CHECK(std::string(peer) == "192.0.2.9");
    HostResolver::formatPeer(peer, sizeof(peer), longest);
    CHECK(std::string(peer) == "MM-a-name-of-23-letters (255.255.255.254)");   // the longest name a board holds fits whole
    dev.release();
}

// A host setting naming a board resolves from the boards' own announcements, before mDNS, whose multicast a router may drop.
TEST_CASE("DevicesModule: a host setting naming a board resolves from the device list at once, with no lookup") {
    DevicesModule dev;
    dev.setup();
    dev.applyState();
    inject(dev, "MM-Bridge", /*mm=*/true, 192, 0, 2, 101);
    uint8_t ip[4] = {};
    CHECK(HostResolver::lookup("MM-Bridge.local", ip) == HostResolver::State::Resolved);   // no pass of the lookup task ran
    CHECK(ip[3] == 101);
    dev.release();
    CHECK(HostResolver::lookup("MM-Bridge.local", ip) != HostResolver::State::Resolved);   // gone with the module
}

TEST_CASE("DevicesModule: a peer serving on another port is linked and read at that port") {
    DevicesModule dev;
    const uint8_t ip[4] = {192, 0, 2, 61};
    uint8_t pkt[WledPacket::kSizeWithPort];
    WledPacket::build(pkt, ip, "MM-Pi", 32, true);
    WledPacket::stampMarker(pkt);
    dev.injectPacketForTest(pkt, WledPacket::stampHttpPort(pkt, 8080), ip);
    REQUIRE(dev.listRowCount() >= 1);
    CHECK(std::strstr(rowFor(dev, "192.0.2.61").c_str(), "\"port\":8080") != nullptr);
    const std::string detail = discovery_rows::rowAt(dev, "192.0.2.61", true);
    CHECK(std::strstr(detail.c_str(), "\"url\":\"http://192.0.2.61:8080/\"") != nullptr);

    // A peer on port 80 shows no port and links at the bare address.
    inject(dev, "MM-Board", /*mm=*/true, 192, 0, 2, 62);
    CHECK(std::strstr(rowFor(dev, "192.0.2.62").c_str(), "\"port\"") == nullptr);
}

TEST_CASE("DevicesModule: a link-local address gets no row") {
    // A board's second interface without DHCP takes 169.254.x.x (RFC 3927), which no peer off that link reaches: listed, it shows the board twice.
    DevicesModule dev;
    inject(dev, "MM-P4", /*mm=*/true, 169, 254, 248, 172);
    CHECK(rowFor(dev, "169.254.248.172").empty());
    inject(dev, "MM-P4", /*mm=*/true, 192, 0, 2, 32);
    CHECK(rowFor(dev, "169.254.248.172").empty());
    CHECK_FALSE(rowFor(dev, "192.0.2.32").empty());
}

TEST_CASE("DevicesModule: the device list is not persisted") {
    CHECK_FALSE(DevicesModule{}.persistsList());
}

// MoonBase, MoonLight's installer, holds a board during an update or after a migration: its own sentinel keeps the board's node, and the latest marked packet says which image runs.
TEST_CASE("DevicesModule: a board MoonBase holds is listed as MoonBase, and turns back into MoonLight when MoonLight announces again") {
    DevicesModule dev;
    const uint8_t ip[4] = {192, 0, 2, 120};
    discovery_rows::hearPresence(dev, ip, "MM-Olimex", true);
    CHECK(std::strstr(rowFor(dev, "192.0.2.120").c_str(), "\"type\":\"MoonLight\"") != nullptr);
    discovery_rows::hearPresence(dev, ip, "MM-Olimex", true, WledPacket::kMbMarker);
    CHECK(std::strstr(rowFor(dev, "192.0.2.120").c_str(), "\"type\":\"MoonBase\"") != nullptr);
    CHECK(dev.listRowCount() == 1);   // the same board's row, not a second one
    discovery_rows::hearPresence(dev, ip, "MM-Olimex", false);   // a plain WLED packet never downgrades one of ours
    CHECK(std::strstr(rowFor(dev, "192.0.2.120").c_str(), "\"type\":\"MoonBase\"") != nullptr);
    discovery_rows::hearPresence(dev, ip, "MM-Olimex", true);
    CHECK(std::strstr(rowFor(dev, "192.0.2.120").c_str(), "\"type\":\"MoonLight\"") != nullptr);
}

// ESPHome announces nothing on the presence port, only over mDNS: the browse's reply gives it a row, named and versioned as it announces itself.
TEST_CASE("DevicesModule: an ESPHome board answering the mDNS browse is listed as ESPHome, with its friendly name and version") {
    DevicesModule dev;
    const std::vector<uint8_t> reply = mdns_replies::bytes(mdns_replies::kEsphomeReply);
    const uint8_t ip[4] = {192, 0, 2, 227};
    dev.injectMdnsReplyForTest(reply.data(), reply.size(), ip);
    const std::string row = rowFor(dev, "192.0.2.227");
    CHECK(std::strstr(row.c_str(), "\"type\":\"ESPHome\"") != nullptr);
    CHECK(std::strstr(row.c_str(), "ESPHome Web 7388c8") != nullptr);
    CHECK(std::strstr(discovery_rows::rowAt(dev, "192.0.2.227", true).c_str(), "\"version\":\"2026.9.1\"") != nullptr);
    dev.injectMdnsReplyForTest(reply.data(), reply.size(), ip);   // the next minute's answer
    CHECK(dev.listRowCount() == 1);
}

// A Tasmota announces nothing, so the user adds its address; its page's Server header names firmware and version, its first <h2> the name it was given.
TEST_CASE("DevicesModule: an added address whose page says Tasmota is listed as Tasmota, named and versioned by its page, read in any pieces") {
    DevicesModule dev;
    std::snprintf(dev.addedIps, sizeof(dev.addedIps), "192.0.2.103");
    dev.onControlChanged("addedIps");
    const char* page = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nServer: Tasmota/16.0.0 (ESP32-D0WD-V3)\r\n\r\n"
                       "<!DOCTYPE html><html><head><title>Tasmota Main Menu</title></head><body><h3>ESP32-DevKit</h3><h2>Kitchen</h2></body></html>";
    for (size_t piece : {size_t(1), size_t(7), size_t(256)}) {
        dev.injectPageForTest(page, piece);
        const std::string row = rowFor(dev, "192.0.2.103");
        CHECK(std::strstr(row.c_str(), "\"type\":\"Tasmota\"") != nullptr);
        CHECK(std::strstr(row.c_str(), "Kitchen") != nullptr);
        CHECK(std::strstr(discovery_rows::rowAt(dev, "192.0.2.103", true).c_str(), "\"version\":\"16.0.0\"") != nullptr);
    }
    CHECK(dev.listRowCount() == 1);
}

TEST_CASE("DevicesModule: an added address that is not a Tasmota is listed as generic, and a name is refused") {
    DevicesModule dev;
    std::snprintf(dev.addedIps, sizeof(dev.addedIps), "192.0.2.104");
    dev.onControlChanged("addedIps");
    dev.injectPageForTest("HTTP/1.1 200 OK\r\nServer: lighttpd/1.4\r\n\r\n<html><body>router</body></html>", 64);
    CHECK(std::strstr(rowFor(dev, "192.0.2.104").c_str(), "\"type\":\"generic\"") != nullptr);
    std::snprintf(dev.addedIps, sizeof(dev.addedIps), "kitchen-tasmota");
    dev.onControlChanged("addedIps");
    CHECK(std::strstr(dev.status() ? dev.status() : "", "IP addresses only") != nullptr);
    const uint8_t wled[4] = {192, 0, 2, 50};
    discovery_rows::hearPresence(dev, wled, "wled", false);   // a sighting rewrites the status: the refusal stays in it
    CHECK(std::strstr(dev.status() ? dev.status() : "", "IP addresses only") != nullptr);
}

// A typed address may never answer: the probe leaves it after its wait, closing its socket, and moves on, never holding the tick.
TEST_CASE("DevicesModule: a silent added address is left after the probe's wait, its socket closed, and the next one asked") {
    DevicesModule dev;
    std::snprintf(dev.addedIps, sizeof(dev.addedIps), "192.0.2.1, 192.0.2.2");   // RFC 5737 test addresses, which nothing answers
    dev.onControlChanged("addedIps");
    bool moved = false;
    for (int i = 0; i < 10 && !moved; i++) {
        dev.stepProbeForTest();
        moved = dev.probeHostForTest() != 0;
    }
    CHECK(moved);
    CHECK_FALSE(dev.probeOpenForTest());   // the silent one's connection is closed, not left open
    dev.release();
}

// Regression: a saved list was restored without the change hook, so after a restart no added address was asked.
TEST_CASE("DevicesModule: added addresses restored at boot are asked, as a changed list is") {
    DevicesModule dev;
    std::snprintf(dev.addedIps, sizeof(dev.addedIps), "192.0.2.105");   // as the saved file puts it back, no change hook
    dev.setup();
    dev.injectPageForTest("HTTP/1.1 200 OK\r\nServer: Tasmota/16.0.0 (ESP32-D0WD-V3)\r\n\r\n<h2>Garage</h2>", 32);
    CHECK(std::strstr(rowFor(dev, "192.0.2.105").c_str(), "Garage") != nullptr);
    dev.release();
}
