/// @module NoiseEffect
/// @also noise, Palette

/// Noise is the plainest field effect: a gradient-noise sample straight into the palette. Its one character control decides what moves, which is what used to be two separate effects (the second was Noise2D, whose morph behavior is the `morph` option here). These pin that both options render, that they differ, and that each moves the way its name says.

#include "doctest.h"

#include "golden_frame.h"   // ScopedTestClock: restores the real clock on scope exit
#include "light/effects/NoiseEffect.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <array>
#include <cstdint>
#include <set>
#include <vector>

using namespace mm;

namespace {

/// Render Noise for `frames` on a w x h x d fixture and return the final buffer.
std::vector<uint8_t> render(lengthType w, lengthType h, lengthType d, uint8_t motion,
                            uint16_t frames = 40) {
    // RAII, so the override is cleared even if a REQUIRE below exits early: a leaked test clock freezes time for every test that runs after this one in the same binary.
    const mm::golden::ScopedTestClock clock(1000);
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    NoiseEffect effect;
    effect.motion = motion;
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();
    for (uint16_t f = 0; f < frames; f++) {
        platform::setTestNowMs(1000 + static_cast<uint32_t>(f) * 20);
        layer.tick();
    }
    auto& buf = layer.buffer();
    return std::vector<uint8_t>(buf.data(), buf.data() + buf.bytes());
}

std::size_t differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); i++) n += a[i] != b[i] ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE("both motions paint a field rather than a flat wash") {
    for (uint8_t motion = 0; motion < 2; motion++) {
        const auto f = render(16, 16, 1, motion);
        std::set<uint8_t> values(f.begin(), f.end());
        CHECK(values.size() > 8);          // a real field, not one repeated color
    }
}

TEST_CASE("drift moves the field across the fixture; morph changes it in place") {
    // The distinction the control exists for, and the reason the two used to be separate effects. Drift scrolls the sample coordinates, so a later frame is the same field shifted. Morph holds the coordinates and advances time, so the field changes without going anywhere. Either way the picture must move, which is what this checks: a still frame would mean the motion control does nothing at all.
    for (uint8_t motion = 0; motion < 2; motion++) {
        const auto early = render(24, 24, 1, motion, 10);
        const auto late  = render(24, 24, 1, motion, 120);
        CHECK(differing(early, late) > early.size() / 4);
    }
}

TEST_CASE("the two motions are genuinely different fields") {
    const auto drift = render(24, 24, 1, 0, 60);
    const auto morph = render(24, 24, 1, 1, 60);
    CHECK(differing(drift, morph) > drift.size() / 2);
}

TEST_CASE("on a volumetric fixture drifting slices differ from each other") {
    // What a 3D fixture buys: the light's own depth is the third noise axis, so the field has real depth rather than one slice repeated. Morph spends that axis on time instead, so its slices are identical by design and only drift is checked here.
    const auto f = render(8, 8, 4, 0, 40);
    const std::size_t slice = 8 * 8 * 3;
    REQUIRE(f.size() >= slice * 4);
    std::size_t diff = 0;
    for (std::size_t i = 0; i < slice; i++) diff += f[i] != f[slice * 3 + i] ? 1 : 0;
    CHECK(diff > slice / 4);
}

TEST_CASE("Noise renders on a strip, a panel and a cube alike") {
    // Any effect on any fixture: a 1D strip is not what a field effect is designed around, but it must still paint something rather than failing or going dark.
    for (auto dims : {std::array<lengthType, 3>{64, 1, 1}, {16, 16, 1}, {8, 8, 8}}) {
        const auto f = render(dims[0], dims[1], dims[2], 0);
        REQUIRE(f.size() == static_cast<std::size_t>(dims[0]) * dims[1] * dims[2] * 3);
        std::size_t lit = 0;
        for (uint8_t v : f) lit += v > 0 ? 1 : 0;
        CHECK(lit > f.size() / 8);
    }
}

TEST_CASE("Noise survives a degenerate grid rather than faulting") {
    const auto f = render(0, 0, 1, 0, 3);
    CHECK(f.empty());
}

TEST_CASE("morph shows the same field in every slice, because time is its third axis") {
    // The two motions divide the one spare axis between them: drift spends it on depth, morph on time. So a volumetric fixture under morph is the same field repeated, which is what the catalog card promises. The code briefly added a depth term here as well, which made morph a second drift and left the documentation wrong rather than the behavior.
    const auto f = render(8, 8, 4, 1, 40);
    const std::size_t slice = 8 * 8 * 3;
    REQUIRE(f.size() >= slice * 4);
    for (std::size_t s = 1; s < 4; s++)
        for (std::size_t i = 0; i < slice; i++)
            REQUIRE(f[i] == f[slice * s + i]);
}
