/// @module BlendMap
/// @also MappingLUT
/// @also Layer

#include "doctest.h"
#include "light/layers/BlendMap.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstring>

// Identity mapping (logical N → physical N) leaves every byte unchanged.
TEST_CASE("blendMap identity (no LUT) copies buffer") {
    mm::Buffer src, dst;
    src.allocate(4, 3);
    dst.allocate(4, 3);

    // Fill src with known data
    for (size_t i = 0; i < src.bytes(); i++) {
        src.data()[i] = static_cast<uint8_t>(i + 1);
    }

    mm::MappingLUT lut;
    lut.setIdentity(4);

    mm::blendMap(src, dst, lut, 3);

    for (size_t i = 0; i < dst.bytes(); i++) {
        CHECK(dst.data()[i] == src.data()[i]);
    }
}

// One logical light routed to multiple physical positions copies the color to each (mirror-style mappings work).
TEST_CASE("blendMap 1:N mapping duplicates pixels") {
    // 2 logical lights, 4 physical lights Logical 0 → physical {0, 3} Logical 1 → physical {1, 2}
    mm::Buffer src, dst;
    src.allocate(2, 3);  // 2 logical lights
    dst.allocate(4, 3);  // 4 physical lights

    // Set logical light 0 to red, light 1 to blue
    src.data()[0] = 255; src.data()[1] = 0; src.data()[2] = 0;    // red
    src.data()[3] = 0;   src.data()[4] = 0; src.data()[5] = 255;  // blue

    mm::MappingLUT lut;
    lut.build(2, 4);
    mm::nrOfLightsType map0[] = {0, 3};
    lut.setMapping(0, map0, 2);
    mm::nrOfLightsType map1[] = {1, 2};
    lut.setMapping(1, map1, 2);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);

    // Physical 0 = red (from logical 0)
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 0);
    CHECK(dst.data()[2] == 0);

    // Physical 1 = blue (from logical 1)
    CHECK(dst.data()[3] == 0);
    CHECK(dst.data()[4] == 0);
    CHECK(dst.data()[5] == 255);

    // Physical 2 = blue (from logical 1, mirrored)
    CHECK(dst.data()[6] == 0);
    CHECK(dst.data()[7] == 0);
    CHECK(dst.data()[8] == 255);

    // Physical 3 = red (from logical 0, mirrored)
    CHECK(dst.data()[9] == 255);
    CHECK(dst.data()[10] == 0);
    CHECK(dst.data()[11] == 0);
}

// A paged LUT (forced via the maxAllocBlock test cap) must produce a byte-identical dst to a single-alloc LUT with the same mapping. Paging is an allocation detail; blendMap output must not depend on it. This is the end-to-end pin for the no-PSRAM-fragmentation fix.
TEST_CASE("blendMap is identical for single-alloc and paged LUTs") {
    struct Cap { ~Cap() { mm::platform::setTestMaxAllocBlock(0); } } cap;

    // 200 logical → up to 5000 physical destinations: large enough that paging spans multiple 4096-entry pages, with a run crossing a page boundary.
    const mm::nrOfLightsType logical = 200, physical = 5000;
    mm::Buffer src, dstSingle, dstPaged;
    src.allocate(logical, 3);
    dstSingle.allocate(physical, 3);
    dstPaged.allocate(physical, 3);
    for (size_t i = 0; i < src.bytes(); i++) src.data()[i] = static_cast<uint8_t>((i * 37) & 0xFF);

    // Build the same mapping into both LUTs; each logical light maps to ~25 physicals (200 × 25 = 5000), filled sequentially so runs straddle pages.
    auto fill = [&](mm::MappingLUT& lut) {
        REQUIRE(lut.build(logical, physical));
        mm::nrOfLightsType phys = 0;
        for (mm::nrOfLightsType li = 0; li < logical; li++) {
            mm::nrOfLightsType run[25];
            for (int k = 0; k < 25; k++) run[k] = phys++;
            lut.setMapping(li, run, 25);
        }
        lut.finalize();
    };

    mm::MappingLUT single, paged;
    mm::platform::setTestMaxAllocBlock(0);          // unlimited → single block
    fill(single);
    CHECK_FALSE(single.isPaged());
    mm::platform::setTestMaxAllocBlock(4096);       // tiny block → force paging
    fill(paged);
    CHECK(paged.isPaged());

    mm::blendMap(src, dstSingle, single, 3);
    mm::blendMap(src, dstPaged, paged, 3);

    REQUIRE(dstSingle.bytes() == dstPaged.bytes());
    CHECK(std::memcmp(dstSingle.data(), dstPaged.data(), dstSingle.bytes()) == 0);
}

