/// @module WledPacket

/// Pins the WLED presence packet wire format, the 44-byte header MoonLight and WLED both broadcast on UDP 65506. A wire format breaks silently, so build → parse is pinned here directly (the discovery tests exercise it indirectly; this is the focused contract).

#include "doctest.h"
#include "core/system/WledPacket.h"

#include <cstdint>
#include <cstring>

using namespace mm;

TEST_CASE("WledPacket::build produces a valid WLED header (token/id/size)") {
    const uint8_t ip[4] = {192, 168, 1, 42};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "MM-Bench", /*boardType=*/34, /*lightsOn=*/true);

    CHECK(WledPacket::isValid(pkt, sizeof(pkt)));
    CHECK(pkt[0] == 255);                 // token
    CHECK(pkt[1] == 1);                   // id
    CHECK(pkt[2] == 192);                 // ip0 — WLED's subnet check keys on this
    CHECK(pkt[5] == 42);
    CHECK((pkt[WledPacket::kTypeOff] & 0x7f) == 34);   // board type, low 7 bits
    CHECK((pkt[WledPacket::kTypeOff] & 0x80) != 0);    // lights-on bit
}

TEST_CASE("WledPacket::readName round-trips the device name") {
    const uint8_t ip[4] = {10, 0, 0, 1};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "WLEDMM-LowlandsLine", /*boardType=*/34, /*lightsOn=*/false);

    char name[24];
    WledPacket::readName(pkt, name, sizeof(name));
    CHECK(std::strcmp(name, "WLEDMM-LowlandsLine") == 0);
}

TEST_CASE("WledPacket marker is set only when stamped, and stays WLED-valid") {
    const uint8_t ip[4] = {10, 0, 0, 2};
    uint8_t plain[WledPacket::kSize];
    WledPacket::build(plain, ip, "wled-x", 32, false);
    CHECK_FALSE(WledPacket::hasMarker(plain, sizeof(plain)));   // a plain WLED packet

    uint8_t marked[WledPacket::kSize];
    WledPacket::build(marked, ip, "MM-x", 34, true);
    WledPacket::stampMarker(marked);
    CHECK(WledPacket::hasMarker(marked, sizeof(marked)));       // ours
    CHECK(WledPacket::isValid(marked, sizeof(marked)));           // still a valid WLED header
}

TEST_CASE("WledPacket::isValid rejects short / wrong-magic / null input") {
    const uint8_t shortPkt[10] = {255, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(WledPacket::isValid(shortPkt, sizeof(shortPkt)));   // too short
    uint8_t wrongMagic[WledPacket::kSize] = {};
    wrongMagic[0] = 1; wrongMagic[1] = 1;                          // token != 255
    CHECK_FALSE(WledPacket::isValid(wrongMagic, sizeof(wrongMagic)));
    CHECK_FALSE(WledPacket::isValid(nullptr, 0));
}

TEST_CASE("WledPacket::readName truncates a long name to the buffer, never overruns") {
    const uint8_t ip[4] = {10, 0, 0, 3};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "a-very-long-device-name-exceeding-field", 34, true);  // > 24
    char small[8];
    WledPacket::readName(pkt, small, sizeof(small));
    CHECK(std::strlen(small) == 7);   // truncated to cap-1, NUL-terminated
}

TEST_CASE("WledPacket: a marked packet appends the page's port and the board's base MAC, and a plain header means port 80 and no MAC") {
    const uint8_t ip[4] = {192, 168, 1, 147};
    const uint8_t mac[6] = {0x08, 0xb6, 0x1f, 0x6f, 0xbd, 0x3c};
    uint8_t pkt[WledPacket::kSizeWithMac];
    WledPacket::build(pkt, ip, "MM-Desktop", 32, true);
    WledPacket::stampMarker(pkt);
    uint8_t read[6] = {};
    CHECK(WledPacket::httpPort(pkt, WledPacket::kSize) == 80);   // absent means 80, as in an http URL
    CHECK_FALSE(WledPacket::readMac(pkt, WledPacket::kSize, read));   // WLED's header names no MAC

    const size_t len = WledPacket::stampTail(pkt, 8080, mac);
    CHECK(len == WledPacket::kSizeWithMac);
    CHECK(WledPacket::isValid(pkt, len));            // WLED reads 40 bytes or more and ignores the rest
    CHECK(WledPacket::hasMarker(pkt, len));
    CHECK(WledPacket::httpPort(pkt, len) == 8080);
    CHECK(pkt[WledPacket::kPortOff] == 0x90);        // little-endian, like the header's other fields
    CHECK(pkt[WledPacket::kPortOff + 1] == 0x1f);
    REQUIRE(WledPacket::readMac(pkt, len, read));
    CHECK(std::memcmp(read, mac, 6) == 0);
    CHECK(WledPacket::httpPort(pkt, WledPacket::stampTail(pkt, 0, mac)) == 80);   // no port reported yet: 80
}

TEST_CASE("WledPacket: only the header, or the header with MoonLight's tail, is presence") {
    uint8_t pkt[WledPacket::kSizeWithMac + 1] = {255, 1};
    CHECK(WledPacket::isValid(pkt, WledPacket::kSize));
    CHECK(WledPacket::isValid(pkt, WledPacket::kSizeWithMac));
    CHECK_FALSE(WledPacket::isValid(pkt, WledPacket::kSize + 1));
    CHECK_FALSE(WledPacket::isValid(pkt, WledPacket::kSize + 2));   // the port alone is no packet MoonLight sends
    CHECK_FALSE(WledPacket::isValid(pkt, WledPacket::kSizeWithMac + 1));
}

TEST_CASE("WledPacket: MoonBase's sentinel is its own, read as neither MoonLight's nor plain WLED's") {
    const uint8_t ip[4] = {192, 0, 2, 9};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "MM-1234", 0, false);
    WledPacket::stampMarker(pkt, WledPacket::kMbMarker);
    CHECK(WledPacket::isValid(pkt, sizeof(pkt)));                              // WLED-valid still
    CHECK(WledPacket::hasMarker(pkt, sizeof(pkt), WledPacket::kMbMarker));
    CHECK_FALSE(WledPacket::hasMarker(pkt, sizeof(pkt)));                  // not MoonLight's
}
