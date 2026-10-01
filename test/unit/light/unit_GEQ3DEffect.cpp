/// @module GEQ3DEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "platform/platform.h"

// A frozen clock and a sweep-simulating AudioService make the bars deterministic; both are restored when a case ends.
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };

// Vacates the process-wide active-mic seat on scope exit (even if an assertion aborts the case), so a failed REQUIRE can't leak AudioService::active_ into a later test.
struct AudioGuard {
    mm::AudioService& mic;
    ~AudioGuard() { mic.release(); }  // clears AudioService::active_ if it is this mic
};

// Silence (no active mic) leaves the buffer all-black, every band magnitude is 0, so no bar rises.
TEST_CASE("GEQ3DEffect renders black on silence") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQ3DEffect geq;
    layer.addChild(&geq);

    layer.applyState();
    // No AudioService is active, so latestFrame() returns static silence (all bands 0).
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 256);
    for (size_t i = 0; i < buf.bytes(); i++) CHECK(buf.data()[i] == 0);
}

// A synthesized sweep frame with only the lowest band lit paints the LEFT of the grid and leaves the far RIGHT dark.
TEST_CASE("GEQ3DEffect draws a bar where the audio band is energised") {
    ClockGuard guard;
    // Sweep mode: pos = (t/250)%16, env = triwave8((t%250)*255/250). At t=125 → pos 0, env ≈ 254, so only bands[0] is high, the leftmost bar rises, the rest stay flat.
    mm::platform::setTestNowMs(125);

    mm::AudioService mic;
    AudioGuard micGuard{mic};  // vacate the active mic on scope exit (even if a REQUIRE aborts)
    mic.mode = mm::AudioService::kSimMode;   // simulate mode
    mic.simulate = 1;     // sweep pattern
    mic.setup();               // claims the process-wide active mic seat
    mic.tick();                // synthesizes the frame at the frozen time
    REQUIRE(mm::AudioService::latestFrame()->bands[0] > 1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQ3DEffect geq;
    layer.addChild(&geq);

    layer.applyState();
    layer.tick();

    auto* data = layer.buffer().data();
    const int w = 16, h = 16;
    auto lit = [&](int x, int y) {
        size_t idx = (static_cast<size_t>(y) * w + x) * 3;
        return data[idx] || data[idx + 1] || data[idx + 2];
    };

    // Band 0 → leftmost bar (one 16/16 = 1-wide column at x=0), a tall front-fill up the left edge.
    bool leftLit = false;
    for (int y = 0; y < h; y++) leftLit |= lit(0, y);
    CHECK(leftLit);

    // The far-right column carries no bar (only band 0 is energised).
    bool rightLit = false;
    for (int y = 0; y < h; y++) rightLit |= lit(w - 1, y);
    CHECK_FALSE(rightLit);
}

// The effect runs at a degenerate 0×0×0 grid without crashing (the "every grid size" hard rule).
TEST_CASE("GEQ3DEffect survives a zero-size grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQ3DEffect geq;
    layer.addChild(&geq);

    layer.applyState();
    layer.tick();   // must not crash on a 0×0×0 grid
    CHECK(true);
}

// A narrow grid with fewer columns than bands still spreads bars (numBands is clamped to the column count, so no divide-by-zero, no bar pile-up at x=0) and never crashes.
TEST_CASE("GEQ3DEffect handles a grid narrower than numBands") {
    ClockGuard guard;
    mm::platform::setTestNowMs(125);   // sweep → bands[0] high

    mm::AudioService mic;
    AudioGuard micGuard{mic};  // vacate the active mic on scope exit (even if a REQUIRE aborts)
    mic.mode = mm::AudioService::kSimMode;   // simulate mode
    mic.simulate = 1;     // sweep pattern
    mic.setup();
    mic.tick();

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;     // fewer columns than the 16 bands
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQ3DEffect geq;
    geq.numBands = 16;
    layer.addChild(&geq);

    layer.applyState();
    layer.tick();       // clamps bands to 4 cols → bar width ≥ 1, no crash

    // Band 0 maps to the leftmost column of a 4-wide grid; something is lit there.
    auto* data = layer.buffer().data();
    bool leftLit = false;
    for (int y = 0; y < 8; y++) {
        size_t idx = (static_cast<size_t>(y) * 4 + 0) * 3;
        leftLit |= data[idx] || data[idx + 1] || data[idx + 2];
    }
    CHECK(leftLit);
}
