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
#include <memory>

// The E1.31 and DDP receive: round trips, rejected malformed and cross-protocol datagrams, DDP clamping, ArtPoll, and one loopback trip over all three sockets.

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
    uint8_t art[mm::ARTNET_HEADER_SIZE + 4];   // three channels, padded to an even four
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
    // A small Art-Net datagram fails DDP's length check; a large one can pass its version bits, which the dedicated port and the bound check absorb.
    CHECK_FALSE(mm::parseDdpPacket(art, artLen, offset, data, dataLen));
}

// --- ArtPoll recognition -----------------------------------------------------------

// An ArtPoll datagram is recognized (the discovery hook Resolume/Madrix use); OpDmx and non-ArtNet packets are not polls.
TEST_CASE("isArtPoll recognizes polls and nothing else") {
    uint8_t poll[14] = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x20, 0, 14, 0, 0};
    CHECK(mm::isArtPoll(poll, sizeof(poll)));
    CHECK_FALSE(mm::isArtPoll(poll, 10));   // truncated

    uint8_t payload[3] = {1, 2, 3};
    uint8_t art[mm::ARTNET_HEADER_SIZE + 4];   // three channels, padded to an even four
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

// A rebuild prepares again without a release; the effect binds its three ports afresh rather than reporting them in use by itself.
TEST_CASE("NetworkReceiveEffect binds its ports again on a second prepare") {
    auto r = std::make_unique<Rig>();   // on the heap: a rig outgrows a test's stack frame
    r->fx.setup();
    REQUIRE(r->fx.status() == nullptr);   // all three binds succeeded
    r->fx.prepare();
    CHECK(r->fx.status() == nullptr);
}

// Three senders, one per protocol, hit the same effect on its three ports; each payload lands. The autodetect proof.
TEST_CASE("NetworkReceiveEffect receives all three protocols at once over localhost") {
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);   // all three binds succeeded

    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);

    mm::platform::UdpSocket artTx, e131Tx, ddpTx;
    REQUIRE((artTx.open() && e131Tx.open() && ddpTx.open()));
    const uint8_t loopback[4] = {127, 0, 0, 1};

    // Three distinct payloads at three distinct buffer positions: Art-Net universe 0 (offset 0), E1.31 universe 2 (offset 510, since E1.31 numbers from 1), DDP byte 600.
    uint8_t a[3] = {11, 12, 13}, e[3] = {21, 22, 23}, d[3] = {31, 32, 33};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    artTx.sendToAddr(loopback, mm::ARTNET_PORT, pkt, mm::buildArtDmxPacket(pkt, 0, 0, a, 3));
    e131Tx.sendToAddr(loopback, mm::E131_PORT, pkt, mm::buildE131Packet(pkt, 2, 0, cid, e, 3));
    ddpTx.sendToAddr(loopback, mm::DDP_PORT, pkt, mm::buildDdpPacket(pkt, 600, true, d, 3));

    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        r.layer.tick();
        const uint8_t* buf = r.layer.buffer().data();
        landed = buf[0] == 11 && buf[510] == 21 && buf[600] == 31;
        if (!landed) mm::platform::delayMs(1);
    }
    CHECK(landed);
    // The status names the sender, 127.0.0.1 over loopback.
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::strstr(r.fx.status(), "from 127.0.0.1") != nullptr);

    artTx.close();
    e131Tx.close();
    ddpTx.close();
    r.fx.release();
}

// --- E1.31 multicast ------------------------------------------------------------

// E1.31 multicasts each universe to its own group, so the receiver joins the group of every universe its layer covers, from 1 since E1.31 has no universe 0.
TEST_CASE("NetworkReceiveEffect joins the E1.31 group of each universe its layer covers") {
    Rig r(32, 32);   // 3072 bytes: universes 0 to 6
    r.fx.setup();
    REQUIRE(r.fx.groupCountForTest() == 7);
    const auto* g = r.fx.groupsForTest();
    for (uint8_t i = 0; i < 7; i++) {
        CHECK(g[i].ip[0] == 239);
        CHECK(g[i].ip[1] == 255);
        CHECK(g[i].ip[2] == 0);
        CHECK(g[i].ip[3] == i + 1);
    }
    CHECK(r.fx.e131JoinCountForTest() == 7);   // one join each, whatever the host's network answers
    r.fx.release();
}

