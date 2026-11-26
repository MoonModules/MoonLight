/// @module Layer
/// @also ModifierBase

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/RegionModifier.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/CheckerboardModifier.h"
#include "platform/platform_clock.h"   // micros

#include <algorithm>
#include <vector>
#include <initializer_list>

// A Layer folds its enabled static modifiers in child order into one mapping (M₁∘M₂∘…): the chain composes, order matters, a disabled middle one is skipped.

namespace {
// Build a dense w×h grid Layer with the given modifiers added in order, run applyState.
struct ChainRig {
    mm::Layouts group;
    mm::GridLayout grid;
    mm::Layer layer;
    explicit ChainRig(mm::lengthType w, mm::lengthType h,
                      std::initializer_list<mm::MoonModule*> mods) {
        grid.width = w; grid.height = h; grid.depth = 1;
        group.addChild(&grid);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(3);
        for (auto* m : mods) layer.addChild(m);
        layer.defineControls();
        group.applyState();
        layer.applyState();
    }
};
} // namespace

// Region (left half) THEN Multiply (2× mirror): the logical box folds twice. On a 16-wide axis: Region 0..50 → 8, then Multiply 2 → 4. Both modifiers apply, the second is no longer dead weight.
TEST_CASE("Layer chains Region then Multiply (both fold)") {
    mm::RegionModifier region; region.startX = 0; region.endX = 50; region.endY = 100;
    mm::MultiplyModifier mult;  mult.multiplyX = 2; mult.multiplyY = 1; mult.multiplyZ = 1;
    ChainRig rig(16, 4, {&region, &mult});

    CHECK(rig.layer.width() == 4);    // 16 → region 8 → multiply 4
    CHECK(rig.layer.height() == 4);   // y untouched by either
    CHECK(rig.layer.lut().hasLUT());  // a real mapping, not the identity fast path

    // Regression: each modifier folds in its own stage's box; Region tested against the final box mapped a strip. The whole left region, 8 × 4 = 32 lights, reaches all 16 cells.
    std::size_t total = 0;
    std::size_t cellsHit = 0;
    for (mm::nrOfLightsType li = 0; li < rig.layer.lut().logicalCount(); li++) {
        std::size_t here = 0;
        rig.layer.lut().forEachDestination(li, [&](mm::nrOfLightsType) { here++; });
        total += here;
        if (here) cellsHit++;
    }
    CHECK(total == 32);                                  // the full left region, not a strip
    CHECK(cellsHit == rig.layer.lut().logicalCount());   // every logical cell is fed
}

// Order matters: Region-then-Multiply differs from Multiply-then-Region. Region's percentage applies to whatever box it sees, so the composed logical size differs.
TEST_CASE("Layer modifier order matters (A∘B ≠ B∘A)") {
    // A: Region(0..50) then Multiply(2×) on a 16-wide axis → 16→8→4.
    mm::RegionModifier rA; rA.startX = 0; rA.endX = 50;
    mm::MultiplyModifier mA; mA.multiplyX = 2; mA.multiplyY = 1;
    ChainRig a(16, 4, {&rA, &mA});

    // B: Multiply(2×) then Region(0..50), the same size as A, so the mapping pins the order: each order sends a light to a different cell.
    mm::MultiplyModifier mB; mB.multiplyX = 2; mB.multiplyY = 1;
    mm::RegionModifier rB; rB.startX = 0; rB.endX = 50;
    ChainRig b(16, 4, {&mB, &rB});

    // Both fold a 16-wide axis to width 4, but via different intermediate spaces.
    CHECK(a.layer.width() == 4);
    CHECK(b.layer.width() == 4);

    // The mappings differ: fingerprint each LUT preserving the per-logical-cell grouping (a cell-boundary marker between cells), so two mappings that flatten to the same destination sequence but group differently still compare unequal.
    auto fingerprint = [](mm::Layer& L) {
        std::vector<mm::nrOfLightsType> fp;
        const mm::nrOfLightsType kCellBoundary = static_cast<mm::nrOfLightsType>(-1);
        for (mm::nrOfLightsType li = 0; li < L.lut().logicalCount(); li++) {
            L.lut().forEachDestination(li, [&](mm::nrOfLightsType d) { fp.push_back(d); });
            fp.push_back(kCellBoundary);   // mark where each cell's run ends
        }
        return fp;
    };
    CHECK(fingerprint(a.layer) != fingerprint(b.layer));
}