// An additive (overwrites=false) LUT folding two sources onto one physical light adds and clamps at 255 (no overflow). overwrites=false is the opt-in for the within-layer overlap case; the default copy path would instead overwrite, and a full-opacity Overwrite op still routes through this additive accumulate, so this pins the contract explicitly (the regression after the multi-layer rewrite).
TEST_CASE("blendMap additive clamping (overwrites=false)") {
    mm::Buffer src, dst;
    src.allocate(1, 3);
    dst.allocate(1, 3);

    src.data()[0] = 200; src.data()[1] = 200; src.data()[2] = 200;

    // Map logical 0 to physical 0 TWICE, forces double-add with clamping
    mm::MappingLUT lut;
    lut.build(1, 2);
    lut.setOverwrites(false);  // opt into additive blending
    mm::nrOfLightsType map[] = {0, 0};
    lut.setMapping(0, map, 2);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);
    // 200 + 200 = 400 → clamped to 255
    CHECK(dst.data()[0] == 255);
    CHECK(dst.data()[1] == 255);
    CHECK(dst.data()[2] == 255);
}

// The default (overwrites=true) path plain-copies: two sources mapped to the same physical means the LAST writer wins, no addition. Pins the fast path.
TEST_CASE("blendMap overwrite path: last write wins, no add") {
    mm::Buffer src, dst;
    src.allocate(2, 3);
    dst.allocate(1, 3);
    src.data()[0] = 200; src.data()[1] = 200; src.data()[2] = 200;  // logical 0
    src.data()[3] = 50;  src.data()[4] = 50;  src.data()[5] = 50;   // logical 1

    mm::MappingLUT lut;
    lut.build(2, 2);  // overwrites_ defaults true
    mm::nrOfLightsType m0[] = {0};
    mm::nrOfLightsType m1[] = {0};  // both logical lights → physical 0
    lut.setMapping(0, m0, 1);
    lut.setMapping(1, m1, 1);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);
    // Copy, not add: logical 1 (last) wins → 50, not 250-clamped.
    CHECK(dst.data()[0] == 50);
    CHECK(dst.data()[1] == 50);
    CHECK(dst.data()[2] == 50);
}

// Sparse overwrite mapping clears untouched physical cells. A sphere-style layout maps only a subset of the physical box to a source; the rest must end up black, not retain stale data from a previous frame. Pre-fills dst dirty and asserts unmapped cells are zeroed, fails if BlendMap's dst.clear() is removed (the regression target).
TEST_CASE("blendMap overwrite path clears untouched cells (sparse mapping)") {
    mm::Buffer src, dst;
    src.allocate(2, 3);   // 2 logical lights
    dst.allocate(4, 3);   // 4 physical; only 2 are mapped
    src.data()[0] = 10; src.data()[1] = 20; src.data()[2] = 30;   // logical 0
    src.data()[3] = 40; src.data()[4] = 50; src.data()[5] = 60;   // logical 1

    // Dirty the whole dst so a missing clear would leave stale bytes behind.
    for (size_t i = 0; i < dst.bytes(); i++) dst.data()[i] = 0xFF;

    mm::MappingLUT lut;
    lut.build(2, 2);                 // overwrites_ defaults true
    mm::nrOfLightsType m0[] = {0};   // logical 0 → physical 0
    mm::nrOfLightsType m1[] = {2};   // logical 1 → physical 2 (1 and 3 unmapped)
    lut.setMapping(0, m0, 1);
    lut.setMapping(1, m1, 1);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3);

    // Mapped cells hold their source values.
    CHECK(dst.data()[0] == 10); CHECK(dst.data()[1] == 20); CHECK(dst.data()[2] == 30);
    CHECK(dst.data()[6] == 40); CHECK(dst.data()[7] == 50); CHECK(dst.data()[8] == 60);
    // Unmapped cells (physical 1 and 3) were cleared, not left dirty.
    CHECK(dst.data()[3] == 0); CHECK(dst.data()[4] == 0); CHECK(dst.data()[5] == 0);
    CHECK(dst.data()[9] == 0); CHECK(dst.data()[10] == 0); CHECK(dst.data()[11] == 0);
}

// --- Multi-layer composition: BlendOp + opacity + clearFirst (the new params). ---

