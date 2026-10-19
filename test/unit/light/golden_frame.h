#pragma once

/// Pins an effect's exact output, so a refactor claiming it renders the same is proved.
///
/// @technical
///
/// ## Why it exists
///
/// A refactor rewrites an effect's internals under the contract that it stays pixel-identical.
/// A behavior test, one asserting that something is written or that it varies along an axis, passes just as happily when the arithmetic drifted by one unit.
/// That is exactly the regression such a refactor introduces.
/// A hash over the rendered bytes catches it.
///
/// ## Where the determinism comes from
///
/// Three fixed inputs: a fixed grid, a fixed frame count, and a fixed clock.
/// The clock matters most, every pinned effect being time-driven and the tick reading the platform's own millisecond source.
/// A test seam makes the sequence reproducible, and a scoped guard restores real time afterwards.
///
/// ## Why a hash and not a stored frame
///
/// Repo health tracks the repository's size, and checking in buffers for dozens of effects would grow it for no diagnostic gain.
/// A mismatch tells you the same thing either way, and the effect is one test case away from being rendered by hand.
///
/// ## What a golden is not
///
/// It is not a statement that the effect looks good.
/// It pins what the code renders today, so a refactor claiming to change nothing can be checked.
/// Several effects await a tuning pass, and when tuning changes one deliberately the golden moves with it, which is the system working. The rule is only that no hash moves silently, so an intentional divergence is updated in the same commit with its reason in the message.
///
/// A golden is also only as strong as the effect's visible output.
/// Two effects here saturate their field at their default settings, so their frames barely vary and their hashes cannot detect a phase error, which mutation-testing confirmed.
/// Those goldens guard the pixel-addressing path and nothing more.
///
/// ## Why the cadence and the frame count are fixed
///
/// 20 ms/frame is deliberate: it is the real tick20ms cadence, so the phase accumulators under test see the same dt production gives them.
/// A sub-millisecond desktop dt (which rounds to zero in a naive accumulator) cannot mask a bug.
///
/// ## Why the frame count is not a handful
///
/// At a typical default speed the phase advances only a few units over eight frames.
/// On a 16-wide grid moves nothing by a whole pixel, so a short render hashes two nearly-static frames and passes even when the animation is wrong.
/// This was found by mutation-testing the harness itself (perturbing an effect's bpm and watching the golden still pass). 200 frames is still a millisecond-scale test.

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>

namespace mm::golden {

/// Fix the clock for a deterministic render, and restore real time on scope exit, including on a failed REQUIRE, which unwinds through here.
struct ScopedTestClock {
    /// Fixes the clock at a chosen moment for as long as this lives.
    explicit ScopedTestClock(uint32_t startMs) { platform::setTestNowMs(startMs); }
    /// Releases it, so the real clock runs again.
    ~ScopedTestClock() { platform::setTestNowMs(0); }   // 0 = back to the real clock
};

/// FNV-1a over the buffer. Any stable hash works; FNV-1a is 4 lines and needs no dependency.
inline uint64_t hashBuffer(const uint8_t* data, size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

/// Render a fixed number of ticks of one effect on a grid, and hash the final buffer.
template <typename EffectT>
uint64_t renderHash(EffectT& effect, lengthType w, lengthType h, lengthType d, uint16_t frames = 200) {
    ScopedTestClock clock(1000);   // start away from 0 so a first-tick guard is exercised

    Layouts layouts;
    GridLayout grid;
    Layer layer;
    grid.width = w; grid.height = h; grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    layer.applyState();

    for (uint16_t f = 0; f < frames; f++) {
        platform::setTestNowMs(1000 + static_cast<uint32_t>(f) * 20);
        layer.tick();
    }
    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    return hashBuffer(buf.data(), buf.bytes());
}

/// Check a render against its golden, and on mismatch print the value to paste back in, the workflow when a divergence is intentional and reviewed.
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
