/// @module VideoService
///
/// Pins the PPM header grammar VideoService's file source accepts, the seat election that decides which service publishes, and the test pattern's sweep rate.
///
/// @moreinfo
///
/// The parser is the part with real edge cases: comments, whitespace runs, a 16-bit maxval, a truncated header.
/// It also decides where pixel data starts, so a wrong offset shows as a picture shifted by a few bytes rather than as a clean failure.
/// Driven directly, since it is a pure static, so these run without a filesystem and a malformed file is testable without writing one.

#include "doctest.h"
#include "core/services/VideoService.h"

#include <cstdint>
#include <cstring>

using mm::VideoService;

namespace {
// Parse helper: returns the pixel offset, or -1, and reports the dimensions it read.
int parse(const char* text, uint16_t& w, uint16_t& h) {
    return VideoService::parsePpmHeader(text, static_cast<int>(std::strlen(text)), w, h);
}
} // namespace

// The canonical form ffmpeg and ImageMagick emit: magic, dimensions, maxval, one newline, pixels.
TEST_CASE("VideoService PPM: a canonical P6 header yields the dimensions and the pixel offset") {
    uint16_t w = 0, h = 0;
    const char* hdr = "P6\n64 36\n255\n";
    CHECK(parse(hdr, w, h) == 13); // pixels begin straight after the final newline
    CHECK(w == 64);
    CHECK(h == 36);
}

// Netpbm allows any run of whitespace between tokens and `#` comments to end of line. Both appear in real files, so both must be skipped without shifting the offset.
TEST_CASE("VideoService PPM: comments and whitespace runs are skipped, not counted as pixels") {
    uint16_t w = 0, h = 0;
    const char* hdr = "P6\n# CREATOR: GIMP\n  16   9  \n255\n";
    const int off = parse(hdr, w, h);
    CHECK(off == static_cast<int>(std::strlen(hdr)));
    CHECK(w == 16);
    CHECK(h == 9);
}

// Exactly ONE whitespace byte separates the header from the binary block; any further byte is already a pixel. Consuming two would tint the whole image by shifting every channel one place.
TEST_CASE("VideoService PPM: only one separator byte is consumed before the pixels") {
    uint16_t w = 0, h = 0;
    // A leading pixel byte that happens to be whitespace-valued (0x20) must survive as data.
    const char hdr[] = {'P', '6', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', ' ', 'X'};
    const int off = VideoService::parsePpmHeader(hdr, static_cast<int>(sizeof(hdr)), w, h);
    CHECK(off == 11); // after the newline: NOT after the following 0x20
    CHECK(w == 2);
    CHECK(h == 2);
}

// P3 is the ASCII variant: same dimensions, completely different body (decimal text, not bytes). Accepting it would read numerals as pixel values and render noise.
TEST_CASE("VideoService PPM: the ASCII variant P3 is rejected, not read as binary") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P3\n8 8\n255\n", w, h) == -1);
}

// A 16-bit maxval means two big-endian bytes per sample: a different pixel format. Reading it as 8-bit would show the high bytes as a dim, doubled image, so it is refused rather than guessed at.
TEST_CASE("VideoService PPM: a 16-bit maxval is rejected rather than misread as 8-bit") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n8 8\n65535\n", w, h) == -1);
}

// Garbage, an empty buffer, and a header cut off mid-token must all fail cleanly. The file source is fed by whatever the user uploads, so this is the ordinary case.
TEST_CASE("VideoService PPM: malformed and truncated headers fail without reading past the buffer") {
    uint16_t w = 0, h = 0;
    CHECK(parse("", w, h) == -1);
    CHECK(parse("not an image at all", w, h) == -1);
    CHECK(parse("P6", w, h) == -1);             // magic only
    CHECK(parse("P6\n64", w, h) == -1);         // no height
    CHECK(parse("P6\n64 36\n", w, h) == -1);    // no maxval
    CHECK(parse("P6\n64 36\n255", w, h) == -1); // no separator, so no pixel data can follow
}

// A zero side has no pixels, and an absurd dimension would overflow the width*height*3 allocation size. Both are refused at the header rather than at the allocation.
TEST_CASE("VideoService PPM: zero and out-of-range dimensions are refused at the header") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n0 36\n255\n", w, h) == -1);
    CHECK(parse("P6\n64 0\n255\n", w, h) == -1);
    CHECK(parse("P6\n99999 36\n255\n", w, h) == -1); // past kMaxDim
}

// With no service instantiated, latestFrame() still returns a readable struct, so an effect null-checks the frame's contents rather than the POINTER. Every device is in that state at boot.
TEST_CASE("VideoService: latestFrame is readable with no service present and reports no frame") {
    const mm::VideoFrame* f = VideoService::latestFrame();
    REQUIRE(f != nullptr);
    CHECK(f->rgb == nullptr);
    CHECK(f->seq == 0);
    CHECK(f->width == 0);
    CHECK(f->height == 0);
}

// Deleting the elected source while a second one runs must hand the seat over rather than go dark. The destructor vacates it and a running module re-claims an empty one on its next tick.
TEST_CASE("VideoService: a survivor takes over the seat when the elected source is destroyed") {
    auto* elected = new VideoService(); // constructed first, so it claims the seat
    elected->source = VideoService::kSourcePattern; // needs no file
    elected->applyState();
    REQUIRE(VideoService::latestFrame()->rgb != nullptr);

    VideoService survivor; // seat already held, so its claim is a no-op
    survivor.source = VideoService::kSourcePattern;
    survivor.applyState();

    delete elected; // ~ActiveInstance vacates: the seat is now empty
    CHECK(VideoService::latestFrame()->rgb == nullptr);

    survivor.tick(); // the survivor inherits it
    CHECK(VideoService::latestFrame()->rgb != nullptr);
}

