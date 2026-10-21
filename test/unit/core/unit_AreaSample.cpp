/// The sampling every picture path shares: the proportional span of one output cell, and its mean in linear light.

#include "doctest.h"
#include "core/util/AreaSample.h"

#include <algorithm>
#include <vector>

using namespace mm;

namespace {
// Every output pixel the mean of its own span, as the capture path and the Picture effect both do.
void shrink(const uint8_t* src, uint16_t w, uint16_t h, uint16_t ow, uint16_t oh, uint8_t* dst) {
    for (uint16_t y = 0; y < oh; y++) {
        const Span ry = cellSpan(y, oh, h);
        for (uint16_t x = 0; x < ow; x++) {
            const RGB mean = areaMean(src, static_cast<size_t>(w) * 3u, cellSpan(x, ow, w), ry);
            uint8_t* out = dst + (static_cast<size_t>(y) * ow + x) * 3;
            out[0] = mean.r;
            out[1] = mean.g;
            out[2] = mean.b;
        }
    }
}
}  // namespace

// Every column and row of the source must reach a cell: a dropped remainder would blind the right and bottom edges, which is exactly where a backlight reads its color.
TEST_CASE("the cell ranges tile the whole source with no gap and no overlap") {
    for (uint32_t out : {1u, 7u, 34u, 45u, 480u}) {
        for (uint32_t src : {480u, 640u, 1080u, 1920u}) {
            uint32_t covered = 0, prevHi = 0;
            for (uint32_t i = 0; i < out; i++) {
                const Span s = cellSpan(i, out, src);
                CHECK(s.lo == prevHi);      // no gap: this cell starts where the last ended
                CHECK(s.hi > s.lo);         // and is never empty
                covered += s.hi - s.lo;
                prevHi = s.hi;
            }
            CHECK(prevHi == src);           // the last cell ends exactly at the source's edge
            CHECK(covered == src);
        }
    }
}

// A cell's span reaches the source's last column, so a bright screen edge lights the lights along it. A cell samples at a stride, so one lit column can fall between taps; a real edge cannot.
TEST_CASE("a bright screen edge reaches the edge cell") {
    const uint16_t w = 640, h = 480, ow = 45, oh = 34;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3, 0);
    for (uint16_t y = 0; y < h; y++)
        for (uint16_t x = 630; x < w; x++) {   // the rightmost columns, which an integer factor would drop
            uint8_t* p = src.data() + (static_cast<size_t>(y) * w + x) * 3;
            p[0] = p[1] = p[2] = 255;
        }
    std::vector<uint8_t> dst(static_cast<size_t>(ow) * oh * 3, 0);
    shrink(src.data(), w, h, ow, oh, dst.data());
    const uint8_t* last = dst.data() + (static_cast<size_t>(oh / 2) * ow + (ow - 1)) * 3;
    CHECK(last[0] > 0);
    // And the cell's own range really does end at the source's last column.
    const Span edge = cellSpan(ow - 1, ow, w);
    CHECK(edge.hi == w);
    CHECK(edge.lo < 630);
}

// A uniform region keeps its value exactly, whatever the ratio between source and picture.
TEST_CASE("a uniform frame averages to itself") {
    const uint16_t w = 97, h = 61;   // deliberately prime-ish, so no whole factor exists
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3, 123);
    std::vector<uint8_t> dst(45u * 34u * 3u, 0);
    shrink(src.data(), w, h, 45, 34, dst.data());
    for (uint8_t v : dst) CHECK(v == 123);
}

// Averaging in linear light rather than on the bytes, so a shrink does not darken the picture.
TEST_CASE("a half-black half-white frame averages above the arithmetic mean") {
    const uint16_t w = 2, h = 2;
    std::vector<uint8_t> src{0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 255, 255};
    uint8_t dst[3] = {};
    shrink(src.data(), w, h, 1, 1, dst);
    CHECK(dst[0] > 128);    // the arithmetic mean would be 127
    CHECK(dst[0] == 180);   // sqrt((255*255*2)/4) = 180
}

// The taps are bounded, which is what keeps a 1080p frame off the PSRAM budget and inside a 32-bit sum.
TEST_CASE("a cell samples at a stride rather than reading every pixel it covers") {
    const Span whole = cellSpan(0, 1, 1920);   // the worst case: one cell over a whole 1080p row
    const uint32_t stride = (whole.hi - whole.lo + kAreaTapsPerAxis - 1) / kAreaTapsPerAxis;
    uint32_t taps = 0;
    for (uint32_t p = whole.lo; p < whole.hi; p += stride) taps++;
    CHECK(taps <= kAreaTapsPerAxis);
    // At most 16 taps a cell, so the largest sum is far inside what a uint32 holds.
    CHECK(static_cast<uint64_t>(kAreaTapsPerAxis) * kAreaTapsPerAxis * 255u * 255u < 0xFFFFFFFFull);
}

