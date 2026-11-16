#pragma once

/// Pins an effect's exact output, so a refactor claiming it renders the same is proved: @xref{why-it-exists|why}.
///
/// @technical
///
/// ## Why it exists
///
/// A refactor rewrites an effect's internals under the contract that it stays pixel-identical.
///
/// A behavior test, one asserting that something is written or that it varies along an axis, passes as happily when the arithmetic drifted by one unit, the regression such a refactor brings. A hash over the rendered bytes catches it.
///
/// ## Where the determinism comes from
///
/// Three fixed inputs: a fixed grid, a fixed frame count, and a fixed clock.
///
/// The clock matters most, every pinned effect being time-driven and the tick reading the platform's own millisecond source. A test seam makes the sequence reproducible, and a scoped guard restores real time afterwards.
///
/// ## Why a hash and not a stored frame
///
/// Repo health tracks the repository's size, and checking in buffers for dozens of effects would grow it for no diagnostic gain. A mismatch tells the same either way, and the effect is one test case away from being rendered by hand.
///
/// ## What a golden is not
///
/// It is not a statement that the effect looks good: it pins what the code renders, so a refactor claiming to keep the output can be checked.
///
/// When tuning changes an effect deliberately, its golden moves with it, which is the system working. The rule is only that no hash moves silently: an intended divergence is updated in the same commit, with its reason in the message.
///
/// A golden is only as strong as the effect's visible output. Two effects here saturate their field at their defaults, so their hashes cannot detect a phase error, as mutation testing confirmed; those guard the pixel-addressing path alone.
///
/// ## Why the cadence and the frame count are fixed
///
/// 20 ms a frame is the real tick20ms cadence, so the phase accumulators under test see the dt production gives them. A sub-millisecond desktop dt, which rounds to zero in a naive accumulator, cannot mask a bug.
///
/// ## Why the frame count is not a handful
///
/// At a typical default speed the phase advances a few units over eight frames, which on a 16-wide grid stays under a whole pixel, so a short render hashes two near-static frames and passes even when the animation is wrong.
///
/// Mutation testing the harness found this, perturbing an effect's bpm and watching the golden still pass. 200 frames is still a millisecond-scale test.

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace mm::golden {

/// Fix the clock for @xref{where-the-determinism-comes-from|a deterministic render}, and restore real time on scope exit, including on a failed REQUIRE, which unwinds through here.
struct ScopedTestClock {
    /// Fixes the clock at a chosen moment for as long as this lives.
    explicit ScopedTestClock(uint32_t startMs) { platform::setTestNowMs(startMs); }
    /// Releases it, so the real clock runs again.
    ~ScopedTestClock() { platform::setTestNowMs(0); }   // 0 = back to the real clock
};

/// FNV-1a over the buffer, @xref{why-a-hash-and-not-a-stored-frame|rather than the frame}. Any stable hash works; FNV-1a is 4 lines and needs no dependency.
inline uint64_t hashBuffer(const uint8_t* data, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

/// The test fixture an effect renders on: a layer over one grid of `w` by `h` by `d` lights, `cpl` channels each.
struct LayerFixture {
    Layouts layouts;   ///< the layouts the layer maps
    GridLayout grid;   ///< the one grid in them
    Layer layer;       ///< the layer the effect draws on

    /// The grid and the layer, ready for an effect.
    explicit LayerFixture(lengthType w, lengthType h, lengthType d = 1, uint8_t cpl = 3) {
        grid.width = w; grid.height = h; grid.depth = d;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(cpl);
    }

    /// Put `effect` on the layer and apply the state, which prepares it.
    void add(MoonModule& effect) {
        layer.addChild(&effect);
        layer.applyState();
    }

    /// Tick the layer with the clock at `ms`.
    void tickAt(uint32_t ms) {
        platform::setTestNowMs(ms);
        layer.tick();
    }

    /// A copy of the layer's bytes as they are now.
    std::vector<uint8_t> frame() {
        auto& buf = layer.buffer();
        return std::vector<uint8_t>(buf.data(), buf.data() + buf.bytes());
    }
};

/// Render a fixed number of ticks of one effect on a grid, and hash the final buffer: 20 ms apart, @xref{why-the-cadence-and-the-frame-count-are-fixed|the real cadence}, and @xref{why-the-frame-count-is-not-a-handful|200 of them} unless asked.
template <typename EffectT>
uint64_t renderHash(EffectT& effect, lengthType w, lengthType h, lengthType d, uint16_t frames = 200) {
    ScopedTestClock clock(1000);   // start away from 0 so a first-tick guard is exercised
    LayerFixture fx(w, h, d);
    fx.add(effect);
    for (uint16_t f = 0; f < frames; f++) fx.tickAt(1000 + static_cast<uint32_t>(f) * 20);
    auto& buf = fx.layer.buffer();
    REQUIRE(buf.data() != nullptr);
    return hashBuffer(buf.data(), buf.bytes());
}

/// Check a render against its golden, and on mismatch print the value to paste back in, the workflow when a divergence is @xref{what-a-golden-is-not|intended and reviewed}.
inline void checkGolden(const char* name, uint64_t actual, uint64_t expected) {
    if (actual != expected) {
        std::printf("golden mismatch for %s:\n  expected 0x%016llxull\n  actual   0x%016llxull\n"
                    "  (if this change is intended and reviewed, update the golden in the same commit)\n",
                    name, static_cast<unsigned long long>(expected),
                    static_cast<unsigned long long>(actual));
    }
    CHECK(actual == expected);
}

}  // namespace mm::golden
