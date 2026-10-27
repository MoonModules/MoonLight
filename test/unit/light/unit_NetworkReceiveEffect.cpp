/// @module NetworkReceiveEffect
/// @also NetworkSendDriver

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/util/ArtNetPacket.h"
#include "light/effects/NetworkReceiveEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/MirrorModifier.h"
#include "light/modifiers/TransposeModifier.h"
#include "light/effects/SolidEffect.h"
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
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 4];   // three channels, padded to an even four
    const size_t len = mm::buildArtDmxPacket(pkt, 0, 0, payload, 3);

    uint16_t universe = 0, dataLen = 0;
    const uint8_t* data = nullptr;

    // Bad magic.
    uint8_t bad[mm::ARTNET_HEADER_SIZE + 4];
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
    uint8_t pkt[mm::ARTNET_HEADER_SIZE + 4];   // three channels, padded to an even four
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

// The status sets these counts beside what the sender sent: one DDP frame of two packets reads as two packets and one frame, and the next second starts at zero.
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
    CHECK(r.fx.packetsPerS() == 2);
    CHECK(r.fx.framesPerS() == 1);
    mm::platform::setTestNowMs(3000);
    r.fx.tick1s();
    CHECK(r.fx.packetsPerS() == 0);
    CHECK(r.fx.framesPerS() == 0);
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

// Beside another effect a sender-mapped frame shares the layer's buffer, so it is placed on the grid by the layout's positions and mapped forward like any effect.
TEST_CASE("a sender-mapped frame beside another effect still reaches each light as sent") {
    mm::SolidEffect under;   // ticks first, so the received frame is the last word on every cell
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender);
    r.layer.removeChild(&r.fx);
    r.layer.addChild(&under);
    r.layer.addChild(&r.fx);
    under.defineControls();
    r.layer.applyState();
    CHECK(r.layer.wiringSource() == nullptr);
    r.sendNumbered(16);
    r.checkLightsAsSent(16);
}

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
    CHECK(std::string(plain.fx.status()) == "0 fps, 0 packets/s, 0 lost/s, DDP from 10.0.0.7, one copy");
    plain.fx.release();

    OrderRig serpentine(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender);
    serpentine.fx.recordSender("DDP", ip);
    serpentine.layer.tick();
    REQUIRE(serpentine.fx.status() != nullptr);
    CHECK(std::string(serpentine.fx.status()) == "0 fps, 0 packets/s, 0 lost/s, DDP from 10.0.0.7, one copy");   // straight to the output
    serpentine.fx.release();

    mm::MirrorModifier mirror;
    OrderRig mirrored(4, 4, 1, mm::NetworkReceiveEffect::kMappedBySender, true, &mirror);
    mirrored.fx.recordSender("DDP", ip);
    mirrored.layer.tick();
    REQUIRE(mirrored.fx.status() != nullptr);
    CHECK(std::string(mirrored.fx.status()) == "0 fps, 0 packets/s, 0 lost/s, DDP from 10.0.0.7, mapped twice");
    mirrored.fx.release();
}

// Receiver mapping maps once, as every layer does, so the status names only the sender.
TEST_CASE("NetworkReceiveEffect mapped by the receiver names only the sender in its status") {
    const uint8_t ip[4] = {10, 0, 0, 7};
    OrderRig r(4, 4, 1, mm::NetworkReceiveEffect::kMappedByReceiver);
    r.fx.recordSender("Art-Net", ip);
    r.layer.tick();
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::string(r.fx.status()) == "0 fps, 0 packets/s, 0 lost/s, Art-Net from 10.0.0.7");
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
    CHECK(r.fx.packetsPerS() == 2);       // four packets over two seconds
    CHECK(r.fx.framesPerS() == 2);
    mm::platform::setTestNowMs(0);
    tx.close();
    r.fx.release();
}

// --- what the status counts ---------------------------------------------------

