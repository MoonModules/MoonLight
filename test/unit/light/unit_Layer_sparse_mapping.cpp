/// @module Layer

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/GridBlacksLayout.h"
#include "light/layouts/SphereLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/RegionModifier.h"

// The driver/output buffer must hold ONLY the real lights, never the dense bounding box. A sphere defines a 9x9x9 (=729) render grid but only 210 shell lights; the MappingLUT extracts those 210 into the driver buffer (what ArtNet and the preview consume). These tests pin:
//  - sparse layout (sphere) builds a LUT whose destinations are driver indices
//    in [0, lightCount), never a box index >= lightCount (the latent overflow);
//  - dense grid stays on the identity fast path (no LUT), unchanged;
//  - a sphere + Mirror modifier still maps into driver-index space.
// The Layer buffer (where effects render) stays the dense box in all cases.

namespace {

// Build a Layer over a single layout, run applyState, return it wired.
struct LayerRig {
    mm::Layouts group;
    mm::Layer layer;

    explicit LayerRig(mm::LayoutBase* layout, uint8_t cpl = 3) {
        group.addChild(layout);
        layer.setLayouts(&group);
        layer.setChannelsPerLight(cpl);
        layer.defineControls();
        layer.applyState();
    }
};

} // namespace

// Dense grid: every box cell is a light, so no LUT, the identity/memcpy fast path is preserved exactly (the grid short-circuit).
TEST_CASE("Layer: dense grid stays on the identity path (no LUT)") {
    mm::GridLayout g;
    g.width = 8; g.height = 8; g.depth = 1;   // 64 lights, box == sparse
    LayerRig rig(&g);

    CHECK_FALSE(rig.layer.lut().hasLUT());          // identity, no table
    CHECK(rig.layer.physicalLightCount() == 64);
    CHECK(rig.layer.buffer().count() == 64);        // render buffer == box == lights
}

// Serpentine grid: dense (every box cell is a light, so the count check alone would pick the identity fast path) but SHUFFLED (driver index i != box cell i). isNaturalOrder() measures that from the coords and routes it through the box->driver LUT instead. This is the lever for exercising the non-identity mapping path without a sparse layout or a modifier.
TEST_CASE("Layer: serpentine grid leaves the identity path and builds a LUT") {
    mm::GridLayout g;
    g.width = 4; g.height = 4; g.depth = 1;   // 16 lights, dense
    g.serpentine = true;
    LayerRig rig(&g);

    CHECK(rig.layer.lut().hasLUT());                // dense-but-shuffled → a real LUT, not memcpy
    CHECK(rig.layer.physicalLightCount() == 16);
    CHECK(rig.layer.buffer().count() == 16);        // render buffer still the dense box

    // The LUT maps box cell -> driver index. Row 0 (even) is natural: box 0 -> driver 0. Row 1 (odd) is reversed: box cell (x=0,y=1) = box 4 should map to driver 7 (the strip enters that row from the high-x end), and box (x=3,y=1) = box 7 -> driver 4.
    const mm::MappingLUT& lut = rig.layer.lut();
    auto driverOf = [&](mm::nrOfLightsType box) {
        mm::nrOfLightsType d = 0xFFFF;
        lut.forEachDestination(box, [&](mm::nrOfLightsType dst) { d = dst; });
        return d;
    };
    CHECK(driverOf(0) == 0);    // row 0, x=0 — natural
    CHECK(driverOf(4) == 7);    // row 1, x=0 — reversed: last of the row's driver indices
    CHECK(driverOf(7) == 4);    // row 1, x=3 — reversed: first

    // Flipping serpentine off returns it to the identity fast path (no LUT).
    g.serpentine = false;
    rig.layer.applyState();
    CHECK_FALSE(rig.layer.lut().hasLUT());
}

// Sparse sphere: a LUT is built; its destinations are driver indices in [0, lightCount), and the render buffer stays the dense bounding box.
TEST_CASE("Layer: sparse sphere builds a box->driver LUT, no out-of-range index") {
    mm::SphereLayout s;
    s.radius = 4;                                   // 210 shell lights in a 9^3 box
    const mm::nrOfLightsType N = s.lightCount();
    CHECK(N == 210);
    LayerRig rig(&s);

    CHECK(rig.layer.lut().hasLUT());                // sparse → a real LUT exists
    CHECK(rig.layer.physicalLightCount() == N);     // driver buffer = real lights, not box
    CHECK(rig.layer.buffer().count() == 9 * 9 * 9); // render buffer = dense box (729)

    // Every LUT destination is a driver index < N, the fix for the latent overflow where box indices (0..728) were written into an N-sized buffer.
    const mm::MappingLUT& lut = rig.layer.lut();
    mm::nrOfLightsType maxDest = 0;
    mm::nrOfLightsType totalDests = 0;
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++) {
        lut.forEachDestination(li, [&](mm::nrOfLightsType d) {
            if (d > maxDest) maxDest = d;
            totalDests++;
        });
    }
    CHECK(totalDests == N);        // exactly the 210 shell cells map to a light
    CHECK(maxDest < N);            // no destination is >= N (no overflow)
    CHECK(lut.logicalCount() == 9 * 9 * 9);  // one logical entry per box cell
}

