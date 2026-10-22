/// @module PictureService

/// The picture source: the test pattern it publishes, the generation that tells a consumer it changed, and the empty frame when nothing publishes.

#include "doctest.h"
#include "core/services/PictureService.h"

#include <atomic>
#include <chrono>
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

// The capture needs the transport to link, so a build without it offers the test pattern alone rather than a source that cannot open.
TEST_CASE("the source list holds only what the build can open") {
    Picture p(16, 9);
    uint8_t options = 0;
    for (uint8_t i = 0; i < p.svc.controls().count(); i++)
        if (std::strcmp(p.svc.controls()[i].name, "source") == 0) options = p.svc.controls()[i].max;
    CHECK(options == (mm::platform::hasUsbVideo ? mm::PictureService::kSourceCount : 1));
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

namespace {
// A stand-in grabber advertising one mode, detached again when the test ends.
struct Grabber {
    Grabber(uint16_t w, uint16_t h, uint8_t fps, uint8_t r, uint8_t g, uint8_t b) {
        const mm::platform::VideoCaptureFormat f{w, h, fps};
        mm::platform::setTestVideoCaptureFormats(&f, 1);
        mm::platform::setTestVideoCaptureColor(r, g, b);
    }
    ~Grabber() {
        mm::platform::setTestVideoCaptureFormats(nullptr, 0);
        mm::platform::setTestVideoCaptureDecodeFails(false);
    }
};
// The published generation, or `was` while the writer holds the swap, so a poll never fails on timing.
uint32_t generationNow(uint32_t was) {
    const mm::PictureService::Reader r;
    return r ? r.frame().generation : was;
}
// Whether a picture past `was` arrives within two seconds, which a decode on its own task has ample time for.
bool publishedPast(uint32_t was) {
    for (int i = 0; i < 400; i++) {
        if (generationNow(was) != was) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
// Whether the grabber has answered `n` decodes, passed or refused, within two seconds: the task's own count rather than a guess at its pace.
bool decodedAtLeast(uint32_t n) {
    for (int i = 0; i < 400; i++) {
        const mm::platform::VideoCaptureStats st = mm::platform::videoCaptureStats();
        if (st.decoded + st.failed >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
// The status line once tick1s finds the lock free, which a publishing task holds only for a swap.
std::string captureStatus(mm::PictureService& svc) {
    for (int i = 0; i < 400; i++) {
        svc.tick1s();
        if (svc.status() && std::strchr(svc.status(), 'x')) return svc.status();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return {};
}
// The capture source, started on a service sized `w` x `h`; false when no picture arrived.
bool startCapture(Picture& p, uint32_t& was) {
    p.svc.source = mm::PictureService::kUsbCapture;
    p.svc.prepare();
    was = current().generation;   // prepareBuffers published the resized picture; a frame comes after it
    return publishedPast(was);
}
}  // namespace

// The whole picture, as the test pattern fills it. A 4:3 frame fills a 2:1 picture rather than leaving it short.
TEST_CASE("the usb capture source publishes the grabber's frames at the size the picture is set to") {
    Grabber g(640, 480, 30, 10, 200, 90);
    Picture p(200, 100);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    const mm::PictureFrame f = current();
    CHECK(f.width == 200);
    CHECK(f.height == 100);
    const uint8_t* px = pixel(f, 100, 50);
    CHECK(px[0] == 10);
    CHECK(px[1] == 200);
    CHECK(px[2] == 90);
    p.svc.release();
}

// A board whose USB port has nothing on it says so, rather than failing or waiting on a device.
TEST_CASE("the usb capture source without a grabber reports it") {
    mm::platform::setTestVideoCaptureFormats(nullptr, 0);
    Picture p(64, 48);
    p.svc.source = mm::PictureService::kUsbCapture;
    p.svc.prepare();
    REQUIRE(p.svc.status() != nullptr);
    CHECK(std::string(p.svc.status()) == "no grabber attached");
    p.svc.release();
}

// A refused bitstream leaves the lights the picture they have, rather than a new generation of nothing.
TEST_CASE("a frame the decoder refuses publishes nothing, and recovery publishes again") {
    Grabber g(320, 240, 30, 1, 2, 3);
    mm::platform::setTestVideoCaptureDecodeFails(true);
    Picture p(64, 48);
    p.svc.source = mm::PictureService::kUsbCapture;
    p.svc.prepare();
    const uint32_t settled = current().generation;
    REQUIRE(decodedAtLeast(3));   // three refusals answered
    CHECK(generationNow(settled) == settled);
    mm::platform::setTestVideoCaptureDecodeFails(false);
    CHECK(publishedPast(settled));
    p.svc.release();
}

// The status says whether the capture is live right now, which a total since boot cannot.
TEST_CASE("the capture status reports a live rate, not a total") {
    Grabber g(320, 240, 30, 5, 5, 5);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    const std::string line = captureStatus(p.svc);
    CHECK(line.rfind("live, 64 x 48 at ", 0) == 0);   // the picture's size, whatever the grabber streams
    CHECK(line.find(" at ") != std::string::npos);
    CHECK(line.find("fps") != std::string::npos);
    CHECK(line.find("skipped") == std::string::npos);   // nothing skipped, so the line says nothing of it
    CHECK(line.find("frames failing") == std::string::npos);
    CHECK(line.find("one picture") == std::string::npos);
    CHECK(p.svc.severity() == mm::MoonModule::Severity::Status);
    p.svc.release();
}

// Short of memory for a second picture, the capture still publishes and readers still read. The task holds the one picture only while it decodes, never while it waits for a frame.
TEST_CASE("the usb capture with one picture publishes, and the status says it runs on one") {
    Grabber g(320, 240, 30, 7, 7, 7);
    Picture p(64, 48);
    mm::platform::setTestMaxAllocBlock(64u * 48u * 3u - 1);   // the optional picture finds no block
    uint32_t was = 0;
    const bool published = startCapture(p, was);
    mm::platform::setTestMaxAllocBlock(0);
    REQUIRE(published);
    CHECK_FALSE(p.svc.twoBuffers());
    int reads = 0;
    const uint32_t target = mm::platform::videoCaptureStats().decoded + 3;
    while (mm::platform::videoCaptureStats().decoded < target) {
        const mm::PictureService::Reader r;
        if (r) reads++;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(reads > 0);
    CHECK(captureStatus(p.svc).find(", one picture") != std::string::npos);
    CHECK(p.svc.severity() == mm::MoonModule::Severity::Warning);
    p.svc.release();
}

// Switching back stops the decode and draws the card, so the grabber is released rather than still writing.
TEST_CASE("switching away from the capture stops its task") {
    Grabber g(320, 240, 30, 0, 0, 0);
    Picture p(160, 120);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    p.svc.source = mm::PictureService::kTestPattern;
    p.svc.prepare();
    mm::platform::VideoCaptureHandle again;
    CHECK(mm::platform::videoCaptureInit(again, 320, 240, 30));   // the grabber was let go, so it opens anew
    mm::platform::videoCaptureDeinit(again);
    const mm::PictureFrame f = current();
    CHECK(f.width == 160);
    CHECK(f.height == 120);
    const uint8_t* px = pixel(f, 5, 2);   // the card's grid gray, not the black the grabber decoded
    CHECK(px[0] == 128);
    CHECK(px[1] == 128);
    CHECK(px[2] == 128);
    p.svc.release();
}

// A frame the decoder refuses is a fault and must warn, where a spare frame must not.
TEST_CASE("a failed frame warns where a spare one does not") {
    Grabber g(320, 240, 30, 5, 5, 5);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    REQUIRE(p.svc.severity() == mm::MoonModule::Severity::Status);
    mm::platform::setTestVideoCaptureDecodeFails(true);
    // The rate is a per-tick delta, so read it on the tick that sees the failures, not after.
    bool failing = false;
    for (int i = 0; i < 200 && !failing; i++) {
        failing = captureStatus(p.svc).find("frames failing") != std::string::npos;
        if (!failing) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(failing);
    CHECK(p.svc.severity() == mm::MoonModule::Severity::Warning);
    p.svc.release();
}

// A source that stops leaves its last picture lighting the room. Past staleMs it goes, generation and all.
TEST_CASE("a capture that stops sending has its picture dropped") {
    Grabber g(320, 240, 30, 9, 9, 9);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    REQUIRE(current().rgb != nullptr);
    p.svc.staleMs = 100;
    mm::platform::setTestVideoCaptureDecodeFails(true);   // the grabber goes quiet: the gap opens
    const uint32_t held = current().generation;
    bool dropped = false;
    for (int i = 0; i < 200 && !dropped; i++) {
        p.svc.tick1s();
        dropped = current().rgb == nullptr;
        if (!dropped) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(dropped);
    CHECK(current().generation != held);   // or an effect's redraw guard would skip the change
    p.svc.release();
}

// The open zeroes the device's counters, so a re-open that kept its baselines would read a rate of four billion.
TEST_CASE("a re-opened capture reports a plain rate, not an underflowed one") {
    Grabber g(320, 240, 30, 5, 5, 5);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    REQUIRE(captureStatus(p.svc).rfind("live, ", 0) == 0);   // a first run, so the baselines are set
    p.svc.prepare();                                         // the same source again: stop, then open
    const std::string line = captureStatus(p.svc);
    CHECK(line.find("4294") == std::string::npos);   // no wrapped delta anywhere on the line
    CHECK(line.find("frames failing") == std::string::npos);
    p.svc.release();
}

// A capture whose frames are landing holds its picture through the stale guard, whatever the board's uptime.
TEST_CASE("a capture that has just opened keeps its first picture") {
    Grabber g(320, 240, 30, 9, 9, 9);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    p.svc.tick1s();   // the stale guard runs here, with staleMs at its default
    CHECK(current().rgb != nullptr);
    p.svc.release();
}

// A gap of one frame is ordinary, since the capture runs at its own rate: it must not strobe.
TEST_CASE("an ordinary gap between frames keeps the picture") {
    Grabber g(320, 240, 30, 9, 9, 9);
    Picture p(64, 48);
    uint32_t was = 0;
    REQUIRE(startCapture(p, was));
    p.svc.staleMs = 2000;   // far longer than the 33 ms the grabber paces at
    for (int i = 0; i < 20; i++) {
        p.svc.tick1s();
        CHECK(current().rgb != nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    p.svc.release();
}

// A grabber's black is rarely a 0, and a 1 no screen shows is a lit LED. The floor cuts it.
TEST_CASE("the noise floor cuts a grabber's near-black to black") {
    Grabber g(320, 240, 30, 2, 2, 2);   // a frame of 2s, which is what a dark source decodes to
    Picture p(8, 8);
    uint32_t was = 0;
    p.svc.noiseFloor = 0;
    REQUIRE(startCapture(p, was));
    CHECK(*pixel(current(), 4, 4) == 2);   // off: the byte arrives as sent
    p.svc.noiseFloor = 4;
    p.svc.prepare();
    REQUIRE(publishedPast(current().generation));
    CHECK(*pixel(current(), 4, 4) == 0);   // at or under the floor: black, so the lights stay off
    p.svc.release();
}
