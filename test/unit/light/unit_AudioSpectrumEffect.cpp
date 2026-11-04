/// @module AudioSpectrumEffect
/// @also AudioService

/// The sweep colors with the readouts off draw WLED-MM's GEQ PPA: bar `i` over columns `i*w/16` up to `(i+1)*w/16`, one color per bar, rising from the floor, so two boards differ only in their audio.

#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs
#include "light/layouts/Layouts.h"
#include "light/effects/AudioSpectrumEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"
#include "../core/conditional_controls.h"   // mm::test::controlIndex

namespace {

constexpr int W = 20, H = 6;   // not a multiple of 16, so some bars take two columns

bool lit(const uint8_t* p) { return p[0] || p[1] || p[2]; }
bool same(const uint8_t* a, const uint8_t* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }
bool is(const uint8_t* p, mm::RGB c) { return p[0] == c.r && p[1] == c.g && p[2] == c.b; }
mm::RGB palette(uint8_t index) { return mm::colorFromPalette(*mm::Palettes::active(), index, 255); }

// The effect on a w x h grid, fed by simulated music with hits, every band above zero, the clock at 1000 ms.
struct Spectrum {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::AudioService audio;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::AudioSpectrumEffect effect;
    int w;

    Spectrum(int width, int height, uint8_t colorMode, bool readouts, uint8_t bands = mm::AudioSpectrumEffect::kBands16) : w(width) {
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
        effect.readouts = readouts;
        effect.bands = bands;
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
    CHECK(risen >= 8);   // the readouts are off, so the floor row is the bars' own
    CHECK(same(s.px(3, H - 1), s.px(4, H - 1)));   // 3*20/16 = 3 up to 4*20/16 = 5: one bar over two columns
    CHECK_FALSE(same(s.px(0, H - 1), s.px(15, H - 1)));   // the sweep spreads the bars over the palette
}

TEST_CASE("AudioSpectrumEffect's level meter is as tall as a bar is wide, and never more than the spectrum keeps") {
    constexpr int GW = 32, GH = 8;   // two columns a bar, so a meter of two rows
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, true);   // sweep bars, so the palette's first color is the meter's
    s.run(8);

    // The meter's first pixel is the palette's start, on exactly the bottom two rows.
    auto meter = [&](int y) { return is(s.px(0, y), palette(0)); };
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

// The 31 third octaves take the width when the frame carries them: of 31 bars on 62 columns each takes two, and the simulated music lights most.
TEST_CASE("AudioSpectrumEffect draws the 31 third octaves, two columns a bar on a 62-wide grid") {
    constexpr int GW = 62, GH = 6;
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, false, mm::AudioSpectrumEffect::kBandsAll);
    s.run(8);
    int risen = 0;
    for (int band = 0; band < 31; band++) {
        const int x0 = band * GW / 31;
        CHECK(same(s.px(x0, GH - 1), s.px(x0 + 1, GH - 1)));   // one bar over its two columns
        if (lit(s.px(x0, GH - 1))) risen++;
    }
    CHECK(risen >= 16);
}

// The level meter is as tall as a bar is wide at 31 bars too: on 62 columns a bar is two wide, so the meter takes two rows.
TEST_CASE("AudioSpectrumEffect's level meter follows the bar width at 31 bands") {
    constexpr int GW = 62, GH = 8;
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, true, mm::AudioSpectrumEffect::kBandsAll);
    s.run(8);
    auto meter = [&](int y) { return is(s.px(0, y), palette(0)); };   // the meter starts on the palette's first color
    CHECK(meter(GH - 1));
    CHECK(meter(GH - 2));
    CHECK_FALSE(meter(GH - 3));   // three rows would be a sixteen-band bar's width on 62 columns
}

// The gate marked at the right edge, with the readouts: where a band must reach to count as sound, white while open and red while the room is gated, so a dark rig reads as "gated" rather than as a dead input.
TEST_CASE("AudioSpectrumEffect marks the gate a bar wide at its right edge, white while open and red while gated") {
    constexpr int GW = 32, GH = 10;   // a two-row meter under eight rows of spectrum
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, true);
    auto draw = [&](uint8_t gate, bool open) {
        s.audio.tick();
        s.audio.frameForTest().gate = gate;
        s.audio.frameForTest().gateOpen = open;
        s.layer.tick();
    };
    auto is = [&](int y, uint8_t r, uint8_t g, uint8_t b) {   // the mark: the right bar's two columns, and not the one beside them
        for (int x = GW - 2; x < GW; x++) { const uint8_t* p = s.px(x, y); if (p[0] != r || p[1] != g || p[2] != b) return false; }
        const uint8_t* beside = s.px(GW - 3, y);
        return !(beside[0] == r && beside[1] == g && beside[2] == b);
    };
    draw(128, true);              // halfway up the eight spectrum rows: four rows over the meter
    CHECK(is(GH - 2 - 4, 255, 255, 255));
    draw(128, false);
    CHECK(is(GH - 2 - 4, 255, 0, 0));
    draw(1, false);               // a gate at the bottom still shows, on the spectrum's lowest row
    CHECK(is(GH - 2 - 1, 255, 0, 0));
}

