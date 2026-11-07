/// @module FreqMatrixEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/FreqMatrixEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "platform/platform.h"   // setTestNowMs: deterministic virtual time

// FreqMatrixEffect is audio-driven: it paints the new x=0/y=0 pixel from AudioService::latestFrame() (hue from peakHz, brightness from levelSmoothed) and scrolls the column away from y=0 each tick. To pin real behavior the frame is fed through a live AudioService in simulate = kSimSweep (sweep): on desktop (hasI2sMic=false) tick() runs synthesizeFrame, elects the module as the active mic, and fills a DETERMINISTIC frame off platform::millis(). At the frozen time t=375 the sweep is at step pos=1 (peakHz = 80 + 1*700 = 780 Hz, comfortably above the effect's 80 Hz tone gate) and env=triwave8(127)=254 (a near-full level). Driving the mic's tick() repeatedly at that time converges the smoothed level (a /4 EMA of 254) well past the effect's `levelSmoothed > 64` gate, so the painted pixel is guaranteed lit, letting us assert the paint and the scroll rather than "renders non-zero".

namespace {

// Owns the Layouts/GridLayout/Layer wiring every case shares: build a grid of the given dimensions, parent it, and configure the layer's channels. Same struct-Ctx idiom as unit_effects_render.cpp; each case adds its FreqMatrixEffect afterward.
struct GridLayer {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    GridLayer(mm::lengthType w, mm::lengthType h, mm::lengthType d, uint8_t channels) {
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(channels);
    }
};

// Restores real-clock behavior and vacates the process-wide active-mic seat so each case is independent (both are global state a prior case could leave set).
struct AudioGuard {
    mm::AudioService& mic;
    ~AudioGuard() {
        mic.release();                 // clears AudioService::active_ if it is this mic
        mm::platform::setTestNowMs(0);  // restore the platform clock
    }
};

// Frozen time at which the sweep sits on band 1 with a near-full level.
constexpr uint32_t kToneMs = 375;

// One simulated frame at the frozen time, the mic then the active source: the smoothed level rises at once, past the effect's gate.
void driveLoudTone(mm::AudioService& mic) {
    mic.simulate = mm::AudioService::kSimSweep;   // sweep: deterministic single-band sweep
    mm::platform::setTestNowMs(kToneMs);
    mic.tick();
    const mm::AudioFrame* f = mm::AudioService::latestFrame();
    REQUIRE(f->peakHz > 80);          // a real tone, above the effect's gate
    REQUIRE(f->levelSmoothed > 64);   // above the effect's brightness/color gate
}

} // namespace

// A real tone above the 80 Hz gate paints a lit color at the source pixel (0,0).
TEST_CASE("FreqMatrixEffect paints a lit source pixel from a live tone") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveLoudTone(mic);

    GridLayer g(1, 8, 1, 3);

    mm::FreqMatrixEffect fx;
    fx.speed = 255;   // the first tick scrolls one row
    g.layer.addChild(&fx);
    g.layer.applyState();

    g.layer.tick();   // paints the new pixel at y=0 (elapsed = kToneMs, throttle passes)

    auto* data = g.layer.buffer().data();
    // Pixel (0,0) is index 0: the source end carries the freshly-painted lit color.
    CHECK((data[0] > 0 || data[1] > 0 || data[2] > 0));
}

// The column is a shift register: a lit source pixel scrolls to y=1 on the next tick.
TEST_CASE("FreqMatrixEffect scrolls the painted pixel one step along Y") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveLoudTone(mic);

    GridLayer g(1, 8, 1, 3);

    mm::FreqMatrixEffect fx;
    fx.speed = 255;   // 60 rows a second: a row every 16 ms
    g.layer.addChild(&fx);
    g.layer.applyState();

    g.layer.tick();   // tick 1 at kToneMs: paints the lit color at y=0
    auto* data = g.layer.buffer().data();
    const uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    REQUIRE((r0 > 0 || g0 > 0 || b0 > 0));   // something lit landed at the source

    // Advance the clock so the throttle passes again and the sweep still lands a lit band (pos = (t/250)%16 = 1 at t=380, env still high), the column shifts.
    mm::platform::setTestNowMs(kToneMs + 16);
    mic.tick();     // refresh the frame at the new time (still a loud tone on band 1)
    g.layer.tick();   // tick 2: the y=0 color of tick 1 moves up to y=1

    data = g.layer.buffer().data();   // buffer may have been rebuilt; re-read
    // Pixel (0,1) is index 1*3 = 3: it now carries the color painted at y=0 on tick 1.
    CHECK(data[3] == r0);
    CHECK(data[4] == g0);
    CHECK(data[5] == b0);
}