// Sphere + Mirror: the modifier's box-coordinate destinations are translated into driver-index space; no destination escapes [0, lightCount).
TEST_CASE("Layer: sphere + mirror maps into driver-index space") {
    mm::SphereLayout s;
    s.radius = 4;
    const mm::nrOfLightsType N = s.lightCount();

    mm::Layouts group;
    group.addChild(&s);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    mm::MultiplyModifier mirror;
    mirror.mirrorX = true;
    layer.addChild(&mirror);
    layer.defineControls();
    layer.applyState();

    CHECK(layer.lut().hasLUT());
    CHECK(layer.physicalLightCount() == N);

    const mm::MappingLUT& lut = layer.lut();
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++) {
        lut.forEachDestination(li, [&](mm::nrOfLightsType d) {
            CHECK(d < N);   // mirror destinations are driver indices, never box indices
        });
    }
}

// REGRESSION: a high fan-out Multiply (8×8×4 = 256) on a 128×128 grid must build a NON-EMPTY LUT that covers every physical light. The maxDest estimate (logicalCount × maxMultiplier) is computed in 64-bit; before that fix it overflowed uint16 on no-PSRAM boards (256 × 256 = 65536 wraps to 0), sized the LUT to ~nothing, and blanked the display. Here we assert the LUT actually maps the full light set, in range, the symptom that black-screened the device.
TEST_CASE("Layer: high fan-out Multiply builds a full, in-range LUT (no overflow)") {
    mm::GridLayout g;
    g.width = 128; g.height = 128; g.depth = 1;     // 16384 physical lights
    mm::Layouts group;
    group.addChild(&g);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    mm::MultiplyModifier mult;
    mult.multiplyX = 8; mult.multiplyY = 8; mult.multiplyZ = 4;  // raw product 256
    layer.addChild(&mult);
    layer.defineControls();
    layer.applyState();

    const mm::nrOfLightsType N = layer.physicalLightCount();   // 16384
    CHECK(N == 16384);
    CHECK(layer.lut().hasLUT());

    // multiplyZ clamps to depth-1 → effective 8×8×1; logical box 16×16 = 256.
    CHECK(layer.lut().logicalCount() == 16 * 16);

    // Count destinations: the LUT must NOT be empty (the black-screen bug) and every destination must be a valid driver index. 256 logical × 64 tiles = 16384 destinations = full coverage.
    std::size_t total = 0;
    bool inRange = true;
    for (mm::nrOfLightsType li = 0; li < layer.lut().logicalCount(); li++) {
        layer.lut().forEachDestination(li, [&](mm::nrOfLightsType d) {
            total++;
            if (d >= N) inRange = false;
        });
    }
    CHECK(total == 16384);   // full physical coverage, not a collapsed/empty LUT
    CHECK(inRange);
}

// Region carving: a RegionModifier shrinks the Layer's LOGICAL box to the region (so the effect renders only there), and the LUT maps each region cell to its box cell at the start offset, every destination in range, none outside the region. The driver buffer still holds all physical lights; cells outside the region simply get no logical source (dark). Default 0/100 = full box (the no-carve fast path) is covered by unit_RegionModifier; here we carve a quarter.
TEST_CASE("Layer: RegionModifier carves the logical box to a sub-region") {
    mm::GridLayout g;
    g.width = 8; g.height = 8; g.depth = 1;   // 64 lights, dense
    mm::Layouts group;
    group.addChild(&g);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    mm::RegionModifier region;
    region.startX = 0; region.endX = 50;      // left half  → pixels 0..3
    region.startY = 0; region.endY = 50;      // top half   → pixels 0..3
    layer.addChild(&region);
    layer.defineControls();
    layer.applyState();

    // Logical box is the carved quarter (4×4), not the full 8×8 box.
    CHECK(layer.width() == 4);
    CHECK(layer.height() == 4);
    CHECK(layer.lut().hasLUT());
    CHECK(layer.lut().logicalCount() == 4 * 4);

    // Physical driver buffer is unchanged, all 64 lights still exist; carving only restricts which of them the effect sources into.
    CHECK(layer.physicalLightCount() == 64);

    // Every destination is a real box light inside the carved quarter (x<4, y<4), there are exactly 16 of them (one per logical cell, no fan-out), and they are all DISTINCT, a 1:1 carve must reach 16 different physical lights, never collapse two logical cells onto one destination or leave a cell unreached.
    std::size_t total = 0;
    bool insideRegion = true;
    bool seen[64] = {false};     // 8×8 box
    bool duplicate = false;
    for (mm::nrOfLightsType li = 0; li < layer.lut().logicalCount(); li++) {
        layer.lut().forEachDestination(li, [&](mm::nrOfLightsType d) {
            total++;
            const mm::nrOfLightsType x = d % 8, y = d / 8;  // 8-wide box
            if (x >= 4 || y >= 4) insideRegion = false;
            if (d < 64) { if (seen[d]) duplicate = true; seen[d] = true; }
        });
    }
    CHECK(total == 16);          // 4×4 region, 1:1, nothing outside
    CHECK(insideRegion);
    CHECK_FALSE(duplicate);      // 16 distinct physical lights — no cell collapses onto another
}

