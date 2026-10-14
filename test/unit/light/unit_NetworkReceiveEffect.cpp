/// @module NetworkReceiveEffect
/// @also NetworkSendDriver

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/util/ArtNetPacket.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"

#include <cstring>
#include <string>
#include <vector>

// The Art-Net receive side: parsing, universe placement and clamping, the staging buffer's lifecycle, and one real loopback round trip.

namespace {

// A 16×16 RGB grid is 768 bytes: universe 0 holds bytes 0..509 and universe 1 bytes 510..767.
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

// --- wire format (shared header) ---------------------------------------------

// A packet built by the sender's builder parses back to the same universe and payload, the two sides can't drift.
TEST_CASE("ArtNet OpDmx build→parse round-trip") {
    uint8_t payload[6] = {1, 2, 3, 4, 5, 6};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 6];
    const size_t len = mm::buildArtDmxPacket(pkt, 259, 7, payload, 6);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;
    REQUIRE(mm::parseArtDmxPacket(pkt, len, universe, data, dataLen));
    CHECK(universe == 259);
    CHECK(dataLen == 6);
    CHECK(std::memcmp(data, payload, 6) == 0);
}

// Bad magic, non-OpDmx opcodes, truncated headers, and lying length fields are all rejected, the receiver drops them.
TEST_CASE("ArtNet OpDmx parse rejects malformed packets") {
    uint8_t payload[3] = {9, 9, 9};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 3];
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;

    // Bad magic.
    uint8_t bad[mm::ARTNET_HEADER_SIZE + 3];
    std::memcpy(bad, pkt, len);
    bad[0] = 'X';
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));

    // Wrong opcode (OpPoll 0x2000 instead of OpDmx 0x5000).
    std::memcpy(bad, pkt, len);
    bad[9] = 0x20;
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));

    // Truncated header.
    CHECK_FALSE(mm::parseArtDmxPacket(pkt, mm::ARTNET_HEADER_SIZE - 1, universe, data, dataLen));

    // Length field claims more data than the datagram carries.
    std::memcpy(bad, pkt, len);
    bad[16] = 0x01; bad[17] = 0xFE;   // declares 510, datagram has 3
    CHECK_FALSE(mm::parseArtDmxPacket(bad, len, universe, data, dataLen));
}

// --- universe placement (applyDmx, no sockets) --------------------------------

// Universe universe_start lands at byte 0; the next universe lands at byte 510, the same split the sender uses.
TEST_CASE("NetworkReceiveEffect places universes at consecutive 510-byte offsets") {
    Rig r;
    uint8_t u0[510], u1[258];
    std::memset(u0, 0xAA, sizeof(u0));
    std::memset(u1, 0xBB, sizeof(u1));

    r.fx.applyDmx(0, u0, sizeof(u0));
    r.fx.applyDmx(1, u1, sizeof(u1));
    r.layer.tick();   // staging → layer buffer

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 0xAA);
    CHECK(buf[509] == 0xAA);
    CHECK(buf[510] == 0xBB);
    CHECK(buf[767] == 0xBB);
}

// The layer clears its buffer every tick; staging holds the last frame, so the lights don't strobe black between packets.
TEST_CASE("NetworkReceiveEffect holds the last frame across ticks without new packets") {
    Rig r;
    uint8_t u0[3] = {10, 20, 30};
    r.fx.applyDmx(0, u0, sizeof(u0));

    r.layer.tick();
    r.layer.tick();   // no new packet — must still show the last frame
    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 10);
    CHECK(buf[1] == 20);
    CHECK(buf[2] == 30);
}

// A tick with no new packet skips the copy, which costs 3.5 ms at 12288 lights on an S3; a later writer's bytes surviving proves it.
TEST_CASE("NetworkReceiveEffect does not touch the layer buffer on a tick with no packet") {
    Rig r;
    uint8_t u0[3] = {10, 20, 30};
    r.fx.applyDmx(0, u0, sizeof(u0));
    r.layer.tick();                         // the packet lands in the layer buffer

    uint8_t* buf = r.layer.buffer().data();
    buf[0] = 99;                            // stand in for a later writer of the shared buffer
    r.layer.tick();                         // no packet: must leave the buffer alone
    CHECK(buf[0] == 99);

    r.fx.applyDmx(0, u0, sizeof(u0));       // a new packet repaints
    r.layer.tick();
    CHECK(buf[0] == 10);
}

