/// @module TrailsEffect
/// @also Layer, draw

/// Dots carried on a flow field, each leaving a tail behind it.

#include "doctest.h"
#include "golden_frame.h"                 // the effect harness: Layouts, Grid, Layer
#include "light/effects/TrailsEffect.h"

using namespace mm;

namespace {
/// How much light the layer holds, summed over every channel: a tail's length shows up as this total.
uint64_t litTotal(const Layer& layer) {
    const auto& buf = layer.buffer();
    uint64_t sum = 0;
    for (size_t b = 0; b < buf.bytes(); b++) sum += buf.data()[b];
    return sum;
}

/// Render a panel for 40 frames at 20 ms, on a hand-driven clock: the decay is on elapsed time, so a real clock would measure the host.
uint64_t renderWith(uint8_t persistence) {
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; TrailsEffect effect;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    effect.persistence = persistence;   // the knob under test
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t i = 0; i < 40; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }
    return litTotal(layer);
}
}  // namespace

TEST_CASE("a longer persistence leaves more of the tail behind, so the control buys reach") {
    // Same heads, same flow, same clock: the whole difference is how much of each tail survived.
    const uint64_t shortTail = renderWith(20);
    const uint64_t longTail  = renderWith(220);
    CHECK(longTail > shortTail);
}

TEST_CASE("a trail decays on elapsed time, so a slower device shows the same tail") {
    // 20 frames of 40 ms must leave the tail where 40 frames of 20 ms leaves it, or the decay is per frame.
    auto run = [](uint16_t frames, uint32_t stepMs) {
        golden::ScopedTestClock clock(1000);
        Layouts layouts; GridLayout grid; Layer layer; TrailsEffect effect;
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
        layer.applyState();
        for (uint16_t i = 0; i < frames; i++) { platform::setTestNowMs(1000 + i * stepMs); layer.tick(); }
        return litTotal(layer);
    };
    const uint64_t fast = run(40, 20);        // 800 ms in 40 frames
    const uint64_t slow = run(20, 40);        // the same 800 ms in half the frames
    REQUIRE(fast > 0);
    // Not equality, since the heads land on different pixels: the claim is the tail's WEIGHT follows elapsed time.
    const uint64_t larger = fast > slow ? fast : slow;
    const uint64_t smaller = fast > slow ? slow : fast;
    CHECK(larger < smaller * 2);
}

TEST_CASE("Trails reshaped to the same light count starts from black rather than the old layout's tail") {
    // 8x32 to 32x8 is the same sample count, so resize() keeps a plane laid out for the OLD geometry.
    golden::ScopedTestClock clock(1000);   // read the planes, not the buffer: that is cleared regardless
    Layouts layouts; GridLayout grid; Layer layer; TrailsEffect effect;
    grid.width = 8; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t i = 0; i < 40; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }

    uint64_t before = 0;
    for (size_t k = 0; k < effect.trailSamples(); k++) before += effect.trailAt(k);
    REQUIRE(before > 0);                           // there is a tail that could carry over

    grid.width = 32; grid.height = 8;              // the same count, transposed
    layer.applyState();
    uint64_t after = 0;
    for (size_t k = 0; k < effect.trailSamples(); k++) after += effect.trailAt(k);
    CHECK(after == 0);                             // both planes cleared for the new geometry
}