// Unit tests link the desktop platform, which cannot capture, so the usb source is not offered. The static_assert fails loudly if the suite ever runs somewhere that CAN.
TEST_CASE("VideoService: a platform that cannot capture does not offer the usb source") {
    static_assert(!mm::platform::hasUsbVideo, "tests assume the desktop platform");
    CHECK(VideoService::kSourceCount == 2);

    VideoService v;
    v.source = VideoService::kSourceUsb;
    v.applyState();
    CHECK(v.source == VideoService::kSourcePattern); // fell back
    CHECK(VideoService::latestFrame()->rgb != nullptr);
}

// A platform with no capture must report an EMPTY format list rather than a placeholder, since a phantom entry would let the user pick a dead format.
TEST_CASE("VideoService: a platform with no capture advertises no formats") {
    mm::platform::VideoCaptureFormat formats[4];
    CHECK(mm::platform::videoCaptureFormats(formats, 4) == 0);
}

// Accepting a non-whitespace separator eats a pixel and shifts every channel one place, which tints the whole image rather than failing.
TEST_CASE("VideoService PPM: the separator must be whitespace, not merely present") {
    uint16_t w = 0, h = 0;
    CHECK(parse("P6\n2 2\n255X", w, h) == -1);
    CHECK(parse("P6\n2 2\n255\n", w, h) == 11);   // the same header with a real separator
}

// The sweep is a RATE rather than a phase of the clock, so a rate change moves smoothly rather than jumping, and 0 parks the block as a still reference.
TEST_CASE("VideoService: the test pattern sweeps at patternSpeed pixels per second, and 0 parks it") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    // Column of the white block on the top row, which is otherwise the red band.
    auto sweepX = [] {
        const mm::VideoFrame* f = VideoService::latestFrame();
        for (int x = 0; x < f->width; x++)
            if (f->rgb[static_cast<size_t>(x) * 3 + 1] == 255) return x;
        return -1;
    };

    mm::platform::setTestNowMs(1000);
    VideoService v;
    v.source = VideoService::kSourcePattern;
    v.patternSpeed = 10;
    v.applyState(); // the first frame takes the clock and charges nothing
    const int start = sweepX();
    REQUIRE(start >= 0);

    mm::platform::setTestNowMs(1500); // 0.5 s at 10 px/s
    v.tick();
    CHECK(sweepX() == (start + 5) % VideoService::kPatternW);

    v.patternSpeed = 0;
    mm::platform::setTestNowMs(4000);
    v.tick();
    CHECK(sweepX() == (start + 5) % VideoService::kPatternW); // parked, however long passes
}

// Pinned to a measured case: a source showing (255,127,0) captured under PQ as (206,171,0). The curve has to land that pair on the LINEAR ratio the original color has, sRGB(127)/sRGB(255) = 0.212, which is the quantity an average is then taken of.
TEST_CASE("VideoService: the PQ tone table recovers the source's linear ratio") {
    VideoService v;
    v.source = VideoService::kSourceUsb;
    v.hdr = VideoService::kHdrPq;
    v.hdrNits = 2000;
    v.onControlChanged("hdr"); // the path a UI edit takes
    const uint16_t* t = v.toneForTest();
    REQUIRE(t != nullptr);

    CHECK(171.0 / 206.0 > 0.80); // the defect, as the bytes arrive
    const double linear = static_cast<double>(t[171]) / t[206];
    CHECK(linear > 0.15); // recovered toward 0.212
    CHECK(linear < 0.30);

    // Monotonic: a curve that reorders levels would posterize.
    CHECK(t[0] == 0);
    for (int i = 1; i < 256; i++) CHECK(t[i] >= t[i - 1]);
}

// Every source publishes one, SDR included: sRGB is a curve like any other, and a consumer averaging raw bytes averages a quantity that is not proportional to light.
TEST_CASE("VideoService: an SDR source publishes the sRGB curve, not nothing") {
    VideoService v;
    v.source = VideoService::kSourcePattern;
    v.applyState();
    const uint16_t* t = VideoService::latestFrame()->tone;
    REQUIRE(t != nullptr);
    CHECK(t[0] == 0);
    CHECK(t[255] == mm::VideoFrame::kLinearMax);
    // sRGB's midpoint is about 21% of full light: the whole reason averaging bytes is wrong.
    CHECK(t[128] < mm::VideoFrame::kLinearMax / 3);
    CHECK(t[128] > mm::VideoFrame::kLinearMax / 8);
}

// The frame carries its curve and consumers read through channel(), so every reader corrects the same way and none has to know which curve it is. Null means the bytes are taken as they are.
TEST_CASE("VideoFrame: channel() reads through the tone curve when one is published") {
    uint8_t px[3] = {10, 20, 30};
    uint16_t tone[256];
    for (int i = 0; i < 256; i++) tone[i] = static_cast<uint16_t>(255 - i);
    mm::VideoFrame f;
    f.rgb = px;
    CHECK(f.channel(px, 0) == 10);
    CHECK(f.channel(px, 2) == 30);
    f.tone = tone;
    CHECK(f.channel(px, 0) == 245);
    CHECK(f.channel(px, 1) == 235);
}
