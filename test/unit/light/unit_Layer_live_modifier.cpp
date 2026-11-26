/// @module Layer
/// @also RotateModifier, ModifierBase

#include "doctest.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/modifiers/RotateModifier.h"
#include "light/modifiers/RandomMapModifier.h"
#include "light/effects/EffectBase.h"
#include "platform/platform_clock.h"   // setTestNowMs
#include "golden_frame.h"   // LayerFixture, the shared grid and layer

#include <vector>
#include <utility>
#include <cstring>

// The live modifier seam: a live modifier remaps the rendered buffer every frame with no rebuild, and a Layer with static modifiers alone skips that pass.

namespace {

// A fixed gradient (R = x, G = y), the same every frame, so a rotation visibly rearranges the bytes and any change between frames is the live pass.
class GradientEffect : public mm::EffectBase {
public:
    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        if (!buf) return;
        const mm::lengthType w = width(), h = height();
        const uint8_t cpl = channelsPerLight();
        for (mm::lengthType y = 0; y < h; y++)
            for (mm::lengthType x = 0; x < w; x++) {
                uint8_t* p = buf + (static_cast<size_t>(y) * w + x) * cpl;
                p[0] = static_cast<uint8_t>(x);
                if (cpl > 1) p[1] = static_cast<uint8_t>(y);
                if (cpl > 2) p[2] = 0;
            }
    }
};

// Snapshot the Layer's buffer after one tick() at virtual time `t`.
std::vector<uint8_t> frameAt(mm::Layer& layer, uint32_t t) {
    mm::platform::setTestNowMs(t);
    layer.tick();
    const mm::Buffer& b = layer.buffer();
    std::vector<uint8_t> out(b.bytes());
    if (b.data()) std::memcpy(out.data(), b.data(), b.bytes());
    return out;
}

// A w x h grid showing the gradient through a RotateModifier turning at `speed`, on the suite's shared layer fixture.
struct RotatingGradient {
    mm::golden::LayerFixture fx;
    mm::Layer& layer = fx.layer;
    mm::RotateModifier& rot = fx.own<mm::RotateModifier>();
    RotatingGradient(mm::lengthType w, mm::lengthType h, uint8_t speed) : fx(w, h) {
        fx.add(fx.own<GradientEffect>());
        rot.speed = speed;
        fx.add(rot);
    }
};

} // namespace

// With a Rotate present, the live pass rotates the gradient each frame as the angle advances, so two frames at different times differ. A static GradientEffect alone would produce identical frames, so any difference is the live remap.
TEST_CASE("Layer live pass: Rotate remaps the buffer per frame") {
    RotatingGradient r(16, 16, 200);   // fast enough to cross several angle steps
    mm::Layer& layer = r.layer;

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 1000);   // same time → same angle → identical frame
    auto f2 = frameAt(layer, 1300);   // later → angle advanced → rotated frame

    CHECK(f0 == f1);                   // deterministic at a fixed clock
    CHECK(f0 != f2);                   // the live pass rotated the gradient
    mm::platform::setTestNowMs(0);
}

// PAY-FOR-WHAT-YOU-USE: a Layer with no live modifier must NOT run the live pass, the static gradient is byte-identical across frames regardless of the clock.
TEST_CASE("Layer live pass: skipped when no modifier is live") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);   // no modifiers at all
    layouts.applyState();
    layer.applyState();

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 5000);   // far later — but nothing animates the buffer
    CHECK(f0 == f1);                  // no live pass perturbed it
    mm::platform::setTestNowMs(0);
}

// A DISABLED Rotate must not run the live pass either (the gate keys off ENABLED live modifiers). Same static gradient → identical frames.
TEST_CASE("Layer live pass: a disabled live modifier does not run") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);
    mm::RotateModifier rot; rot.speed = 200; rot.setEnabled(false);
    layer.addChild(&rot);
    layouts.applyState();
    layer.applyState();

    auto f0 = frameAt(layer, 1000);
    auto f1 = frameAt(layer, 1300);
    CHECK(f0 == f1);                  // disabled → no remap
    mm::platform::setTestNowMs(0);
}

