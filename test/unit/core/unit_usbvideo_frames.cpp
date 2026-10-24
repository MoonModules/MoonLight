/// The hardware-free half of USB video capture: the payload headers it strips, the mode it asks for, and the shrink onto a picture.

#include "doctest.h"
#include "platform/usbvideo_frames.h"

#include <algorithm>
#include <cstring>
#include <vector>


using namespace mm::usbvideo;

namespace {
// A bulk frame carrying `payload`-byte payloads, each opening with a `hl`-byte header the driver left in.
std::vector<uint8_t> framed(size_t hl, size_t payload, size_t payloads, uint8_t fill) {
    std::vector<uint8_t> d;
    // The flags must agree with the length, or a header declares one size and occupies another.
    const uint8_t flags =
        static_cast<uint8_t>(kEoh | (hl == 6 || hl == 12 ? kPts : 0) | (hl == 8 || hl == 12 ? kScr : 0));
    for (size_t p = 0; p < payloads; p++) {
        if (p) {   // the driver strips only the first, so every later payload keeps its header
            d.push_back(static_cast<uint8_t>(hl));
            d.push_back(flags);
            for (size_t i = 2; i < hl; i++) d.push_back(i == 2 ? 0x11 : 0x00);   // a constant PTS, zero SCR
        }
        const size_t data = payload - hl;
        for (size_t i = 0; i < data; i++) d.push_back(fill);
    }
    return d;
}
}  // namespace

// A grabber's payload headers sit at the packet stride, and stripping them leaves only the bitstream.
TEST_CASE("payload headers are stripped at the stride the first one sets") {
    const size_t hl = 12, payload = 512;
    std::vector<uint8_t> d = framed(hl, payload, 4, 0xAB);
    const size_t before = d.size();
    const size_t after = stripPayloadHeaders(d.data(), d.size());
    CHECK(after < before);
    CHECK(after == before - 3 * hl);   // three later payloads, each losing its header
    for (size_t i = 0; i < after; i++) CHECK(d[i] == 0xAB);
}

// A grabber that sends neither a PTS nor an SCR has 2-byte headers, which must still be found.
TEST_CASE("a header's length comes from its own flags") {
    CHECK(headerLenFor(kEoh) == 2);
    CHECK(headerLenFor(kEoh | kPts) == 6);
    CHECK(headerLenFor(kEoh | kScr) == 8);
    CHECK(headerLenFor(kEoh | kPts | kScr) == 12);
}

// Each length headerLenFor names must be found and stripped, since a grabber picks one and keeps it.
TEST_CASE("every header length is stripped, not only the longest") {
    for (size_t hl : {size_t(2), size_t(6), size_t(8), size_t(12)}) {
        std::vector<uint8_t> d = framed(hl, 512, 4, 0x7E);
        const size_t before = d.size();
        const size_t after = stripPayloadHeaders(d.data(), d.size());
        CHECK(after == before - 3 * hl);   // three later payloads, each losing its header
        for (size_t i = 0; i < after; i++) CHECK(d[i] == 0x7E);
    }
}

// A frame with no headers left in it is handed back byte for byte, since a false positive corrupts it.
TEST_CASE("a frame the driver already stripped is left alone") {
    std::vector<uint8_t> d(2048, 0x5A);
    const std::vector<uint8_t> copy = d;
    CHECK(stripPayloadHeaders(d.data(), d.size()) == d.size());
    CHECK(std::memcmp(d.data(), copy.data(), d.size()) == 0);
}

// UVC states a rate as a period, and 59.94 fps is a real mode that should read as 60.
TEST_CASE("a frame interval becomes the rate it names") {
    CHECK(fpsFrom(333333) == 30);    // 30 fps
    CHECK(fpsFrom(166833) == 60);   // 59.94 fps, rounded up
    CHECK(fpsFrom(0) == 0);
}

