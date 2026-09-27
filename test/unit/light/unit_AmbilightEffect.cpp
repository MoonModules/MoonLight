/// @module AmbilightEffect
///
/// Pins the frame to light mapping end to end through the real static seam: a live VideoService publishes a frame, the effect renders it, the buffer is read back.
///
/// @moreinfo
///
/// The checks are about orientation and coverage, because a picture averaged over the wrong rectangle, or flipped top for bottom, still looks like *a* picture.

#include "doctest.h"
#include "core/services/VideoService.h"
#include "light/effects/AmbilightEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/RectangleLayout.h"
#include "light/layouts/SphereLayout.h"
#include "platform/platform.h"   // fsRootPath: ctest roots the filesystem in the build tree
#include "light/layouts/Layouts.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using mm::AmbilightEffect;
using mm::VideoService;
namespace platform = mm::platform;

namespace {

// A live VideoService in test-pattern mode: red top band, blue bottom, yellow left, green right. Its seat is claimed on construction and vacated on destruction, so each case starts clean.
struct PatternSource {
    VideoService svc;
    PatternSource() {
        svc.source = 0;   // test pattern
        svc.applyState(); // builds the buffer and renders the first frame
    }
};

// Render one frame of the effect over a `w`x`h` grid and hand back the layer for inspection.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    AmbilightEffect fx;

    Rig(uint16_t w, uint16_t h) {
        grid.width = w;
        grid.height = h;
        grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
    void render() {
        layer.applyState();
        layer.tick();
    }
    // applyState() re-runs prepare(), which un-primes the smoother and makes every frame jump. Anything testing the SMOOTHING has to tick without it.
    void tickOnly() { layer.tick(); }
    /// A tick with a NEW frame behind it: the effect skips a repeated one, so anything measuring per-frame behavior has to advance the source too, as the scheduler does.
    void tickOnly(VideoService& source) {
        source.tick();
        layer.tick();
    }
    const uint8_t* px(int x, int y) const {
        return layer.buffer().data() + (static_cast<size_t>(y) * grid.width + x) * 3;
    }
};

} // namespace

// Orientation must survive to the buffer: red top band → red first row. Swapped, the whole picture is upside down: on a TV border, the difference between matching the screen and mirroring it.
TEST_CASE("AmbilightEffect: the frame's orientation reaches the buffer, top band to top row") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100; // identity, so the raw zone means are what we read
    rig.render();

    const uint8_t* top = rig.px(4, 0);
    const uint8_t* bottom = rig.px(4, 7);
    CHECK(top[0] > top[2]);       // top row reads red-dominant
    CHECK(bottom[2] > bottom[0]); // bottom row reads blue-dominant
}

// The horizontal counterpart of the test above; together they pin all four edges. Red separates the bands: it is in yellow and absent from green.
TEST_CASE("AmbilightEffect: the frame's left and right bands reach the matching columns") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.render();

    const uint8_t* left = rig.px(0, 4); // mid-height, so neither the top nor bottom band
    const uint8_t* right = rig.px(7, 4);
    CHECK(left[0] > right[0]); // yellow carries red; green does not
    CHECK(right[1] > 0);       // and the right column is lit at all
}

// Every light must be written. An empty zone leaves its light holding the previous frame, so a mapping bug shows as dead lights scattered through the strip rather than an obvious failure.
TEST_CASE("AmbilightEffect: every light is written, none left dark by an empty zone") {
    PatternSource src;
    Rig rig(16, 9);
    rig.fx.saturation = 100;
    rig.render();

    // WRITTEN, not lit: the pattern's center is black, so counting lit cells cannot tell "every position was painted" from "one was". Fill with a value the effect cannot produce instead.
    constexpr uint8_t kSentinel = 0x5A;
    std::memset(rig.layer.buffer().data(), kSentinel, static_cast<size_t>(16) * 9 * 3);
    rig.render();

    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            const uint8_t* p = rig.px(x, y);
            const bool untouched = p[0] == kSentinel && p[1] == kSentinel && p[2] == kSentinel;
            CHECK_FALSE(untouched);
        }
    // The corners sit inside the colored bands and must never be dark.
    for (const auto& [x, y] : {std::pair{0, 0}, std::pair{15, 0}, std::pair{0, 8}, std::pair{15, 8}}) {
        const uint8_t* p = rig.px(x, y);
        CHECK((p[0] || p[1] || p[2]));
    }
}

