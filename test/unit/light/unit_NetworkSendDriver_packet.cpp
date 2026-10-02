/// @module NetworkSendDriver

#include "doctest.h"
#include "light/drivers/NetworkSendDriver.h"

#include <cstring>
#include <cstdio>
#include <string>

// The built packet contains the exact header layout the Art-Net spec mandates: ID, OpCode, version, sequence, physical, universe, length, data.
TEST_CASE("ArtNet packet header format") {
    uint8_t data[3] = {255, 0, 128};
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 3];

    size_t len = mm::buildArtDmxPacket(packet, 0, 42, data, 3);

    CHECK(len == mm::ARTNET_HEADER_SIZE + 3);

    // "Art-Net\0" at offset 0
    CHECK(std::memcmp(packet, "Art-Net", 8) == 0);

    // OpCode: 0x5000 little-endian at offset 8
    CHECK(packet[8] == 0x00);
    CHECK(packet[9] == 0x50);

    // Protocol version: 14 big-endian at offset 10
    CHECK(packet[10] == 0x00);
    CHECK(packet[11] == 0x0e);

    // Sequence at offset 12
    CHECK(packet[12] == 42);

    // Physical at offset 13
    CHECK(packet[13] == 0);

    // Universe: 0 little-endian at offset 14
    CHECK(packet[14] == 0x00);
    CHECK(packet[15] == 0x00);

    // Length: 3 big-endian at offset 16
    CHECK(packet[16] == 0x00);
    CHECK(packet[17] == 0x03);

    // Data at offset 18
    CHECK(packet[18] == 255);
    CHECK(packet[19] == 0);
    CHECK(packet[20] == 128);
}

// Universe 259 (0x0103) is encoded little-endian (low byte first), matching the Art-Net wire format.
TEST_CASE("ArtNet packet with non-zero universe") {
    uint8_t data[6] = {1, 2, 3, 4, 5, 6};
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 6];

    mm::buildArtDmxPacket(packet, 259, 0, data, 6);

    // Universe 259 = 0x0103, little-endian
    CHECK(packet[14] == 0x03);
    CHECK(packet[15] == 0x01);
}

// 256 RGB lights (768 bytes) split across exactly 2 universes (510 + 258), matching the 510-channel-per-universe cap.
TEST_CASE("ArtNet universe splitting for 256 RGB lights") {
    // 256 RGB lights = 768 bytes = 2 universes (510 + 258)
    constexpr size_t maxPerUniverse = mm::MAX_CHANNELS_PER_UNIVERSE;
    constexpr size_t totalBytes = 256 * 3;

    size_t universeCount = 0;
    size_t sent = 0;
    while (sent < totalBytes) {
        size_t chunk = totalBytes - sent;
        if (chunk > maxPerUniverse) chunk = maxPerUniverse;
        sent += chunk;
        universeCount++;
    }

    CHECK(universeCount == 2);
    CHECK(sent == totalBytes);
}

// The data-length field is encoded big-endian (high byte first), unlike the universe field, matching the Art-Net spec.
TEST_CASE("ArtNet packet length field is big-endian") {
    uint8_t data[510];
    std::memset(data, 0, sizeof(data));
    uint8_t packet[mm::ARTNET_HEADER_SIZE + 510];

    mm::buildArtDmxPacket(packet, 0, 0, data, 510);

    // 510 = 0x01FE, big-endian
    CHECK(packet[16] == 0x01);
    CHECK(packet[17] == 0xFE);
}