// The fastest rate at or below the one asked for, so a 60 fps row never doubles the work for a 30 fps request.
TEST_CASE("the rate asked for wins over a faster one the device offers") {
    CHECK(betterFps(0, 60, 30) == 60);    // nothing held yet: take what there is
    CHECK(betterFps(60, 30, 30) == 30);   // 30 is at the request, 60 above it
    CHECK(betterFps(30, 60, 30) == 30);
    CHECK(betterFps(15, 30, 30) == 30);   // both fit, so the faster
    CHECK(betterFps(60, 90, 30) == 60);   // neither fits, so the slower
}

// A continuous-range device names no rate, so a rung is computed for each rate a picture asks for.
TEST_CASE("a continuous range yields the rung for a rate it covers") {
    const uint32_t fast = intervalFor(60), slow = intervalFor(5);   // 100 ns ticks: a shorter interval is a faster rate
    CHECK(rungFor(30, fast, slow, 1) == intervalFor(30));
    CHECK(rungFor(5, fast, slow, 1) == slow);
    CHECK(rungFor(60, fast, slow, 1) == fast);
    CHECK(rungFor(90, fast, slow, 1) == 0);     // above the range: the device cannot reach it
    CHECK(rungFor(1, fast, slow, 1) == 0);      // below it
    CHECK(rungFor(30, fast, slow, 0) == intervalFor(30));   // no step: every interval in the range
}

// A step the request falls between must round to the slower rung, since betterFps prefers a rate at or under the one asked for.
TEST_CASE("a rung lands at or under the rate asked for") {
    const uint32_t fast = intervalFor(60), slow = intervalFor(10);
    const uint32_t step = 100000;   // 10 ms rungs, so 30 fps (333333) is between two of them
    const uint32_t rung = rungFor(30, fast, slow, step);
    REQUIRE(rung != 0);
    CHECK((rung - fast) % step == 0);          // on the device's ladder
    CHECK(rung >= intervalFor(30));            // at or slower than asked
    CHECK(fpsFrom(rung) <= 30);
}

// The smallest mode at or above the picture, at the rate asked for: the decode and the shrink both scale with it.
TEST_CASE("the mode asked for is the smallest that covers the picture") {
    const Mode rows[] = {{1920, 1080, 30}, {1280, 720, 60}, {1280, 720, 30}, {640, 480, 60}, {640, 480, 30}};
    uint16_t w = 64, h = 48;
    uint8_t fps = 30;
    REQUIRE(pickMode(rows, 5, w, h, fps));
    CHECK(w == 640);
    CHECK(h == 480);
    CHECK(fps == 30);   // not the 60 the same resolution also offers
}

// A grabber offering a small mode saves the decode, so it must be preferred.
TEST_CASE("a smaller advertised mode is preferred over a larger one") {
    const Mode rows[] = {{640, 480, 60}, {320, 240, 30}};
    uint16_t w = 64, h = 48;
    uint8_t fps = 30;
    REQUIRE(pickMode(rows, 2, w, h, fps));
    CHECK(w == 320);
    CHECK(h == 240);
}

// Two shapes can share an area, so the rates must accumulate per resolution or a mode is invented.
TEST_CASE("two modes of equal area do not mix their rates") {
    const Mode rows[] = {{1280, 720, 30}, {960, 960, 15}};   // both 921,600 pixels
    uint16_t w = 640, h = 480;
    uint8_t fps = 30;
    REQUIRE(pickMode(rows, 2, w, h, fps));
    // Whichever resolution wins, its own rate comes with it: never 960x960 at the 1280x720 row's 30.
    const bool coherent = (w == 1280 && h == 720 && fps == 30) || (w == 960 && h == 960 && fps == 15);
    CHECK(coherent);
}

// Nothing big enough leaves the request alone, so the device picks and the picture is told what arrived.
TEST_CASE("a device with nothing above the picture leaves the request alone") {
    const Mode rows[] = {{160, 120, 30}};
    uint16_t w = 320, h = 240;
    uint8_t fps = 30;
    CHECK_FALSE(pickMode(rows, 1, w, h, fps));
    CHECK(w == 320);
    CHECK(h == 240);
}

