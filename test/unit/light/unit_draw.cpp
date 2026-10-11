/// @module draw

#include "doctest.h"
#include "light/powerfunctions/draw.h"

#include <cstdlib>
#include <vector>   // the upscale tests' planes: GCC needs it named

using namespace mm;

namespace {
// A small helper: read the RGB at (x,y,z) on a w×h×d, cpl=3 buffer.
RGB at(Buffer& b, Coord3D dims, lengthType x, lengthType y, lengthType z) {
    const size_t off = (static_cast<size_t>(z) * dims.y * dims.x
                        + static_cast<size_t>(y) * dims.x + x) * 3;
    const uint8_t* d = b.data();
    return {d[off], d[off + 1], d[off + 2]};
}
bool isBlack(RGB c) { return c.r == 0 && c.g == 0 && c.b == 0; }
}  // namespace

// mm::draw::pixel() writes inside the grid and silently clips outside it (no out-of-bounds write).
TEST_CASE("draw: pixel writes in-bounds and clips out-of-bounds") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(dims.x * dims.y * dims.z, 3));

    draw::pixel(buf, dims, {2, 1, 0}, {10, 20, 30});
    CHECK(at(buf, dims, 2, 1, 0).r == 10);
    CHECK(at(buf, dims, 2, 1, 0).g == 20);
    CHECK(at(buf, dims, 2, 1, 0).b == 30);

    // Out-of-bounds (negative + past the edge) must be a no-op, not a crash/overwrite.
    draw::pixel(buf, dims, {-1, 0, 0}, {99, 99, 99});
    draw::pixel(buf, dims, {4, 0, 0}, {99, 99, 99});
    draw::pixel(buf, dims, {0, 0, 5}, {99, 99, 99});
    // The one written pixel is still the only non-black one.
    int lit = 0;
    for (lengthType y = 0; y < dims.y; y++)
        for (lengthType x = 0; x < dims.x; x++)
            if (!isBlack(at(buf, dims, x, y, 0))) lit++;
    CHECK(lit == 1);
}

// A 1D line (a row): every pixel from a.x to b.x inclusive is lit.
TEST_CASE("draw: line fills a 1D row inclusive of both endpoints") {
    Buffer buf;
    Coord3D dims{8, 1, 1};
    REQUIRE(buf.allocate(8, 3));
    draw::line(buf, dims, {2, 0, 0}, {6, 0, 0}, {255, 0, 0});
    for (lengthType x = 0; x < 8; x++) {
        const bool shouldLit = (x >= 2 && x <= 6);
        CHECK(isBlack(at(buf, dims, x, 0, 0)) == !shouldLit);
    }
}

// A 2D diagonal: endpoints are lit and the line is contiguous (one pixel per step on the main diagonal of a square).
TEST_CASE("draw: line draws a 2D diagonal, endpoints inclusive") {
    Buffer buf;
    Coord3D dims{5, 5, 1};
    REQUIRE(buf.allocate(25, 3));
    draw::line(buf, dims, {0, 0, 0}, {4, 4, 0}, {0, 255, 0});
    // The exact diagonal cells are lit.
    for (lengthType i = 0; i < 5; i++) CHECK_FALSE(isBlack(at(buf, dims, i, i, 0)));
    // An off-diagonal corner is not.
    CHECK(isBlack(at(buf, dims, 4, 0, 0)));
}

// A 3D line: drives all three axes, endpoints lit, no out-of-bounds on a small cube.
TEST_CASE("draw: line spans a 3D cube diagonal") {
    Buffer buf;
    Coord3D dims{4, 4, 4};
    REQUIRE(buf.allocate(64, 3));
    draw::line(buf, dims, {0, 0, 0}, {3, 3, 3}, {0, 0, 255});
    CHECK_FALSE(isBlack(at(buf, dims, 0, 0, 0)));   // start endpoint
    CHECK_FALSE(isBlack(at(buf, dims, 3, 3, 3)));   // end endpoint
}

// A line running off the grid clips: it draws the on-grid part and stops, no crash.
TEST_CASE("draw: a line partly off the grid clips cleanly") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    draw::line(buf, dims, {2, 2, 0}, {10, 2, 0}, {255, 255, 255});  // runs off the right edge
    CHECK_FALSE(isBlack(at(buf, dims, 2, 2, 0)));   // on-grid start lit
    CHECK_FALSE(isBlack(at(buf, dims, 3, 2, 0)));   // last on-grid cell lit
    // (cells x>=4 don't exist; the test passing without a crash proves the clip)
}

// The `shorten` parameter pulls the far endpoint back toward `a` by shorten/255 (with WLEDMM *2 rounding), so an effect can sweep a partial segment. For a→b = (0,0)→(8,0): shorten 255 draws the whole line (tip at 8), 128 ≈ half (tip at (16*128/255+1)/2 = 4), 1 = just the start pixel (tip 0), 0 = nothing. This pins the rounding of the shorten branch.
TEST_CASE("draw: line shorten pulls the far endpoint back toward the start") {
    Coord3D dims{9, 1, 1};
    auto litUpTo = [&](uint8_t shorten) {
        Buffer buf; REQUIRE(buf.allocate(9, 3));
        draw::line(buf, dims, {0, 0, 0}, {8, 0, 0}, {0, 0, 255}, shorten);
        int last = -1;
        for (lengthType x = 0; x < 9; x++) if (!isBlack(at(buf, dims, x, 0, 0))) last = x;
        return last;   // highest lit x, or -1 if nothing drawn
    };
    CHECK(litUpTo(255) == 8);   // full line reaches the far endpoint
    CHECK(litUpTo(128) == 4);   // ~half: tip pulled back to x=4
    CHECK(litUpTo(1)   == 0);   // only the start pixel
    CHECK(litUpTo(0)   == -1);  // shorten 0 draws nothing
    // Monotonic: a larger shorten never draws a shorter segment.
    CHECK(litUpTo(200) >= litUpTo(100));
}

