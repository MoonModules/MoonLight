/// @module BeatRipplesEffect
/// @also golden_frame

/// The wave surface is pinned as visible and moving. A golden hash passes on an all-black frame too, which is how this effect once shipped: its stone was scaled far below the slope that lights a pixel.

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/BeatRipplesEffect.h"

using namespace mm;

namespace {
/// Brightest byte anywhere in the frame: how strongly the surface catches the light.
int brightestLight(const Layer& layer) {
    const auto& b = layer.buffer();
    int peak = 0;
    for (size_t i = 0; i < b.bytes(); i++) if (b.data()[i] > peak) peak = b.data()[i];
    return peak;
}
}  // namespace

TEST_CASE("still water shows its ripples, and they spread") {
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; BeatRipplesEffect e;
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&e);
    layer.applyState();

    // With no audio at all the idle rain is the only source, and the first drop lands on the opening frame rather than after a silent interval of black water.
    for (int f = 0; f < 40; f++) { platform::setTestNowMs(1000 + f * 20u); layer.tick(); }
    const int early = brightestLight(layer);
    CHECK_MESSAGE(early > 16, "idle rain must light the surface, not leave it black");

    // And the surface keeps ringing rather than settling immediately: a wave that vanished in a frame would satisfy the check above while still looking like nothing.
    for (int f = 40; f < 200; f++) { platform::setTestNowMs(1000 + f * 20u); layer.tick(); }
    CHECK_MESSAGE(brightestLight(layer) > 16, "the water keeps moving while the rain falls");
}

// Ripples a few hundred high cover most of the palette rather than its middle, and each stone moves where the colors start.
TEST_CASE("ripples walk the whole palette, and each stone shifts it") {
    bool seen[256] = {};
    int distinct = 0;
    for (int h = -500; h <= 500; h++) {
        const uint8_t i = mm::BeatRipplesEffect::paletteIndex(static_cast<int16_t>(h), 0);
        if (!seen[i]) { seen[i] = true; distinct++; }
    }
    CHECK(distinct > 100);
    CHECK(mm::BeatRipplesEffect::paletteIndex(0, 1) != mm::BeatRipplesEffect::paletteIndex(0, 0));
}
