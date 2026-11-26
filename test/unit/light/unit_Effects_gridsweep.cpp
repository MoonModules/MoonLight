/// @module EffectBase

/// The grid-size floor, swept across EVERY registered effect rather than one at a time.
///
/// The hard rule (CLAUDE.md § Principles, Robustness; architecture.md § Robustness rules): an effect must produce a correct result at ANY grid size, including a degenerate one, no crash, no divide-by-zero, no out-of-bounds write. A modifier can shrink the logical grid to 0x0x0 (every layout child disabled), and a 1-wide or 1-deep grid is what a single strand or a flat panel actually is.
///
/// Layer::tick owns the degenerate-grid gate and skips the effect pass when any axis is 0, so effects may assume width, height and depth >= 1. The 0x0x0 row pins the layer's clean no-op and zero-byte buffer; the three non-degenerate rows run the effect bodies.
///
/// Do NOT "improve" this by calling fx->tick() directly on the zero grid. That asserts a contract no effect makes, and it fails: GEQ3DEffect divides by width() (imap over a zero range) and takes SIGFPE. The single Layer gate is what makes that safe.
///
/// Per-effect tests cover a new effect only if its author writes that case. This sweep asks the factory for every registered effect, so an effect is swept the moment it lands. A failure names the effect and the grid.


#include "effect_grid_sweep.h"

using namespace mm::sweep;

// One TEST_CASE over every effect keeps failure output readable: a broken effect names itself and its grid. 1 and 2 channels matter because an effect assuming RGB writes past its light or declines to render, and the fixture stays black.
TEST_CASE("every effect renders at any channel count") {
    // The active palette is process-wide and DemoReelEffect reassigns it, which changes what every later effect paints (WaveEffect drew black at cpl=1, where only red is visible); this guard restores it afterwards.
    struct RestorePalette {
        mm::Palette palette = *mm::Palettes::active();
        ~RestorePalette() { mm::Palettes::setActiveDirect(palette); }
    } restore;
    // Select a known palette: at cpl=1 the probe sees only red, and an earlier test's palette decides whether WaveEffect's color has any red (palette 9 has none at its index).
    mm::Palettes::setActive(0);

    int swept = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        for (uint8_t cpl : kChannelCounts) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(cpl);
            // Per effect: DemoReelEffect reassigns the global palette mid-loop, and a red-free color in a red-only buffer makes WaveEffect look blank.
            mm::Palettes::setActive(0);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            const GridCase g{8, 8, 1, "8x8x1"};
            const bool wrote = runEffectOnGrid(effectName, fx, g, cpl);
            // Effects that always paint every pixel must do so at ANY channel count. The rest (audio-reactive, sparse, or input-driven) legitimately render black here, so they are checked for holding together only.
            if (effectName == "SolidEffect" || effectName == "RainbowEffect" ||
                effectName == "WaveEffect"  || effectName == "PlasmaEffect")
                CHECK_MESSAGE(wrote, effectName << " drew nothing at cpl=" << int(cpl));
            delete fx;
            swept++;
        }
    });
    MESSAGE("swept " << swept << " effect x channel-count combinations");
}

TEST_CASE("every effect survives degenerate grid sizes") {
    int swept = 0;

    mm::forEachEffect([&](const char* name, auto make) {
        for (const auto& g : kGrids) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(g.label);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            runEffectOnGrid(effectName, fx, g);
            delete fx;
        }
        swept++;
    });

    // An empty effect list would make this test vacuously green, the most dangerous kind of passing test. The generator refuses to emit an empty list; this is the second lock.
    CHECK_MESSAGE(swept > 0, "no effects swept — the test would pass without testing anything");
    MESSAGE("swept " << swept << " effects x " << (sizeof(kGrids) / sizeof(kGrids[0])) << " grids");
}