TEST_CASE("AudioSpectrumEffect draws no gate for a frame no dynamics measured, or with the readouts off") {
    constexpr int GW = 32, GH = 10;
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorSweep, true);
    s.run(8);                     // simulated audio: no gate in the frame
    for (int y = 0; y < GH; y++) { const uint8_t* p = s.px(0, y); CHECK_FALSE((p[0] == 255 && p[1] == 255 && p[2] == 255)); }
    Spectrum off(GW, GH, mm::AudioSpectrumEffect::kColorSweep, false);
    off.audio.tick();
    off.audio.frameForTest().gate = 128;
    off.audio.frameForTest().gateOpen = false;
    off.layer.tick();
    int red = 0;
    for (int y = 0; y < GH; y++) { const uint8_t* p = off.px(GW - 1, y); if (p[0] == 255 && !p[1] && !p[2]) red++; }
    CHECK(red == 0);              // no mark: the instrument comparing with GEQ PPA stays clean
}

// Every mode colors from the selected palette, so a palette that follows the music, or the show's own, reaches the spectrum too.
TEST_CASE("AudioSpectrumEffect's height mode climbs the selected palette, its first color at the bottom") {
    Spectrum s(W, H, mm::AudioSpectrumEffect::kColorHeight, false);
    s.run(8);
    int bottoms = 0;
    for (int x = 0; x < W; x++) {
        if (!lit(s.px(x, H - 1))) continue;
        CHECK(is(s.px(x, H - 1), palette(0)));
        bottoms++;
        for (int y = 0; y < H; y++)
            if (lit(s.px(x, y))) CHECK(is(s.px(x, y), palette(static_cast<uint8_t>((H - 1 - y) * 255 / (H - 1)))));
    }
    CHECK(bottoms >= 8);
}

TEST_CASE("AudioSpectrumEffect's sweep draws each bar in a color of the selected palette") {
    Spectrum s(W, H, mm::AudioSpectrumEffect::kColorSweep, false);
    s.run(8);
    int found = 0;
    for (int band = 0; band < 16; band++) {
        const uint8_t* p = s.px(band * W / 16, H - 1);
        if (!lit(p)) continue;
        bool inPalette = false;
        for (int i = 0; i < 256 && !inPalette; i++) inPalette = is(p, palette(static_cast<uint8_t>(i)));
        CHECK(inPalette);
        found++;
    }
    CHECK(found >= 8);
}

// The dominant frequency is part of what the frame gives an effect, so the instrument draws it: a white mark a bar wide along the top, at the same place on the log axis the bars use.
TEST_CASE("AudioSpectrumEffect marks the dominant frequency a bar wide along its top, with the readouts") {
    constexpr int GW = 32, GH = 8;
    Spectrum s(GW, GH, mm::AudioSpectrumEffect::kColorPerBand, true);
    auto column = [&](uint16_t hz) {
        s.audio.tick();
        s.audio.frameForTest().peakHz = hz;
        s.layer.tick();
        for (int x = 0; x < GW; x++) if (is(s.px(x, 0), mm::RGB{255, 255, 255})) return x;
        return -1;
    };
    const int at1k = column(1000);
    CHECK(at1k == 9 * GW / 16);         // 1 kHz is the tenth of the sixteen: the mark covers its two columns
    CHECK(is(s.px(at1k + 1, 0), mm::RGB{255, 255, 255}));
    CHECK_FALSE(is(s.px(at1k + 2, 0), mm::RGB{255, 255, 255}));
    CHECK(column(125) < at1k);           // lower is to the left
    CHECK(column(6300) > at1k);
    CHECK(column(0) == -1);              // no peak, no column
    s.effect.readouts = false;
    CHECK(column(1000) == -1);           // the readouts off: the bars alone, as GEQ PPA draws them
}

TEST_CASE("AudioSpectrumEffect shows its speed only for the sweep") {
    Spectrum s(W, H, mm::AudioSpectrumEffect::kColorSweep, false);
    s.effect.rebuildControls();
    const int i = mm::test::controlIndex(s.effect, "speed");
    REQUIRE(i >= 0);
    CHECK_FALSE(s.effect.controls()[static_cast<uint8_t>(i)].hidden);
    s.effect.colorMode = mm::AudioSpectrumEffect::kColorHeight;
    s.effect.rebuildControls();
    CHECK(s.effect.controls()[static_cast<uint8_t>(mm::test::controlIndex(s.effect, "speed"))].hidden);
}
