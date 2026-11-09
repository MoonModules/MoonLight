/// @module SoapEffect

/// Pins the soap film: it moves, it flows alike at any render rate, it survives any grid and lets its memory go.

#include "doctest.h"
#include "golden_frame.h"                 // the effect harness: Layouts, Grid, Layer
#include "light/effects/SoapEffect.h"

#include <vector>

using namespace mm;

namespace {

/// A grid of `w` by `h` with the effect on a layer.
struct Rig {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    SoapEffect soap;
    Rig(uint16_t w, uint16_t h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&soap);
        layer.applyState();
    }
    std::vector<uint8_t> frame() const {
        const auto& b = layer.buffer();
        return std::vector<uint8_t>(b.data(), b.data() + b.bytes());
    }
};

/// The film after `totalMs` of render time, drawn every `frameMs`.
std::vector<uint8_t> filmAfter(uint32_t totalMs, uint32_t frameMs) {
    golden::ScopedTestClock clock(1000);
    Rig rig(24, 16);
    for (uint32_t t = 0; t <= totalMs; t += frameMs) { platform::setTestNowMs(1000 + t); rig.layer.tick(); }
    return rig.frame();
}

}  // namespace

TEST_CASE("the soap film lights the panel and keeps moving") {
    golden::ScopedTestClock clock(1000);
    Rig rig(24, 16);
    rig.layer.tick();
    const std::vector<uint8_t> first = rig.frame();
    bool lit = false;
    for (uint8_t v : first) lit = lit || v != 0;
    CHECK(lit);
    platform::setTestNowMs(1500);
    rig.layer.tick();
    CHECK(rig.frame() != first);
}

/// The mean difference per channel between two films.
double difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); i++) sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
    return a.empty() ? 0.0 : sum / static_cast<double>(a.size());
}

// Shears do not commute, so the image differs per rate, and many small blends smear more than a few large ones, as in WLED: a fast film is softer, 0.62 at 5 ms frames, 0.75 at 100 fps against 25.
TEST_CASE("the soap film flows at one pace at any render rate: in 100 ms it changes at 5 ms frames within half to one and a half times what it does at 50 ms frames") {
    const double fast = difference(filmAfter(500, 5), filmAfter(600, 5));
    const double slow = difference(filmAfter(500, 50), filmAfter(600, 50));
    CHECK(fast > 0.5 * slow);
    CHECK(fast < 1.5 * slow);
}

TEST_CASE("the soap film keeps its brightness on a fast render: two seconds at 2 ms frames stay as bright as at 40 ms") {
    auto mean = [](const std::vector<uint8_t>& f) { double s = 0.0; for (uint8_t v : f) s += v; return f.empty() ? 0.0 : s / static_cast<double>(f.size()); };
    CHECK(mean(filmAfter(2000, 2)) > 0.8 * mean(filmAfter(2000, 40)));
}

TEST_CASE("the soap film survives a 1x1 and a 0x0 grid") {
    golden::ScopedTestClock clock(1000);
    Rig one(1, 1);
    for (uint32_t t = 0; t < 200; t += 20) { platform::setTestNowMs(1000 + t); one.layer.tick(); }
    Rig none(0, 0);
    none.layer.tick();
    CHECK(true);   // reached without a crash
}

TEST_CASE("the soap film frees its noise, image and line when disabled") {
    Rig rig(8, 8);
    CHECK(rig.soap.dynamicBytes() == 8 * 8 + 8 * 8 * 3 + 8 * 3);
    rig.soap.setEnabled(false);
    rig.layer.applyState();
    CHECK(rig.soap.dynamicBytes() == 0);
}
