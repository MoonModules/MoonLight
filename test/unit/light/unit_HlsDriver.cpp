/// @module HlsDriver
/// @also Drivers, Correction

/// The HLS driver's encode hand-off, pinned without an ffmpeg. CI never has one installed, so the desktop platform RECORDS the spawn argv and the frames the driver piped (platform.h § HLS test seam), exactly as the NDI seam records frames. These tests state what ffmpeg would receive; the bench then only has to confirm a player shows it.

#include "doctest.h"
#include "light/drivers/HlsDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include <string>
#include <vector>

namespace {

// Seam + virtual time are process-global; the guard restores both however a case exits (the NdiSeamGuard rationale, unit_NdiDriver.cpp).
struct EncSeamGuard {
    explicit EncSeamGuard(mm::platform::EncoderTestMode mode) {
        mm::platform::setTestEncoderMode(mode);
    }
    ~EncSeamGuard() {
        mm::platform::setTestEncoderMode(mm::platform::EncoderTestMode::Off);
        mm::platform::setTestNowMs(0);
    }
};

// A wall wired as production wires it: the Layout gives the Layer its physical size and the driver reads that, never its own controls.
struct Wall {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

void setUp(mm::HlsDriver& driver, mm::Buffer& source, Wall& wall, mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::encoderTestClearFrames();
    // Virtual time from the START: prepare() stamps the warm-up deadline from millis(), and a real-clock stamp against later virtual ticks is a wraparound flake (passes in isolation, fails in the full run depending on process uptime).
    mm::platform::setTestNowMs(1);
}

void paint(mm::Buffer& b, mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t bl) {
    uint8_t* p = b.data() + static_cast<size_t>(i) * 3;
    p[0] = r; p[1] = g; p[2] = bl;
}

}  // namespace

// Without ffmpeg the driver is inert but SAFE, and says why: the state every machine without it is in, including CI, so it is the default path rather than an edge case.
TEST_CASE("HlsDriver reports a missing ffmpeg instead of failing") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::ForceMissing};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);

    driver.prepare();
    CHECK(std::string(driver.status()).find("ffmpeg not found") != std::string::npos);

    mm::platform::setTestNowMs(1000);
    driver.tick();                                   // must not crash, must pipe nothing
    CHECK(mm::platform::encoderTestFrameCount() == 0);
}

// The ffmpeg invocation IS the desktop encode contract: raw RGB in at the grid size and chosen rate, zerolatency x264 at the chosen bitrate, 1 s segments on a short rolling playlist (the live tuning behind the documented 2-5 s latency), segments deleted as they age out. The driver states only the numbers (EncoderConfig); this pins what the desktop platform makes of them.
TEST_CASE("HlsDriver hands ffmpeg the exact live-HLS invocation") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(6, 4);
    setUp(driver, source, wall, 24);
    driver.targetFps = 25;
    driver.scale = 1;                                 // pin 1:1; auto-scale has its own case
    driver.prepare();

    const std::string args = mm::platform::encoderTestArgs();
    CHECK(args.find("-f rawvideo -pix_fmt rgb24 -s 6x4 -r 25 -i -") != std::string::npos);
    CHECK(args.find("-c:v libx264 -preset veryfast -tune zerolatency -g 25") != std::string::npos);
    // The bitrate is DERIVED (6x4 at 25 fps floors to the 500 kbit minimum), not a control: see HlsDriver::autoBitrateKbit. What matters is that the invocation carries one.
    CHECK(args.find("-b:v 500k") != std::string::npos);
    CHECK(args.find("-f hls -hls_time 1 -hls_list_size 6 -hls_flags delete_segments+temp_file")
          != std::string::npos);
    CHECK(args.find("/.hls/stream.m3u8") != std::string::npos);
}

// A grid change re-states the geometry: the encoder is fixed at start, so the numbers the driver hands the platform must follow the layout rather than any control of its own.
TEST_CASE("HlsDriver encodes at the layout's size, not a control's") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(32, 18);
    setUp(driver, source, wall, 576);
    driver.scale = 1;                                 // pin 1:1; auto-scale has its own case
    driver.prepare();

    CHECK(std::string(mm::platform::encoderTestArgs()).find("-s 32x18") != std::string::npos);
    CHECK(std::string(driver.status()).find("streaming 32x18") != std::string::npos);
}

