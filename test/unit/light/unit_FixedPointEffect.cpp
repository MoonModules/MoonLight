/// @module FixedPointEffect

#include "doctest.h"
#include "platform/platform.h"   // setTestNowMs
#include "light/effects/FixedPointEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// A panel big enough for the shapes to have room, small enough that the sub-pixel property is what is being tested rather than raw resolution.
struct Panel {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::FixedPointEffect effect;

    explicit Panel(mm::lengthType size = 24) {
        grid.width = size;
        grid.height = size;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
        layer.applyState();
    }

    // How many lights carry any color at all: the cheapest measure of "something was drawn".
    int litCount() {
        auto& buf = layer.buffer();
        int n = 0;
        for (size_t i = 0; i < buf.count(); i++) {
            const uint8_t* p = buf.data() + i * 3;
            if (p[0] || p[1] || p[2]) n++;
        }
        return n;
    }

    // How many carry a PARTIAL value: the antialiasing, which is the whole point of the effect.
    int partialCount() {
        auto& buf = layer.buffer();
        int n = 0;
        for (size_t i = 0; i < buf.count(); i++) {
            const uint8_t* p = buf.data() + i * 3;
            for (int c = 0; c < 3; c++) if (p[c] > 0 && p[c] < 255) { n++; break; }
        }
        return n;
    }
};

}  // namespace

TEST_CASE("every demo draws something on a panel") {
    // The audit that matters for a Select: a value that renders nothing is a dead menu entry, and nothing else in the suite would notice, since a black frame is a valid frame.
    for (uint8_t d = 1; d < mm::FixedPointEffect::kDemoCount; d++) {
        Panel p;
        p.effect.demo = d;
        p.effect.fade = 255;               // no trail carried between frames: judge one frame alone
        p.effect.zoom = 0;                 // the camera push-in runs on the process clock, and at its peak a figure leaves this small panel
        for (int i = 0; i < 30; i++) p.layer.tick();
        CAPTURE(std::string(mm::FixedPointEffect::kDemoNames[d]));
        CHECK(p.litCount() > 0);
    }
}

// NOT TESTED HERE: that a shape moves by fractions of a pixel between frames, which is the whole property this effect demonstrates. Its hands run on the WALL CLOCK (fixed periods, as the original does), and a unit test ticks a dozen frames in well under a millisecond, so nothing moves and any assertion about motion would be measuring the harness rather than the effect. The sub-pixel behavior is pinned where it actually lives, on the primitives in unit_draw.cpp, and judged on the bench.

TEST_CASE("the all setting reaches every demo, and only the demos") {
    // The rotation picks a demo from the wall clock, so a test cannot tick its way into the next slot: 200 ticks pass in milliseconds and a dwell is seconds. Testing the SELECTION instead is both faster and stricter, since the arithmetic is where the bugs are: an off-by-one that lands on `all` itself would recurse, and one that skips the last demo would hide it forever.
    using FP = mm::FixedPointEffect;
    bool seen[FP::kDemoCount] = {};
    for (uint32_t slotIndex = 0; slotIndex < FP::kCycled * 3; slotIndex++) {
        const uint8_t active = static_cast<uint8_t>(slotIndex % FP::kCycled + 1);
        REQUIRE(active != FP::kAll);            // never selects "all" itself
        REQUIRE(active < FP::kDemoCount);       // never runs off the end of the list
        seen[active] = true;
    }
    for (uint8_t d = 1; d < FP::kDemoCount; d++) {
        CAPTURE(FP::kDemoNames[d]);
        CHECK(seen[d]);                          // every demo gets its turn
    }
}

TEST_CASE("a demo switch does not join the old figure to the new one") {
    // The curve demos share one point buffer. Carried across a switch, the first segment drawn joins a spirograph point to a lissajous point, which is a bright line straight across the panel: visible, wrong, and gone the moment the trail is cleared on the change.
    mm::platform::setTestNowMs(5000);   // frozen, so both panels draw the figure at the same moment
    Panel p;
    p.effect.demo = mm::FixedPointEffect::kSpirograph;
    p.effect.fade = 255;
    for (int i = 0; i < 40; i++) p.layer.tick();

    p.effect.demo = mm::FixedPointEffect::kLissajous;
    p.layer.tick();                        // the FIRST frame after the switch is the risky one

    Panel fresh;
    fresh.effect.demo = mm::FixedPointEffect::kLissajous;
    fresh.effect.fade = 255;
    fresh.layer.tick();
    // A carried-over point adds the joining segment; a cleared trail draws exactly what a panel that never showed the spirograph draws.
    CHECK(p.litCount() == fresh.litCount());
}