// Build an identity LUT (1 logical → 1 physical) in place, MappingLUT owns a heap buffer and is non-copyable, so it can't be returned by value.
static void buildIdentityLut1(mm::MappingLUT& lut) {
    lut.build(1, 1);
    mm::nrOfLightsType m[] = {0};
    lut.setMapping(0, m, 1);
    lut.finalize();
}

// Alpha-over at half opacity: dst = src*α + dst*(255-α). With dst=200, src=100, α=128 → 100*128 + 200*127 = 12800 + 25400 = 38200; /255 ≈ 150.
TEST_CASE("blendMap alpha-over blends src over dst by opacity") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = src.data()[1] = src.data()[2] = 100;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 200;
    mm::MappingLUT lut; buildIdentityLut1(lut);
    // clearFirst=false: blend ONTO the existing dst (a layer above the bottom).
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, /*opacity=*/128, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 149);  // div255(100*128 + 200*127) = div255(38200) = 149
    CHECK(dst.data()[1] == 149);
    CHECK(dst.data()[2] == 149);
}

// Alpha at full opacity collapses to overwrite (src replaces dst exactly).
TEST_CASE("blendMap alpha at opacity 255 == overwrite") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = 10; src.data()[1] = 20; src.data()[2] = 30;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 99;
    mm::MappingLUT lut; buildIdentityLut1(lut);
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, 255, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 10); CHECK(dst.data()[1] == 20); CHECK(dst.data()[2] == 30);
}

// Alpha at opacity 0 is a no-op (dst unchanged), the invisible-layer case.
TEST_CASE("blendMap alpha at opacity 0 leaves dst unchanged") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = src.data()[1] = src.data()[2] = 200;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 77;
    mm::MappingLUT lut; buildIdentityLut1(lut);
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, 0, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 77); CHECK(dst.data()[1] == 77); CHECK(dst.data()[2] == 77);
}

// Additive with opacity scales the source before adding, then clamps. dst=100, src=200, opacity=128 → add 200*128/255 ≈ 100 → 200.
TEST_CASE("blendMap additive scales source by opacity then clamps") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = src.data()[1] = src.data()[2] = 200;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 100;
    mm::MappingLUT lut; buildIdentityLut1(lut);
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Additive, /*opacity=*/128, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 200);  // 100 + round(200*128/255)=100
    CHECK(dst.data()[1] == 200);
    CHECK(dst.data()[2] == 200);
}

// clearFirst=false preserves dst cells the source doesn't touch, the mechanic that lets a top layer blend onto the bottom layer's already-composited frame.
TEST_CASE("blendMap clearFirst=false accumulates onto existing frame") {
    mm::Buffer src, dst;
    src.allocate(2, 3); dst.allocate(2, 3);
    // src lights only physical 0; physical 1 left to the previous (bottom) layer.
    src.data()[0] = src.data()[1] = src.data()[2] = 50;
    src.data()[3] = src.data()[4] = src.data()[5] = 0;
    // dst holds a prior frame: physical 1 is green from the layer below.
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 0;
    dst.data()[3] = 0; dst.data()[4] = 255; dst.data()[5] = 0;

    mm::MappingLUT lut;
    lut.build(2, 1);
    mm::nrOfLightsType m0[] = {0}, m1[] = {1};
    lut.setMapping(0, m0, 1); lut.setMapping(1, m1, 1);
    lut.finalize();

    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Additive, 255, /*clearFirst=*/false);
    // Physical 0 got the additive source; physical 1's prior green survives + its src is 0.
    CHECK(dst.data()[0] == 50);
    CHECK(dst.data()[4] == 255);  // bottom layer's green preserved (not cleared)
}

// --- No-LUT (dense grid, identity 1:1) blend paths. blendMap has a SEPARATE
// implementation for a layer with no LUT (logical index == physical index, no lookup), the common dense-grid case that composites directly. These mirror the LUT blend tests above but on the no-LUT branch (an empty MappingLUT, so hasLUT()==false). This is the path that runs on a real grid layer; it was the one that initially failed to composite, so each op is pinned here explicitly.

// No-LUT alpha-over at half opacity: dst = div255(src*α + dst*(255-α)). dst=200, src=100, α=128 → div255(100*128 + 200*127) = div255(38200) = 149.
TEST_CASE("blendMap no-LUT alpha-over blends 1:1 by opacity") {
    mm::Buffer src, dst;
    src.allocate(2, 3); dst.allocate(2, 3);
    for (size_t i = 0; i < src.bytes(); i++) { src.data()[i] = 100; dst.data()[i] = 200; }
    mm::MappingLUT lut;   // no build/setMapping → hasLUT()==false (identity 1:1)
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, /*opacity=*/128, /*clearFirst=*/false);
    for (size_t i = 0; i < dst.bytes(); i++) CHECK(dst.data()[i] == 149);
}