// The held frame survives a fading sibling effect on the same layer, rather than an idle stream fading to black.
TEST_CASE("a held frame survives a sibling effect fading the layer") {
    Rig r;
    uint8_t u0[3] = {200, 200, 200};
    r.fx.applyDmx(0, u0, sizeof(u0));
    r.layer.tick();                       // the frame lands
    const uint8_t* buf = r.layer.buffer().data();
    REQUIRE(buf[0] == 200);

    // A sibling asks for a fade, the way a trail effect does. No new packet arrives.
    for (int i = 0; i < 20; i++) {
        r.layer.fadeToBlackBy(64);
        r.layer.tick();
    }
    CHECK(buf[0] == 200);                 // still held, not faded away
}

// Universes below universe_start are ignored; universes relative to a non-zero start land at offset 0.
TEST_CASE("NetworkReceiveEffect respects universe_start") {
    Rig r;
    r.fx.universeStart = 5;
    uint8_t below[3] = {1, 1, 1};
    uint8_t at[3] = {7, 8, 9};

    r.fx.applyDmx(4, below, sizeof(below));   // below start — ignored
    r.fx.applyDmx(5, at, sizeof(at));         // at start — offset 0
    r.layer.tick();

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[0] == 7);
    CHECK(buf[1] == 8);
    CHECK(buf[2] == 9);
}

// A payload overrunning the buffer end is clamped; a universe entirely beyond the buffer is ignored.
TEST_CASE("NetworkReceiveEffect clamps payloads to the buffer") {
    Rig r;   // 768 bytes
    uint8_t u1[510];
    std::memset(u1, 0xCC, sizeof(u1));

    r.fx.applyDmx(1, u1, sizeof(u1));   // offset 510 + 510 bytes > 768 → clamp to 258
    r.fx.applyDmx(2, u1, sizeof(u1));   // offset 1020 — entirely beyond → ignored
    r.layer.tick();

    const uint8_t* buf = r.layer.buffer().data();
    CHECK(buf[510] == 0xCC);
    CHECK(buf[767] == 0xCC);
}

// A 0×0×0 grid accepts packets as a clean no-op, degraded, not crashed.
TEST_CASE("NetworkReceiveEffect tolerates a zero-light grid") {
    Rig r(0, 0);
    uint8_t u0[3] = {1, 2, 3};
    r.fx.applyDmx(0, u0, sizeof(u0));
    r.layer.tick();
    CHECK(true);   // reaching here without a crash is the assertion
}

// --- staging lifecycle ---------------------------------------------------------

// Staging is sized in prepare (off the hot path), tick() never reallocates it, release frees it.
TEST_CASE("NetworkReceiveEffect staging buffer lifecycle") {
    Rig r;
    REQUIRE(r.fx.stagingData() != nullptr);
    CHECK(r.fx.stagingBytes() == r.layer.buffer().bytes());

    const uint8_t* before = r.fx.stagingData();
    r.layer.tick();
    CHECK(r.fx.stagingData() == before);   // no realloc in the hot path

    r.fx.release();
    CHECK(r.fx.stagingData() == nullptr);
    CHECK(r.fx.stagingBytes() == 0);
}

// --- localhost round-trip (real UDP through the platform bind/recvFrom path) ---

// A real packet sent over localhost UDP lands in the layer buffer, the end-to-end proof of the platform receive path.
TEST_CASE("NetworkReceiveEffect receives over localhost UDP") {
    Rig r;
    // The effect binds the three well-known protocol ports (constants by design). A running MoonLight desktop app would hold them, don't run the app and ctest at once; CI runners have the ports free.
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);   // binds succeeded

    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    uint8_t payload[3] = {42, 43, 44};
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 3];
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);
    const uint8_t loopback[4] = {127, 0, 0, 1};
    REQUIRE(tx.sendToAddr(loopback, mm::ARTNET_PORT, pkt, len));

    // UDP on loopback is reliable but asynchronous, poll the frame loop with a bounded retry (≤100 ms) so CI stays deterministic.
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        r.layer.tick();
        const uint8_t* buf = r.layer.buffer().data();
        landed = buf[0] == 42 && buf[1] == 43 && buf[2] == 44;
        if (!landed) mm::platform::delayMs(1);
    }
    CHECK(landed);

    tx.close();
    r.fx.release();
}