// The universe is in the address, its high byte in the third octet, so a live universe_start change moves the groups with it.
TEST_CASE("NetworkReceiveEffect moves its E1.31 groups with a live universe_start") {
    Rig r;   // 768 bytes: two universes
    r.fx.setup();
    REQUIRE(r.fx.affectsPrepare("universe_start"));
    r.fx.universeStart = 300;
    r.fx.prepare();   // what the live change runs
    REQUIRE(r.fx.groupCountForTest() == 2);
    const auto* g = r.fx.groupsForTest();
    CHECK(g[0].ip[2] == 1);
    CHECK(g[0].ip[3] == 44);   // 300 is 1 × 256 + 44
    CHECK(g[1].ip[2] == 1);
    CHECK(g[1].ip[3] == 45);
    r.fx.release();
}

// The status says what multicast cannot reach: the universes it joined, or none, and nothing when it joined them all.
TEST_CASE("NetworkReceiveEffect's multicast note names the universes only unicast reaches") {
    char s[80];
    mm::NetworkReceiveEffect::formatMulticastNote(s, sizeof(s), 1, 25, 13, false);
    CHECK(std::strcmp(s, "E1.31 multicast reaches universes 1 to 13, the rest only by unicast") == 0);
    mm::NetworkReceiveEffect::formatMulticastNote(s, sizeof(s), 1, 25, 13, true);
    CHECK(std::strcmp(s, ", multicast to universe 13") == 0);
    mm::NetworkReceiveEffect::formatMulticastNote(s, sizeof(s), 1, 25, 0, false);
    CHECK(std::strcmp(s, "E1.31 multicast unavailable: unicast only") == 0);
    mm::NetworkReceiveEffect::formatMulticastNote(s, sizeof(s), 1, 25, 0, true);
    CHECK(std::strcmp(s, ", unicast only") == 0);
    mm::NetworkReceiveEffect::formatMulticastNote(s, sizeof(s), 1, 25, 25, false);
    CHECK(s[0] == 0);
}

// A network without multicast still delivers unicast: the warning shows until E1.31 arrives, and the receiving status then carries the note.
TEST_CASE("NetworkReceiveEffect warns when E1.31 multicast is unavailable, and unicast still lands") {
    mm::platform::setTestJoin(mm::platform::TestJoin::Fail);
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::strcmp(r.fx.status(), "E1.31 multicast unavailable: unicast only") == 0);

    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    uint8_t payload[3] = {5, 6, 7};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    const uint8_t loopback[4] = {127, 0, 0, 1};
    REQUIRE(tx.sendToAddr(loopback, mm::E131_PORT, pkt, mm::buildE131Packet(pkt, 1, 0, cid, payload, 3)));
    // Loopback UDP is asynchronous: tick until the status names the sender, bounded at about 100 ms.
    for (int i = 0; i < 100 && std::strstr(r.fx.status(), "E1.31 from") == nullptr; i++) {
        r.layer.tick();
        mm::platform::delayMs(1);
    }
    CHECK(r.layer.buffer().data()[0] == 5);   // E1.31's universe 1 is the first
    CHECK(std::strstr(r.fx.status(), "E1.31 from") != nullptr);
    CHECK(std::strstr(r.fx.status(), ", unicast only") != nullptr);
    tx.close();
    r.fx.release();
}

// sACN senders multicast by default: a datagram to the universe's group reaches the layer, joined through the host's network.
TEST_CASE("NetworkReceiveEffect receives E1.31 multicast on its universe's group") {
    mm::platform::setTestJoin(mm::platform::TestJoin::Network);
    Rig r;
    r.fx.setup();
    r.fx.universeStart = 63000;   // a universe no sender on the LAN is likely to use, so only this case's datagram lands
    r.fx.prepare();               // what the live change runs
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    uint8_t payload[3] = {8, 9, 10};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    const uint8_t group[4] = {239, 255, 63000 >> 8, 63000 & 0xFF};
    if (!tx.sendToAddr(group, mm::E131_PORT, pkt, mm::buildE131Packet(pkt, 63000, 0, cid, payload, 3))) {
        MESSAGE("this host has no multicast route, so only the joins are checked");
        tx.close();
        r.fx.release();
        return;
    }
    if (r.fx.e131JoinedForTest() == 0) {   // a host that sends to the group but refused the join delivers nothing to it
        MESSAGE("this host refused the group's join, so only the send is checked");
        tx.close();
        r.fx.release();
        return;
    }
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        r.layer.tick();
        landed = r.layer.buffer().data()[0] == 8;
        if (!landed) mm::platform::delayMs(1);
    }
    CHECK(landed);
    tx.close();
    r.fx.release();
}