// More lights than source pixels: neighboring positions must SHARE one rather than resolve to an empty zone. The pattern is 64 wide, so a 128-wide layer forces the guard.
TEST_CASE("AmbilightEffect: a layer finer than the frame still writes every light") {
    PatternSource src;
    Rig rig(128, 4);
    rig.fx.saturation = 100;
    rig.render();

    // Top row of the pattern is the red band. At 4 rows tall every row samples some band, so the whole first row must be lit across its full width.
    for (int x = 0; x < 128; x++) {
        const uint8_t* p = rig.px(x, 0);
        CHECK((p[0] || p[1] || p[2]));
    }
}

// brightness scales the result down uniformly. Distinct from the driver's brightness: this one dims the video relative to whatever else is composited beside it.
TEST_CASE("AmbilightEffect: brightness scales the sampled color down") {
    PatternSource src;
    Rig full(8, 8);
    full.fx.saturation = 100;
    full.fx.brightness = 255;
    full.render();
    const int fullRed = full.px(4, 0)[0];

    Rig dim(8, 8);
    dim.fx.saturation = 100;
    dim.fx.brightness = 64;
    dim.render();
    const int dimRed = dim.px(4, 0)[0];

    CHECK(fullRed > 0);
    CHECK(dimRed < fullRed);
    CHECK(dimRed == (fullRed * 64) / 255);
}

// Saturation stretches each channel away from the zone's luma: above 100 a colored zone gets more saturated, which is what pulls averaged means back off gray.
TEST_CASE("AmbilightEffect: saturation above 100 pushes a colored zone further from gray") {
    PatternSource src;
    Rig flat(8, 8);
    flat.fx.saturation = 100;
    flat.render();
    const uint8_t* a = flat.px(4, 0);
    const int flatSpread = static_cast<int>(a[0]) - static_cast<int>(a[2]);

    Rig boosted(8, 8);
    boosted.fx.saturation = 180;
    boosted.render();
    const uint8_t* b = boosted.px(4, 0);
    const int boostedSpread = static_cast<int>(b[0]) - static_cast<int>(b[2]);

    CHECK(flatSpread > 0);
    CHECK(boostedSpread >= flatSpread); // red pulled further from blue, or already clipped at 255
}

// No source paints black rather than returning early, which would leave the PREVIOUS effect's picture frozen on the strip. Every effect owns its background (unit_Effects_gridsweep).
TEST_CASE("AmbilightEffect: no video source paints black, never the previous effect's frame") {
    // No PatternSource here, so no service holds the seat and latestFrame() reports nothing.
    REQUIRE(VideoService::latestFrame()->rgb == nullptr);

    Rig rig(4, 4);
    rig.layer.applyState();
    // Paint a recognizable frame, standing in for whatever effect ran before this one.
    uint8_t* buf = rig.layer.buffer().data();
    REQUIRE(buf != nullptr);
    for (size_t i = 0; i < rig.layer.buffer().count(); i++) {
        buf[i * 3 + 0] = 11;
        buf[i * 3 + 1] = 22;
        buf[i * 3 + 2] = 33;
    }
    rig.layer.tick();
    CHECK(rig.px(2, 2)[0] == 0);
    CHECK(rig.px(2, 2)[1] == 0);
    CHECK(rig.px(2, 2)[2] == 0);
}