namespace {
// Reference blur (the FastLED blur1d carryover-seep, written the slow-but-obvious way) along x for one row, used to pin draw::blur's fast byte-level pass to the canonical behavior. Mirrors MoonLight's blurRows for a single row.
void refBlurRowX(Buffer& b, Coord3D dims, lengthType y, lengthType z, uint8_t amt) {
    const uint8_t keep = static_cast<uint8_t>(255 - amt), seep = static_cast<uint8_t>(amt >> 1);
    uint8_t* d = b.data();
    auto at3 = [&](lengthType x) { return d + (static_cast<size_t>(z) * dims.y * dims.x + static_cast<size_t>(y) * dims.x + x) * 3; };
    uint8_t cr = 0, cg = 0, cb = 0;
    for (lengthType x = 0; x < dims.x; x++) {
        uint8_t* px = at3(x);
        const uint8_t pr = scale8(px[0], seep), pg = scale8(px[1], seep), pb = scale8(px[2], seep);
        px[0] = qadd8(scale8(px[0], keep), cr); px[1] = qadd8(scale8(px[1], keep), cg); px[2] = qadd8(scale8(px[2], keep), cb);
        if (x) { uint8_t* pv = at3(x - 1); pv[0] = qadd8(pv[0], pr); pv[1] = qadd8(pv[1], pg); pv[2] = qadd8(pv[2], pb); }
        cr = pr; cg = pg; cb = pb;
    }
    if (dims.x) { uint8_t* last = at3(dims.x - 1); last[0] = qadd8(last[0], cr); last[1] = qadd8(last[1], cg); last[2] = qadd8(last[2], cb); }
}
}  // namespace

// draw::blur on a 1D row matches the canonical carryover-seep reference byte-for-byte (same behavior as FastLED blur1d / MoonLight blurRows), and is symmetric around a centerd bright pixel.
TEST_CASE("draw: blur matches the reference carryover-seep on a 1D row") {
    Buffer got, ref;
    Coord3D dims{5, 1, 1};
    REQUIRE(got.allocate(5, 3));
    REQUIRE(ref.allocate(5, 3));
    // A single white pixel in the center of both buffers.
    draw::pixel(got, dims, {2, 0, 0}, {255, 255, 255});
    draw::pixel(ref, dims, {2, 0, 0}, {255, 255, 255});

    draw::blur(got, dims, 128);
    refBlurRowX(ref, dims, 0, 0, 128);

    for (lengthType x = 0; x < 5; x++) {
        const RGB g = at(got, dims, x, 0, 0), r = at(ref, dims, x, 0, 0);
        CHECK(g.r == r.r); CHECK(g.g == r.g); CHECK(g.b == r.b);
    }
    // Center stays brightest, the two immediate neighbors are equally lit (symmetry), the center still has the most energy, and it spread outward (neighbors non-black).
    CHECK(at(got, dims, 1, 0, 0).r == at(got, dims, 3, 0, 0).r);
    CHECK(at(got, dims, 2, 0, 0).r > at(got, dims, 1, 0, 0).r);
    CHECK_FALSE(isBlack(at(got, dims, 1, 0, 0)));
    CHECK_FALSE(isBlack(at(got, dims, 3, 0, 0)));
}

// blur runs separably on every axis with extent>1: a 2D blur spreads a center pixel to all four orthogonal neighbors; a 3D blur reaches the z neighbors too. And it never writes out of bounds.
TEST_CASE("draw: blur spreads in 2D and 3D and is safe at degenerate sizes") {
    {   // 2D: center pixel of a 5×5 reaches its 4 orthogonal neighbors.
        Buffer buf; Coord3D dims{5, 5, 1};
        REQUIRE(buf.allocate(25, 3));
        draw::pixel(buf, dims, {2, 2, 0}, {255, 255, 255});
        draw::blur(buf, dims, 160);
        CHECK_FALSE(isBlack(at(buf, dims, 1, 2, 0)));   // -x
        CHECK_FALSE(isBlack(at(buf, dims, 3, 2, 0)));   // +x
        CHECK_FALSE(isBlack(at(buf, dims, 2, 1, 0)));   // -y
        CHECK_FALSE(isBlack(at(buf, dims, 2, 3, 0)));   // +y
        // x/y symmetry: the four orthogonal neighbors carry equal energy.
        CHECK(at(buf, dims, 1, 2, 0).r == at(buf, dims, 2, 1, 0).r);
    }
    {   // 3D: the z neighbors light up too.
        Buffer buf; Coord3D dims{3, 3, 3};
        REQUIRE(buf.allocate(27, 3));
        draw::pixel(buf, dims, {1, 1, 1}, {255, 255, 255});
        draw::blur(buf, dims, 160);
        CHECK_FALSE(isBlack(at(buf, dims, 1, 1, 0)));   // -z
        CHECK_FALSE(isBlack(at(buf, dims, 1, 1, 2)));   // +z
    }
    {   // Degenerate: amt=0 is a no-op; 1×1×1 and a single-pixel axis don't crash.
        Buffer buf; Coord3D dims{1, 1, 1};
        REQUIRE(buf.allocate(1, 3));
        draw::pixel(buf, dims, {0, 0, 0}, {200, 100, 50});
        draw::blur(buf, dims, 255);                     // nothing to seep: must be a safe no-op
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
        draw::blur(buf, dims, 0);                       // amt 0 returns immediately
        CHECK(at(buf, dims, 0, 0, 0).r == 200);
    }
}