// Silence (no active mic → the static silent frame) paints black: no tone, no light.
TEST_CASE("FreqMatrixEffect paints black on silence") {
    // Ensure no mic holds the active seat, so latestFrame() is the all-zero frame (peakHz 0, levelSmoothed 0), both below the effect's gates.
    { mm::AudioService idle; idle.release(); }
    mm::platform::setTestNowMs(1000);

    GridLayer g(1, 8, 1, 3);

    mm::FreqMatrixEffect fx;
    fx.speed = 255;
    g.layer.addChild(&fx);
    g.layer.applyState();

    REQUIRE(mm::AudioService::latestFrame()->peakHz == 0);   // silence: no tone
    g.layer.tick();

    auto* data = g.layer.buffer().data();
    // The freshly-painted source pixel is black, silence scrolls dark.
    CHECK(data[0] == 0);
    CHECK(data[1] == 0);
    CHECK(data[2] == 0);

    mm::platform::setTestNowMs(0);
}

// The "runs at every grid size" hard rule: degenerate grids never crash.
TEST_CASE("FreqMatrixEffect survives degenerate grid sizes") {
    for (auto dims : {mm::Coord3D{0, 0, 0}, mm::Coord3D{1, 1, 1}}) {
        GridLayer g(dims.x, dims.y, dims.z, 3);

        mm::FreqMatrixEffect fx;
        g.layer.addChild(&fx);
        g.layer.applyState();
        g.layer.tick();   // must not crash on 0×0×0 or 1×1×1
    }
    CHECK(true);
}

// Pitch is logarithmic, so the window spreads over the palette octave by octave: the geometric middle of 50 Hz and 5 kHz, 500 Hz, takes the palette's middle, and a peak past either end takes that end's color.
TEST_CASE("FreqMatrixEffect spreads the pitch window over the whole palette, octave by octave") {
    mm::FreqMatrixEffect fx;
    CHECK(fx.paletteIndex(50) == 0);
    CHECK(fx.paletteIndex(5000) == 255);
    CHECK(fx.paletteIndex(500) > 115);
    CHECK(fx.paletteIndex(500) < 140);
    CHECK(fx.paletteIndex(30) == 0);
    CHECK(fx.paletteIndex(9000) == 255);
    fx.lowHz = 5000; fx.highHz = 50;   // the edges swapped read as the same window
    CHECK(fx.paletteIndex(500) > 115);
}

// The scroll keeps its speed at any frame rate: a frame that comes three periods late scrolls three rows, each painted.
TEST_CASE("FreqMatrixEffect scrolls one row per period elapsed, however few frames come") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveLoudTone(mic);
    GridLayer g(1, 8, 1, 3);
    mm::FreqMatrixEffect fx;
    fx.speed = 255;   // a row every 16 ms
    g.layer.addChild(&fx);
    g.layer.applyState();
    g.layer.tick();
    mm::platform::setTestNowMs(kToneMs + 48);   // three periods later
    g.layer.tick();
    auto* data = g.layer.buffer().data();
    for (int y = 0; y < 3; y++) CHECK((data[y * 3] > 0 || data[y * 3 + 1] > 0 || data[y * 3 + 2] > 0));
}

// A pause longer than the column resumes the scroll by a row, so the column never turns one color at once.
TEST_CASE("FreqMatrixEffect resumes after a long pause by one row") {
    mm::AudioService mic;
    AudioGuard guard{mic};
    driveLoudTone(mic);
    auto litRows = [&] {
        GridLayer g(1, 8, 1, 3);
        mm::FreqMatrixEffect fx;
        g.layer.addChild(&fx);
        g.layer.applyState();
        g.layer.tick();   // the first tick, far past the zero time: a pause longer than the column
        auto* data = g.layer.buffer().data();
        int lit = 0;
        for (int y = 0; y < 8; y++) lit += (data[y * 3] > 0 || data[y * 3 + 1] > 0 || data[y * 3 + 2] > 0);
        return lit;
    };
    CHECK(litRows() == 1);
}