// One frame piped per tick within the rate: the grid's pixels, tight RGB, corrected: what the wall shows is what the stream shows, pixel for pixel.
// @step watch-output/add-hls
TEST_CASE("HlsDriver pipes the grid pixel-exact") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.scale = 1;                                 // 1:1, so a light is one video pixel
    driver.prepare();

    paint(source, 0, 10, 20, 30);
    paint(source, 7, 40, 50, 60);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::encoderTestFrameCount() == 1);
    CHECK(mm::platform::encoderTestFrameSize(0) == 4u * 2u * 3u);
    const uint8_t* f = mm::platform::encoderTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] == 10); CHECK(f[1] == 20); CHECK(f[2] == 30);      // first light
    CHECK(f[21] == 40); CHECK(f[22] == 50); CHECK(f[23] == 60);   // eighth light, at 7*3
}

// targetFps is a ceiling the driver enforces itself: the render loop ticks faster and the frames beyond the rate are simply not encoded.
TEST_CASE("HlsDriver encodes no faster than targetFps") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.targetFps = 10;                            // 100 ms budget per frame
    driver.prepare();

    mm::platform::setTestNowMs(1000);
    driver.tick();                                    // sends
    mm::platform::setTestNowMs(1050);
    driver.tick();                                    // inside the budget: skipped
    mm::platform::setTestNowMs(1101);
    driver.tick();                                    // budget passed: sends
    CHECK(mm::platform::encoderTestFrameCount() == 2);
}

// A full pipe is a dropped frame, counted and shown, never a blocked render tick.
TEST_CASE("HlsDriver drops frames the encoder refuses, and says so") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    mm::platform::setTestEncoderWriteResult(0);       // pipe refuses the first byte
    mm::platform::setTestNowMs(1000);
    driver.tick();
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 0);   // nothing piped...
    driver.tick1s();
    CHECK(std::string(driver.status()).find("2 frames dropped") != std::string::npos);   // ...but visible
}

// A dead encoder restarts from the housekeeping tick, and the stream resumes.
TEST_CASE("HlsDriver restarts a dead encoder and resumes streaming") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    mm::platform::setTestEncoderWriteResult(-1);      // the process is gone
    mm::platform::setTestNowMs(1000);
    driver.tick();
    driver.tick1s();
    CHECK(std::string(driver.status()).find("restarted") != std::string::npos);

    mm::platform::setTestEncoderWriteResult(1);       // back to normal recording
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 1);
}

// release() stops the encoder and leaves nothing behind; a re-prepare comes back streaming.
TEST_CASE("HlsDriver releases and re-prepares cleanly") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();
    CHECK(std::string(driver.status()).find("streaming 4x2") != std::string::npos);

    driver.release();
    mm::platform::setTestNowMs(1000);
    driver.tick();                                    // closed: pipes nothing
    CHECK(mm::platform::encoderTestFrameCount() == 0);

    driver.prepare();
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 1);
}

// The restart budget is finite: an encoder that dies on every attempt (an encoder name this ffmpeg's build lacks exits immediately after every spawn) ends at the visible give-up status, not an endless respawn loop. encoderStart() can only verify ffmpeg launches, so this status IS how an unavailable encoder surfaces.
TEST_CASE("HlsDriver gives up visibly when the encoder dies on every restart") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    mm::platform::setTestEncoderWriteResult(-1);      // every write says: process gone
    for (uint32_t t = 1000; t <= 20000; t += 1000) {  // tick1s through all restarts + backoff
        mm::platform::setTestNowMs(t);
        driver.tick();
        driver.tick1s();
    }
    CHECK(std::string(driver.status()).find("encoder exited") != std::string::npos);
}

// The frame rate must hold EXACTLY over time, not just per-frame. `1000/fps` truncates (30 fps asks for a 33 ms period, so 30 frames span 990 ms), and pacing from each frame's arrival time lets every late tick shift the schedule for good. Either way the stream drifts against the player's clock, and the player stalls to re-buffer: the periodic hiccup seen on the bench.
TEST_CASE("HlsDriver holds the exact frame rate over a full second") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.targetFps = 30;
    driver.prepare();

    // Past the encoder warm-up first: nothing is sent during it by design.
    for (uint32_t t = 1; t <= 800; t++) { mm::platform::setTestNowMs(t); driver.tick(); }
    mm::platform::encoderTestClearFrames();

    // Then tick every millisecond across exactly one second, the render loop running far faster than the rate.
    for (uint32_t t = 801; t <= 1800; t++) {
        mm::platform::setTestNowMs(t);
        driver.tick();
    }
    // Exactly the frame rate over that second. A truncated 33 ms period would fit 30 frames into 990 ms and start a 31st inside the window.
    CHECK(mm::platform::encoderTestFrameCount() == 30);
}