// A glyph blits in the correct orientation: neither X-mirrored (a 'b' as a 'd') nor Y-flipped. 'L' is the ideal probe: its vertical bar must be on the LEFT and its foot on the BOTTOM row. This guards the column-bit and row-direction reads, so the DemoReel name overlay renders each letter upright and un-mirrored.
TEST_CASE("draw: glyph renders upright and un-mirrored (the 'L' probe)") {
    Buffer buf;
    const auto& f = fonts::kFont6x8;                 // 6x8: 'L' bar at col 1, foot on row 6
    Coord3D dims{static_cast<lengthType>(f.width), static_cast<lengthType>(f.height), 1};
    REQUIRE(buf.allocate(f.width * f.height, 3));
    draw::glyph(buf, dims, f, 'L', 0, 0, {255, 255, 255});

    // The vertical bar is on the LEFT (column 1 lit down the height), not mirrored to the right.
    int litLeft = 0, litRight = 0;
    for (lengthType y = 0; y < f.height; y++) {
        if (!isBlack(at(buf, dims, 1, y, 0))) litLeft++;                       // left bar column
        if (!isBlack(at(buf, dims, static_cast<lengthType>(f.width - 1), y, 0))) litRight++;  // right edge
    }
    CHECK(litLeft >= 6);    // the bar runs down most of the glyph on the left
    CHECK(litRight <= 1);   // the right edge only carries the foot's last pixel (not a mirrored bar)

    // The foot is on the BOTTOM row (highest y), and the top row is just the bar (not the foot).
    int bottomLit = 0;
    for (lengthType x = 0; x < f.width; x++)
        if (!isBlack(at(buf, dims, x, static_cast<lengthType>(f.height - 2), 0))) bottomLit++;  // row 6 (row 7 is blank)
    CHECK(bottomLit >= 4);  // the foot spans several columns near the bottom
    // The top row has only the single bar pixel, not the foot: so top != bottom (Y not flipped).
    int topLit = 0;
    for (lengthType x = 0; x < f.width; x++)
        if (!isBlack(at(buf, dims, x, 0, 0))) topLit++;
    CHECK(topLit == 1);     // just the bar at the top
}

// draw::sprite, the multi-color sibling of glyph: palette-indexed frames with index 0 as the transparent key. These pin the contract a screensaver effect stands on: the right frame at the right place, holes where the key is, silence at the edges and on bad indices.
TEST_CASE("draw::sprite blits the requested frame with transparent holes") {
    Buffer buf;
    Coord3D dims{6, 4, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    static constexpr RGB pal[] = {{0, 0, 0}, {10, 20, 30}, {40, 50, 60}};
    // 2x2, 2 frames: frame 0 all color 1; frame 1 = color 2 with a transparent hole at (1,0).
    static constexpr uint8_t px[] = {1, 1, 1, 1,   2, 0, 2, 2};
    constexpr draw::sprites::Sprite s{px, pal, 2, 2, 2, 3};

    draw::sprite(cv, s, 1, 1, 1);
    CHECK(at(buf, dims, 1, 1, 0).r == 40);          // frame 1's color, not frame 0's
    CHECK(isBlack(at(buf, dims, 2, 1, 0)));          // the transparent hole
    CHECK(at(buf, dims, 1, 2, 0).r == 40);
    CHECK(at(buf, dims, 2, 2, 0).r == 40);
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));          // untouched background
}

TEST_CASE("draw::sprite clips at every edge and survives bad frame and palette indices") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    static constexpr RGB pal[] = {{0, 0, 0}, {99, 0, 0}};
    static constexpr uint8_t px[] = {1, 1, 1, 1};   // 2x2, 1 frame, all color 1
    constexpr draw::sprites::Sprite s{px, pal, 2, 2, 1, 2};

    draw::sprite(cv, s, 0, -1, -1);                  // upper-left: only (0,0) lands
    CHECK(at(buf, dims, 0, 0, 0).r == 99);
    draw::sprite(cv, s, 0, 3, 3);                    // lower-right: only (3,3) lands
    CHECK(at(buf, dims, 3, 3, 0).r == 99);
    CHECK(isBlack(at(buf, dims, 2, 2, 0)));

    draw::sprite(cv, s, 200, 1, 1);                  // out-of-range frame clamps to the last
    CHECK(at(buf, dims, 1, 1, 0).r == 99);

    static constexpr uint8_t bad[] = {9, 9, 9, 9};   // indices past the palette: render nothing
    constexpr draw::sprites::Sprite sBad{bad, pal, 2, 2, 1, 2};
    buf.clear();
    draw::sprite(cv, sBad, 0, 1, 1);
    CHECK(isBlack(at(buf, dims, 1, 1, 0)));
}

// Art that faces one way serves both: flipX mirrors the sprite's READ, so the blit still lands at the same (x, y) with the same footprint rather than needing a mirrored copy of every frame.
TEST_CASE("draw::sprite mirrors horizontally without moving the sprite") {
    Buffer buf;
    Coord3D dims{4, 2, 1};
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(dims.x) * dims.y, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, dims.x, dims.y, 1);

    // Asymmetric on purpose: one lit pixel at the LEFT of the top row.
    static constexpr RGB pal[] = {{0, 0, 0}, {10, 20, 30}};
    static constexpr uint8_t px[] = {1, 0, 0, 0,
                                     0, 0, 0, 0};
    constexpr draw::sprites::Sprite s{px, pal, 4, 2, 1, 2};

    draw::sprite(cv, s, 0, 0, 0, 1, /*flipX=*/false);
    CHECK(at(buf, dims, 0, 0, 0).r == 10);       // unflipped: leftmost column
    CHECK(isBlack(at(buf, dims, 3, 0, 0)));

    buf.clear();
    draw::sprite(cv, s, 0, 0, 0, 1, /*flipX=*/true);
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));      // flipped: the same footprint, mirrored
    CHECK(at(buf, dims, 3, 0, 0).r == 10);
}

// draw::decay is the framerate-independent trail fade: a duration, not a per-frame amount. These pin the property a user actually sees, which is that the same effect looks the same on a slow device and a fast one.

TEST_CASE("decay dims a plane by half over one half-life at a realistic frame time") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 4, 1);
    draw::fill(cv, RGB{200, 200, 200});
    for (int i = 0; i < 10; i++) draw::decay(cv, 500, 50);      // 500 ms at a 20 fps cadence
    const RGB c = at(buf, dims, 1, 1, 0);
    CHECK(c.r > 90);                       // half of 200, allowing for integer rounding
    CHECK(c.r < 105);
}

TEST_CASE("a 16-bit trail plane decays at the same rate whatever the framerate") {
    // The property a byte plane CANNOT hold: re-rounding a byte hundreds of times a second either erases the trail (truncating) or freezes it solid (rounding). Measured, decaying 200 over a 500 ms half-life in 500 ms of frames, where the exact answer is 100: a byte plane gives 96 at 50 ms frames, 73 at 5 ms and 0 at 1 ms. The wide plane below holds 100/101/102.
    auto runWide = [](int frames, uint32_t dt) {
        uint16_t plane[16];
        for (uint16_t& v : plane) v = 200 * 257;               // 200 widened to 16 bits
        for (int i = 0; i < frames; i++) draw::decay16(plane, 16, 500, dt);
        return static_cast<int>(plane[5] / 257);               // narrowed back to a byte
    };
    const int slow = runWide(10, 50);       // 20 fps
    const int mid  = runWide(100, 5);       // 200 fps
    const int fast = runWide(500, 1);       // 1000 fps
    CHECK(slow > 95);
    CHECK(slow < 105);
    CHECK(std::abs(slow - mid) <= 3);
    CHECK(std::abs(slow - fast) <= 3);      // the framerate independence the half-life form is for
}

