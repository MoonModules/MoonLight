/// @module GEQEffect
/// @also AudioService

#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs
#include "light/layouts/Layouts.h"
#include "light/effects/GEQEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"

#include <array>

// The 16 bands spread across the columns, each a bar rising from the floor (the last row); a simulating AudioService feeds the bands.

// With no live audio source every band is silent, so no bar rises and the buffer stays black.
TEST_CASE("GEQEffect stays black without an audio frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.ripple = 0;   // no falling peak dot: silence must leave the buffer fully dark
    layer.addChild(&geq);

    layer.applyState();
    // No AudioService is active → latestFrame() is the static all-silence frame (bands all 0). Each loop fades then reads silence → every bar height 0 → nothing drawn.
    for (int i = 0; i < 8; i++) layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 64);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// A loud column lights its floor pixel while the pixel above its bar stays dark: bars fill upward, never floating.
TEST_CASE("GEQEffect fills columns from the floor upward") {
    // The sweep follows the clock, so it is frozen where the sweep is near full level, as the FreqMatrix tests do.
    struct RealClockAfter { ~RealClockAfter() { mm::platform::setTestNowMs(0); } } realClockAfter;
    mm::platform::setTestNowMs(375);
    mm::AudioService audio;
    audio.defineControls();
    audio.simulate = mm::AudioService::kSimSweep;   // sweep: one band lit at a time, so a known column goes loud
    audio.setup();

    const int W = 16, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.ripple = 0;    // disable the peak dot so the only lit pixels are the bar itself
    geq.fadeOut = 255; // fully clear the previous frame so a lit floor pixel is this frame's bar
    layer.addChild(&geq);

    layer.applyState();

    auto floorLit = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(H - 1) * W + x) * 3;   // bottom row of column x
        return d[idx] || d[idx + 1] || d[idx + 2];
    };
    auto topLit = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(0) * W + x) * 3;       // top row of column x
        return d[idx] || d[idx + 1] || d[idx + 2];
    };

    // Run until some floor lights, then check that column: a dark floor means a dark top.
    bool sawBar = false;
    for (int i = 0; i < 64; i++) {
        audio.tick();
        layer.tick();
        for (int x = 0; x < W; x++) {
            if (floorLit(x)) sawBar = true;
            // A lit top pixel with a dark floor would mean the bar didn't grow from the bottom.
            if (topLit(x)) CHECK(floorLit(x));
        }
    }
    CHECK(sawBar);   // at least one column rose during the sweep

    audio.release();
}

// colorBars colors each bar by its column index, so two well-separated lit columns take different hues rather than sharing the row-height gradient, the toggle changes what color a bar is.
TEST_CASE("GEQEffect colorBars colors bars per column") {
    // Virtual time, since the simulated music follows the clock and another test's state must not decide the bands.
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::AudioService audio;
    audio.defineControls();
    audio.simulate = mm::AudioService::kSimMusic;   // music: keeps every band non-zero so many columns rise together
    audio.setup();

    const int W = 16, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.colorBars = true;   // hue per column: imap(x,0,W-1,0,255)
    geq.ripple = 0;
    geq.fadeOut = 255;
    layer.addChild(&geq);

    layer.applyState();
    mm::Palettes::setActive(0);   // Rainbow: index maps to a spread of hues, order-independent

    // Advance until both an early and a late column have a lit floor, then compare their colors.
    const int xa = 0, xb = W / 2;   // the top band can stay silent, so the middle column is the far one
    auto color = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(H - 1) * W + x) * 3;
        return std::array<uint8_t, 3>{d[idx], d[idx + 1], d[idx + 2]};
    };
    auto lit = [](const std::array<uint8_t, 3>& c) { return c[0] || c[1] || c[2]; };

    bool compared = false;
    for (uint32_t i = 0; i < 64 && !compared; i++) {
        mm::platform::setTestNowMs(1000 + i * 25);
        audio.tick();
        layer.tick();
        auto ca = color(xa), cb = color(xb);
        if (lit(ca) && lit(cb)) {
            // Column 0 (hue 0) and column 8 (hue 136) sit far apart on the palette: their bar colors differ, so the color follows the column, not the shared row height.
            CHECK((ca[0] != cb[0] || ca[1] != cb[1] || ca[2] != cb[2]));
            compared = true;
        }
    }
    CHECK(compared);

    audio.release();
}

// The hard rule: the effect runs at any grid size without crashing, including 0×0×0 and 1×1, with a live audio frame feeding it every tick.
TEST_CASE("GEQEffect survives degenerate grid sizes") {
    mm::AudioService audio;
    audio.defineControls();
    audio.simulate = mm::AudioService::kSimMusic;
    audio.setup();

    for (auto dims : {mm::Coord3D{0, 0, 0}, mm::Coord3D{1, 1, 1}}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dims.x;
        grid.height = dims.y;
        grid.depth = dims.z;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::GEQEffect geq;
        layer.addChild(&geq);

        layer.applyState();
        for (int i = 0; i < 4; i++) { audio.tick(); layer.tick(); }
    }
    CHECK(true);   // no crash at 0×0×0 or 1×1

    audio.release();
}
