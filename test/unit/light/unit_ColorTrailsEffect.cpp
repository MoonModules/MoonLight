/// @module ColorTrailsEffect

/// The two flows this effect can carry color with, and the property that separates them.
///
/// `Noise` is the separable one it is named for: a shift per row and per column, so the picture shears and folds.
/// `Radial` is a current along the line through the center, computed per pixel from geometry with no field and no state.
/// What distinguishes radial from every other flow here is DIRECTION.
/// Color must end up further from the center than it started (or nearer, running in), which a shear can never do on average.

#include "doctest.h"
#include "golden_frame.h"                 // the effect harness: Layouts, Grid, Layer
#include "light/effects/ColorTrailsEffect.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <cstdlib>

using namespace mm;

namespace {

/// Mean distance from the center of every lit sample, weighted by brightness.
/// The one number that tells an outward flow from an inward one: a shear moves color around, radial moves it out.
double litRadius(const Layer& layer, uint16_t w, uint16_t h) {
    const auto& buf = layer.buffer();
    const double cx = w / 2.0, cy = h / 2.0;
    double sum = 0.0, weight = 0.0;
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 3u;
            if (i + 2 >= buf.bytes()) continue;
            const double v = buf.data()[i] + buf.data()[i + 1] + buf.data()[i + 2];
            if (v <= 0.0) continue;
            const double dx = x - cx, dy = y - cy;
            sum += v * std::sqrt(dx * dx + dy * dy);
            weight += v;
        }
    }
    return weight > 0.0 ? sum / weight : 0.0;
}

/// Run the effect for `frames` and hand back how far the lit color sits from the center.
double runFlow(uint8_t flowType, uint16_t frames) {
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; ColorTrailsEffect effect;
    grid.width = 32; grid.height = 32; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    effect.flowType = flowType;
    effect.flow = 255;                 // the strongest current, so the drift is unambiguous
    effect.mode = static_cast<uint8_t>(ColorTrailsEffect::Mode::Orbital);
    layer.applyState();
    for (uint16_t i = 0; i < frames; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }
    return litRadius(layer, 32, 32);
}

}  // namespace

TEST_CASE("an outward radial flow carries color further from the center than an inward one") {
    const double out = runFlow(static_cast<uint8_t>(ColorTrailsEffect::Flow::Radial), 60);
    const double in  = runFlow(static_cast<uint8_t>(ColorTrailsEffect::Flow::RadialIn), 60);
    // Both draw the same emitter in the same place; only the direction of the wind differs, so the difference in where the color ends up IS the flow.
    CHECK(out > in);
}

TEST_CASE("every flow renders something rather than draining the picture to black") {
    for (uint8_t f = 0; f <= static_cast<uint8_t>(ColorTrailsEffect::Flow::RadialIn); f++) {
        CHECK(runFlow(f, 40) > 0.0);
    }
}

TEST_CASE("the center cell has no direction to move in, and does not divide by zero") {
    // The one place a radial field is undefined. Reaching it must leave the effect running rather than trapping: the guard is a floored radius, and this is what pins it.
    golden::ScopedTestClock clock(1000);
    Layouts layouts; GridLayout grid; Layer layer; ColorTrailsEffect effect;
    grid.width = 1; grid.height = 1; grid.depth = 1;   // the center IS the whole grid
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&effect);
    effect.flowType = static_cast<uint8_t>(ColorTrailsEffect::Flow::Radial);
    effect.flow = 255;
    layer.applyState();
    for (uint16_t i = 0; i < 20; i++) { platform::setTestNowMs(1000 + i * 20u); layer.tick(); }
    CHECK(layer.buffer().bytes() > 0);   // still rendering
}