TEST_CASE("decay leaves a plane alone when no time has passed, and clears it after a long stall") {
    Buffer buf;
    Coord3D dims{4, 4, 1};
    REQUIRE(buf.allocate(16, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 4, 1);
    draw::fill(cv, RGB{123, 45, 67});

    draw::decay(cv, 500, 0);               // a frame that took no time changes nothing
    CHECK(at(buf, dims, 0, 0, 0).r == 123);
    draw::decay(cv, 0, 100);               // no half-life asked for: also nothing
    CHECK(at(buf, dims, 0, 0, 0).g == 45);

    draw::decay(cv, 10, 100000);           // a long stall goes black rather than wrapping bright
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));
}

// draw::advect moves a plane along a velocity field: the transport half of a flow, and what a trail is made of. It samples BACKWARD, so every destination pixel is written exactly once.

TEST_CASE("advect carries the picture along the flow, one whole pixel at a time") {
    Buffer src, dst;
    Coord3D dims{8, 8, 1};
    REQUIRE(src.allocate(64, 3));
    REQUIRE(dst.allocate(64, 3));
    const draw::Canvas s = draw::Canvas::of(src, 8, 8, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 8, 8, 1);
    draw::pixel(s, {2, 3, 0}, RGB{200, 100, 50});

    // One pixel to the right per frame, so what was at x=2 must be found at x=3.
    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    });
    CHECK(at(dst, dims, 3, 3, 0).r == 200);
    CHECK(isBlack(at(dst, dims, 2, 3, 0)));
}

TEST_CASE("a uniform field survives being advected, so a flow does not dim what it carries") {
    // The property that separates transport from blur: moving a region of equal values must not change them, whatever the sub-pixel offset. A half-pixel step is the worst case, since it blends two neighbors at full weight.
    Buffer src, dst;
    Coord3D dims{8, 8, 1};
    REQUIRE(src.allocate(64, 3));
    REQUIRE(dst.allocate(64, 3));
    const draw::Canvas s = draw::Canvas::of(src, 8, 8, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 8, 8, 1);
    draw::fill(s, RGB{180, 180, 180});
    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne / 2; vy = draw::kSubOne / 2;
    });
    CHECK(at(dst, dims, 4, 4, 0).r == 180);
}

TEST_CASE("the edge rule decides whether a flow loops the grid or leaves it") {
    Buffer src, dst;
    Coord3D dims{4, 4, 1};
    REQUIRE(src.allocate(16, 3));
    REQUIRE(dst.allocate(16, 3));
    const draw::Canvas s = draw::Canvas::of(src, 4, 4, 1);
    const draw::Canvas d = draw::Canvas::of(dst, 4, 4, 1);
    draw::pixel(s, {3, 1, 0}, RGB{255, 0, 0});    // lit at the right edge

    auto right = [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    };
    // Wrapping: what leaves the right edge arrives at the left.
    draw::advect(d, s, right, draw::Edge::Wrap);
    CHECK(at(dst, dims, 0, 1, 0).r == 255);
    // Clamping: it does not come back, and the edge column holds its own value instead.
    draw::fill(d, RGB{0, 0, 0});
    draw::advect(d, s, right, draw::Edge::Clamp);
    CHECK(isBlack(at(dst, dims, 0, 1, 0)));
}

TEST_CASE("advect moves every slice of a volume, so a cube flows like a panel") {
    // 3D is the default shape for this phase: the bench fixture is a 20-cube. A D2 rule leaves z alone, and each slice must still be carried.
    Buffer src, dst;
    Coord3D dims{4, 4, 3};
    REQUIRE(src.allocate(48, 3));
    REQUIRE(dst.allocate(48, 3));
    const draw::Canvas s = draw::Canvas::of(src, 4, 4, 3);
    const draw::Canvas d = draw::Canvas::of(dst, 4, 4, 3);
    draw::pixel(s, {1, 1, 0}, RGB{90, 0, 0});
    draw::pixel(s, {1, 1, 2}, RGB{200, 0, 0});     // a different value in the far slice

    draw::advect(d, s, [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::kSubOne; vy = 0;
    });
    CHECK(at(dst, dims, 2, 1, 0).r == 90);         // each slice carried its own content
    CHECK(at(dst, dims, 2, 1, 2).r == 200);
}

// disc and sphere: the SDF-shaded fills, where a sub-pixel center means a small shape can move between cells instead of jumping one at a time.

TEST_CASE("a disc lights its interior and softens its edge") {
    Buffer buf;
    Coord3D dims{9, 9, 1};
    REQUIRE(buf.allocate(81, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 9, 9, 1);
    draw::disc(cv, draw::toSub(4), draw::toSub(4), draw::toSub(3), RGB{255, 255, 255});

    CHECK(at(buf, dims, 4, 4, 0).r == 255);            // the middle is solid
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));            // a far corner is untouched
    // The rim is partial: neither full nor black, which is the anti-aliasing.
    const uint8_t rim = at(buf, dims, 4, 1, 0).r;
    CHECK(rim > 0);
    CHECK(rim < 255);
}