// A late tick must not shift the schedule: the frames after it stay on the original grid, so a one-off stall costs one frame rather than permanently offsetting the stream.
TEST_CASE("HlsDriver keeps its schedule after a late tick") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.targetFps = 10;                            // a 100 ms grid
    driver.prepare();

    // Past the warm-up, then settle onto the 100 ms grid.
    for (uint32_t t = 1; t <= 900; t++) { mm::platform::setTestNowMs(t); driver.tick(); }
    mm::platform::encoderTestClearFrames();

    mm::platform::setTestNowMs(1000);
    driver.tick();                                    // a frame on the grid
    const size_t onGrid = mm::platform::encoderTestFrameCount();
    mm::platform::setTestNowMs(1155);                 // 55 ms LATE
    driver.tick();
    const size_t afterLate = mm::platform::encoderTestFrameCount();
    CHECK(afterLate == onGrid + 1);

    // The next grid point is 1200, not 1255: a schedule re-based on arrival time would skip it.
    mm::platform::setTestNowMs(1200);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == afterLate + 1);
}

// A wall smaller than the encoder's minimum frame is blown up rather than refused: the P4's hardware encoder will not accept anything under 80x80, and a player showing a 4x2 stream renders a postage stamp. Auto picks the smallest whole factor that clears the floor on BOTH axes, so the aspect ratio is untouched.
TEST_CASE("HlsDriver blows a small wall up to the encoder's minimum, keeping its shape") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(20, 10);
    setUp(driver, source, wall, 200);
    driver.prepare();                                 // scale left at 0 = auto

    // 20x10 needs x8 to lift the SHORT axis to 80; the long axis follows at 160.
    const std::string args = mm::platform::encoderTestArgs();
    CHECK(args.find("-s 160x80") != std::string::npos);
    CHECK(std::string(driver.status()).find("20x10 as 160x80") != std::string::npos);
}

// A wall already past the minimum is left alone: auto never upscales what does not need it.
TEST_CASE("HlsDriver leaves a large enough wall at 1:1") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(128, 96);
    setUp(driver, source, wall, 128 * 96);
    driver.prepare();

    CHECK(std::string(mm::platform::encoderTestArgs()).find("-s 128x96") != std::string::npos);
}

// Upscaling replicates, never interpolates: each light becomes a solid square block, so the stream introduces no color the wall does not have and every light stays individually visible.
TEST_CASE("HlsDriver upscales by whole blocks, inventing no colors") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(2, 2);
    setUp(driver, source, wall, 4);
    driver.scale = 4;                                 // a 2x2 wall becomes an 8x8 frame
    driver.prepare();

    paint(source, 0, 10, 20, 30);                     // top-left light
    paint(source, 3, 40, 50, 60);                     // bottom-right light

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::encoderTestFrameCount() == 1);
    CHECK(mm::platform::encoderTestFrameSize(0) == 8u * 8u * 3u);
    const uint8_t* f = mm::platform::encoderTestFrameData(0);
    REQUIRE(f != nullptr);

    auto px = [&](int x, int y) { return f + (static_cast<size_t>(y) * 8 + x) * 3; };
    // Every pixel of the top-left 4x4 block is that light's color, corners included.
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        CHECK(px(x, y)[0] == 10); CHECK(px(x, y)[1] == 20); CHECK(px(x, y)[2] == 30);
    }
    // ...and the bottom-right block is the other light's, so the blocks land where the lights are.
    CHECK(px(7, 7)[0] == 40); CHECK(px(7, 7)[1] == 50); CHECK(px(7, 7)[2] == 60);
    CHECK(px(4, 4)[0] == 40); CHECK(px(4, 4)[1] == 50); CHECK(px(4, 4)[2] == 60);
}