// The per-second counts are what a scenario compares with what the sender sent: one DDP frame of two packets, the second pushing, reads as two packets and one frame over one second, and the next second starts from zero.
TEST_CASE("NetworkReceiveEffect counts the packets and frames of the last second") {
    mm::platform::setTestNowMs(1000);   // the rate window starts when the stream does, on the test clock
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);   // binds succeeded
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t loopback[4] = {127, 0, 0, 1};
    uint8_t payload[3] = {1, 2, 3};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 3];
    REQUIRE(tx.sendToAddr(loopback, mm::DDP_PORT, pkt, mm::buildDdpPacket(pkt, 0, false, payload, 3)));
    REQUIRE(tx.sendToAddr(loopback, mm::DDP_PORT, pkt, mm::buildDdpPacket(pkt, 3, true, payload, 3)));
    // UDP on loopback is asynchronous: poll the frame loop until the pushed packet has landed, bounded at about 100 ms.
    for (int i = 0; i < 100 && r.layer.buffer().data()[3] != 1; i++) {
        r.layer.tick();
        mm::platform::delayMs(1);
    }
    mm::platform::setTestNowMs(2000);   // one second into the stream: the rate is the count
    r.fx.tick1s();
    CHECK(r.fx.packetsPerS == 2);
    CHECK(r.fx.framesPerS == 1);
    mm::platform::setTestNowMs(3000);
    r.fx.tick1s();
    CHECK(r.fx.packetsPerS == 0);
    CHECK(r.fx.framesPerS == 0);
    mm::platform::setTestNowMs(0);
    tx.close();
    r.fx.release();
}

// --- mapped by: the receiver against the sender ------------------------------

namespace {

// Which value each physical light shows once the layer maps its buffer out: index = driver index, value = the red byte that reached it, 0 for none.
std::vector<uint8_t> redPerDriverLight(const mm::Layer& layer) {
    const mm::nrOfLightsType n = layer.physicalLightCount();
    std::vector<uint8_t> out(n, 0);
    const uint8_t* buf = layer.buffer().data();
    const mm::MappingLUT& lut = layer.lut();
    for (mm::nrOfLightsType cell = 0; cell < layer.buffer().count(); cell++) {
        const uint8_t red = buf[static_cast<size_t>(cell) * 3];
        if (!lut.hasLUT()) { if (cell < n) out[cell] = red; continue; }
        lut.forEachDestination(cell, [&](mm::nrOfLightsType dst) { if (dst < n) out[dst] = red; });
    }
    return out;
}

// A frame whose light k carries red = k + 1, so every value names the wiring position it was sent for.
std::vector<uint8_t> numberedFrame(size_t lights) {
    std::vector<uint8_t> f(lights * 3, 0);
    for (size_t k = 0; k < lights; k++) f[k * 3] = static_cast<uint8_t>(k + 1);
    return f;
}

struct OrderRig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::NetworkReceiveEffect fx;

    OrderRig(mm::lengthType w, mm::lengthType h, mm::lengthType d, uint8_t mappedBy, bool serpentine = true) {
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        grid.serpentine = serpentine;
        fx.mappedBy = mappedBy;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
        fx.defineControls();
        layer.applyState();
    }
    // A numbered frame of `lights` lights into staging, then one frame of the layer.
    void sendNumbered(size_t lights) {
        const std::vector<uint8_t> frame = numberedFrame(lights);
        fx.applyBytes(0, frame.data(), static_cast<uint16_t>(frame.size()));
        layer.tick();
    }
    // Each of the first `cells` logical cells shows what was sent for it.
    void checkCellsAsSent(size_t cells) {
        const uint8_t* buf = layer.buffer().data();
        for (size_t cell = 0; cell < cells; cell++) CHECK(buf[cell * 3] == cell + 1);
    }
    // Each of the first `lights` physical lights shows what was sent for it.
    void checkLightsAsSent(size_t lights) {
        const std::vector<uint8_t> seen = redPerDriverLight(layer);
        for (size_t k = 0; k < lights; k++) CHECK(seen[k] == k + 1);
    }
};

}  // namespace