// --- Smoothing ---------------------------------------------------------------------------------
// The accumulators are 8.8 precisely so a slow setting still ARRIVES: in whole bytes every step rounds to zero and the light stalls short of its target forever. These check that it converges, and that a big move still lands at once.

// Off must be bit-identical to no smoothing at all, since it is the default.
TEST_CASE("AmbilightEffect: smoothing off follows the frame exactly") {
    PatternSource src;
    Rig plain(8, 8), off(8, 8);
    plain.fx.saturation = 100;
    off.fx.saturation = 100;
    off.fx.smoothing = 0;
    plain.render();
    off.render();
    CHECK(std::memcmp(plain.px(4, 0), off.px(4, 0), 3) == 0);
}

// The first frame after a gap must land immediately: creeping up from black would show as a fade-in every time the source reconnects.
TEST_CASE("AmbilightEffect: the first frame lands whole, not smoothed up out of black") {
    PatternSource src;
    Rig fast(8, 8), slow(8, 8);
    fast.fx.saturation = 100;
    slow.fx.saturation = 100;
    slow.fx.smoothing = 240; // very slow, so a smoothed first frame would be nearly black
    fast.render();
    slow.render();
    CHECK(std::memcmp(fast.px(4, 0), slow.px(4, 0), 3) == 0);
}

// The one that 8-bit state would fail: with heavy smoothing each step is a fraction of a byte. Both directions, because >> floors and the two arrive by different routes.
TEST_CASE("AmbilightEffect: a heavily smoothed light converges exactly, rising and falling") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.smoothing = 250; // a step of ~6/256 of the remaining distance
    rig.fx.snapAbove = 0;   // never jump, so only the smoothing can get it there
    rig.render();           // primes on the first frame, so this one lands whole

    const uint8_t bright = rig.px(4, 0)[0];
    REQUIRE(bright > 8); // the band has somewhere to fall from

    // Falling: drive the target down and let it smooth in.
    rig.fx.brightness = 8;
    for (int i = 0; i < 2000; i++) rig.tickOnly(src.svc);
    const uint8_t dim = rig.px(4, 0)[0];
    CHECK(dim < bright / 2);

    // Rising back to where it started must land on the SAME value, not one short.
    rig.fx.brightness = 255;
    for (int i = 0; i < 2000; i++) rig.tickOnly(src.svc);
    CHECK(rig.px(4, 0)[0] == bright);
}

// --- edgeDepth ---------------------------------------------------------------------------------
// A border light's own share is 1/height of the frame, a sliver at the very edge. edgeDepth lets the outermost positions reach further in without changing how many there are.

// Off is the default, so it must be the plain division exactly.
TEST_CASE("AmbilightEffect: edgeDepth 0 leaves the zones as the plain division") {
    PatternSource src;
    Rig plain(8, 8), zero(8, 8);
    plain.fx.saturation = 100;
    zero.fx.saturation = 100;
    zero.fx.edgeDepth = 0;
    plain.render();
    zero.render();
    CHECK(std::memcmp(plain.px(4, 0), zero.px(4, 0), 3) == 0);
}

// The point of the control: the top row must see further down the picture than its own 1/8 share.
TEST_CASE("AmbilightEffect: edgeDepth makes the outer row sample deeper") {
    PatternSource src;
    Rig shallow(8, 8), deep(8, 8);
    shallow.fx.saturation = 100;
    deep.fx.saturation = 100;
    deep.fx.edgeDepth = 50; // half the frame, so it reaches well past the top band
    shallow.render();
    deep.render();
    CHECK(std::memcmp(shallow.px(4, 0), deep.px(4, 0), 3) != 0);
}

// The control SETS the depth, it does not raise a floor: a value below a position's own share must make its zone thinner
TEST_CASE("AmbilightEffect: edgeDepth below the natural share makes the outer row thinner") {
    PatternSource src;
    // Three rows over a 36-tall pattern is a 12-row share, which reaches past the 9-row red band into the black center. A shallower zone stays inside the band, so it reads BRIGHTER.
    Rig plain(8, 3), thin(8, 3);
    plain.fx.saturation = 100;
    thin.fx.saturation = 100;
    thin.fx.edgeDepth = 10; // ~4 rows, against a natural share of 12
    plain.render();
    thin.render();
    CHECK(thin.px(4, 0)[0] > plain.px(4, 0)[0]);
}