TEST_CASE("two overlapping discs brighten where they meet, because light adds") {
    Buffer buf;
    Coord3D dims{8, 8, 1};
    REQUIRE(buf.allocate(64, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    draw::disc(cv, draw::toSub(3), draw::toSub(4), draw::toSub(2), RGB{100, 0, 0});
    const uint8_t single = at(buf, dims, 4, 4, 0).r;
    draw::disc(cv, draw::toSub(5), draw::toSub(4), draw::toSub(2), RGB{100, 0, 0});
    CHECK(at(buf, dims, 4, 4, 0).r > single);          // the overlap is brighter than one alone
}

TEST_CASE("a sphere fills a volume, so a cube gets a ball rather than a stack of discs") {
    Buffer buf;
    Coord3D dims{7, 7, 7};
    REQUIRE(buf.allocate(343, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 7, 7, 7);
    draw::sphere(cv, draw::toSub(3), draw::toSub(3), draw::toSub(3), draw::toSub(2),
                 RGB{255, 255, 255});
    CHECK(at(buf, dims, 3, 3, 3).r == 255);            // the center of the volume
    CHECK(at(buf, dims, 3, 3, 1).r > 0);               // and it reaches along z
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));            // but not into the corners
}

// The velocity rules: plain functions, so an effect can drive them from anything.

TEST_CASE("the wind blows every point the same way") {
    draw::pos_t vx = 0, vy = 0;
    draw::flowWind(0, 256, vx, vy);                    // angle 0 is +x
    CHECK(vx > 200);
    CHECK(vy > -20);
    CHECK(vy < 20);
    draw::flowWind(16384, 256, vx, vy);                // a quarter turn is +y
    CHECK(vy > 200);
}

TEST_CASE("a radial flow points away from its center, and inward when reversed") {
    draw::pos_t vx = 0, vy = 0;
    draw::flowRadial(6, 3, 3, 3, 256, vx, vy);         // to the right of center: pushed further right
    CHECK(vx > 100);
    draw::flowRadial(6, 3, 3, 3, -256, vx, vy);        // negative speed draws it back in
    CHECK(vx < -100);
    // The center itself has no direction to move, and must not divide by zero reaching for one.
    draw::flowRadial(3, 3, 3, 3, 256, vx, vy);
    CHECK(vx == 0);
    CHECK(vy == 0);
}

TEST_CASE("a spiral is a radial flow with a turn added, so it both circles and escapes") {
    draw::pos_t rx = 0, ry = 0, sx = 0, sy = 0;
    draw::flowRadial(6, 3, 3, 3, 256, rx, ry);
    draw::flowSpiral(6, 3, 3, 3, 256, 256, sx, sy);
    CHECK(sx == rx);                                   // the outward part is the same
    CHECK(sy != ry);                                   // and the angular part is what it adds
}

// draw::quantize is the 16-to-8 boundary every wide pipeline ends at. What the modes buy is not more levels, it is where the discarded half of the value goes.

TEST_CASE("truncating a slow gradient bands it, and dithering restores its true mean") {
    // A dark, slow ramp is the worst case: 64 distinct 16-bit values that truncation flattens to a handful of steps. Neither mode can add levels to a single frame; what ordered dithering fixes is the MEAN, so the band sits where the real value is rather than below it.
    double trueSum = 0, truncSum = 0, ditherSum = 0;
    uint8_t carry = 0;
    for (int i = 0; i < 64; i++) {
        const uint16_t v = static_cast<uint16_t>(65535 * 0.10 + (65535 * 0.02) * i / 64);
        trueSum += v / 257.0;
        truncSum += draw::quantize(v, draw::Dither::None, carry);
        ditherSum += draw::quantize(v, draw::Dither::Ordered, carry, i % 8, i / 8);
    }
    const double trueMean = trueSum / 64, truncMean = truncSum / 64, ditherMean = ditherSum / 64;
    CHECK(std::abs(ditherMean - trueMean) < std::abs(truncMean - trueMean));   // closer to the truth
    CHECK(std::abs(ditherMean - trueMean) < 0.5);
}

TEST_CASE("temporal dithering resolves neighbors that a single frame cannot") {
    // The property that makes a 16-bit pipeline worth having on 8-bit LEDs: eight values a fraction of a byte apart all truncate to the same one or two levels, but averaged over frames they separate, and the eye does that averaging. This is what turns a stepped fade into a smooth one.
    constexpr int kLights = 8, kFrames = 32;
    uint16_t v[kLights];
    uint8_t carry[kLights] = {};
    long sum[kLights] = {};
    for (int i = 0; i < kLights; i++) v[i] = static_cast<uint16_t>(65535 * 0.10 + 40 * i);

    for (int f = 0; f < kFrames; f++)
        for (int i = 0; i < kLights; i++) sum[i] += draw::quantize(v[i], draw::Dither::Temporal, carry[i]);

    // Every neighbor's average is distinct and ordered, which truncation cannot manage.
    for (int i = 1; i < kLights; i++) {
        const double lo = double(sum[i - 1]) / kFrames, hi = double(sum[i]) / kFrames;
        CHECK(hi > lo);
    }
    // And each tracks its own true value, not merely its neighbor's order.
    for (int i = 0; i < kLights; i++)
        CHECK(std::abs(double(sum[i]) / kFrames - v[i] / 257.0) < 0.6);
}

TEST_CASE("a dithered value never wraps past full, so a bright light cannot flash black") {
    uint8_t carry = 0;
    for (int f = 0; f < 100; f++) {
        CHECK(draw::quantize(65535, draw::Dither::Temporal, carry) == 255);
        CHECK(draw::quantize(65535, draw::Dither::Ordered, carry, f % 4, f % 4) == 255);
    }
    // Black stays black under every mode: a carry must not light an unlit pixel.
    uint8_t c2 = 0;
    for (int f = 0; f < 100; f++) CHECK(draw::quantize(0, draw::Dither::Temporal, c2) == 0);
}

TEST_CASE("the ordered pattern differs per z, so a volume does not repeat one texture") {
    // On a cube every slice would otherwise share a threshold, and the dither would read as a pattern stamped through the volume rather than as noise.
    uint8_t carry = 0;
    const uint16_t v = 0x2080;                      // a value squarely between two byte levels
    bool differs = false;
    for (lengthType z = 1; z < 4; z++)
        for (lengthType y = 0; y < 4 && !differs; y++)
            for (lengthType x = 0; x < 4 && !differs; x++)
                if (draw::quantize(v, draw::Dither::Ordered, carry, x, y, z)
                    != draw::quantize(v, draw::Dither::Ordered, carry, x, y, 0)) differs = true;
    CHECK(differs);
}

// draw::upscale16 is the fieldScale lever: compute a smooth field at a fraction of the fixture's resolution and interpolate the rest, which is nearly free visually because a field is smooth.

TEST_CASE("an upscaled plane keeps a uniform value, so a flat field does not gain texture") {
    std::vector<uint16_t> src(4 * 4 * 3, 30000), dst(16 * 16 * 3, 0);
    std::vector<draw::UpscaleTap> taps(16);
    draw::upscale16(dst.data(), 16, 16, 1, src.data(), 4, 4, 1, taps.data(), taps.size());
    for (uint16_t v : dst) CHECK(v == 30000);
}

TEST_CASE("an upscaled ramp stays monotonic, so a gradient does not gain steps or reversals") {
    // 8 wide, ramping left to right; stretched to 32. Every step must be non-decreasing, which is what a bilinear stretch guarantees and a nearest-neighbor one would not.
    std::vector<uint16_t> src(8 * 3), dst(32 * 3, 0);
    for (int x = 0; x < 8; x++)
        for (int c = 0; c < 3; c++) src[x * 3 + c] = static_cast<uint16_t>(x * 8000);
    std::vector<draw::UpscaleTap> taps(32);
    draw::upscale16(dst.data(), 32, 1, 1, src.data(), 8, 1, 1, taps.data(), taps.size());
    for (int x = 1; x < 32; x++) CHECK(dst[x * 3] >= dst[(x - 1) * 3]);
    CHECK(dst[0] == 0);                                  // the ends reach the source's ends
    CHECK(dst[31 * 3] == 7 * 8000);
}

TEST_CASE("a flat field stretches across a volume's depth without z work") {
    // A 2D field on a 3D fixture: every slice gets the same picture, which is what a depth-1 source means, and it costs no interpolation along z.
    std::vector<uint16_t> src(4 * 4 * 3), dst(8 * 8 * 4 * 3, 0);
    for (size_t i = 0; i < src.size(); i++) src[i] = static_cast<uint16_t>(i * 700);
    std::vector<draw::UpscaleTap> taps(8);
    draw::upscale16(dst.data(), 8, 8, 4, src.data(), 4, 4, 1, taps.data(), taps.size());
    const size_t slice = 8 * 8 * 3;
    for (size_t z = 1; z < 4; z++)
        for (size_t i = 0; i < slice; i++) REQUIRE(dst[i] == dst[z * slice + i]);
}

TEST_CASE("a volumetric field interpolates along z as well, so a coarse cube fills a fine one") {
    // Two source slices, black and white. The destination's middle slices must land between them rather than snapping to one.
    std::vector<uint16_t> src(2 * 2 * 2 * 3), dst(4 * 4 * 8 * 3, 0);
    for (int z = 0; z < 2; z++)
        for (int i = 0; i < 2 * 2 * 3; i++) src[z * 12 + i] = z ? 60000 : 0;
    std::vector<draw::UpscaleTap> taps(4);
    draw::upscale16(dst.data(), 4, 4, 8, src.data(), 2, 2, 2, taps.data(), taps.size());
    const size_t slice = 4 * 4 * 3;
    CHECK(dst[0] == 0);                                   // the near face is still black
    CHECK(dst[7 * slice] == 60000);                       // the far face still white
    CHECK(dst[3 * slice] > 5000);                         // and the middle is genuinely between
    CHECK(dst[3 * slice] < 55000);
}

TEST_CASE("an upscaled saddle stays inside the values it was given, so a field that dips does not light up") {
    // The fixtures above are uniform or monotone, so every interpolation ascends. A noise field is a saddle almost everywhere: along one diagonal it rises, along the other it falls. A descending pair is where an unsigned difference wraps, and blending a wrapped row against an unwrapped one lands far outside the input range (this fixture produced 98354 for inputs of 0 and 100).
    std::vector<uint16_t> src(2 * 2 * 3, 0), dst(8 * 8 * 3, 0);
    for (int c = 0; c < 3; c++) {
        src[(0 * 2 + 0) * 3 + c] = 100;      // high, low
        src[(1 * 2 + 1) * 3 + c] = 100;      // low,  high
    }
    std::vector<draw::UpscaleTap> taps(8);
    draw::upscale16(dst.data(), 8, 8, 1, src.data(), 2, 2, 1, taps.data(), taps.size());
    for (uint16_t v : dst) CHECK(v <= 100);
}

TEST_CASE("upscale16 declines rather than writing when the caller's tap table is too small") {
    // The table is the caller's because tick() can neither allocate nor spare a large frame. A short one is a programming error, and the safe answer is to do nothing rather than run off its end.
    std::vector<uint16_t> src(4 * 4 * 3, 30000), dst(16 * 16 * 3, 7);
    std::vector<draw::UpscaleTap> taps(4);               // 16 columns need 16
    draw::upscale16(dst.data(), 16, 16, 1, src.data(), 4, 4, 1, taps.data(), taps.size());
    for (uint16_t v : dst) CHECK(v == 7);                // untouched
}

// advect16 and blit16 are what every wide-plane effect (Trails, Nebula, Fluid, and a scripted trail) actually renders through, and neither had a test: the 8-bit advect was pinned and its 16-bit sibling was not, which is how a plane-wide primitive ships unverified.

TEST_CASE("a 16-bit plane carried by a whole-cell flow arrives intact, so light is transported rather than smeared away") {
    // One lit cell, pushed one whole cell to the right each frame. A whole-cell step has no bilinear fraction, so the value must survive exactly: any loss here is the transport itself leaking, which over a hundred frames of a trail is the difference between a tail and a haze.
    const lengthType W = 8, H = 1;
    std::vector<uint16_t> a(size_t(W) * H * 3, 0), b(a.size(), 0);
    a[(0 * 3) + 0] = 60000; a[(0 * 3) + 1] = 40000; a[(0 * 3) + 2] = 20000;
    uint16_t* src = a.data(); uint16_t* dst = b.data();
    for (int step = 1; step <= 3; step++) {
        draw::advect16(dst, src, W, H, 1,
                       [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
                           vx = draw::pos_t(draw::kSubOne);   // exactly one cell
                           vy = 0;
                       }, draw::Edge::Clamp);
        std::swap(src, dst);
        CHECK(src[(size_t(step) * 3) + 0] == 60000);
        CHECK(src[(size_t(step) * 3) + 1] == 40000);
        CHECK(src[(size_t(step) * 3) + 2] == 20000);
    }
}

TEST_CASE("a half-cell flow splits a 16-bit sample between the two cells it straddles, and loses none of it") {
    // The bilinear case: half a cell of motion puts half the light in each neighbor. What matters is that the TOTAL is conserved, which is what keeps a long trail from fading on its own.
    const lengthType W = 8;
    std::vector<uint16_t> a(size_t(W) * 3, 0), b(a.size(), 0);
    a[(4 * 3) + 0] = 40000;
    draw::advect16(b.data(), a.data(), W, 1, 1,
                   [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
                       vx = draw::pos_t(draw::kSubOne / 2); vy = 0;
                   }, draw::Edge::Clamp);
    uint32_t total = 0;
    for (lengthType x = 0; x < W; x++) total += b[(size_t(x) * 3)];
    CHECK(total >= 39000);                      // conserved, bar the fixed-point rounding
    CHECK(total <= 40000);
    CHECK(b[(4 * 3)] > 0);                      // and it straddles the two cells
    CHECK(b[(5 * 3)] > 0);
}

TEST_CASE("a flow off the edge circulates under Wrap and carries the light out of the grid under Clamp") {
    // The advection samples BACKWARD (every destination asks where its contents came from), so a leftward flow means cell x reads cell x+1. Under Wrap the left column reads the right one and the light comes round; under Clamp nothing reads past the wall, so light that flows off the edge is GONE rather than piling up against it. Both are correct and an effect picks one: a trail that should circulate wants Wrap, a fluid in a box wants Clamp.
    const lengthType W = 4;
    std::vector<uint16_t> a(size_t(W) * 3, 0), b(a.size(), 0);
    a[(0 * 3)] = 50000;                          // at the left wall, pushed further left
    auto push = [](lengthType, lengthType, lengthType, draw::pos_t& vx, draw::pos_t& vy) {
        vx = draw::pos_t(-draw::kSubOne); vy = 0;
    };
    draw::advect16(b.data(), a.data(), W, 1, 1, push, draw::Edge::Wrap);
    CHECK(b[(size_t(W - 1) * 3)] == 50000);      // came round the far side, intact
    std::fill(b.begin(), b.end(), uint16_t(0));
    draw::advect16(b.data(), a.data(), W, 1, 1, push, draw::Edge::Clamp);
    uint32_t total = 0;
    for (lengthType x = 0; x < W; x++) total += b[(size_t(x) * 3)];
    CHECK(total == 0);                           // left the grid rather than banking against the wall
}

TEST_CASE("blit16 narrows a wide plane to the canvas, and dithering carries the error a truncation would drop") {
    // A value just under the halfway point of an 8-bit step: truncation reports the lower step on every frame forever, while the carry accumulates and reaches the higher one part of the time. That difference IS the smooth fade the 16-bit planes exist for.
    const lengthType W = 4, H = 4;
    Buffer buf;
    REQUIRE(buf.allocate(static_cast<nrOfLightsType>(W) * H, 3));
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, W, H, 1);
    std::vector<uint16_t> plane(size_t(W) * H * 3, uint16_t(0x01C0));   // 1.75 of an 8-bit step
    std::vector<uint8_t> carry(plane.size(), 0);

    draw::blit16(cv, plane.data(), W, H, 1, nullptr);
    for (size_t i = 0; i < size_t(W) * H * 3; i++) REQUIRE(buf.data()[i] == 1);   // truncated, always

    int higher = 0;
    for (int frame = 0; frame < 8; frame++) {
        draw::blit16(cv, plane.data(), W, H, 1, carry.data());
        if (buf.data()[0] == 2) higher++;
    }
    CHECK(higher > 0);                            // the carry reaches the step above
    CHECK(higher < 8);                            // but not on every frame: it is a ratio, not a bias
}