namespace {

// Send `count` packets over loopback and run the frame loop until `landed` sees the last one, bounded at about 100 ms.
template <typename Landed>
void deliver(Rig& r, mm::platform::UdpSocket& tx, uint16_t port, const std::vector<std::vector<uint8_t>>& packets, Landed landed) {
    const uint8_t loopback[4] = {127, 0, 0, 1};
    for (const auto& p : packets) REQUIRE(tx.sendToAddr(loopback, port, p.data(), p.size()));
    for (int i = 0; i < 100 && !landed(); i++) {
        r.layer.tick();
        mm::platform::delayMs(1);
    }
    REQUIRE(landed());
}

/// Sets the test clock where a member list starts, before the members after it are built.
struct ClockAt {
    explicit ClockAt(uint32_t ms) { mm::platform::setTestNowMs(ms); }
};

/// A receiver over loopback whose rate window opens at 1000 ms: send it packets, then let a second pass and read its rates.
struct LoopbackStream {
    ClockAt clock{1000};   // before the rig, whose prepare opens the window
    Rig r;
    mm::platform::UdpSocket tx;
    const uint8_t* buf = nullptr;
    LoopbackStream() {
        r.fx.setup();
        REQUIRE(tx.open());
        buf = r.layer.buffer().data();
    }
    ~LoopbackStream() {
        mm::platform::setTestNowMs(0);
        tx.close();
        r.fx.release();
    }
    template <typename Landed>
    void send(uint16_t port, const std::vector<std::vector<uint8_t>>& packets, Landed landed) { deliver(r, tx, port, packets, landed); }
    void secondPassed() {
        mm::platform::setTestNowMs(2000);
        r.fx.tick1s();
    }
};

std::vector<uint8_t> ddp(uint32_t offset, uint8_t red, bool push) {
    std::vector<uint8_t> p(mm::DDP_HEADER_SIZE + 3);
    const uint8_t payload[3] = {red, 0, 0};
    p.resize(mm::buildDdpPacket(p.data(), offset, push, payload, 3));
    return p;
}

std::vector<uint8_t> artnet(uint16_t universe, uint8_t red, uint8_t seq) {
    std::vector<uint8_t> p(mm::ARTNET_HEADER_SIZE + 4);   // three channels, padded to an even four
    const uint8_t payload[3] = {red, 0, 0};
    p.resize(mm::buildArtDmxPacket(p.data(), universe, seq, payload, 3));
    return p;
}

}  // namespace

// A frame of four 3-byte packets: one whole, one missing its middle two, one missing its push, so the next frame shows the lost tail.
TEST_CASE("NetworkReceiveEffect counts the DDP packets missing from a frame by their byte offsets") {
    LoopbackStream s;
    s.send(mm::DDP_PORT, {ddp(0, 1, false), ddp(3, 1, false), ddp(6, 1, false), ddp(9, 1, true),   // whole
                                  ddp(0, 2, false), ddp(9, 2, true),                                        // 3 and 6 lost
                                  ddp(0, 3, false), ddp(3, 3, false), ddp(6, 3, false),                     // the push lost
                                  ddp(0, 4, false), ddp(3, 4, false), ddp(6, 4, false), ddp(9, 4, true)},
            [&] { return s.buf[9] == 4; });
    s.secondPassed();
    CHECK(s.r.fx.packetsPerS() == 13);
    CHECK(s.r.fx.lostPerS() == 3);
}

// Under heavy loss a frame's last packet, its push, is the one most often missing, so the count starts at a frame's first packet too.
TEST_CASE("NetworkReceiveEffect counts DDP packets missing inside frames whose pushes never arrive") {
    LoopbackStream s;
    s.send(mm::DDP_PORT, {ddp(0, 1, false), ddp(6, 1, false), ddp(0, 2, false), ddp(6, 2, false)},   // 3 lost twice, 9 (the push) never sent
            [&] { return s.buf[6] == 2; });
    s.secondPassed();
    CHECK(s.r.fx.packetsPerS() == 4);
    CHECK(s.r.fx.lostPerS() == 2);
}

// Art-Net numbers each universe on its own here, as E1.31 requires, so two universes interleaved lose nothing, and 0 is a sender without a sequence.
TEST_CASE("NetworkReceiveEffect counts Art-Net losses per universe, and none for a sender without a sequence") {
    LoopbackStream s;
    // Universe 0 starts at byte 0, universe 1 at byte 510: frame 1, frame 2, then universe 0 skips frame 3.
    s.send(mm::ARTNET_PORT, {artnet(0, 1, 1), artnet(1, 1, 1), artnet(0, 2, 2), artnet(1, 2, 2), artnet(0, 4, 4), artnet(1, 3, 3),
                                     artnet(1, 9, 0), artnet(1, 10, 0)},
            [&] { return s.buf[0] == 4 && s.buf[510] == 10; });
    s.secondPassed();
    CHECK(s.r.fx.packetsPerS() == 8);
    CHECK(s.r.fx.lostPerS() == 1);
}