// Only the outermost ring moves. An interior position has no edge to reach in from, so a video wall is unaffected even with the control turned up.
TEST_CASE("AmbilightEffect: edgeDepth leaves interior positions alone") {
    PatternSource src;
    Rig plain(8, 8), deep(8, 8);
    plain.fx.saturation = 100;
    deep.fx.saturation = 100;
    deep.fx.edgeDepth = 50;
    plain.render();
    deep.render();
    for (int y = 1; y < 7; y++)
        for (int x = 1; x < 7; x++) CHECK(std::memcmp(plain.px(x, y), deep.px(x, y), 3) == 0);
}

// --- Black bars --------------------------------------------------------------------------------
// A letterboxed film puts bars exactly where the top and bottom lights look, so those lights go dark while the picture is bright. Detection moves the zones past the bar, which edgeDepth cannot do.

namespace {

// A VideoService reading a P6 PPM written here: `bar` black rows top and bottom, green between. Written to a file because that is the seam the file source actually uses.
struct Letterbox {
    VideoService svc;
    char path[64] = {};
    Letterbox(int w, int h, int bar) {
        // The service resolves a bare name under the desktop filesystem root, which ctest points into the build tree. Ask for the resolved root rather than assuming one.
        std::snprintf(path, sizeof(path), "mm_letterbox_%dx%d_%d.ppm", w, h, bar);
        char real[256];
        std::snprintf(real, sizeof(real), "%s/%s", mm::platform::fsRootPath(), path);
        mm::platform::fsMkdir("/");   // ctest's root is a path, not necessarily a directory yet
        std::FILE* f = std::fopen(real, "wb");
        REQUIRE(f != nullptr);
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const bool picture = y >= bar && y < h - bar;
                const uint8_t px[3] = {0, picture ? uint8_t(200) : uint8_t(0), 0};
                std::fwrite(px, 1, 3, f);
            }
        std::fclose(f);
        svc.source = 1;
        std::strncpy(svc.file, path, sizeof(svc.file) - 1);
        svc.applyState();
    }
    ~Letterbox() {
        char real[256];
        std::snprintf(real, sizeof(real), "%s/%s", mm::platform::fsRootPath(), path);
        std::remove(real);
    }
};


// A frame every line of which reads DARK to the bar probe, with brighter outer rows than middle ones, so a wrongly adopted bar changes what the edge lights see. Same shape as Letterbox.
struct DimFrame {
    VideoService svc;
    char path[64] = {};
    DimFrame(int w, int h, uint8_t outer, uint8_t inner, int band) {
        std::snprintf(path, sizeof(path), "mm_dim_%dx%d_%d.ppm", w, h, band);
        char real[256];
        std::snprintf(real, sizeof(real), "%s/%s", mm::platform::fsRootPath(), path);
        mm::platform::fsMkdir("/");
        std::FILE* f = std::fopen(real, "wb");
        REQUIRE(f != nullptr);
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t v = (y < band || y >= h - band) ? outer : inner;
                const uint8_t px[3] = {v, v, v};
                std::fwrite(px, 1, 3, f);
            }
        std::fclose(f);
        svc.source = 1;
        std::strncpy(svc.file, path, sizeof(svc.file) - 1);
        svc.applyState();
    }
    ~DimFrame() {
        char real[256];
        std::snprintf(real, sizeof(real), "%s/%s", mm::platform::fsRootPath(), path);
        std::remove(real);
    }
};

} // namespace