TEST_CASE("scrolling along y moves every column, and along z every cell of the volume") {
    // The lines of a y-scroll are the (z, x) COLUMNS, and those are not evenly spaced by one stride: x steps by a light within a slice, z steps by a whole slice. Treating them as one evenly-spaced sequence scrolled the first column of each slice and left the rest standing, which reads as a partial scroll nobody would call a scroll.
    Buffer buf; REQUIRE(buf.allocate(9, 3)); buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 3, 3, 1);
    for (lengthType y = 0; y < 3; y++)
        for (lengthType x = 0; x < 3; x++)
            draw::pixel(cv, {x, y, 0}, RGB{static_cast<uint8_t>(10 * (y + 1)), static_cast<uint8_t>(x + 1), 0});
    draw::scroll(cv, 1, 1, true);                     // rotate down by one row
    for (lengthType x = 0; x < 3; x++) {              // EVERY column rotated, and kept its own tag
        CHECK(buf.data()[(size_t(0) * 3 + x) * 3] == 30);
        CHECK(buf.data()[(size_t(1) * 3 + x) * 3] == 10);
        CHECK(buf.data()[(size_t(2) * 3 + x) * 3] == 20);
        CHECK(buf.data()[(size_t(0) * 3 + x) * 3 + 1] == x + 1);
    }

    // The z axis has the same shape: its lines are the (y, x) cells, one per column of the volume.
    Buffer vol; REQUIRE(vol.allocate(8, 3)); vol.clear();
    const draw::Canvas cube = draw::Canvas::of(vol, 2, 2, 2);
    for (lengthType z = 0; z < 2; z++)
        for (lengthType y = 0; y < 2; y++)
            for (lengthType x = 0; x < 2; x++)
                draw::pixel(cube, {x, y, z}, RGB{static_cast<uint8_t>(z + 1), 0, 0});
    draw::scroll(cube, 2, 1, true);                   // swap the two slices
    for (lengthType y = 0; y < 2; y++)
        for (lengthType x = 0; x < 2; x++) {
            CHECK(vol.data()[((size_t(0) * 2 + y) * 2 + x) * 3] == 2);   // the far slice came near
            CHECK(vol.data()[((size_t(1) * 2 + y) * 2 + x) * 3] == 1);
        }
}

