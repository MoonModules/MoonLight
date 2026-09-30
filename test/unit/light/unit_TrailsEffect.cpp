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

}  // namespace

TEST_CASE("a longer persistence leaves more of the tail behind, so the control buys reach") {
    // The MEAN over the run, since one frame is dominated by where the heads sit at that instant. Cadence cannot be compared instead: a per-frame decay makes two cadences CONVERGE, and unit_Effects_framerate pins that property where it measures cleanly.
    auto meanOver = [](uint8_t persistence) {
        golden::ScopedTestClock clock(1000);
        Layouts layouts; GridLayout grid; Layer layer; TrailsEffect effect;
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        effect.persistence = persistence;
        layer.addChild(&effect);
        layer.applyState();
        uint64_t sum = 0;
        for (uint16_t i = 1; i <= 40; i++) {
            platform::setTestNowMs(1000 + i * 20u);
            layer.tick();
            sum += litTotal(layer);
        }
        return static_cast<double>(sum) / 40.0;
    };
    const double brief = meanOver(20);
    const double lasting = meanOver(220);
    CAPTURE(brief); CAPTURE(lasting);
    REQUIRE(brief > 0.0);
    CHECK(lasting > brief * 1.5);   // a half-life an order apart is not a few percent of tail
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
