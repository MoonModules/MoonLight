/// @module RandomMapModifier
/// @also Layer

#include "doctest.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <vector>

// RandomMapModifier remaps every light to another, a 1:1 permutation, and reshuffles on a bpm timer. A static fold: modifyLogical folds a physical coord to its permuted logical coord (the box is unchanged). These pin the bijection, determinism, reshuffle, and the empty-grid degrade, plus the tick() beat behavior through a real Layer.

namespace {

// Flatten a coord to an index in a w×h box.
mm::nrOfLightsType flat(mm::Coord3D p, mm::lengthType w, mm::lengthType h) {
    return static_cast<mm::nrOfLightsType>(p.z) * w * h +
           static_cast<mm::nrOfLightsType>(p.y) * w + p.x;
}

// Fold one light; returns its permuted destination index.
mm::nrOfLightsType mapOne(mm::RandomMapModifier& m,
                          mm::lengthType x, mm::lengthType y, mm::lengthType z,
                          mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    mm::Coord3D size{w, h, d};
    m.modifyLogicalSize(size);   // stashes the box for the permutation
    mm::Coord3D p{x, y, z};
    m.modifyLogical(p);
    return flat(p, w, h);
}

// Collect the destination of every light in a w×h×d grid, in index order.
std::vector<mm::nrOfLightsType> mapAll(mm::RandomMapModifier& m,
                                       mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    std::vector<mm::nrOfLightsType> out;
    for (mm::lengthType z = 0; z < d; z++)
        for (mm::lengthType y = 0; y < h; y++)
            for (mm::lengthType x = 0; x < w; x++)
                out.push_back(mapOne(m, x, y, z, w, h, d));
    return out;
}

} // namespace

// A remap leaves the logical box unchanged.
TEST_CASE("RandomMapModifier does not resize the logical box") {
    mm::RandomMapModifier m;
    mm::Coord3D size{64, 32, 4};
    m.modifyLogicalSize(size);
    CHECK(size == mm::Coord3D{64, 32, 4});
}

// The core property: a true bijection over [0, w*h*d), every destination index appears exactly once (no gaps, no duplicates).
TEST_CASE("RandomMapModifier is a bijection (every pixel mapped once)") {
    mm::RandomMapModifier m;
    const mm::lengthType w = 8, h = 8, d = 1;
    const mm::nrOfLightsType n = static_cast<mm::nrOfLightsType>(w) * h * d;
    auto dests = mapAll(m, w, h, d);

    REQUIRE(dests.size() == n);
    std::vector<int> seen(n, 0);
    for (auto dst : dests) { REQUIRE(dst < n); seen[dst]++; }
    for (mm::nrOfLightsType i = 0; i < n; i++) CHECK(seen[i] == 1);
}

// Deterministic seed → reproducible permutation (what makes it testable).
TEST_CASE("RandomMapModifier is deterministic for a fixed generation") {
    mm::RandomMapModifier a, b;
    CHECK(mapAll(a, 8, 8, 1) == mapAll(b, 8, 8, 1));
}

// Reshuffling (a beat) changes the mapping, still a bijection.
TEST_CASE("RandomMapModifier reshuffle changes the mapping, stays a bijection") {
    mm::RandomMapModifier m;
    const mm::lengthType w = 8, h = 8, d = 1;
    const mm::nrOfLightsType n = static_cast<mm::nrOfLightsType>(w) * h * d;

    auto before = mapAll(m, w, h, d);
    m.reshuffle();
    auto after = mapAll(m, w, h, d);
    CHECK(before != after);

    std::vector<int> seen(n, 0);
    for (auto dst : after) { REQUIRE(dst < n); seen[dst]++; }
    for (mm::nrOfLightsType i = 0; i < n; i++) CHECK(seen[i] == 1);
}

// Robustness: an empty (0×0×0) box must not crash, it folds to a no-op.
TEST_CASE("RandomMapModifier tolerates an empty box") {
    mm::RandomMapModifier m;
    mm::Coord3D size{0, 0, 0};
    m.modifyLogicalSize(size);
    mm::Coord3D p{0, 0, 0};
    CHECK(m.modifyLogical(p));   // no crash, never rejects
}

// A resize (different box count) rebuilds the permutation to the new size.
TEST_CASE("RandomMapModifier rebuilds on a grid resize") {
    mm::RandomMapModifier m;
    CHECK(mapAll(m, 4, 4, 1).size() == 16);

    auto big = mapAll(m, 8, 8, 1);
    CHECK(big.size() == 64);
    const mm::nrOfLightsType n = 64;
    std::vector<int> seen(n, 0);
    for (auto dst : big) { REQUIRE(dst < n); seen[dst]++; }
    for (mm::nrOfLightsType i = 0; i < n; i++) CHECK(seen[i] == 1);
}

// tick() timer behavior through a real Layer: the modifier reads the Layer clock and, on a beat, asks the Layer to rebuild (coalesced). We observe the MODIFIER'S MAPPING before vs after a timed run. A beat reshuffles (mapping differs); bpm 0 freezes it.
namespace {
bool mappingChangesOverMs(uint8_t bpm, int total_ms) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::RandomMapModifier mod;
    mod.bpm = bpm;
    layer.addChild(&mod);
    layouts.applyState();
    layer.applyState();

    const auto before = mapAll(mod, 8, 8, 1);
    uint32_t now = 1000;
    for (int e = 0; e <= total_ms; e += 50) {
        mm::platform::setTestNowMs(now + e);
        layer.tick();   // sets the Layer clock, then ticks the modifier's tick()
    }
    const auto after = mapAll(mod, 8, 8, 1);
    mm::platform::setTestNowMs(0);
    return before != after;
}
} // namespace

TEST_CASE("RandomMapModifier tick() reshuffles on a beat (bpm 60 ≈ 1/s)") {
    CHECK(mappingChangesOverMs(60, 1500) == true);
}

TEST_CASE("RandomMapModifier tick() with bpm 0 never reshuffles (frozen)") {
    CHECK(mappingChangesOverMs(0, 5000) == false);
}
