/// @module AudioSpectrumEffect
/// @also AudioService

/// The sweep colors with the level bar off draw WLED-MM's GEQ PPA: bar `i` over columns `i*w/16` up to `(i+1)*w/16`, one color per bar, rising from the floor, so two boards differ only in their audio.

#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs
#include "light/layouts/Layouts.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"

namespace {

constexpr int W = 20, H = 6;   // not a multiple of 16, so some bars take two columns

bool lit(const uint8_t* p) { return p[0] || p[1] || p[2]; }
bool same(const uint8_t* a, const uint8_t* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

// The effect on a w x h grid, fed by simulated music with hits, every band above zero, the clock at 1000 ms.
struct Spectrum {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::AudioService audio;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::AudioSpectrumEffect effect;
    int w;

    Spectrum(int width, int height, uint8_t colorMode, bool levelBar) : w(width) {
        mm::platform::setTestNowMs(1000);
        audio.defineControls();
        audio.simulate = mm::AudioService::kSimMusic;
        audio.setup();
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        effect.colorMode = colorMode;
        effect.levelBar = levelBar;
        layer.addChild(&effect);
        layer.applyState();
    }
    ~Spectrum() { audio.release(); }

    void run(int frames) { for (int i = 0; i < frames; i++) { audio.tick(); layer.tick(); } }
    const uint8_t* px(int x, int y) { return layer.buffer().data() + (static_cast<size_t>(y) * w + x) * 3; }
};

}  // namespace

TEST_CASE("AudioSpectrumEffect's sweep colors draw GEQ PPA's bars: its column spans, one color per bar, from the floor") {
    Spectrum s(W, H, mm::AudioSpectrumEffect::kColorSweep, false);
    s.run(8);

    int risen = 0;
    for (int band = 0; band < 16; band++) {
        const int x0 = band * W / 16, x1 = (band + 1) * W / 16;
        const uint8_t* floor = s.px(x0, H - 1);
        if (!lit(floor)) continue;   // a band too quiet for one row of six
        risen++;
        for (int x = x0; x < x1; x++)
            for (int y = 0; y < H; y++) {
                const uint8_t* p = s.px(x, y);
                if (lit(p)) CHECK(same(p, floor));   // one color per bar
                if (y + 1 < H && lit(p)) CHECK(lit(s.px(x, y + 1)));   // no gap below a lit pixel
            }
    }
    CHECK(risen >= 8);   // the level bar is off, so the floor row is the bars' own
    CHECK(same(s.px(3, H - 1), s.px(4, H - 1)));   // 3*20/16 = 3 up to 4*20/16 = 5: one bar over two columns
    CHECK_FALSE(same(s.px(0, H - 1), s.px(15, H - 1)));   // the sweep spreads the bars over the color wheel
}

TEST_CASE("AudioSpectrumEffect's level bar is as tall as a bar is wide, and never more than the spectrum keeps") {
    constexpr int GW = 32, GH = 8;   // two columns a bar, so a level bar of two rows
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, true);   // bars never pure green, so a green pixel is the meter's
    s.run(8);

    // The meter's first pixel is pure green, its gradient's start, on exactly the bottom two rows.
    auto meter = [&](int y) { const uint8_t* p = s.px(0, y); return p[0] == 0 && p[1] == 255 && p[2] == 0; };
    CHECK(meter(GH - 1));
    CHECK(meter(GH - 2));
    CHECK_FALSE(meter(GH - 3));
}

TEST_CASE("AudioSpectrumEffect's square at the meter's right end flashes red on a detected hit") {
    constexpr int GW = 32, GH = 8;
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorPerBand, true);

    // One audio block at a time, until the detector reports a hit.
    bool hit = false;
    for (uint32_t t = 1000; t < 11000 && !hit; t += 23) {
        mm::platform::setTestNowMs(t);
        s.audio.tick();
        hit = mm::AudioService::latestFrame()->onset != 0;
    }
    REQUIRE(hit);
    s.layer.tick();

    auto red = [&](int x, int y) { const uint8_t* p = s.px(x, y); return p[0] == 255 && p[1] == 0 && p[2] == 0; };
    // The meter is two rows tall at 32 wide, so the square is the last two columns of those rows.
    CHECK(red(GW - 1, GH - 1));
    CHECK(red(GW - 2, GH - 2));
    CHECK_FALSE(red(GW - 3, GH - 1));   // the meter itself stops short of the square
}