// The built E1.31 packet carries the exact ACN layout strict sACN receivers (and tools like xLights) validate. Identifier, the three flags+length fields, CID, source name, priority, universe, property count, start code.
TEST_CASE("E1.31 packet header format") {
    uint8_t cid[mm::E131_CID_LENGTH];
    for (uint8_t i = 0; i < mm::E131_CID_LENGTH; i++) cid[i] = static_cast<uint8_t>(0xC0 + i);
    uint8_t data[3] = {255, 0, 128};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];

    const size_t len = mm::buildE131Packet(pkt, 0x0103, 42, cid, data, 3);
    REQUIRE(len == mm::E131_HEADER_SIZE + 3);   // totalLen = 129

    // Root layer: preamble, ACN identifier, flags+length (0x7000 | 129-16), vector, CID.
    CHECK(pkt[0] == 0x00); CHECK(pkt[1] == 0x10);
    CHECK(std::memcmp(pkt + 4, "ASC-E1.17\0\0\0", 12) == 0);
    CHECK(pkt[16] == 0x70); CHECK(pkt[17] == 113);
    CHECK(pkt[21] == 0x04);
    CHECK(std::memcmp(pkt + 22, cid, mm::E131_CID_LENGTH) == 0);

    // Framing layer: flags+length (129-38=91), vector, source name, priority 100, sequence, universe big-endian.
    CHECK(pkt[38] == 0x70); CHECK(pkt[39] == 91);
    CHECK(pkt[43] == 0x02);
    CHECK(std::strcmp(reinterpret_cast<const char*>(pkt + 44), "MoonLight") == 0);
    CHECK(pkt[108] == 100);
    CHECK(pkt[111] == 42);
    CHECK(pkt[113] == 0x01); CHECK(pkt[114] == 0x03);

    // DMP layer: flags+length (129-115=14), vector, address/data type, increment, property count = 1 + 3, start code 0; then the data.
    CHECK(pkt[115] == 0x70); CHECK(pkt[116] == 14);
    CHECK(pkt[117] == 0x02);
    CHECK(pkt[118] == 0xA1);
    CHECK(pkt[122] == 0x01);
    CHECK(pkt[123] == 0x00); CHECK(pkt[124] == 4);
    CHECK(pkt[125] == 0x00);
    CHECK(pkt[126] == 255); CHECK(pkt[127] == 0); CHECK(pkt[128] == 128);
}

// The built DDP packet carries version+push bits, RGB data type, default destination, and big-endian offset/length.
TEST_CASE("DDP packet header format") {
    uint8_t data[3] = {255, 0, 128};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 3];

    const size_t len = mm::buildDdpPacket(pkt, 0x01020304u, /*push=*/false, data, 3);
    REQUIRE(len == mm::DDP_HEADER_SIZE + 3);
    CHECK(pkt[0] == 0x40);                    // version 01, push clear
    CHECK(pkt[2] == 0x01);                    // RGB
    CHECK(pkt[3] == 0x01);                    // default display
    CHECK(pkt[4] == 0x01); CHECK(pkt[5] == 0x02);
    CHECK(pkt[6] == 0x03); CHECK(pkt[7] == 0x04);   // offset big-endian
    CHECK(pkt[8] == 0x00); CHECK(pkt[9] == 0x03);   // length big-endian
    CHECK(pkt[10] == 255);

    mm::buildDdpPacket(pkt, 0, /*push=*/true, data, 3);
    CHECK(pkt[0] == 0x41);                    // push set on the frame's last packet
}

// No hosts by default, so an unconfigured driver idles rather than broadcasting: a 128x128 grid's broadcast starved an ESP32 on the bench.
TEST_CASE("NetworkSendDriver: no destination by default: it idles, and says why") {
    mm::NetworkSendDriver d;
    CHECK(d.hosts[0] == '\0');            // blank, NOT an inherited broadcast address
    d.defineControls();
    d.prepare();
    CHECK(d.status() != nullptr);        // explains itself rather than idling silently
}

// One driver feeds N tubes, each its own host and its own run of the window, the unicast shape Art-Net 4 requires.
TEST_CASE("NetworkSendDriver: a range fans the window out over its tubes, one slice each") {
    mm::Buffer src;
    src.allocate(300, 3);                     // 300 lights to spread over the tubes
    mm::NetworkSendDriver d;
    d.defineControls();
    std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70-74");    // 5 tubes: .70 .71 .72 .73 .74
    d.setSourceBuffer(&src);
    d.prepare();

    REQUIRE(d.destinationCount() == 5);
    CHECK(d.destinationAt(0)[3] == 70);
    CHECK(d.destinationAt(4)[3] == 74);       // inclusive at BOTH ends: 70-74 is five tubes
    CHECK(d.destinationAt(2)[0] == 192);      // the typed-once subnet carries across the range
    CHECK(d.destinationAt(2)[2] == 1);

    // Blank lightsPerHost → the window splits evenly across the tubes (the ledsPerPin idiom).
    for (uint8_t i = 0; i < 5; i++) CHECK(d.lightsAt(i) == 60);   // 300 / 5
}