// Two packets swapped on the way arrive one behind the other; E1.31 reads a packet less than 20 behind as late, so a swap loses nothing and is no wrap of the sequence.
TEST_CASE("NetworkReceiveEffect counts a late packet as none lost, not as a wrap of the sequence") {
    LoopbackStream s;
    s.send(mm::ARTNET_PORT, {artnet(0, 1, 8), artnet(0, 2, 10), artnet(0, 3, 9), artnet(0, 4, 11)},   // 9 overtaken by 10
            [&] { return s.buf[0] == 4; });
    s.secondPassed();
    CHECK(s.r.fx.packetsPerS() == 4);
    CHECK(s.r.fx.lostPerS() == 1);   // 9 missing when 10 came; its late arrival and 11 lose nothing
}

// A narrower universe spreads the frame over more of them, so a live change of channels_per_universe sizes the sequences anew and a loss in the upper universes still counts.
TEST_CASE("NetworkReceiveEffect counts losses in every universe once channels_per_universe narrows live") {
    LoopbackStream s;
    REQUIRE(s.r.fx.affectsPrepare("channels_per_universe"));
    s.r.fx.channelsPerUniverse = 170;   // 768 bytes: universes 0 to 4
    s.r.fx.prepare();                   // what the live change runs
    s.send(mm::ARTNET_PORT, {artnet(4, 1, 1), artnet(4, 3, 3)}, [&] { return s.buf[680] == 3; });   // 2 lost in universe 4
    s.secondPassed();
    CHECK(s.r.fx.lostPerS() == 1);
}

// The status reads like the sender's, frames and packets a second first, so the two cards compare at a glance.
TEST_CASE("NetworkReceiveEffect shows the last second's frames, packets and losses in its status") {
    LoopbackStream s;
    s.send(mm::DDP_PORT, {ddp(0, 7, true), ddp(6, 8, true)}, [&] { return s.buf[6] == 8; });   // the packet at 3 never came
    s.secondPassed();
    s.r.layer.tick();
    REQUIRE(s.r.fx.status() != nullptr);
    CHECK(std::string(s.r.fx.status()).rfind("2 fps, 2 packets/s, 1 lost/s, DDP from ", 0) == 0);
}

// The layer counts this effect's own tick as a write, so an unchanged frame once read as overwritten and was copied again every other tick.
TEST_CASE("NetworkReceiveEffect copies an unchanged frame once, not again on later ticks") {
    Rig r;
    const uint8_t a[3] = {10, 0, 0};
    r.fx.applyBytes<false>(0, a, 3);
    r.layer.tick();
    REQUIRE(r.layer.buffer().data()[0] == 10);
    r.layer.buffer().data()[0] = 99;   // a mark only a fresh copy would overwrite
    for (int i = 0; i < 4; i++) r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 99);
    r.fx.release();
}

// A sender that marks its frames' ends shows each frame whole, once, at its push, and one that never marks them shows every packet.
TEST_CASE("NetworkReceiveEffect shows a DDP frame at its push once the sender marks pushes, and every packet before") {
    Rig r;
    r.fx.setup();
    mm::platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t* shown = r.layer.buffer().data();
    deliver(r, tx, mm::DDP_PORT, {ddp(0, 10, false)}, [&] { return shown[0] == 10; });   // no push seen yet: it shows
    deliver(r, tx, mm::DDP_PORT, {ddp(3, 20, true)}, [&] { return shown[3] == 20; });    // the first push
    deliver(r, tx, mm::DDP_PORT, {ddp(0, 30, false)}, [&] { return r.fx.stagedAt(0) == 30; });   // under the receive task's lock
    r.layer.tick();
    CHECK(shown[0] == 10);   // held: its frame has not ended
    deliver(r, tx, mm::DDP_PORT, {ddp(3, 40, true)}, [&] { return shown[3] == 40; });
    CHECK(shown[0] == 30);   // the push shows the whole frame
    tx.close();
    r.fx.release();
}

