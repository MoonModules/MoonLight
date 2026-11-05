/// @module WaterfallEffect
/// @also AudioService

/// The waterfall keeps the music's recent shape in view: the newest bands paint the top row and each step moves the history down one.

#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs
#include "light/layouts/Layouts.h"
#include "light/effects/WaterfallEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"

namespace {

constexpr int W = 31, H = 8;

bool lit(const uint8_t* p) { return p[0] || p[1] || p[2]; }

// The effect on a 31 x 8 grid, fed by simulated music, stepping every millisecond the clock advances.
struct Waterfall {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::AudioService audio;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::WaterfallEffect effect;
    uint32_t now = 1000;

    Waterfall() {
        mm::platform::setTestNowMs(now);
        audio.defineControls();
        audio.simulate = mm::AudioService::kSimMusic;
        audio.setup();
        grid.width = W;
        grid.height = H;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        effect.speed = 206;   // a row every 50 ms, one per step below
        layer.addChild(&effect);
        layer.applyState();
    }
    ~Waterfall() { audio.release(); }

    void step() { now += 50; mm::platform::setTestNowMs(now); audio.tick(); layer.tick(); }
    const uint8_t* px(int x, int y) { return layer.buffer().data() + (static_cast<size_t>(y) * W + x) * 3; }
};

}  // namespace

TEST_CASE("the waterfall paints the newest bands on top and moves the history down one row a step") {
    Waterfall wf;
    wf.step();
    int topLit = 0;
    for (int x = 0; x < W; x++) if (lit(wf.px(x, 0))) topLit++;
    CHECK(topLit >= 16);   // the simulated music lights most of the 31 bands
    uint8_t top[W][3];
    for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) top[x][c] = wf.px(x, 0)[c];
    wf.step();
    for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) CHECK(wf.px(x, 1)[c] == top[x][c]);   // last step's top row, one row down
}