// A DISABLED middle modifier is skipped, the chain folds only the enabled ones.
TEST_CASE("Layer skips a disabled modifier in the chain") {
    mm::RegionModifier region; region.startX = 0; region.endX = 50;
    mm::CheckerboardModifier mask; mask.setEnabled(false);   // disabled — must be skipped
    mm::MultiplyModifier mult; mult.multiplyX = 2; mult.multiplyY = 1;
    ChainRig rig(16, 4, {&region, &mask, &mult});

    // Region(8) then Multiply(4), the disabled mask between them contributes nothing.
    CHECK(rig.layer.width() == 4);

    // Enabling the mask drops cells (it rejects half), so the destination COUNT shrinks vs the disabled run, proof the enable flag is honoured per modifier.
    std::size_t withoutMask = 0;
    for (mm::nrOfLightsType li = 0; li < rig.layer.lut().logicalCount(); li++)
        rig.layer.lut().forEachDestination(li, [&](mm::nrOfLightsType) { withoutMask++; });

    mask.setEnabled(true);
    rig.layer.applyState();
    std::size_t withMask = 0;
    for (mm::nrOfLightsType li = 0; li < rig.layer.lut().logicalCount(); li++)
        rig.layer.lut().forEachDestination(li, [&](mm::nrOfLightsType) { withMask++; });

    CHECK(withMask < withoutMask);   // the mask now drops some physical lights
}

// Multiply folds the box and Checkerboard does not, so a round-trip is shrink, grow, shrink, each needing its own LUT.
TEST_CASE("Layer swaps a box-resizing modifier for one that does not, and back, resizing the mapping each time") {
    mm::MultiplyModifier mult;
    ChainRig rig(32, 32, {&mult});

    // Multiply's default is a 2x mirror fold on every axis, so the logical box is half the grid per axis.
    REQUIRE(rig.layer.width() == 16);
    REQUIRE(rig.layer.height() == 16);
    const mm::nrOfLightsType folded = rig.layer.lut().logicalCount();
    CHECK(folded == 16 * 16);

    // Swap in a Checkerboard at the same slot, which masks rather than folds: the box returns to the grid's own size.
    mm::CheckerboardModifier check;
    mm::MoonModule* out = rig.layer.replaceChildAt(0, &check);
    REQUIRE(out == &mult);
    rig.layer.applyState();
    CHECK(rig.layer.width() == 32);
    CHECK(rig.layer.height() == 32);
    CHECK(rig.layer.lut().logicalCount() == 32 * 32);

    // And back to the Multiply, which must fold again rather than keep the box the Checkerboard left behind.
    mm::MoonModule* out2 = rig.layer.replaceChildAt(0, &mult);
    REQUIRE(out2 == &check);
    rig.layer.applyState();
    CHECK(rig.layer.width() == 16);
    CHECK(rig.layer.height() == 16);
    CHECK(rig.layer.lut().logicalCount() == folded);   // the size it started at, not a residue of the round-trip
    CHECK(rig.layer.lut().hasLUT());                   // still a real mapping, not the identity fast path

    // Reached exactly once, not merely counted: a total alone passes on a mapping that doubled one light and dropped another.
    std::vector<int> seen(32 * 32, 0);
    std::size_t cellsHit = 0, total = 0;
    for (mm::nrOfLightsType li = 0; li < rig.layer.lut().logicalCount(); li++) {
        std::size_t here = 0;
        rig.layer.lut().forEachDestination(li, [&](mm::nrOfLightsType d) {
            here++;
            if (d < seen.size()) seen[d]++;
        });
        total += here;
        if (here) cellsHit++;
    }
    CHECK(cellsHit == rig.layer.lut().logicalCount());
    CHECK(total == 32 * 32);                           // all 1024 physical lights
    const bool eachOnce = std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; });
    CHECK(eachOnce);                                   // and none of them twice, none missed
}

namespace {
// A modifier whose tick takes a measurable while.
struct SlowModifier : mm::CheckerboardModifier {
    void tick() MM_NONBLOCKING override { const uint32_t t0 = mm::platform::micros(); while (mm::platform::micros() - t0 < 3000) {} }   // a spin, as a render tick holds its thread
};
}  // namespace

// The layer times each modifier as it times each effect, so a modifier's own figure is not left at 0.
TEST_CASE("Layer times its modifiers, own time included") {
    SlowModifier mod;
    ChainRig rig(8, 4, {&mod});
    mm::MoonModule root;   // brackets the layer, as the Effects container does
    root.addChild(&rig.layer);
    root.tick();
    root.publishTiming(1);

    CHECK(mod.tickTimeUs() >= 3000u);
    CHECK(mod.ownTimeUs() >= 3000u);
    CHECK(rig.layer.ownTimeUs() < rig.layer.tickTimeUs());
}
