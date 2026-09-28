/// @module MoonLive
/// @also draw, MoonLiveEffect

/// The scripted drawing vocabulary: `circle` and `line` as a script calls them, pinning the seam where unsigned ABI words become the light domain's signed lengths.

#include "doctest.h"
#include "MoonLiveScriptFixture.h"
#include "../core/moonlive_script_wrap.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "core/moonlive/moonlive_emit.h"   // MM_MOONLIVE_HAS_HOST_JIT
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// A wired effect on a real layer, the way the module tree builds one.
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    MoonLiveEffect effect;
    Scene(int w = 16, int h = 16) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
        effect.defineControls();
    }
    void run(const char* script) {
        effect.setScript(mmWriteScript(script));
        layouts.applyState();
        layer.applyState();
    }
    /// How many lights carry any color, which is the coverage a stroke width decides.
    int lit() {
        int n = 0;
        const auto* px = layer.buffer().data();
        for (nrOfLightsType i = 0; i < layer.buffer().count(); i++)
            if (px[i * 3] || px[i * 3 + 1] || px[i * 3 + 2]) n++;
        return n;
    }
};

/// One frame of a tick() body on a 16x16 grid.
int litAfter(const char* body) {
    Scene s;
    std::string src = std::string("class T { void tick() { fill(0, 0, 0); ") + body + " } }";
    s.run(src.c_str());
    s.layer.tick();
    return s.lit();
}
}  // namespace

TEST_CASE("a scripted circle draws its rim and leaves the middle dark") {
    Scene s;
    s.run("class T { void tick() { fill(0, 0, 0); circle(8, 8, 5, 1, 255, 0, 0); } }");
    s.layer.tick();
    const auto* px = s.layer.buffer().data();
    const auto at = [&](int x, int y) { return px[(y * 16 + x) * 3]; };
    CHECK(at(8, 8) == 0);            // hollow: the center is not part of an outline
    CHECK(at(8, 3) > 0);             // and the rim is up at the radius
    CHECK(s.lit() > 0);
}

// A script's arguments arrive as unsigned ABI words, so a negative width is 4,294,967,293 to a `> 0` test: read signed, it is the thinnest line instead.
TEST_CASE("a scripted circle with a negative stroke width draws a thin rim, not a filled grid") {
    const int thin     = litAfter("circle(8, 8, 5, 1, 255, 0, 0);");
    const int negative = litAfter("circle(8, 8, 5, -3, 255, 0, 0);");
    REQUIRE(thin > 0);            // the control case drew, so the comparison means something
    CHECK(negative == thin);      // reads as the thinnest line
    CHECK(negative < 16 * 16);    // and emphatically does not swallow the grid
}

TEST_CASE("a scripted circle with a zero stroke width still draws") {
    // Zero would draw nothing at all, so it reads as the thinnest line a caller can mean.
    CHECK(litAfter("circle(8, 8, 5, 0, 255, 0, 0);") == litAfter("circle(8, 8, 5, 1, 255, 0, 0);"));
}

TEST_CASE("a thicker scripted circle covers more lights than a thin one") {
    CHECK(litAfter("circle(8, 8, 5, 3, 255, 0, 0);") > litAfter("circle(8, 8, 5, 1, 255, 0, 0);"));
}

TEST_CASE("a scripted circle outside the grid lights nothing") {
    // Wholly off a 16x16 grid, so the count is ZERO rather than merely bounded: `>= 0` is vacuously true of an unsigned count.
    CHECK(litAfter("circle(200, 200, 40, 2, 255, 0, 0);") == 0);
}

TEST_CASE("a scripted line reaches both of its endpoints") {
    Scene s;
    s.run("class T { void tick() { fill(0, 0, 0); line(2, 2, 13, 13, 0, 255, 0); } }");
    s.layer.tick();
    const auto* px = s.layer.buffer().data();
    const auto at = [&](int x, int y) { return px[(y * 16 + x) * 3 + 1]; };
    CHECK(at(2, 2) > 0);
    CHECK(at(13, 13) > 0);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