// Off by default: no scan, no shift, identical to before the feature existed.
TEST_CASE("AmbilightEffect: blackBars off maps across the whole frame") {
    PatternSource src;
    Rig plain(8, 8), off(8, 8);
    plain.fx.saturation = 100;
    off.fx.saturation = 100;
    off.fx.detectBlackBars = false;
    plain.render();
    off.render();
    CHECK(std::memcmp(plain.px(4, 0), off.px(4, 0), 3) == 0);
}

// The top light sits in the bar and reads black; with detection it reads the picture instead.
TEST_CASE("AmbilightEffect: the top light escapes the letterbox once bars are detected") {
    Letterbox src(64, 64, 16); // a quarter of the height black at each end
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.detectBlackBars = true;
    rig.render();

    CHECK(rig.px(4, 0)[1] == 0);                       // first frame: still inside the bar
    for (int i = 0; i < 60; i++) rig.tickOnly(src.svc); // past the hysteresis window
    CHECK(rig.px(4, 0)[1] > 100);                // now reading the green picture
}

// Dark EVERYWHERE by the probe, but not uniform. Scanning to the ceiling used to report the deepest bar on all four edges, cropping a dark scene to its middle for kStableFrames after it ended.
TEST_CASE("AmbilightEffect: an all-dark frame reports no bars, not the deepest ones") {
    // 40% of 64 is a 25-line ceiling. Rows 0..24 sit at 60, under barLevel 64, so the scan runs the whole way; rows 25..38 are black, which the top light would read if a bar were adopted.
    DimFrame src(64, 64, 60, 0, 25);
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.detectBlackBars = true;
    rig.fx.barLevel = 64;
    rig.render();

    const uint8_t first = rig.px(4, 0)[0];
    REQUIRE(first > 0);                             // reading the bright outer rows, not the middle
    for (int i = 0; i < 60; i++) rig.tickOnly(src.svc);   // past the hysteresis window
    CHECK(rig.px(4, 0)[0] == first);                // nothing adopted, so nothing moved
}

// The pattern's center is black and its edges are colored: the OPPOSITE of a letterbox. Nothing must be detected in it, or a picture that fills the frame would get cropped.
TEST_CASE("AmbilightEffect: a frame that fills the picture reports no bars") {
    PatternSource src;
    Rig plain(8, 8), armed(8, 8);
    plain.fx.saturation = 100;
    armed.fx.saturation = 100;
    armed.fx.detectBlackBars = true;
    plain.render();
    armed.render();
    for (int i = 0; i < 60; i++) armed.tickOnly(src.svc);
    CHECK(std::memcmp(plain.px(4, 0), armed.px(4, 0), 3) == 0);
}

// --- Skipping positions that reach no LED -------------------------------------------------------
// On a RectangleLayout the interior maps to nothing, so the effect asks the LUT and skips those positions. GridLayout is identity, so the video-wall case is unaffected.

namespace {

// The Rig above, with a hollow rectangle in place of the grid: perimeter lit, interior dead.
struct RectRig {
    mm::Layouts layouts;
    mm::RectangleLayout rect;
    mm::Layer layer;
    AmbilightEffect fx;

