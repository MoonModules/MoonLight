/// @module PaintBrushEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

// Lines oscillate with a length scaled by their band, so silence draws nothing; a frozen clock makes the frame and the oscillators deterministic.

namespace {

// Restores real-clock behavior and vacates the process-wide active-mic seat so each case is independent (both are global state a prior case could leave set).
struct AudioGuard {
    mm::AudioService& mic;
    ~AudioGuard() {
        mic.release();                 // clears AudioService::active_ if it is this mic
        mm::platform::setTestNowMs(0);  // restore the platform clock
    }
};

// Bring the mic up in "music (always)" at a frozen time so every band carries a magnitude and the synthesized frame is deterministic; several loops let the levelSmoothed EMA settle.
void driveMusic(mm::AudioService& mic, uint32_t ms) {
    mic.defineControls();
    mic.simulate = 3;   // music, always: keeps every band non-zero (loud, broadband)
    mm::platform::setTestNowMs(ms);
    mic.setup();
    for (int i = 0; i < 8; i++) mic.tick();   // fill the frame off the frozen clock
}

} // namespace

// A live broadband signal draws strokes: at least some lights are lit after a frame.
TEST_CASE("PaintBrushEffect draws lit strokes from a live audio frame") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveMusic(mic, 500);
    // Sanity: the synthesized frame actually carries band energy the effect can react to.
    const mm::AudioFrame* f = mm::AudioService::latestFrame();
    bool anyBand = false;
    for (uint8_t b = 0; b < 16; b++) if (f->bands[b] > 0) anyBand = true;
    REQUIRE(anyBand);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PaintBrushEffect fx;
    fx.minLength = 0;   // no length gate: any stroke with band energy draws
    layer.addChild(&fx);
    layer.applyState();

    // Several frames of virtual time: at the first, every oscillator reads the same phase and each line has no length.
    for (uint32_t i = 1; i <= 10; i++) {
        mm::platform::setTestNowMs(500 + i * 100);
        layer.tick();
    }

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 256);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK(anyLit);
}

// Silence draws nothing: with no active mic the frame is all-zero bands, so every line's length maps to 0 (below the minLength gate) and the buffer stays fully black.
TEST_CASE("PaintBrushEffect stays black on silence") {
    // Ensure no mic holds the active seat, so latestFrame() is the static all-zero frame.
    { mm::AudioService idle; idle.release(); }
    mm::platform::setTestNowMs(1000);
    REQUIRE(mm::AudioService::latestFrame()->bands[0] == 0);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PaintBrushEffect fx;
    layer.addChild(&fx);
    layer.applyState();

    // Run several frames: the per-frame fade only decays what's there, and silence never adds a stroke, so the field never lights.
    for (int i = 0; i < 8; i++) layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 256);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// Raised to its maximum, minLength suppresses every stroke, so even a loud frame leaves the buffer black.
TEST_CASE("PaintBrushEffect minLength gate suppresses all strokes") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveMusic(mic, 500);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PaintBrushEffect fx;
    fx.minLength = 255;   // length is a 0..255 fraction; "> 255" is never true → no line draws
    layer.addChild(&fx);
    layer.applyState();

    layer.tick();

    auto& buf = layer.buffer();
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// The "runs at every grid size" hard rule: degenerate grids never crash, with a live frame each tick.
TEST_CASE("PaintBrushEffect survives degenerate grid sizes") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveMusic(mic, 500);

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

        mm::PaintBrushEffect fx;
        layer.addChild(&fx);
        layer.applyState();
        for (int i = 0; i < 4; i++) { mic.tick(); layer.tick(); }   // must not crash on 0×0×0 or 1×1×1
    }
    CHECK(true);
}
