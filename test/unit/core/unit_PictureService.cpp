/// @module PictureService

/// The picture source: the test pattern it publishes, the generation that tells a consumer it changed, and the empty frame when nothing publishes.

#include "doctest.h"
#include "core/services/PictureService.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace {
// A service drawing a `w` x `h` test pattern, as the UI's prepare does.
struct Picture {
    mm::PictureService svc;
    Picture(uint16_t w, uint16_t h) {
        svc.defineControls();
        svc.width = w;
        svc.height = h;
        svc.prepare();
    }
};
const uint8_t* pixel(const mm::PictureFrame& f, uint32_t x, uint32_t y) { return f.rgb + (y * f.width + x) * 3; }
// The active service's frame, read through a Reader as an effect does.
mm::PictureFrame current() {
    const mm::PictureService::Reader r;
    REQUIRE(bool(r));
    return r.frame();
}
}  // namespace

// The test card at 160 x 120: the color bars and the gray steps inside the round circle, a colored block and the gray grid outside it.
TEST_CASE("the test pattern publishes the test card") {
    Picture p(160, 120);
    const mm::PictureFrame f = current();
    REQUIRE(f.rgb != nullptr);
    CHECK(f.width == 160);
    CHECK(f.height == 120);
    const auto is = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) {
        const uint8_t* px = pixel(f, x, y);
        return px[0] == r && px[1] == g && px[2] == b;
    };
    CHECK(is(37, 51, 190, 190, 50));     // the yellow bar at the circle's left
    CHECK(is(124, 51, 0, 30, 190));      // the blue bar at its right
    CHECK(is(48, 89, 50, 50, 50));       // the gray steps start dark
    CHECK(is(117, 89, 255, 255, 255));   // and end white
    CHECK(is(22, 38, 80, 160, 140));     // the left side's tall block, upper half
    CHECK(is(5, 2, 128, 128, 128));      // a grid square's gray
}

// A consumer compares generations to skip a picture it already sampled, so a new size is a new generation.
TEST_CASE("a new picture size publishes a new generation") {
    Picture p(16, 9);
    const uint32_t first = current().generation;
    p.svc.width = 32;
    p.svc.prepare();
    CHECK(current().generation != first);
    CHECK(current().width == 32);
}

// An effect with no source reads an empty frame rather than a dangling pointer, before a service exists and after it leaves.
TEST_CASE("with no Picture service the frame is empty") {
    CHECK(current().rgb == nullptr);
    {
        Picture p(8, 8);
        CHECK(current().rgb != nullptr);
        p.svc.release();
        CHECK(current().rgb == nullptr);
    }
    CHECK(current().rgb == nullptr);
}

namespace {
// A writer task publishing `pictures` whole pictures, each one value throughout, while this thread reads: every picture read must be whole.
void writeWhileReading(Picture& p, int pictures) {
    // A uniform first picture, since the test pattern the service starts with is not one value throughout.
    const size_t seed = static_cast<size_t>(p.svc.width) * p.svc.height * 3;
    std::memset(p.svc.beginWrite(), 0, seed);
    p.svc.endWrite();
    std::atomic<bool> done{false};
    std::thread writer([&] {
        const size_t bytes = static_cast<size_t>(p.svc.width) * p.svc.height * 3;
        for (int k = 1; k <= pictures; k++) {
            uint8_t* px = p.svc.beginWrite();
            for (size_t i = 0; i < bytes; i++) px[i] = static_cast<uint8_t>(k);
            p.svc.endWrite();
        }
        done = true;
    });
    int torn = 0, reads = 0;
    while (!done) {
        const mm::PictureService::Reader r;
        if (!r) continue;   // the writer holds it: an effect samples on the next tick
        const mm::PictureFrame& f = r.frame();
        const size_t bytes = static_cast<size_t>(f.width) * f.height * 3;
        for (size_t i = 1; i < bytes; i++) if (f.rgb[i] != f.rgb[0]) { torn++; break; }
        reads++;
    }
    writer.join();
    CHECK(torn == 0);
    CHECK(reads > 0);
    CHECK(current().rgb[0] == static_cast<uint8_t>(pictures));   // the last picture is the one shown
}
}  // namespace

// A source writing from its own task fills the back picture and swaps, so a reader never sees half a picture.
TEST_CASE("a picture written from another task is always read whole, with two pictures") {
    Picture p(64, 36);
    REQUIRE(p.svc.prepareBuffers(/*writerTask=*/true));
    REQUIRE(p.svc.twoBuffers());
    writeWhileReading(p, 300);
}

// Short of memory for the second picture, the one picture is locked while written: frames may be skipped, never torn, and the status says why.
TEST_CASE("without room for a second picture the service degrades to one, locked while written") {
    Picture p(64, 36);
    const size_t bytes = 64u * 36u * 3u;
    mm::platform::setTestMaxAllocBlock(bytes - 1);   // the optional picture finds no block
    REQUIRE(p.svc.prepareBuffers(/*writerTask=*/true));
    mm::platform::setTestMaxAllocBlock(0);
    CHECK_FALSE(p.svc.twoBuffers());
    REQUIRE(p.svc.status() != nullptr);
    CHECK(std::string(p.svc.status()) == "one picture buffer: not enough memory, a capture may skip frames");
    writeWhileReading(p, 300);
}