    RectRig(uint16_t w, uint16_t h) {
        rect.width = w;
        rect.height = h;
        layouts.addChild(&rect);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
    void render() {
        layer.applyState();
        layer.tick();
    }
    const uint8_t* px(int x, int y) const {
        return layer.buffer().data() + (static_cast<size_t>(y) * rect.width + x) * 3;
    }
};

} // namespace

TEST_CASE("AmbilightEffect: a border layout paints its perimeter and skips the interior") {
    PatternSource src;
    RectRig rig(8, 8);
    rig.fx.saturation = 100;
    rig.render();

    // The perimeter reaches LEDs, so it carries the picture.
    const uint8_t* top = rig.px(4, 0);
    CHECK((top[0] | top[1] | top[2]) != 0);

    // The interior reaches none. Never averaged, never written. it holds the black that Layer::prepare() left in the buffer on the rebuild the effect's own prepare() rode in on.
    for (int y = 1; y < 7; y++)
        for (int x = 1; x < 7; x++) {
            const uint8_t* p = rig.px(x, y);
            CHECK((p[0] | p[1] | p[2]) == 0);
        }

    // And it stays black across further frames, rather than only on the first.
    for (int i = 0; i < 5; i++) rig.layer.tick();
    for (int y = 1; y < 7; y++)
        for (int x = 1; x < 7; x++) {
            const uint8_t* p = rig.px(x, y);
            CHECK((p[0] | p[1] | p[2]) == 0);
        }
}

// The effect is D2 and Layer::extrude() copies its front slice across the depth. On a sparse 3D layout a column whose only LEDs sit at z > 0 must be painted at z = 0 too.
TEST_CASE("AmbilightEffect: a sparse 3D layout lights an LED behind an empty front-face position") {
    PatternSource src;
    mm::Layouts layouts;
    mm::SphereLayout sphere;
    mm::Layer layer;
    AmbilightEffect fx;
    sphere.radius = 2; // a 5x5x5 box; the shell point (4,2,2) has no LED anywhere at z = 0 in its column
    layouts.addChild(&sphere);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&fx);
    fx.saturation = 100;
    layer.applyState();
    layer.tick();

    const mm::MappingLUT& lut = layer.lut();
    REQUIRE(layer.depth() == 5);
    REQUIRE_FALSE(lut.hasDestination(2 * 5 + 4));               // (4,2) at z = 0: nothing
    REQUIRE(lut.hasDestination((2 * 5 + 2) * 5 + 4));            // (4,2) at z = 2: on the shell
    const uint8_t* p = layer.buffer().data() + ((2 * 5 + 2) * 5 + 4) * 3;
    CHECK((p[0] | p[1] | p[2]) != 0);
}


// --- Skipping a repeated frame -----------------------------------------------------------------
// The render loop outruns the source (60 Hz against 30 fps video, or a still picture) and re-averaging a frame already on the strip buys nothing. What the effect advances then runs at the source's rate, which is the rate it should run at.

// A skipped tick must leave the strip exactly as it was, not half-painted or cleared.
TEST_CASE("AmbilightEffect: a repeated frame leaves the strip untouched") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.render();

    uint8_t before[3];
    std::memcpy(before, rig.px(4, 0), 3);
    for (int i = 0; i < 5; i++) rig.tickOnly();   // no new frame published
    CHECK(std::memcmp(before, rig.px(4, 0), 3) == 0);
}

// Turning detection off has to clear the hysteresis, not only the adopted bars. A surviving candidate with a saturated counter agrees with itself the moment detection returns, so the adoption branch never fires again and the control looks dead.
TEST_CASE("AmbilightEffect: detection can be turned off and on again") {
    Letterbox src(64, 64, 16);
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.detectBlackBars = true;
    rig.render();
    for (int i = 0; i < 60; i++) rig.tickOnly(src.svc);
    REQUIRE(rig.px(4, 0)[1] > 100);              // bars adopted, reading the picture

    rig.fx.detectBlackBars = false;
    rig.tickOnly(src.svc);
    CHECK(rig.px(4, 0)[1] == 0);                 // back to the frame, so back inside the bar

    rig.fx.detectBlackBars = true;
    for (int i = 0; i < 60; i++) rig.tickOnly(src.svc);
    CHECK(rig.px(4, 0)[1] > 100);                // and adopted again rather than stuck
}

// Averaging happens in LINEAR light rather than in the encoding: half black and half white is half the light, which sRGB writes as 188 where the byte mean is 128.
TEST_CASE("AmbilightEffect: a part-lit zone averages as light, not as bytes") {
    PatternSource src;
    Rig rig(1, 1);
    rig.fx.saturation = 100; // identity, so the zone mean is what we read
    rig.render();
    CHECK(rig.px(0, 0)[0] > 120);
    CHECK(rig.px(0, 0)[0] < 180);
}