// A large set joins again one group per read while silent, so no read pays for the whole set, and a finished pass waits the interval again.
TEST_CASE("UdpSocket joins a silent set of groups again one group per read") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1);
    mm::platform::UdpSocket s;
    REQUIRE(s.open());
    REQUIRE(s.bind(0));
    const mm::platform::UdpSocket::MulticastGroup groups[3] = {{{239, 255, 0, 1}}, {{239, 255, 0, 2}}, {{239, 255, 0, 3}}};
    s.setMulticastGroups(groups, 3);
    CHECK(s.joinCountForTest() == 3);
    uint8_t buf[8];
    mm::platform::setTestNowMs(1 + mm::platform::UdpSocket::kRejoinMs);
    for (uint32_t joins : {4u, 5u, 6u, 6u}) {
        CHECK(s.recvFrom(buf, sizeof(buf)) == -1);
        CHECK(s.joinCountForTest() == joins);
    }
    s.close();
}

// A second joinMulticast leaves the group the first joined, not the one it is about to join, so a reused socket keeps one membership.
TEST_CASE("UdpSocket's joinMulticast leaves the earlier group before it joins the next") {
    mm::platform::UdpSocket s;
    REQUIRE(s.open());
    REQUIRE(s.bind(0));
    const uint8_t first[4] = {239, 255, 0, 1}, second[4] = {239, 255, 0, 2};
    s.joinMulticast(first);
    s.joinMulticast(second);
    const uint8_t* left = mm::platform::testLastLeftGroup();
    CHECK((left[0] == 239 && left[1] == 255 && left[2] == 0 && left[3] == 1));
    s.close();
}

// E1.31 has no universe 0, so with universe_start 0 its universe 1 lands where Art-Net's universe 0 does, as xLights and FPP number them; a set start maps as is.
TEST_CASE("NetworkReceiveEffect maps E1.31 from universe 1 when universe_start is 0, and from the start otherwise") {
    Rig r;
    r.fx.setup();
    uint8_t cid[mm::E131_CID_LENGTH];
    cidFill(cid);
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t loopback[4] = {127, 0, 0, 1};
    uint8_t pkt[mm::E131_HEADER_SIZE + 3];
    const auto sendUntil = [&](uint16_t universe, uint8_t value, size_t at) {
        const uint8_t payload[3] = {value, value, value};
        REQUIRE(tx.sendToAddr(loopback, mm::E131_PORT, pkt, mm::buildE131Packet(pkt, universe, 0, cid, payload, 3)));
        for (int i = 0; i < 100 && r.layer.buffer().data()[at] != value; i++) {
            r.layer.tick();
            mm::platform::delayMs(1);
        }
        return r.layer.buffer().data()[at] == value;
    };
    CHECK(sendUntil(1, 41, 0));     // universe 1 is the first
    CHECK(sendUntil(2, 42, 510));   // and 2 the second
    r.fx.universeStart = 5;
    r.fx.prepare();   // what the live change runs
    CHECK(sendUntil(5, 45, 0));     // a set start maps as is
    tx.close();
    r.fx.release();
}

namespace {
// The rig one rejoin period on, `each` run every millisecond while the task gets its pass: true once the E1.31 socket joined its groups again.
template <class Each>
bool rejoinsAfterOnePeriod(Each each) {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.receivesInTask());
    const uint32_t joins = r.fx.e131JoinCountForTest();
    REQUIRE(joins == r.fx.groupCountForTest());
    mm::platform::setTestNowMs(1000 + mm::platform::UdpSocket::kRejoinMs);
    // The task waits 20 ms per read: poll a bounded while for its pass.
    for (int i = 0; i < 300 && r.fx.e131JoinCountForTest() == joins; i++) {
        each();
        mm::platform::delayMs(1);
    }
    const bool rejoined = r.fx.e131JoinCountForTest() > joins;
    r.fx.release();
    return rejoined;
}
}  // namespace

// The receive task reads on a timeout too, so a silent E1.31 socket joins its groups again rather than letting a router forget them.
TEST_CASE("NetworkReceiveEffect joins its groups again while no packet arrives") {
    CHECK(rejoinsAfterOnePeriod([] {}));
}

// Art-Net streaming keeps the receive task's wait ready on every wake, so the E1.31 socket's rejoin check runs on the clock rather than only when its own socket is silent.
TEST_CASE("NetworkReceiveEffect joins its E1.31 groups again while Art-Net keeps streaming") {
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t loopback[4] = {127, 0, 0, 1};
    const uint8_t payload[3] = {1, 2, 3};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 4];
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);
    CHECK(rejoinsAfterOnePeriod([&] { tx.sendToAddr(loopback, mm::ARTNET_PORT, pkt, len); }));
    tx.close();
}