// The decoder pads to its MCU, which is 16x16 for YUV420, 16x8 for 422 and 8x8 for 444.
TEST_CASE("the row stride comes from the size the decoder wrote") {
    CHECK(strideFor(640, 480, 16, 16, 640u * 480u * 3u) == 640 * 3);     // both axes already aligned
    CHECK(strideFor(1920, 1080, 16, 16, 1920u * 1088u * 3u) == 1920 * 3);  // the height padded, not the width
    CHECK(strideFor(1288, 1080, 16, 8, 1296u * 1080u * 3u) == 1296 * 3);   // a 422 width padded to 16
    CHECK(strideFor(1288, 1080, 16, 8, 999u) == 0);                        // neither layout: refuse the frame
}

// The picture is exactly the size asked for, as the test pattern's is, whatever the source's shape.
TEST_CASE("a frame of any shape fills the whole picture that was asked for") {
    // A 4:3 source into a 16:9 picture: squashed rather than letterboxed, which for edge color is what is wanted, and every source pixel still lands in exactly one cell.
    const uint16_t w = 640, h = 480, ow = 60, oh = 34;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3, 77);
    std::vector<uint8_t> dst(static_cast<size_t>(ow) * oh * 3 + 3, 0xEE);
    areaAverage(src.data(), static_cast<size_t>(w) * 3, w, h, mm::Sink{dst.data(), ow, oh});
    for (size_t i = 0; i < static_cast<size_t>(ow) * oh * 3; i++) CHECK(dst[i] == 77);
    for (size_t i = static_cast<size_t>(ow) * oh * 3; i < dst.size(); i++) CHECK(dst[i] == 0xEE);
}

// A picture larger than the source upscales rather than reading past it, so no cell is left empty.
TEST_CASE("a picture larger than the frame stays inside the source") {
    const uint16_t w = 4, h = 3, ow = 9, oh = 7;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3, 200);
    std::vector<uint8_t> dst(static_cast<size_t>(ow) * oh * 3, 0);
    areaAverage(src.data(), static_cast<size_t>(w) * 3, w, h, mm::Sink{dst.data(), ow, oh});
    for (uint8_t v : dst) CHECK(v == 200);
    for (uint32_t i = 0; i < ow; i++) {   // no cell may name a column the source does not have
        const mm::Span s = mm::cellSpan(i, ow, w);
        CHECK(s.hi <= w);
        CHECK(s.lo < s.hi);
    }
}

namespace {
// A JPEG's start: SOI, an APP0 segment to walk past, then a baseline frame header for `w` x `h` whose luma samples at `sampling`.
std::vector<uint8_t> jpegHead(uint16_t w, uint16_t h, uint8_t sampling) {
    std::vector<uint8_t> d = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x06, 'J', 'F', 'I', 'F'};
    const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 8, uint8_t(h >> 8), uint8_t(h), uint8_t(w >> 8), uint8_t(w), 3,
                           1, sampling, 0, 2, 0x11, 1, 3, 0x11, 1};
    d.insert(d.end(), sof, sof + sizeof sof);
    return d;
}
}  // namespace

// Read from the bitstream, since the decoder's own header call allocates on every frame.
TEST_CASE("a JPEG's size and MCU come from its frame header") {
    JpegShape s;
    std::vector<uint8_t> d = jpegHead(640, 480, 0x21);   // 4:2:2
    REQUIRE(jpegShape(d.data(), d.size(), s));
    CHECK(s.width == 640);
    CHECK(s.height == 480);
    CHECK(s.mcuW == 16);
    CHECK(s.mcuH == 8);
    d = jpegHead(1920, 1080, 0x22);   // 4:2:0
    REQUIRE(jpegShape(d.data(), d.size(), s));
    CHECK(s.mcuW == 16);
    CHECK(s.mcuH == 16);
}

// A frame cut short or one that is not a JPEG at all is refused rather than read past its end.
TEST_CASE("a truncated or foreign frame has no JPEG shape") {
    JpegShape s;
    std::vector<uint8_t> d = jpegHead(640, 480, 0x21);
    CHECK_FALSE(jpegShape(d.data(), d.size() - 4, s));   // the frame header cut short
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A};
    CHECK_FALSE(jpegShape(png, sizeof png, s));
    const uint8_t scanFirst[] = {0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02};
    CHECK_FALSE(jpegShape(scanFirst, sizeof scanFirst, s));
}
