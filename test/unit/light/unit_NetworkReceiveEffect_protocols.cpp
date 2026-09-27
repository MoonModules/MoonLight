/// @module NetworkReceiveEffect
/// @also NetworkSendDriver

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/util/ArtNetPacket.h"
#include "light/util/DdpPacket.h"
#include "light/util/E131Packet.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

#include <cstring>

// These tests pin the E1.31 and DDP halves of the multi-protocol receive: each wire format round-trips build→parse, malformed and cross-protocol datagrams are rejected, DDP's byte-direct placement clamps safely, ArtPoll is recognized, and one real localhost round-trip drives all three protocol sockets at once. (The ArtNet half is pinned by unit_NetworkReceiveEffect.cpp.)

namespace {

void cidFill(uint8_t cid[mm::E131_CID_LENGTH]) {
    for (uint8_t i = 0; i < mm::E131_CID_LENGTH; i++) cid[i] = static_cast<uint8_t>(i + 1);
}

struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::NetworkReceiveEffect fx;

    explicit Rig(mm::lengthType w = 16, mm::lengthType h = 16) {
        grid.width = w;
        grid.height = h;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
        fx.defineControls();
        layer.applyState();
    }
};

} // namespace

// --- E1.31 wire format ---------------------------------------------------------

// A packet built by the sender's builder parses back to the same universe and payload, the two sides can't drift.
TEST_CASE("E1.31 build→parse round-trip") {
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
    uint8_t pkt[mm::E131_HEADER_SIZE + 6];
    const size_t len = mm::buildE131Packet(pkt, 42, 7, cid, payload, 6);
    REQUIRE(len == mm::E131_HEADER_SIZE + 6);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;
    REQUIRE(mm::parseE131Packet(pkt, len, universe, data, dataLen));
    CHECK(universe == 42);
    CHECK(dataLen == 6);
    CHECK(std::memcmp(data, payload, 6) == 0);
}