// A sender sending row by row leaves the wiring to this board, so the layer applies the serpentine once and cell (x, y) shows what was sent for it.
TEST_CASE("NetworkReceiveEffect mapped by the receiver lets the layer apply this board's wiring") {
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedByReceiver);
    REQUIRE(r.layer.lut().hasLUT());   // serpentine: the mapping is not the identity
    r.sendNumbered(16);
    r.checkCellsAsSent(16);
}

// A mapping sender (xLights, FSEQ, Network Send) sends in wiring order, so light k shows what was sent for k; copied by index, every other row would reverse.
TEST_CASE("NetworkReceiveEffect mapped by the sender reaches each light as sent, on a serpentine grid") {
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender);
    r.sendNumbered(16);
    r.checkLightsAsSent(16);
}

// The same on a 3D layout, two slices of a serpentine grid, the shape of TouchMyLight's cube in small.
TEST_CASE("NetworkReceiveEffect mapped by the sender reaches each light as sent, on a 3D layout") {
    OrderRig r(4, 4, 2, mm::NetworkReceiveEffect::kMappedBySender);
    r.sendNumbered(32);
    r.checkLightsAsSent(32);
}

// On a layout already in wiring order, a plain grid with no modifiers, the two coincide and sender mapping is the same single copy.
TEST_CASE("NetworkReceiveEffect mapped by the sender on an identity layout is the same copy as mapped by the receiver") {
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, /*serpentine=*/false);
    REQUIRE_FALSE(r.layer.lut().hasLUT());   // the identity fast path
    r.sendNumbered(16);
    r.checkCellsAsSent(16);
}

// The status tells the user what sender mapping costs: one copy on an identity mapping, mapped back and forward again on any other.
TEST_CASE("NetworkReceiveEffect mapped by the sender says in its status whether a frame is one copy or mapped twice") {
    const uint8_t ip[4] = {10, 0, 0, 7};
    OrderRig plain(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, /*serpentine=*/false);
    plain.fx.recordSender("DDP", ip);
    plain.layer.tick();
    REQUIRE(plain.fx.status() != nullptr);
    CHECK(std::string(plain.fx.status()) == "receiving DDP from 10.0.0.7, one copy");
    plain.fx.release();

    OrderRig serpentine(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender);
    serpentine.fx.recordSender("DDP", ip);
    serpentine.layer.tick();
    REQUIRE(serpentine.fx.status() != nullptr);
    CHECK(std::string(serpentine.fx.status()) == "receiving DDP from 10.0.0.7, mapped twice");
    serpentine.fx.release();
}

// Receiver mapping maps once, as every layer does, so the status names only the sender.
TEST_CASE("NetworkReceiveEffect mapped by the receiver names only the sender in its status") {
    const uint8_t ip[4] = {10, 0, 0, 7};
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedByReceiver);
    r.fx.recordSender("Art-Net", ip);
    r.layer.tick();
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::string(r.fx.status()) == "receiving Art-Net from 10.0.0.7");
    r.fx.release();
}

// A rate divides by the time that actually passed, so a late housekeeping tick reports a rate rather than the sum of several seconds.
TEST_CASE("NetworkReceiveEffect reports its counts per second of elapsed time") {
    mm::platform::setTestNowMs(1000);   // the rate window starts when the stream does, on the test clock
    Rig r;
    r.fx.setup();
    REQUIRE(r.fx.status() == nullptr);
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t loopback[4] = {127, 0, 0, 1};
    uint8_t pkt[mm::DDP_HEADER_SIZE + 3];
    for (uint8_t k = 0; k < 4; k++) {   // four lights, each its own packet, the last marking the frame
        uint8_t payload[3] = {static_cast<uint8_t>(10 + k), 0, 0};
        REQUIRE(tx.sendToAddr(loopback, mm::DDP_PORT, pkt, mm::buildDdpPacket(pkt, k * 3u, true, payload, 3)));
    }
    // A packet is counted before it is written, so the last light landing means all four were counted; bounded at about 100 ms.
    for (int i = 0; i < 100 && r.fx.stagingData()[9] != 13; i++) mm::platform::delayMs(1);
    REQUIRE(r.fx.stagingData()[9] == 13);
    mm::platform::setTestNowMs(3000);   // two seconds into the stream
    r.fx.tick1s();
    CHECK(r.fx.packetsPerS == 2);       // four packets over two seconds
    CHECK(r.fx.framesPerS == 2);
    mm::platform::setTestNowMs(0);
    tx.close();
    r.fx.release();
}