// --- ring and strokeLine -----------------------------------------------------------------------
//
// The two shapes a fixed-point canvas needs beyond a disc: an outline and a thick line. Ported for Sutaburosu's fixed-point demos, which draw clock hands, orbits and spokes from them.

TEST_CASE("a ring is hollow: lit on its band, dark at its center") {
    Buffer buf;
    Coord3D dims{11, 11, 1};
    REQUIRE(buf.allocate(121, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 11, 11, 1);
    draw::ring(cv, draw::toSub(5), draw::toSub(5), draw::toSub(3), draw::toSub(1),
               RGB{255, 255, 255});

    // The hole is what separates a ring from a disc, and the property a caller relies on when stacking rings: the middle must stay dark however thick the stroke.
    CHECK(isBlack(at(buf, dims, 5, 5, 0)));
    CHECK(at(buf, dims, 5, 2, 0).r > 0);       // on the band, three up from center
    CHECK(isBlack(at(buf, dims, 0, 0, 0)));    // outside it entirely
}

TEST_CASE("a ring's inner edge is antialiased, not cut") {
    Buffer buf;
    Coord3D dims{21, 21, 1};
    REQUIRE(buf.allocate(441, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 21, 21, 1);
    draw::ring(cv, draw::toSub(10), draw::toSub(10), draw::toSub(6), draw::toSub(2),
               RGB{255, 255, 255});

    // Walking in from the band toward the center must pass through a PARTIAL pixel. Two discs subtracted would step straight from full to black here, which is the bug this shape exists to avoid: the inner edge of a subtracted ring is never blended, only removed.
    bool sawPartial = false;
    for (int y = 10; y >= 0; y--) {
        const uint8_t v = at(buf, dims, 10, static_cast<lengthType>(y), 0).r;
        if (v > 0 && v < 255) sawPartial = true;
    }
    CHECK(sawPartial);
}

TEST_CASE("a thick line covers pixels a one-pixel line would miss") {
    Buffer thin, thick;
    Coord3D dims{15, 15, 1};
    REQUIRE(thin.allocate(225, 3));
    REQUIRE(thick.allocate(225, 3));
    const draw::Canvas cvThin = draw::Canvas::of(thin, 15, 15, 1);
    const draw::Canvas cvThick = draw::Canvas::of(thick, 15, 15, 1);

    draw::line(cvThin, {2, 7, 0}, {12, 7, 0}, RGB{255, 255, 255});
    draw::strokeLine(cvThick, draw::toSub(2), draw::toSub(7), draw::toSub(12), draw::toSub(7),
                     draw::toSub(3), RGB{255, 255, 255});

    // Width is the whole point: a row either side of the axis is dark for the thin line and lit for the thick one.
    CHECK(isBlack(at(thin, dims, 7, 5, 0)));
    CHECK(at(thick, dims, 7, 6, 0).r > 0);
    CHECK(at(thick, dims, 7, 8, 0).r > 0);
}

TEST_CASE("a thick line stops at its endpoints instead of running along the infinite line") {
    Buffer buf;
    Coord3D dims{15, 15, 1};
    REQUIRE(buf.allocate(225, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 15, 15, 1);
    // A short horizontal stroke in the middle of the grid.
    draw::strokeLine(cv, draw::toSub(6), draw::toSub(7), draw::toSub(8), draw::toSub(7),
                     draw::toSub(2), RGB{255, 255, 255});

    CHECK(at(buf, dims, 7, 7, 0).r > 0);       // on the segment
    // Well past both ends must be dark. Without clamping the projection to the segment, every pixel in the row would be "on the line" and the stroke would span the whole grid.
    CHECK(isBlack(at(buf, dims, 0, 7, 0)));
    CHECK(isBlack(at(buf, dims, 14, 7, 0)));
}

TEST_CASE("a zero-length thick line draws a dot rather than nothing") {
    Buffer buf;
    Coord3D dims{9, 9, 1};
    REQUIRE(buf.allocate(81, 3));
    const draw::Canvas cv = draw::Canvas::of(buf, 9, 9, 1);
    // What a fully retracted clock hand asks for. Dividing by the length would be a divide by zero, so this case is handled rather than guarded against.
    draw::strokeLine(cv, draw::toSub(4), draw::toSub(4), draw::toSub(4), draw::toSub(4),
                     draw::toSub(3), RGB{255, 255, 255});

    CHECK(at(buf, dims, 4, 4, 0).r > 0);
}

// A light by coordinate and by its index in buffer order are the same bytes, so a script's index and an effect's coordinate share one operation.
TEST_CASE("a light reached by index is the light reached by its coordinate") {
    Buffer buf;
    buf.allocate(4 * 3, 3);
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 3, 1);
    draw::pixel(cv, Coord3D{1, 2, 0}, RGB{200, 100, 50});
    const RGB byIndex = draw::get(cv, size_t{2 * 4 + 1});
    CHECK(byIndex.r == 200); CHECK(byIndex.g == 100); CHECK(byIndex.b == 50);
    CHECK(draw::get(cv, size_t{12}).r == 0);   // past the end reads black
    draw::pixel(cv, size_t{12}, RGB{9, 9, 9});  // and writes nothing
    CHECK(buf.bytes() == 36);
}

// The per-light read-modify-write operations, by coordinate: scale dims, add saturates, blend mixes.
TEST_CASE("a pixel is scaled, added to and blended in place") {
    Buffer buf;
    buf.allocate(3, 3);
    const draw::Canvas cv = draw::Canvas::of(buf, 3, 1, 1);
    draw::fill(cv, RGB{200, 100, 50});
    draw::scalePixel(cv, Coord3D{0, 0, 0}, 128);
    draw::addPixel(cv, Coord3D{1, 0, 0}, RGB{100, 200, 0});
    draw::blendPixel(cv, Coord3D{2, 0, 0}, RGB{0, 0, 250}, 255);
    const RGB a = draw::get(cv, Coord3D{0, 0, 0}), b = draw::get(cv, Coord3D{1, 0, 0}), c = draw::get(cv, Coord3D{2, 0, 0});
    CHECK(a.r == 100); CHECK(a.g == 50); CHECK(a.b == 25);
    CHECK(b.r == 255); CHECK(b.g == 255); CHECK(b.b == 50);
    CHECK(c.r == 0); CHECK(c.g == 0); CHECK(c.b == 250);
}