// No-LUT alpha at full opacity collapses to a plain copy (overwrite).
TEST_CASE("blendMap no-LUT alpha at opacity 255 == overwrite") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = 10; src.data()[1] = 20; src.data()[2] = 30;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 99;
    mm::MappingLUT lut;
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, 255, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 10); CHECK(dst.data()[1] == 20); CHECK(dst.data()[2] == 30);
}

// No-LUT alpha at opacity 0 is a no-op (the invisible top layer).
TEST_CASE("blendMap no-LUT alpha at opacity 0 leaves dst unchanged") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = src.data()[1] = src.data()[2] = 200;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 77;
    mm::MappingLUT lut;
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Alpha, 0, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 77); CHECK(dst.data()[1] == 77); CHECK(dst.data()[2] == 77);
}

// No-LUT additive with opacity scales the source then clamps at 255. dst=100, src=200, opacity=128 → 100 + div255(200*128)=100 → 200.
TEST_CASE("blendMap no-LUT additive scales by opacity then clamps") {
    mm::Buffer src, dst;
    src.allocate(2, 3); dst.allocate(2, 3);
    for (size_t i = 0; i < src.bytes(); i++) { src.data()[i] = 200; dst.data()[i] = 100; }
    mm::MappingLUT lut;
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Additive, /*opacity=*/128, /*clearFirst=*/false);
    for (size_t i = 0; i < dst.bytes(); i++) CHECK(dst.data()[i] == 200);
}

// No-LUT additive at full opacity saturates: 200 + 100 = 300 → clamp 255.
TEST_CASE("blendMap no-LUT additive clamps at 255") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    src.data()[0] = src.data()[1] = src.data()[2] = 200;
    dst.data()[0] = dst.data()[1] = dst.data()[2] = 100;
    mm::MappingLUT lut;
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Additive, 255, /*clearFirst=*/false);
    CHECK(dst.data()[0] == 255); CHECK(dst.data()[1] == 255); CHECK(dst.data()[2] == 255);
}

// A mapping outliving the buffer it was built for must not write past that buffer.
//
// This is the resize window: prepareTree rebuilds the layer's mapping and the driver's output buffer in separate steps, and a render tick landing between them sees the NEW mapping's physical indices with the OLD, smaller buffer. Unbounded, the write ran off the end and corrupted the heap, the crash then surfaced later inside an unrelated allocation, which is why resizing a scripted layout looked intermittently fatal rather than pointing at the real writer.
//
// A mapped light the destination cannot hold is skipped. The frame is briefly wrong (the window is an ordering problem, not fixed here); it cannot corrupt memory, which is the property that counts.
TEST_CASE("a mapping larger than its destination writes only what the buffer holds") {
    mm::Buffer src, dst;
    src.allocate(8, 3);        // eight logical lights
    dst.allocate(4, 3);        // a destination sized for the count BEFORE the resize
    for (size_t i = 0; i < src.bytes(); i++) src.data()[i] = 0xAB;

    // A LUT that maps light i to physical i, for more lights than dst holds.
    mm::MappingLUT lut;
    REQUIRE(lut.build(8, 8));
    for (mm::nrOfLightsType i = 0; i < 8; i++) {
        const mm::nrOfLightsType phys = i;
        lut.setMapping(i, &phys, 1);
    }

    mm::blendMap(src, dst, lut, 3);

    // Everything the destination does hold was written; nothing beyond it was touched.
    for (size_t i = 0; i < dst.bytes(); i++) {
        INFO("byte " << i);
        CHECK(dst.data()[i] == 0xAB);
    }
}

namespace {
// The specification's formula in real arithmetic, rounded, so the integer kernels are checked against the math rather than against themselves.
int specBlend(mm::BlendOp op, int s, int d) {
    const double a = s / 255.0, b = d / 255.0;
    double r = 0;
    switch (op) {
        case mm::BlendOp::Multiply:   r = a * b; break;
        case mm::BlendOp::Screen:     r = a + b - a * b; break;
        case mm::BlendOp::Darken:     r = a < b ? a : b; break;
        case mm::BlendOp::Lighten:    r = a > b ? a : b; break;
        case mm::BlendOp::Difference: r = a > b ? a - b : b - a; break;
        case mm::BlendOp::Exclusion:  r = a + b - 2 * a * b; break;
        default:                      r = a; break;
    }
    return static_cast<int>(r * 255 + 0.5);
}

// One light per pair, blended at `opacity` onto the backdrop with no LUT.
uint8_t blendOne(mm::BlendOp op, uint8_t s, uint8_t d, uint8_t opacity = 255) {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    for (size_t i = 0; i < 3; i++) { src.data()[i] = s; dst.data()[i] = d; }
    mm::MappingLUT lut;
    mm::blendMap(src, dst, lut, 3, op, opacity, /*clearFirst=*/false);
    return dst.data()[0];
}
}  // namespace