TEST_CASE("NetworkSendDriver: lightsPerHost follows the ledsPerPin idiom") {
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70,71,72");
    d.setSourceBuffer(&src);

    SUBCASE("one number = that many to EVERY tube") {
        std::snprintf(d.lightsPerHost, sizeof(d.lightsPerHost), "%s", "100");
        d.prepare();
        CHECK(d.lightsAt(0) == 100);
        CHECK(d.lightsAt(1) == 100);
        CHECK(d.lightsAt(2) == 100);
    }
    SUBCASE("a list = one per tube, by position") {
        std::snprintf(d.lightsPerHost, sizeof(d.lightsPerHost), "%s", "150,100,50");
        d.prepare();
        CHECK(d.lightsAt(0) == 150);          // tubes may differ in length
        CHECK(d.lightsAt(1) == 100);
        CHECK(d.lightsAt(2) == 50);
    }
}

// A typo in tube 3 of 5 idles the driver rather than sending to the valid prefix while the card shows an error.
TEST_CASE("NetworkSendDriver: a malformed hosts entry idles the driver, publishing no partial destination list") {
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    d.setSourceBuffer(&src);

    // A good run, so there IS prior state to leak.
    std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70-74");
    d.prepare();
    REQUIRE(d.destinationCount() == 5);

    SUBCASE("a bad address after good ones publishes NOTHING") {
        std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70,71,999,73");
        d.prepare();
        CHECK(d.destinationCount() == 0);          // not 2: the valid prefix must not go live
        CHECK(d.status() != nullptr);              // and the user is told
    }
    SUBCASE("a bad lightsPerHost publishes NOTHING either") {
        std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70-74");
        std::snprintf(d.lightsPerHost, sizeof(d.lightsPerHost), "%s", "abc");
        d.prepare();
        CHECK(d.destinationCount() == 0);          // the hosts parsed fine, but the split did not
        CHECK(d.status() != nullptr);
    }
}

// A short lightsPerHost list splits the rest evenly, as ledsPerPin does, and a long one ignores the extras.
TEST_CASE("NetworkSendDriver: a lightsPerHost list that doesn't match the tube count still slices sanely") {
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    d.setSourceBuffer(&src);
    std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70,71,72");   // 3 tubes

    SUBCASE("a SHORT list: the named tubes take their counts, the rest split the remainder") {
        std::snprintf(d.lightsPerHost, sizeof(d.lightsPerHost), "%s", "150");   // one value = every tube
        d.prepare();
        REQUIRE(d.destinationCount() == 3);
        CHECK(d.lightsAt(0) == 150);
        CHECK(d.lightsAt(1) == 150);
        CHECK(d.lightsAt(2) == 0);     // the 300-light window is exhausted: clamped, not wrapped
    }
    SUBCASE("a LONGER list than there are tubes: the extras are ignored, no overrun") {
        std::snprintf(d.lightsPerHost, sizeof(d.lightsPerHost), "%s", "50,60,70,80,90");
        d.prepare();
        REQUIRE(d.destinationCount() == 3);   // still 3 tubes: the list does not invent destinations
        CHECK(d.lightsAt(0) == 50);
        CHECK(d.lightsAt(1) == 60);
        CHECK(d.lightsAt(2) == 70);
    }
}

// sACN's group carries the universe, 239.255.{hi}.{lo} (E1.31 section 9.3.1), so a snooping switch filters per universe.
TEST_CASE("sACN multicast puts the universe number in the destination address") {
    uint8_t addr[4];
    mm::NetworkSendDriver::e131MulticastAddr(1, addr);
    CHECK(addr[0] == 239); CHECK(addr[1] == 255); CHECK(addr[2] == 0); CHECK(addr[3] == 1);

    mm::NetworkSendDriver::e131MulticastAddr(2, addr);
    CHECK(addr[3] == 2);                       // a different universe is a different group

    // The high byte carries universes past 255, which is where a big rig lives.
    mm::NetworkSendDriver::e131MulticastAddr(300, addr);
    CHECK(addr[2] == 1); CHECK(addr[3] == 44);  // 300 = 0x012C
}