// Black pixels (mid-strand dark gaps): a GridLayout with a dark column run leaves the identity fast path (an identity map would light the gap) and builds a LUT that maps only the LIT cells. The gap's physical index is a real wire slot (counted in the physical/driver total) but is NO logical cell's destination, so the scatter never writes it and it stays black, a "physical pixel that stays black". This is the GridLayout-native form of the sparse mapping the sphere/region tests above pin.
TEST_CASE("Layer: GridBlacks black columns build a gap-dropping LUT") {
    mm::GridBlacksLayout g;
    g.width = 8; g.height = 4; g.depth = 1;   // 32-cell box
    g.blackStart = 3; g.blackCount = 2;       // columns 3,4 dark → 8 gap cells, 24 lit
    LayerRig rig(&g);

    // A gap forces the folded LUT (the identity map would light the gap).
    CHECK(rig.layer.lut().hasLUT());
    // Physical/driver buffer is the FULL box, every wire slot exists, gaps included, so the driver clocks the dark LEDs and data flows through them.
    CHECK(rig.layer.physicalLightCount() == 32);
    // The effect renders the full box (holed model): logical box == physical box.
    CHECK(rig.layer.buffer().count() == 32);

    // The LUT maps only the 24 lit cells; NO destination is a gap physical index. A gap index is one whose column x is in [3,5): idx%8 in {3,4}, for every row.
    const mm::MappingLUT& lut = rig.layer.lut();
    std::size_t total = 0;
    bool anyGapDest = false;
    bool inRange = true;
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++) {
        lut.forEachDestination(li, [&](mm::nrOfLightsType d) {
            total++;
            if (d >= 32) inRange = false;
            const mm::nrOfLightsType col = d % 8;
            if (col == 3 || col == 4) anyGapDest = true;   // a gap slot must never be a destination
        });
    }
    CHECK(total == 24);            // exactly the lit cells reach a physical light
    CHECK(inRange);
    CHECK_FALSE(anyGapDest);       // the 8 dark slots carry no color — they stay black

    // Clearing the run returns to the dense identity fast path (no LUT), byte-identical to a plain grid, the gap machinery must not perturb the common case (regression).
    g.blackCount = 0;
    rig.layer.applyState();
    CHECK_FALSE(rig.layer.lut().hasLUT());
    CHECK(rig.layer.physicalLightCount() == 32);
}

// The LUT checks above pass equally against a blit that writes nothing, which is a dark wall with every assertion green.
TEST_CASE("Layer: an effect renders through a gapped GridBlacks layer") {
    mm::GridBlacksLayout g;
    g.width = 8; g.height = 4; g.depth = 1;
    g.blackStart = 3; g.blackCount = 2;
    LayerRig rig(&g);
    mm::RainbowEffect fx;
    rig.layer.addChild(&fx);
    rig.layer.applyState();
    REQUIRE(rig.layer.lut().hasLUT());

    rig.layer.tick();

    const mm::Buffer& buf = rig.layer.buffer();
    std::size_t lit = 0;
    for (std::size_t b = 0; b < buf.bytes(); b++) if (buf.data()[b]) lit++;
    CHECK(lit > 0);                        // the frame reached the buffer rather than stopping at the table
}

// Robustness: an ALL-black grid (every column dark) is a valid degenerate config, every physical slot is a real wire position the driver still clocks, but NO cell maps to a light, so the LUT has zero destinations. Must build and run (buffer stays black), never crash.
TEST_CASE("Layer: an all-black GridBlacks builds an empty LUT without crashing") {
    mm::GridBlacksLayout g;
    g.width = 8; g.height = 4; g.depth = 1;
    g.blackStart = 0; g.blackCount = 8;       // every column dark → 32 gap slots, 0 lit
    LayerRig rig(&g);

    CHECK(rig.layer.physicalLightCount() == 32);   // all 32 wire slots exist and are clocked
    // Every logical cell maps to nothing: the LUT has zero destinations (a valid empty map).
    const mm::MappingLUT& lut = rig.layer.lut();
    std::size_t total = 0;
    for (mm::nrOfLightsType li = 0; li < lut.logicalCount(); li++)
        lut.forEachDestination(li, [&](mm::nrOfLightsType) { total++; });
    CHECK(total == 0);                              // no cell reaches a light — the whole grid is dark
}
