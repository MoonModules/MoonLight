/// @module NetworkReceiveEffect
/// @also NetworkSendDriver

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/util/ArtNetPacket.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/modifiers/TransposeModifier.h"
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

// A scenario compares these counts with what the sender sent: one DDP frame of two packets reads as two packets and one frame, and the next second starts at zero.
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

// Which value each physical light shows, by Drivers' rule: a wiring-order source copied as it is, else the layer's buffer mapped out. Index = driver index, value = the red byte that reached it, 0 for none.
std::vector<uint8_t> redPerDriverLight(const mm::Layer& layer) {
    const mm::nrOfLightsType n = layer.physicalLightCount();
    std::vector<uint8_t> out(n, 0);
    if (mm::EffectBase* wired = layer.wiringSource()) {
        std::vector<uint8_t> frame(static_cast<size_t>(n) * 3, 0);
        wired->copyWiringFrame(frame.data(), frame.size(), /*force=*/true);
        for (mm::nrOfLightsType k = 0; k < n; k++) out[k] = frame[static_cast<size_t>(k) * 3];
        return out;
    }
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

    OrderRig(mm::lengthType w, mm::lengthType h, mm::lengthType d, uint8_t mappedBy, bool serpentine = true,
             mm::ModifierBase* modifier = nullptr) {
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        grid.serpentine = serpentine;
        fx.mappedBy = mappedBy;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
        if (modifier) layer.addChild(modifier);
        fx.defineControls();
        layer.applyState();
    }
    ~OrderRig() {
        // The modifier is the test's, not the tree's to free.
        for (uint8_t i = layer.childCount(); i-- > 0;) if (layer.child(i) != &fx) layer.removeChild(layer.child(i));
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

// A mapping sender (xLights, FSEQ, Network Send) sends in wiring order, so light k shows what was sent for k; alone on its layer that is one copy.
TEST_CASE("NetworkReceiveEffect mapped by the sender reaches each light as sent, on a serpentine grid") {
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender);
    CHECK(r.layer.wiringSource() == &r.fx);
    r.sendNumbered(16);
    r.checkLightsAsSent(16);
}

// The same on a 3D layout, two slices of a serpentine grid, the shape of TouchMyLight's cube in small.
TEST_CASE("NetworkReceiveEffect mapped by the sender reaches each light as sent, on a 3D layout") {
    OrderRig r(4, 4, 2, mm::NetworkReceiveEffect::kMappedBySender);
    CHECK(r.layer.wiringSource() == &r.fx);
    r.sendNumbered(32);
    r.checkLightsAsSent(32);
}

// A layer with a modifier transforms its logical frame, so a sender-mapped stream there is placed in the grid first; a receiver-mapped one never passes through.
TEST_CASE("a frame goes to the output as it is only when nothing on its layer transforms it") {
    mm::MirrorModifier mirror;
    OrderRig mirrored(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, true, &mirror);
    CHECK(mirrored.layer.wiringSource() == nullptr);
    OrderRig receiver(4, 4, 1, mm::NetworkReceiveEffect::kMappedByReceiver);
    CHECK(receiver.layer.wiringSource() == nullptr);
    OrderRig plain(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, /*serpentine=*/false);
    CHECK(plain.layer.wiringSource() == nullptr);   // an identity layout is already one copy through the layer
}

namespace {
// The driver index of the light at (x, y, z) on a serpentine w x h grid, each slice wired row by row.
size_t serpentineIndex(size_t x, size_t y, size_t z, size_t w, size_t h) {
    return z * w * h + y * w + (y % 2 ? w - 1 - x : x);
}
}  // namespace

// A mirror on a sender-mapped stream mirrors the frame: each light shows what was sent for the light its position folds onto, rather than the mirror undoing itself.
TEST_CASE("a mirror on a sender-mapped serpentine grid mirrors the frame") {
    mm::MirrorModifier mirror;
    mirror.mirrorY = false;
    mirror.mirrorZ = false;
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, true, &mirror);
    r.sendNumbered(16);
    const std::vector<uint8_t> seen = redPerDriverLight(r.layer);
    for (size_t y = 0; y < 4; y++)
        for (size_t x = 0; x < 4; x++) {
            const size_t folded = x < 2 ? x : 3 - x;
            CHECK(seen[serpentineIndex(x, y, 0, 4, 4)] == serpentineIndex(folded, y, 0, 4, 4) + 1);
        }
}

// A transpose on a sender-mapped 3D layout swaps the frame's axes in every slice.
TEST_CASE("a transpose on a sender-mapped 3D layout transposes the frame") {
    mm::TransposeModifier transpose;   // x and y swap by default
    OrderRig r(4, 4, 2, mm::NetworkReceiveEffect::kMappedBySender, true, &transpose);
    r.sendNumbered(32);
    const std::vector<uint8_t> seen = redPerDriverLight(r.layer);
    for (size_t z = 0; z < 2; z++)
        for (size_t y = 0; y < 4; y++)
            for (size_t x = 0; x < 4; x++)
                CHECK(seen[serpentineIndex(x, y, z, 4, 4)] == serpentineIndex(y, x, z, 4, 4) + 1);
}

// On a layout already in wiring order, a plain grid with no modifiers, the two coincide and sender mapping is the same single copy.
TEST_CASE("NetworkReceiveEffect mapped by the sender on an identity layout is the same copy as mapped by the receiver") {
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, /*serpentine=*/false);
    REQUIRE_FALSE(r.layer.lut().hasLUT());   // the identity fast path
    r.sendNumbered(16);
    r.checkCellsAsSent(16);
}

// The status tells the user what sender mapping costs: one copy on an identity mapping or alone on its layer, mapped back and forward again under a modifier.
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
    CHECK(std::string(serpentine.fx.status()) == "receiving DDP from 10.0.0.7, one copy");   // straight to the output
    serpentine.fx.release();

    mm::MirrorModifier mirror;
    OrderRig mirrored(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, true, &mirror);
    mirrored.fx.recordSender("DDP", ip);
    mirrored.layer.tick();
    REQUIRE(mirrored.fx.status() != nullptr);
    CHECK(std::string(mirrored.fx.status()) == "receiving DDP from 10.0.0.7, mapped twice");
    mirrored.fx.release();
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
    // Counted before written, so the last light landing in the layer means all four were counted; bounded at about 100 ms.
    for (int i = 0; i < 100 && r.layer.buffer().data()[9] != 13; i++) {
        r.layer.tick();
        mm::platform::delayMs(1);
    }
    REQUIRE(r.layer.buffer().data()[9] == 13);
    mm::platform::setTestNowMs(3000);   // two seconds into the stream
    r.fx.tick1s();
    CHECK(r.fx.packetsPerS == 2);       // four packets over two seconds
    CHECK(r.fx.framesPerS == 2);
    mm::platform::setTestNowMs(0);
    tx.close();
    r.fx.release();
}