// A fixture never straddles two universes, or an 11-channel moving head would read a neighbor's channels as its pan and tilt.
TEST_CASE("A DMX universe carries whole fixtures, never a split one") {
    constexpr size_t kUniverse = 512;
    // The driver's rounding restated, since its send path has no seam without a socket: floor the universe to whole fixtures.
    auto wholeFixtures = [](size_t chunk, uint8_t bytesPerLight) {
        const size_t whole = (chunk / bytesPerLight) * bytesPerLight;
        return whole > 0 ? whole : chunk;
    };

    CHECK(wholeFixtures(kUniverse, 11) == 506);   // 46 fixtures x 11, not 512
    CHECK(wholeFixtures(kUniverse, 11) % 11 == 0);
    CHECK(wholeFixtures(kUniverse, 3) == 510);    // 170 pixels x 3
    CHECK(wholeFixtures(kUniverse, 4) == 512);    // divides exactly: unchanged
    CHECK(wholeFixtures(kUniverse, 1) == 512);

    // A fixture wider than a universe cannot be served: keep the full universe rather than sending zero bytes forever, so it fails visibly instead of going silently dead.
    CHECK(wholeFixtures(kUniverse, 255) == 510);
}

// E1.31's multicast and Art-Net's broadcast are one stream of universes for the whole window, so the host list plays no part.
TEST_CASE("NetworkSendDriver: E1.31 multicast sends the whole window as one stream, whatever the hosts say") {
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    d.setSourceBuffer(&src);
    d.protocol = mm::NetworkSendDriver::kProtoE131;
    d.addressing = 1;
    CHECK(d.mode() == mm::Addressing::Multicast);
    d.prepare();
    REQUIRE(d.destinationCount() == 1);
    CHECK(d.lightsAt(0) == 300);
}

TEST_CASE("NetworkSendDriver: a protocol without the chosen mode falls back to unicast") {
    mm::NetworkSendDriver d;
    d.defineControls();
    d.protocol = mm::NetworkSendDriver::kProtoE131;
    d.addressing = 1;                         // multicast
    d.protocol = 2;                           // DDP has unicast only
    d.rebuildControls();                      // what every control write does
    CHECK(d.addressing == 0);
    CHECK(d.mode() == mm::Addressing::Unicast);
}

namespace {
bool panelAt80(const char* name, uint8_t* ip) {
    if (std::strcmp(name, "panel.local") != 0) return false;
    ip[0] = 192; ip[1] = 168; ip[2] = 1; ip[3] = 80;
    return true;
}
}  // namespace

// A name is looked up off the render thread, so it sends nothing until it resolves while the addresses beside it keep sending.
TEST_CASE("NetworkSendDriver: a named host waits for its address while the others send") {
    const mm::HostResolver::TestScope resolver(&panelAt80);
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    d.setSourceBuffer(&src);
    std::snprintf(d.hosts, sizeof(d.hosts), "%s", "192.168.1.70, panel.local");
    d.prepare();
    REQUIRE(d.destinationCount() == 2);
    CHECK(d.destinationAt(1)[3] == 0);                          // not resolved yet: skipped
    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "not resolved") != nullptr);  // and the card says so
    mm::HostResolver::passForTest();
    d.tick1s();
    CHECK(d.destinationAt(1)[3] == 80);
    CHECK(d.status() == nullptr);
}

// A light stream never falls back to broadcast on a network that drops multicast; the card says what to do instead.
TEST_CASE("NetworkSendDriver: E1.31 multicast on a network that drops it says to use unicast") {
    mm::Buffer src;
    src.allocate(300, 3);
    mm::NetworkSendDriver d;
    d.defineControls();
    d.setSourceBuffer(&src);
    d.protocol = mm::NetworkSendDriver::kProtoE131;
    d.addressing = 1;
    mm::NetworkPath::multicastDropped = true;
    d.prepare();
    const std::string status = d.status() ? d.status() : "";
    mm::NetworkPath::multicastDropped = false;
    CHECK(status.find("use unicast") != std::string::npos);
}
