/// @module polar
/// @also PolarNoiseEffect

/// The migration check for the polar table: an effect reading the table must look like the same effect, not merely a plausible one.
/// PolarNoise is rendered twice on the same grid at the same frames, once through the table and once computing atan16/dist16 per pixel, and the frames are compared pixel by pixel.
/// This is the evidence behind the re-baselined golden.

#include "doctest.h"

#include "golden_frame.h"   // ScopedTestClock: restores the real clock on scope exit
#include "light/effects/AuroraEffect.h"
#include "light/effects/PolarNoiseEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/TunnelEffect.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "platform/platform_clock.h"   // setTestNowMs

#include <cstdint>
#include <vector>

using namespace mm;

namespace {

/// Render one effect for `frames` and return the final buffer.
/// `EffectT` is any effect carrying the polar-table controls, so the same comparison covers every effect that reads the address.
template <typename EffectT>
std::vector<uint8_t> render(lengthType w, lengthType h, bool useTable, bool wide = false,
                            uint16_t frames = 60, lengthType d = 1, uint8_t mapping = 0) {
    // RAII, so the override is cleared even if a REQUIRE below exits early. A leaked test clock freezes time for every test that runs after this one in the same binary.
    const mm::golden::ScopedTestClock clock(1000);
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    EffectT effect;
    effect.polar.use = useTable;
    effect.polar.wide = wide;
    effect.polar.mapping = mapping;
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
    return std::vector<uint8_t>(buf.data(), buf.data() + buf.bytes());
}

/// The share of bytes that differ by more than `tol`, and the worst difference seen.
struct Diff { double share; int worst; };
Diff compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tol) {
    REQUIRE(a.size() == b.size());
    REQUIRE(!a.empty());
    std::size_t over = 0;
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); i++) {
        const int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > worst) worst = d;
        if (d > tol) over++;
    }
    return {double(over) / a.size(), worst};
}

}  // namespace

TEST_CASE("the 16-bit table renders exactly what computing the address per pixel renders") {
    // At full precision the table IS the computation, cached: same angle, same radius, same field.
    // Anything less would mean the table had introduced an error of its own.
    // Every effect that reads the address is checked, because each scales and truncates it differently.
    SUBCASE("PolarNoise") {
        const Diff d = compare(render<PolarNoiseEffect>(64, 64, true, true), render<PolarNoiseEffect>(64, 64, false), 0);
        CHECK(d.worst == 0);
    }
    SUBCASE("Tunnel") {
        const Diff d = compare(render<TunnelEffect>(64, 64, true, true), render<TunnelEffect>(64, 64, false), 0);
        CHECK(d.worst == 0);
    }
    SUBCASE("Spiral") {
        const Diff d = compare(render<SpiralEffect>(64, 64, true, true), render<SpiralEffect>(64, 64, false), 0);
        CHECK(d.worst == 0);
    }
}

TEST_CASE("the 8-bit table costs a quantized angle, and nothing else") {
    // The default trades 2 bytes per pixel for 256 angle steps. That shows up where the field is steepest and nowhere else, so most of the picture is untouched and no pixel is wildly wrong.
    const auto tabled = render<PolarNoiseEffect>(64, 64, true);
    const auto exact  = render<PolarNoiseEffect>(64, 64, false);
    const Diff d = compare(tabled, exact, 8);
    CHECK(d.share < 0.15);        // a minority of channels differ at all
    const Diff gross = compare(tabled, exact, 64);
    CHECK(gross.share < 0.02);    // and almost none differ grossly
}

TEST_CASE("an effect still renders when the polar table cannot be built") {
    // Degrade visibly, never crash: with the table off the effect computes the address per pixel.
    const auto exact = render<PolarNoiseEffect>(32, 32, false);
    std::size_t lit = 0;
    for (uint8_t v : exact) lit += v > 0 ? 1 : 0;
    CHECK(lit > exact.size() / 4);
}

TEST_CASE("the computed fallback keeps the mapping the table would have used") {
    // A device too tight for the table must show the SAME composition, not a different one. The fallback computed cylindrical unconditionally at first, so a fixture set to spherical or radial silently reverted the moment memory ran short: still rendering, quietly wrong.
    for (uint8_t mapping = 0; mapping < 3; mapping++) {
        const auto tabled = render<AuroraEffect>(8, 8, true,  /*wide=*/true, 40, 5, mapping);
        const auto exact  = render<AuroraEffect>(8, 8, false, /*wide=*/false, 40, 5, mapping);
        const Diff d = compare(tabled, exact, 0);
        CHECK(d.worst == 0);
    }
}