// An Art-Net frame shows at its highest universe, and a frame whose highest universe went missing still shows when the next begins.
TEST_CASE("NetworkReceiveEffect shows an Art-Net frame at its highest universe, or at the next frame's start when that one is lost") {
    Rig r;   // universes 0 and 1
    const uint8_t u0[3] = {1, 0, 0}, u1[3] = {2, 0, 0}, v0[3] = {3, 0, 0}, w0[3] = {4, 0, 0};
    r.fx.applyDmx<false>(0, u0, 3);
    r.fx.applyDmx<false>(1, u1, 3);
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 1);
    CHECK(r.layer.buffer().data()[510] == 2);
    r.fx.applyDmx<false>(0, v0, 3);   // the next frame's first universe: held for its last
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 1);
    r.fx.applyDmx<false>(0, w0, 3);   // universe 1 never came: the frame after shows what arrived
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 4);
    r.fx.release();
}

// Some senders push every packet; the offsets go on from the last end, so nothing reads as lost.
TEST_CASE("NetworkReceiveEffect counts no loss from a DDP sender that pushes every packet") {
    LoopbackStream s;
    s.send(mm::DDP_PORT, {ddp(0, 1, true), ddp(3, 1, true), ddp(6, 1, true), ddp(0, 2, true), ddp(3, 2, true), ddp(6, 2, true)},
            [&] { return s.buf[6] == 2; });
    s.secondPassed();
    CHECK(s.r.fx.packetsPerS() == 6);
    CHECK(s.r.fx.lostPerS() == 0);
}

// A sender with more lights than this layer still has its frames shown: the frame's last universe lies past staging but ends the frame.
TEST_CASE("NetworkReceiveEffect shows a frame whose last universe lies past the layer") {
    Rig r;   // 768 bytes: universes 0 and 1
    const uint8_t u0[3] = {5, 0, 0}, u1[3] = {6, 0, 0}, u2[3] = {7, 0, 0};
    r.fx.applyDmx<false>(0, u0, 3);
    r.fx.applyDmx<false>(1, u1, 3);
    r.fx.applyDmx<false>(2, u2, 3);   // the first frame learns universe 2 as its end
    r.layer.tick();
    const uint8_t v0[3] = {8, 0, 0};
    r.fx.applyDmx<false>(0, v0, 3);   // the next frame, held for its end
    r.fx.applyDmx<false>(1, u1, 3);
    r.fx.applyDmx<false>(2, u2, 3);   // past staging, and the frame's end
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 8);
    r.fx.release();
}

// A sender that shrinks its stream is learned in one frame, so its frames show at their new end rather than a frame late.
TEST_CASE("NetworkReceiveEffect learns a shrunken Art-Net frame's end in one frame") {
    Rig r;
    const uint8_t a0[3] = {1, 0, 0}, a1[3] = {2, 0, 0}, b0[3] = {3, 0, 0}, c0[3] = {4, 0, 0}, d0[3] = {5, 0, 0};
    r.fx.applyDmx<false>(0, a0, 3);
    r.fx.applyDmx<false>(1, a1, 3);   // two universes a frame
    r.layer.tick();
    r.fx.applyDmx<false>(0, b0, 3);   // now one: held, its end still expected at universe 1
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 1);
    r.fx.applyDmx<false>(0, c0, 3);   // the next frame shows the last and learns the new end
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 4);
    r.fx.applyDmx<false>(0, d0, 3);   // from here each frame shows as it lands
    r.layer.tick();
    CHECK(r.layer.buffer().data()[0] == 5);
    r.fx.release();
}

// A frame written in many packets reaches the layer whole, each byte where its offset put it.
TEST_CASE("NetworkReceiveEffect copies a frame of many packets whole") {
    Rig r(64, 64);   // 12288 bytes, nine DDP-sized writes
    std::vector<uint8_t> frame(64 * 64 * 3);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i * 7 + 1);
    for (size_t at = 0; at < frame.size(); at += 1440)
        r.fx.applyBytes<false>(at, frame.data() + at, static_cast<uint16_t>(std::min<size_t>(1440, frame.size() - at)));
    r.layer.tick();
    CHECK(std::memcmp(r.layer.buffer().data(), frame.data(), frame.size()) == 0);
    r.fx.release();
}