// The SHORT axis decides the factor, and the ceiling must not get in the way. A 4x2 wall needs x40 to lift its height to 80; a ceiling below that would hand the encoder a 64x32 frame it refuses, so the feature would fail precisely on the smallest walls it exists for.
TEST_CASE("HlsDriver auto-scale clears the minimum on both axes, however thin the wall") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();                                 // auto

    const std::string args = mm::platform::encoderTestArgs();
    CHECK(args.find("-s 160x80") != std::string::npos);   // x40, driven by the height
}

// Both operands can be sane while their PRODUCT is not: lengthType is int16_t, so an 821x4 wall at scale 80 wraps to 144x320. The frame buffer would then be sized from the wrapped number while the pixel loop still walks the real 821x4 source, writing ~196 KB past the end of the heap buffer. The scaled geometry is therefore computed wide and rejected before narrowing.
TEST_CASE("HlsDriver refuses a scaled frame too large for the encoder, never wrapping into one") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(821, 4);
    setUp(driver, source, wall, 821 * 4);
    driver.scale = 80;                                // 65680x320: past the encoder, and past int16
    driver.prepare();

    // Refused with a reason, and nothing handed to the encoder.
    CHECK(std::string(driver.status()).find("exceeds the encoder") != std::string::npos);
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 0);
}

// The same guard must not refuse a frame that genuinely fits: 640x480 at scale 3 is 1920x1440, exactly the encoder's width limit and inside its height limit.
TEST_CASE("HlsDriver accepts a scaled frame that exactly meets the encoder's limit") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(640, 480);
    setUp(driver, source, wall, 640 * 480);
    driver.scale = 3;
    driver.prepare();

    CHECK(std::string(mm::platform::encoderTestArgs()).find("-s 1920x1440") != std::string::npos);
}

// A long stall resyncs the schedule rather than firing a burst to catch up. The frame sent AT the resync is the new schedule's frame 0, so the next is due a full period later. Counting it as frame 0 instead made the following tick recompute its due time back to that same instant and fire again one millisecond later, which is a duplicate frame in the stream.
TEST_CASE("HlsDriver resyncs after a stall without sending a duplicate frame") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.targetFps = 10;                            // a 100 ms grid
    driver.scale = 1;
    driver.prepare();

    // Tick every millisecond across the warm-up and well past it. The warm-up itself leaves the schedule far enough behind to trip the resync, which is exactly when the duplicate showed.
    std::vector<uint32_t> sentAt;
    size_t seen = 0;
    for (uint32_t t = 1; t <= 1200; t++) {
        mm::platform::setTestNowMs(t);
        driver.tick();
        const size_t n = mm::platform::encoderTestFrameCount();
        // At most ONE frame per tick. A catch-up burst is the very failure this test exists to catch, and recording a single timestamp for a multi-frame tick would hide it behind the interval check below.
        REQUIRE(n - seen <= 1);
        if (n != seen) { sentAt.push_back(t); seen = n; }
    }

    REQUIRE(sentAt.size() >= 2);
    // No two frames closer than one period: a resync must not emit back-to-back frames.
    for (size_t i = 1; i < sentAt.size(); i++)
        CHECK(sentAt[i] - sentAt[i - 1] >= 100u);
}

// One encoder instance, so a second claimant is refused rather than silently taking the first driver's stream.
TEST_CASE("platform encoder is claimed by one module at a time") {
    const int driverA = 1, driverB = 2;      // stand-ins for two modules' addresses
    REQUIRE(mm::platform::encoderOwner() == nullptr);

    CHECK(mm::platform::encoderClaim(&driverA));
    CHECK(mm::platform::encoderOwner() == &driverA);
    CHECK_FALSE(mm::platform::encoderClaim(&driverB));      // refused while A holds it
    CHECK(mm::platform::encoderOwner() == &driverA);        // and A keeps it

    // A re-claims what it already owns, which is what a rebuild does.
    CHECK(mm::platform::encoderClaim(&driverA));

    // A non-holder's release is ignored, so B cannot free A's encoder.
    mm::platform::encoderRelease(&driverB);
    CHECK(mm::platform::encoderOwner() == &driverA);

    mm::platform::encoderRelease(&driverA);
    CHECK(mm::platform::encoderOwner() == nullptr);
    CHECK(mm::platform::encoderClaim(&driverB));            // now B may have it
    mm::platform::encoderRelease(&driverB);
}
