/// @module BlurzEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/BlurzEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "platform/platform.h"   // setTestNowMs: the simulated music follows the clock

// Blurz places one dot a frame, colored by a band, and blurs the strip; a simulating AudioService feeds the bands, and each case releases it.

// With no live audio source the buffer stays black: the dot is audio-gated, so silence renders nothing.
TEST_CASE("BlurzEffect stays black without an audio frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    layer.addChild(&blurz);

    layer.applyState();
    // No AudioService is active → latestFrame() is the static all-silence frame (bands all 0). tick() does the first-frame clear + fade, then reads silence and returns before drawing any dot.
    for (int i = 0; i < 8; i++) layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 64);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// With a synthesized audio frame the effect lights the buffer: the colored dot appears and the blur smears it into a soft blob, so at least some lights become non-zero.
TEST_CASE("BlurzEffect lights the buffer when fed a signal") {
    mm::AudioService audio;
    audio.defineControls();
    audio.simulate = mm::AudioService::kSimMusic;   // music: synthesizeFrame() fills the bands every loop, no mic needed
    audio.setup();        // claims the active-mic seat so latestFrame() points at this frame

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    layer.addChild(&blurz);

    layer.applyState();

    // Advance the audio and the effect together a few frames: the band cursor wraps 0..15, so over several ticks the dot lands a non-zero magnitude and paints.
    bool anyLit = false;
    for (int i = 0; i < 32 && !anyLit; i++) {
        audio.tick();
        layer.tick();
        auto& buf = layer.buffer();
        for (size_t j = 0; j < buf.bytes(); j++) {
            if (buf.data()[j] != 0) { anyLit = true; break; }
        }
    }
    CHECK(anyLit);

    audio.release();   // release the active-mic seat; leave no residue for other tests
}

// geqScanner sweeps the dot steadily across the strip: one pixel per frame, so consecutive frames land the lit dot at different linear positions rather than the same spot.
TEST_CASE("BlurzEffect geqScanner sweeps the dot to a new position each frame") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::AudioService audio;
    audio.defineControls();
    audio.simulate = mm::AudioService::kSimMusic;   // music: keeps the bands non-zero so the dot has color
    audio.setup();

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 1;      // a strip so the scan position maps directly to the x index
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    blurz.geqScanner = true;   // steady sweep instead of a random jump
    blurz.blur = 1;            // minimal blur so the brightest pixel stays close to the dot core
    blurz.fadeRate = 255;      // fade the previous frame's trail fully, so the current dot dominates
    layer.addChild(&blurz);

    layer.applyState();

    // The brightest pixel, or -1 on a black frame, which a silent band leaves.
    auto brightestIndex = [&]() {
        auto& buf = layer.buffer();
        auto* d = buf.data();
        int best = -1, bestSum = 0;
        for (size_t p = 0; p < buf.count(); p++) {
            int sum = d[p * 3] + d[p * 3 + 1] + d[p * 3 + 2];
            if (sum > bestSum) { bestSum = sum; best = static_cast<int>(p); }
        }
        return best;
    };

    // The scanner advances one pixel per frame, so over a few lit frames the dot shows up in more than one place.
    int first = -1;
    bool moved = false;
    for (uint32_t i = 0; i < 8 && !moved; i++) {
        mm::platform::setTestNowMs(1000 + i * 25);
        audio.tick(); layer.tick();
        const int pos = brightestIndex();
        if (pos < 0) continue;
        if (first < 0) first = pos;
        else if (pos != first) moved = true;
    }
    CHECK(moved);

    audio.release();
}

// The hard rule: the effect runs at any grid size without crashing, including a 0×0×0 and a 1×1 grid.
TEST_CASE("BlurzEffect survives degenerate grid sizes") {
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

        mm::BlurzEffect blurz;
        layer.addChild(&blurz);

        layer.applyState();
        for (int i = 0; i < 4; i++) { audio.tick(); layer.tick(); }
    }
    CHECK(true);   // no crash at 0×0×0 or 1×1

    audio.release();
}
