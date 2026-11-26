#pragma once
/// The rig the effect sweeps share: the grids and channel counts every effect must tolerate, one effect driven through one grid, and a 16x16 layer ready to tick.
///
/// One header so the sweep files split by test case, each compiling its own instantiation of every effect in parallel; one file holding them all was the build's slowest.

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layers/Layer.h"
#include "light/util/Palette.h"
// Generated at build time from src/light/effects/*.h, see test/CMakeLists.txt. Supplies forEachEffect(), so a sweep names no individual effect and cannot drift.
#include "effect_sweep.h"
#include "platform/platform_clock.h"   // setTestNowMs
#include <string>

namespace mm::sweep {

/// A grid every effect must tolerate, each one a configuration a user can produce: the empty grid of a modifier that folded it away, a single light, a strand, a row, tubes and a cube.
struct GridCase {
    mm::lengthType w;    ///< width
    mm::lengthType h;    ///< height
    mm::lengthType d;    ///< depth
    const char* label;   ///< what the grid is, named in a failure
};

/// Channel counts a fixture presents: mono, an oddity, RGB, RGBW and a wide profile slot. An effect renders what the buffer holds, since three bytes written regardless corrupt a narrow buffer or, guarded, leave it black.
inline constexpr uint8_t kChannelCounts[] = {1, 2, 3, 4, 8};

/// The grids the sweeps drive every effect through; the tube rows give the depth axis the others leave at 1, read both ways round, and the cube has all three axes at once.
inline constexpr GridCase kGrids[] = {
    {0, 0, 0, "0x0x0 (empty grid)"},
    {1, 1, 1, "1x1x1 (single light)"},
    {1, 16, 1, "1x16x1 (one strand)"},
    {16, 1, 1, "16x1x1 (one row)"},
    {1, 60, 10, "1x60x10 (ten tubes of 60)"},   // an effect indexing by x + y * width alone lands in the first tube
    {1, 10, 60, "1x10x60 (sixty tubes of 10)"},
    {8, 8, 8, "8x8x8 (a cube)"},
};

/// Drive one effect through one grid, two ticks, since several effects seed on the first tick and read it on the next; answers whether it put light in the buffer.
inline bool runEffectOnGrid(const std::string& name, mm::MoonModule* fx, const GridCase& g, uint8_t cpl = 3) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    grid.width = g.w; grid.height = g.h; grid.depth = g.d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(cpl);

    layer.addChild(fx);
    layer.applyState();
    // A pinned clock: at cpl 1 the probe below sees red alone, and a palette index taken from the wall clock lands on a red-free entry about one run in four.
    mm::platform::setTestNowMs(100000u);
    layer.tick();
    mm::platform::setTestNowMs(100016u);
    layer.tick();
    mm::platform::setTestNowMs(0);

    // A zero-light layer reports zero bytes, a span a driver can read without overrunning it.
    if (g.w == 0 || g.h == 0 || g.d == 0) {
        CHECK_MESSAGE(layer.buffer().bytes() == 0, name << " left a non-empty buffer on " << g.label);
    }

    // Light in the buffer; a write past each light on a narrow buffer looks like output too, which the caller's canary checks.
    bool wrote = false;
    for (size_t i = 0; i < layer.buffer().bytes(); i++)
        if (layer.buffer().data()[i]) { wrote = true; break; }

    // release() frees every buffer in the tree, and the caller deletes the effect; the layer goes out of scope with no detach.
    layer.release();
    return wrote;
}

/// A 16x16 RGB layer running one effect, prepared and ready to tick: the setup the buffer and release sweeps share.
struct OneEffectLayer {
    mm::Layouts layouts;    ///< the layout container
    mm::GridLayout grid;    ///< its 16x16 grid
    mm::Layer layer;        ///< the layer the effect renders into
    /// Wire `effect` into a prepared layer.
    explicit OneEffectLayer(mm::MoonModule* effect) {
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        effect->defineControls();
        layer.addChild(effect);
        layouts.applyState();
        layer.applyState();
    }
};

}  // namespace mm::sweep
