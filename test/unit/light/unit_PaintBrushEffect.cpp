/// @module PaintBrushEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time
#include "audio_effect_test.h"   // EffectOnGrid, seat, play

#include <cstdlib>

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

// Bring the mic up in "music" at a frozen time so every band carries a magnitude and the synthesized frame is deterministic; several loops let the levelSmoothed EMA settle.
void driveMusic(mm::AudioService& mic, uint32_t ms) {
    mic.defineControls();
    mic.simulate = mm::AudioService::kSimMusic;   // music: keeps every band non-zero (loud, broadband)
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

// Raised to its maximum, minLength suppresses every stroke on a grid whose diagonal is shorter, so even a loud frame leaves the buffer black.
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
    fx.minLength = 31;    // a 16 x 16 grid's longest span is its diagonal, 21
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

// With `glide`, the endpoints are an accumulated phase, so ten minutes in a bass change speeds the sweep from where it is, where elapsed time times the rate jumps them ten whole cycles.
using namespace mm;

TEST_CASE("PaintBrushEffect: with glide, a bass change at ten minutes' uptime moves the strokes on from where they are") {
    golden::ScopedTestClock clock(600000);
    AudioService& audio = seat();
    EffectOnGrid<PaintBrushEffect> p(64, 64);
    p.e.glide = true;
    AudioFrame quiet;
    quiet.bandsSmoothed[0] = 0;
    play(p, audio, quiet, 600000);
    play(p, audio, quiet, 600020);
    const int before = p.e.endpointForTest(0, 100, 64);
    AudioFrame bass = quiet;
    bass.bandsSmoothed[0] = 240;   // the bass adds 15 cycles a minute to every oscillator
    play(p, audio, bass, 600021);
    const int after = p.e.endpointForTest(0, 100, 64);
    CHECK(std::abs(after - before) <= 1);   // a millisecond's sweep, not a jump across the grid
    audio.release();
}

// Without `glide`, the original's scatter: a bass step throws the endpoints to new places, which the fading field fills with crossed strokes.
TEST_CASE("PaintBrushEffect: without glide, a bass step ten minutes in throws the strokes elsewhere") {
    golden::ScopedTestClock clock(615000);
    AudioService& audio = seat();
    EffectOnGrid<PaintBrushEffect> p(64, 64);
    AudioFrame quiet;
    play(p, audio, quiet, 615000);
    const int before = p.e.endpointForTest(0, 0, 64);
    AudioFrame bass = quiet;
    bass.bands[0] = 16;   // the raw bass adds a cycle a minute to every endpoint
    play(p, audio, bass, 615001);
    const int after = p.e.endpointForTest(0, 0, 64);
    CHECK(std::abs(after - before) > 16);   // 10.25 turns at 10.25 minutes: a quarter turn on, from the middle to an edge
    audio.release();
}

// With `surge` on, each drum hit stretches the strokes to their span, so the brush answers the drums; off, the default, a hit changes nothing, as in troyhacks's original.
TEST_CASE("PaintBrushEffect: with surge on, a drum hit stretches the strokes, and off it does not") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto litAfter = [&](uint8_t onset, bool surge) {
        EffectOnGrid<PaintBrushEffect> p(32, 32);
        p.e.surge = surge;
        AudioFrame f;
        for (uint8_t b = 0; b < 16; b++) f.bands[b] = f.bandsSmoothed[b] = 40;   // quiet bands: short strokes
        play(p, audio, f, 1000);
        f.onset = onset;
        play(p, audio, f, 1500);
        int lit = 0;
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) lit += p.at(x, y) > 0;
        return lit;
    };
    CHECK(litAfter(200, true) > litAfter(0, true));
    CHECK(litAfter(200, false) == litAfter(0, false));
    audio.release();
}

// The original draws its strokes anti-aliased by default: on a flat grid Wu's line lights the two cells straddling each step of the path, where the crisp line lights one.
TEST_CASE("PaintBrushEffect: anti-aliased strokes light more cells than crisp ones") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto litCells = [&](bool aa) {
        EffectOnGrid<PaintBrushEffect> p(128, 128);
        p.e.antiAlias = aa;
        AudioFrame f;
        for (uint8_t b = 0; b < 16; b++) f.bands[b] = f.bandsSmoothed[b] = static_cast<uint8_t>(120 + b * 8);   // distinct bands: distinct strokes
        play(p, audio, f, 1000);
        play(p, audio, f, 3000);   // the endpoints apart
        int lit = 0;
        for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) lit += p.at(x, y) > 0;
        return lit;
    };
    const int aaLit = litCells(true), crispLit = litCells(false);
    INFO("aa " << aaLit << " crisp " << crispLit);
    CHECK(aaLit > crispLit);
    audio.release();
}

// The surge is read from the time since the hit, so no tick rounds it down: it settles alike at 60 fps and at 1000 fps (review, 2026-11-07).
TEST_CASE("PaintBrushEffect's surge settles over the same time at 60 fps and at 1000 fps") {
    auto leftAfter120ms = [](uint32_t tickMs) {
        golden::ScopedTestClock clock(1000);
        AudioService& audio = seat();
        EffectOnGrid<PaintBrushEffect> p(16, 16);
        p.e.surge = true;
        AudioFrame hit;
        hit.onset = 200;
        play(p, audio, hit, 1000);   // the hit, then frames without one
        tickFor(p, 1000, 120, tickMs);
        const int left = p.e.surgeForTest();
        audio.release();
        return left;
    };
    const int slow = leftAfter120ms(17), fast = leftAfter120ms(1);
    CHECK(fast > 110);
    CHECK(fast < 145);   // half of 255 after one half-life
    CHECK(std::abs(slow - fast) <= 2);
}
