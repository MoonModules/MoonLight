/// @module SpiralEffect
/// @also RingsEffect, RipplesEffect, LavaLampEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"
#include "golden_frame.h"   // cornersDiffer, the suite's one corner check

namespace {

struct Ctx {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Ctx(int w, int h) {
        grid.width = static_cast<mm::lengthType>(w);
        grid.height = static_cast<mm::lengthType>(h);
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
    }

    bool hasNonZero() {
        auto& buf = layer.buffer();
        for (size_t i = 0; i < buf.bytes(); i++) {
            if (buf.data()[i] != 0) return true;
        }
        return false;
    }

    bool cornersDiffer() {
        auto& buf = layer.buffer();
        return mm::golden::cornersDiffer(std::vector<uint8_t>(buf.data(), buf.data() + buf.bytes()));
    }

    // For effects with localized features (e.g. ripples): scan all pixels for at least two distinct RGB triplets.
    bool hasTwoDistinctColors() {
        auto* data = layer.buffer().data();
        size_t pixels = static_cast<size_t>(grid.width) * grid.height;
        for (size_t i = 1; i < pixels; i++) {
            size_t a = (i - 1) * 3;
            size_t b = i * 3;
            if (data[a] != data[b] || data[a + 1] != data[b + 1] || data[a + 2] != data[b + 2]) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

#define STATELESS_EFFECT_TEST(EFFECT) \
    TEST_CASE(#EFFECT " writes non-zero RGB") { \
        Ctx ctx(16, 16); \
        mm::EFFECT effect; \
        ctx.layer.addChild(&effect); \
        ctx.layer.applyState(); \
        ctx.layer.tick(); \
        CHECK(ctx.hasNonZero()); \
    } \
    TEST_CASE(#EFFECT " spatial variation") { \
        Ctx ctx(32, 32); \
        mm::EFFECT effect; \
        ctx.layer.addChild(&effect); \
        ctx.layer.applyState(); \
        ctx.layer.tick(); \
        CHECK(ctx.cornersDiffer()); \
    }

// SpiralEffect renders non-zero pixels and shows spatial variation (corners differ).
STATELESS_EFFECT_TEST(SpiralEffect)

// LavaLampEffect has localized blob features that can land on identical corner palette indices at some t values (corner-pair check is too strict). Scan the whole buffer for any two distinct pixels instead, same approach as RingsEffect below. LavaLamp paints at least one non-zero byte (the effect renders).
TEST_CASE("LavaLampEffect writes non-zero RGB") {
    Ctx ctx(16, 16);
    mm::LavaLampEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    ctx.layer.tick();
    CHECK(ctx.hasNonZero());
}

// Across 10 frames at bpm=60, at least one frame shows two distinct colors somewhere in the buffer (blobs move and the field varies).
TEST_CASE("LavaLampEffect spatial variation") {
    // LavaLamp's blobs cluster at some t values and produce a near-uniform saturated frame at the default slow bpm (=8). Sample several frames across a wider t range, at bpm=60 the blob positions sweep through the grid quickly enough that at least one frame in the window must have spatial variety. (If none do, the effect is genuinely broken.) Time advances through the platform test-clock seam so the loop is instant and deterministic, no real sleeps, no CI flakiness.
    Ctx ctx(32, 32);
    mm::LavaLampEffect effect;
    effect.bpm = 60;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    uint32_t virtualNow = 1000;
    bool varied = false;
    for (int i = 0; i < 10 && !varied; i++) {
        mm::platform::setTestNowMs(virtualNow);
        ctx.layer.tick();
        if (ctx.hasTwoDistinctColors()) varied = true;
        virtualNow += 50;
    }
    mm::platform::setTestNowMs(0);
    CHECK(varied);
}

// RingsEffect has localized features (thin rings); corner-pair check is too strict, so we scan for any two distinct pixels instead. Rings paints at least one non-zero byte (the effect renders).
TEST_CASE("RingsEffect writes non-zero RGB") {
    Ctx ctx(16, 16);
    mm::RingsEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    ctx.layer.tick();
    CHECK(ctx.hasNonZero());
}

// At least two distinct pixels exist somewhere in the buffer (rings are localized, so corner-pair would be too strict).
TEST_CASE("RingsEffect spatial variation") {
    Ctx ctx(32, 32);
    mm::RingsEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    ctx.layer.tick();
    CHECK(ctx.hasTwoDistinctColors());
}

// RipplesEffect (MoonLight sine-wave water surface) lights one pixel per column at a sine-driven height. On a flat 2D layer it still paints a visible wavefront, assert it renders something and varies across the surface.
TEST_CASE("RipplesEffect writes non-zero RGB") {
    Ctx ctx(16, 16);
    mm::RipplesEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    ctx.layer.tick();
    CHECK(ctx.hasNonZero());
}

// Ripples lights one pixel per column at a sine-driven height, so the surface holds at least two distinct colors (wavefront vs background), scan the whole buffer, corner-pair would be too strict.
TEST_CASE("RipplesEffect spatial variation") {
    Ctx ctx(32, 32);
    mm::RipplesEffect effect;
    ctx.layer.addChild(&effect);
    ctx.layer.applyState();
    ctx.layer.tick();
    CHECK(ctx.hasTwoDistinctColors());
}