// Truncated headers, a bad ACN identifier, wrong layer vectors, a non-zero start code, and a lying property count are all rejected.
TEST_CASE("E1.31 parse rejects malformed packets") {
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    uint8_t payload[3] = {9, 9, 9};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    const size_t len = mm::buildE131Packet(pkt, 1, 0, cid, payload, 3);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;
    uint8_t bad[mm::E131_HEADER_SIZE + 3];

    CHECK_FALSE(mm::parseE131Packet(pkt, mm::E131_HEADER_SIZE - 1, universe, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[4] = 'X';                          // ACN identifier broken
    CHECK_FALSE(mm::parseE131Packet(bad, len, universe, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[21] = 0x08;                        // root vector not E131 data
    CHECK_FALSE(mm::parseE131Packet(bad, len, universe, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[117] = 0x03;                       // DMP vector wrong
    CHECK_FALSE(mm::parseE131Packet(bad, len, universe, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[125] = 0x01;                       // non-zero start code: not light data
    CHECK_FALSE(mm::parseE131Packet(bad, len, universe, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[123] = 0x02; bad[124] = 0x00;      // property count claims 511 channels
    CHECK_FALSE(mm::parseE131Packet(bad, len, universe, data, dataLen));
}

// --- DDP wire format -------------------------------------------------------------

// A packet built by the sender's builder parses back to the same byte offset and payload.
TEST_CASE("DDP build→parse round-trip") {
    uint8_t payload[6] = {10, 20, 30, 40, 50, 60};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 6];
    const size_t len = mm::buildDdpPacket(pkt, 1440, /*push=*/true, payload, 6);
    REQUIRE(len == mm::DDP_HEADER_SIZE + 6);
    CHECK((pkt[0] & 0x01) == 0x01);        // push flag on the last packet

    uint32_t offset = 0;
    uint16_t dataLen = 0;
    const uint8_t* data = nullptr;
    REQUIRE(mm::parseDdpPacket(pkt, len, offset, data, dataLen));
    CHECK(offset == 1440);
    CHECK(dataLen == 6);
    CHECK(std::memcmp(data, payload, 6) == 0);
}

// Truncated headers, wrong version bits, and a lying length field are rejected.
TEST_CASE("DDP parse rejects malformed packets") {
    uint8_t payload[3] = {1, 2, 3};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 3];
    const size_t len = mm::buildDdpPacket(pkt, 0, false, payload, 3);

    uint32_t offset = 0;
    uint16_t dataLen = 0;
    const uint8_t* data = nullptr;
    uint8_t bad[mm::DDP_HEADER_SIZE + 3];

    CHECK_FALSE(mm::parseDdpPacket(pkt, mm::DDP_HEADER_SIZE - 1, offset, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[0] = 0x80;                          // version bits not 01
    CHECK_FALSE(mm::parseDdpPacket(bad, len, offset, data, dataLen));

    std::memcpy(bad, pkt, len);
    bad[8] = 0x05; bad[9] = 0x00;           // declares 1280 bytes, datagram has 3
    CHECK_FALSE(mm::parseDdpPacket(bad, len, offset, data, dataLen));
}

// --- cross-protocol rejects -------------------------------------------------------

// Each universe-protocol parser refuses the other protocols' datagrams, port mix-ups degrade to silence, not garbage.
TEST_CASE("cross-protocol datagrams are rejected") {
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    uint8_t payload[3] = {1, 2, 3};
    uint8_t art[mm::ARTNET_HEADER_SIZE + 3];
    uint8_t e131[mm::E131_HEADER_SIZE + 3];
    uint8_t ddp[mm::DDP_HEADER_SIZE + 3];
    const size_t artLen = mm::buildArtDmxPacket(art, 0, 0, payload, 3);
    const size_t e131Len = mm::buildE131Packet(e131, 1, 0, cid, payload, 3);
    const size_t ddpLen = mm::buildDdpPacket(ddp, 0, false, payload, 3);

    uint16_t universe = 0, dataLen = 0;
    uint32_t offset = 0;
    const uint8_t* data = nullptr;
    CHECK_FALSE(mm::parseArtDmxPacket(e131, e131Len, universe, data, dataLen));
    CHECK_FALSE(mm::parseArtDmxPacket(ddp, ddpLen, universe, data, dataLen));
    CHECK_FALSE(mm::parseE131Packet(art, artLen, universe, data, dataLen));
    CHECK_FALSE(mm::parseE131Packet(ddp, ddpLen, universe, data, dataLen));
    CHECK_FALSE(mm::parseDdpPacket(e131, e131Len, offset, data, dataLen));
    // An ArtNet datagram CAN slip past DDP's thin 2-bit version check when its payload is large ('A' = 0x41 has the right version bits), the dedicated port, not the header, is DDP's real discriminator, and the garbage offset it yields is absorbed by applyBytes' bound check. With a small payload the lying length still rejects it:
    CHECK_FALSE(mm::parseDdpPacket(art, artLen, offset, data, dataLen));
}

// --- ArtPoll recognition -----------------------------------------------------------

// An ArtPoll datagram is recognized (the discovery hook Resolume/Madrix use); OpDmx and non-ArtNet packets are not polls.
TEST_CASE("isArtPoll recognizes polls and nothing else") {
    uint8_t poll[14] = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x20, 0, 14, 0, 0};
    CHECK(mm::isArtPoll(poll, sizeof(poll)));
    CHECK_FALSE(mm::isArtPoll(poll, 10));   // truncated

    uint8_t payload[3] = {1, 2, 3};
    uint8_t art[mm::ARTNET_HEADER_SIZE + 3];
    const size_t artLen = mm::buildArtDmxPacket(art, 0, 0, payload, 3);
    CHECK_FALSE(mm::isArtPoll(art, artLen)); // OpDmx is not a poll
}

// The ArtPollReply carries the fields controllers read: opcode, IP, port, names, universe switches, MAC.
TEST_CASE("buildArtPollReply lays out the reply controllers parse") {
    const uint8_t ip[4] = {192, 168, 1, 230};
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t reply[mm::ARTNET_POLL_REPLY_SIZE];
    const size_t len = mm::buildArtPollReply(reply, ip, mac, "MoonLight", "MoonLight node", 0x0123);

    CHECK(len == mm::ARTNET_POLL_REPLY_SIZE);
    CHECK(std::memcmp(reply, "Art-Net", 8) == 0);
    CHECK(reply[8] == 0x00);
    CHECK(reply[9] == 0x21);                          // OpPollReply LE
    CHECK(std::memcmp(reply + 10, ip, 4) == 0);
    CHECK(reply[14] == 0x36);
    CHECK(reply[15] == 0x19);                         // port 6454 LE
    CHECK(reply[18] == 0x01);                         // NetSwitch = universe bits 14-8
    CHECK(reply[19] == 0x02);                         // SubSwitch = bits 7-4
    CHECK(reply[190] == 0x03);                        // SwOut[0] = bits 3-0
    CHECK(std::strcmp(reinterpret_cast<const char*>(reply + 26), "MoonLight") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(reply + 44), "MoonLight node") == 0);
    CHECK(std::memcmp(reply + 201, mac, 6) == 0);
}

// --- placement (applyBytes / per-protocol applyDmx) ---------------------------------

// DDP's byte addressing lands payloads at the exact offset; out-of-range and overflowing offsets are clamped or dropped.
TEST_CASE("applyBytes places, clamps, and survives hostile offsets") {
    Rig r;   // 768-byte staging
    uint8_t solid[16];
    std::memset(solid, 0xAB, sizeof(solid));

    r.fx.applyBytes(100, solid, sizeof(solid));
    CHECK(r.fx.stagingData()[100] == 0xAB);
    CHECK(r.fx.stagingData()[115] == 0xAB);

    r.fx.applyBytes(760, solid, sizeof(solid));   // 8 bytes fit, 8 clamp
    CHECK(r.fx.stagingData()[767] == 0xAB);

    r.fx.applyBytes(768, solid, sizeof(solid));   // exactly past the end — dropped
    r.fx.applyBytes(0xFFFFFFF0u, solid, sizeof(solid));  // hostile: must not overflow
    CHECK(true);   // reaching here without ASAN findings is the assertion
}

// channels_per_universe = 512 maps universes at 512-byte strides and clamps a 512-channel payload to its slot.
TEST_CASE("applyDmx honours channels_per_universe") {
    Rig r;
    r.fx.channelsPerUniverse = 512;
    uint8_t u0[512], u1[3] = {7, 8, 9};
    std::memset(u0, 0xCD, sizeof(u0));

    r.fx.applyDmx(0, u0, sizeof(u0));
    r.fx.applyDmx(1, u1, sizeof(u1));
    CHECK(r.fx.stagingData()[511] == 0xCD);
    CHECK(r.fx.stagingData()[512] == 7);   // universe 1 starts at byte 512, not 510

    // A 512-byte payload with stride 510 clamps to 510, the 2 padding bytes cannot bleed into the next universe's slot.
    r.fx.channelsPerUniverse = 510;
    std::memset(const_cast<uint8_t*>(r.fx.stagingData()), 0, r.fx.stagingBytes());
    r.fx.applyDmx(0, u0, sizeof(u0));
    CHECK(r.fx.stagingData()[509] == 0xCD);
    CHECK(r.fx.stagingData()[510] == 0);
}

// --- localhost round-trip: all three protocols into one effect ------------------------

// Three senders, one per protocol, hit the same effect on its three ports; each payload lands. The autodetect proof.
TEST_CASE("NetworkReceiveEffect receives all three protocols at once over localhost") {
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);   // all three binds succeeded

    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);

    mm::platform::UdpSocket artTx, e131Tx, ddpTx;
    REQUIRE((artTx.open() && artTx.connect("127.0.0.1", mm::ARTNET_PORT)));
    REQUIRE((e131Tx.open() && e131Tx.connect("127.0.0.1", mm::E131_PORT)));
    REQUIRE((ddpTx.open() && ddpTx.connect("127.0.0.1", mm::DDP_PORT)));

    // Three distinct payloads at three distinct buffer positions: ArtNet → universe 0 (offset 0), E1.31 → universe 1 (offset 510), DDP → byte 600.
    uint8_t a[3] = {11, 12, 13}, e[3] = {21, 22, 23}, d[3] = {31, 32, 33};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    artTx.sendTo(pkt, mm::buildArtDmxPacket(pkt, 0, 0, a, 3));
    e131Tx.sendTo(pkt, mm::buildE131Packet(pkt, 1, 0, cid, e, 3));
    ddpTx.sendTo(pkt, mm::buildDdpPacket(pkt, 600, true, d, 3));

    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        r.layer.tick();
        const uint8_t* buf = r.layer.buffer().data();
        landed = buf[0] == 11 && buf[510] == 21 && buf[600] == 31;
        if (!landed) mm::platform::delayMs(1);
    }
    CHECK(landed);
    // The "receiving <protocol> from <ip>" diagnostic carries the sender's IP. This test's packets travel over loopback (the same round-trip `landed` above already relies on), so the source is 127.0.0.1 and the status names it, the direct check that the source IP surfaces in the status.
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::strstr(r.fx.status(), "receiving ") != nullptr);
    CHECK(std::strstr(r.fx.status(), "from 127.0.0.1") != nullptr);

    artTx.close();
    e131Tx.close();
    ddpTx.close();
    r.fx.release();
}