// The separable modes of W3C Compositing and Blending, each within one step of its formula over a grid of source and backdrop values, corners included.
TEST_CASE("each separable blend mode computes its specification formula") {
    const mm::BlendOp ops[] = {mm::BlendOp::Multiply, mm::BlendOp::Screen, mm::BlendOp::Darken,
                               mm::BlendOp::Lighten, mm::BlendOp::Difference, mm::BlendOp::Exclusion};
    for (mm::BlendOp op : ops)
        for (int s = 0; s <= 255; s += 15)
            for (int d = 0; d <= 255; d += 15) {
                const int got = blendOne(op, static_cast<uint8_t>(s), static_cast<uint8_t>(d));
                const int want = specBlend(op, s, d);
                CAPTURE(static_cast<int>(op)); CAPTURE(s); CAPTURE(d);
                CHECK((got >= want - 1 && got <= want + 1));
            }
}

// A multiply layer is a luminance mask over what is below it.
TEST_CASE("a multiply layer keeps the backdrop under white, hides it under black and dims it under grey") {
    CHECK(blendOne(mm::BlendOp::Multiply, 255, 200) == 200);
    CHECK(blendOne(mm::BlendOp::Multiply, 0, 200) == 0);
    CHECK(blendOne(mm::BlendOp::Multiply, 128, 200) == 100);
}

// Opacity weighs every mode the same way, the blend result mixed with the backdrop, so a half-strength mask darkens half as much.
TEST_CASE("a separable mode at partial opacity mixes its result with the backdrop") {
    CHECK(blendOne(mm::BlendOp::Multiply, 0, 200, 0) == 200);     // invisible: the backdrop as it was
    CHECK(blendOne(mm::BlendOp::Multiply, 0, 200, 128) == 99);    // half the way to black: div255(200 * 127)
    CHECK(blendOne(mm::BlendOp::Screen, 255, 0, 128) == 128);     // half the way to white
}

// The mapped path runs the same function: a mode through a one-to-one LUT matches the dense-grid path.
TEST_CASE("a separable mode through a LUT matches the one without") {
    mm::Buffer src, dst;
    src.allocate(1, 3); dst.allocate(1, 3);
    for (size_t i = 0; i < 3; i++) { src.data()[i] = 128; dst.data()[i] = 200; }
    mm::MappingLUT lut; buildIdentityLut1(lut);
    mm::blendMap(src, dst, lut, 3, mm::BlendOp::Multiply, 255, /*clearFirst=*/false);
    CHECK(dst.data()[0] == blendOne(mm::BlendOp::Multiply, 128, 200));
}

// The layer's option list and the op it selects are two tables, so each option is checked to reach its own mode.
TEST_CASE("each layer blendMode option selects its own blend op") {
    const mm::BlendOp want[] = {mm::BlendOp::Alpha, mm::BlendOp::Additive, mm::BlendOp::Multiply, mm::BlendOp::Screen,
                                mm::BlendOp::Darken, mm::BlendOp::Lighten, mm::BlendOp::Difference, mm::BlendOp::Exclusion};
    static_assert(sizeof(want) / sizeof(want[0]) == mm::Layer::kBlendModeCount, "one expectation per mode");
    mm::Layer layer;
    for (uint8_t m = 0; m < mm::Layer::kBlendModeCount; m++) {
        layer.blendMode = m;
        CAPTURE(static_cast<int>(m));
        CHECK(layer.blendOp() == want[m]);
    }
}

// The rounded product keeps exclusion inside 0 to 255 and within one step of its formula, for every pair, as its comment claims.
TEST_CASE("exclusion stays in range and within one step of its formula for every input pair") {
    const mm::blendfn::Exclusion ex;
    int worst = 0;
    for (int a = 0; a <= 255; a++)
        for (int b = 0; b <= 255; b++) {
            const int got = ex(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
            const int want = specBlend(mm::BlendOp::Exclusion, a, b);
            const int off = got > want ? got - want : want - got;
            if (off > worst) worst = off;
        }
    CHECK(worst <= 1);
}