// COALESCED REBUILD: two beat-driven modifiers (RandomMap) on one Layer both ask for a rebuild on a beat; the Layer keeps ONE pending rebuild, which the next frame applies before it renders, and stays valid.
TEST_CASE("a layer with no container applies a requested palette at its next tick") {
    mm::golden::LayerFixture fx(8, 8);
    fx.add(fx.own<GradientEffect>());
    fx.layouts.applyState();
    fx.layer.applyState();
    mm::Palettes::request(5);
    fx.layer.tick();
    const mm::Palette five = mm::Palettes::fromBuiltin(5);
    CHECK(std::memcmp(mm::Palettes::active()->entry, five.entry, sizeof(five.entry)) == 0);
}

TEST_CASE("Layer coalesces rebuilds from two dynamic modifiers") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8; grid.height = 8; grid.depth = 1;
    layouts.addChild(&grid);
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    GradientEffect fx; layer.addChild(&fx);
    mm::RandomMapModifier a; a.bpm = 60;   // both reshuffle on a ~1 Hz beat
    mm::RandomMapModifier b; b.bpm = 60;
    layer.addChild(&a);
    layer.addChild(&b);
    layouts.applyState();
    layer.applyState();

    REQUIRE(layer.lut().logicalCount() == 64);

    // Snapshot the full mapping: each logical cell's single destination (a permutation, so exactly one each). Asserts the composition is a bijection AND lets us compare before vs after to prove a beat actually changed it.
    auto mapping = [&]() {
        std::vector<mm::nrOfLightsType> dst(64, static_cast<mm::nrOfLightsType>(-1));
        std::size_t total = 0;
        for (mm::nrOfLightsType li = 0; li < 64; li++)
            layer.lut().forEachDestination(li, [&](mm::nrOfLightsType d) { dst[li] = d; total++; });
        // a permutation: 64 destinations, all distinct, each in range
        std::vector<int> seen(64, 0);
        for (auto d : dst) if (d < 64) seen[d]++;
        bool bijection = (total == 64);
        for (int s : seen) if (s != 1) bijection = false;
        return std::make_pair(dst, bijection);
    };

    auto [before, beforeOk] = mapping();
    CHECK(beforeOk);   // the composed two-permutation mapping is itself a bijection

    // Advance past a beat boundary in small ticks (both modifiers tick each frame).
    for (uint32_t t = 1000; t <= 2500; t += 50) { mm::platform::setTestNowMs(t); layer.tick(); }
    mm::platform::setTestNowMs(0);

    auto [after, afterOk] = mapping();
    CHECK(afterOk);            // still a valid bijection after the coalesced rebuild
    CHECK(before != after);    // a beat actually reshuffled the composed mapping
}

// The incremental warp the Layer runs for an affine live map draws the same frame as mapping each light through modifyLive.
TEST_CASE("Layer live pass: the stepped affine warp matches the per-light map") {
    RotatingGradient r(20, 12, 37);
    mm::Layer& layer = r.layer;
    mm::RotateModifier& rot = r.rot;
    mm::platform::setTestNowMs(1000);
    layer.tick();
    const auto frame = frameAt(layer, 1777);   // some angle part way round
    const mm::Coord3D box{20, 12, 1};
    int differ = 0;
    for (mm::lengthType y = 0; y < 12; y++)
        for (mm::lengthType x = 0; x < 20; x++) {
            mm::Coord3D src{x, y, 0};
            rot.modifyLive(src, box);
            const bool in = src.x >= 0 && src.x < 20 && src.y >= 0 && src.y < 12;
            const uint8_t* p = frame.data() + (static_cast<size_t>(y) * 20 + x) * 3;
            const uint8_t wantR = in ? static_cast<uint8_t>(src.x) : 0, wantG = in ? static_cast<uint8_t>(src.y) : 0;
            if (p[0] != wantR || p[1] != wantG) differ++;
        }
    CHECK(differ == 0);   // the stepped sum and the per-light product are the same integers
    mm::platform::setTestNowMs(0);
}
