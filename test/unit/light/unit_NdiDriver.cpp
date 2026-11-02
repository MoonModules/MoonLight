/// @module NdiDriver
/// @also Drivers, Correction

/// The NDI driver's frames, pinned without the proprietary runtime. CI never has one installed, so the desktop platform RECORDS what the driver handed over (platform.h § NDI test seam) exactly as it records raw-Ethernet frames for the panel driver. These tests state what a receiver would see; the bench then only has to confirm that a receiver does see it.

#include "doctest.h"
#include "light/drivers/NdiDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include <memory>
#include <string>

namespace {

// The NDI seams and virtual time are process-global, so a REQUIRE that aborts mid-case would strand a forced mode or a frozen clock for the next test, the passes-alone, fails-in-sequence flake; a guard restores both.
struct NdiSeamGuard {
    explicit NdiSeamGuard(mm::platform::NdiTestMode mode) {
        mm::platform::setTestNdiMode(mode);
    }
    ~NdiSeamGuard() {
        mm::platform::setTestNdiMode(mm::platform::NdiTestMode::Off);
        mm::platform::setTestNowMs(0);
    }
};

// A wall of `width` x `height`, wired as production wires it: the Layout gives the Layer its physical size and the driver reads that. The driver has no geometry of its own.
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

void setUp(mm::NdiDriver& driver, mm::Buffer& source, Wall& wall, mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ndiTestClearFrames();
}

// Paint light `i` a flat color, so a test can name the bytes it expects back.
void paint(mm::Buffer& b, mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t bl) {
    uint8_t* p = b.data() + static_cast<size_t>(i) * 3;
    p[0] = r; p[1] = g; p[2] = bl;
}

}  // namespace

// Without the runtime the driver is inert but SAFE, and says why. This is the state every machine without NDI installed is in, including CI, so it is the default path rather than an edge case.
TEST_CASE("NdiDriver reports a missing NDI runtime instead of failing") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceMissing};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);

    driver.prepare();
    CHECK(std::string(driver.status()).find("not installed") != std::string::npos);

    mm::platform::setTestNowMs(1000);
    driver.tick();                                   // must not crash, must send nothing
    CHECK(mm::platform::ndiTestFrameCount() == 0);
}

// The frame a receiver gets is the grid: one pixel per light, at the layer's physical size.
// @step watch-output/add-ndi
TEST_CASE("NdiDriver sends one pixel per light at the layer's size") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    paint(source, 0, 10, 20, 30);
    paint(source, 7, 40, 50, 60);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ndiTestFrameCount() == 1);
    CHECK(mm::platform::ndiTestFrameWidth(0) == 4);
    CHECK(mm::platform::ndiTestFrameHeight(0) == 2);

    const uint8_t* f = mm::platform::ndiTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] == 10); CHECK(f[1] == 20); CHECK(f[2] == 30);      // first light
    CHECK(f[21] == 40); CHECK(f[22] == 50); CHECK(f[23] == 60);   // eighth light, at 7*3
}

// The per-driver output correction is what makes a receiver see what the WALL sees: halving brightness must reach the NDI frame, not just the LEDs.
TEST_CASE("NdiDriver applies the driver's own brightness correction") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(2, 1);
    setUp(driver, source, wall, 2);
    mm::Correction half;
    mm::test::rebuildFromPreset(half, 128, mm::test::PresetOrder::RGB);
    driver.correctionForTest() = half;
    driver.prepare();

    paint(source, 0, 200, 200, 200);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ndiTestFrameCount() == 1);
    const uint8_t* f = mm::platform::ndiTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] < 200);            // dimmed, not passed through raw
    CHECK(f[0] > 0);
}

// A wire that carries RGB alone takes a wider profile's color from where the profile puts it, not its first three channels.
TEST_CASE("NdiDriver sends a white-first profile's colors, not its white") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    // On the heap: a driver and a layer outgrow a test's stack frame.
    auto source = std::make_unique<mm::Buffer>();
    auto driver = std::make_unique<mm::NdiDriver>();
    auto wall = std::make_unique<Wall>(2, 1);
    setUp(*driver, *source, *wall, 2);
    auto wrgb = std::make_unique<mm::Correction>();
    wrgb->curve = mm::Correction::Curve::Linear;
    const mm::ChannelRole roles[4] = {mm::ChannelRole::White, mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::Blue};
    wrgb->rebuild(255, roles, 4);
    driver->correctionForTest() = *wrgb;
    driver->prepare();

    paint(*source, 0, 10, 20, 30);

    mm::platform::setTestNowMs(1000);
    driver->tick();

    REQUIRE(mm::platform::ndiTestFrameCount() == 1);
    const uint8_t* f = mm::platform::ndiTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] == 10); CHECK(f[1] == 20); CHECK(f[2] == 30);
}

// fps is a CEILING: a second tick inside the interval must not produce a second frame, or a fast render loop would flood the receiver with frames it never asked for.
TEST_CASE("NdiDriver holds its frame rate to the fps ceiling") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(2, 1);
    setUp(driver, source, wall, 2);
    driver.fps = 10;                       // one frame per 100 ms
    driver.prepare();

    mm::platform::setTestNowMs(1000);
    driver.tick();
    REQUIRE(mm::platform::ndiTestFrameCount() == 1);

    mm::platform::setTestNowMs(1050);      // inside the interval
    driver.tick();
    CHECK(mm::platform::ndiTestFrameCount() == 1);

    mm::platform::setTestNowMs(1150);      // past it
    driver.tick();
    CHECK(mm::platform::ndiTestFrameCount() == 2);
}

// A blank sourceName means the device's own name, what a user scanning a receiver's source list expects to find, rather than an empty entry.
TEST_CASE("NdiDriver names the source after the device when left blank") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(2, 1);
    setUp(driver, source, wall, 2);
    driver.prepare();
    CHECK(std::string(mm::platform::ndiTestSenderName()).length() > 0);

    driver.sourceName[0] = 'W'; driver.sourceName[1] = 'a'; driver.sourceName[2] = 'l';
    driver.sourceName[3] = 'l'; driver.sourceName[4] = '\0';
    driver.prepare();
    CHECK(std::string(mm::platform::ndiTestSenderName()) == "Wall");
}

// A layer smaller than the frame must not leak the previous frame's pixels into the tail, a shrunk layout should go dark there, not show stale image.
TEST_CASE("NdiDriver blanks the tail when the layer is smaller than the frame") {
    NdiSeamGuard seam{mm::platform::NdiTestMode::ForceAvailable};
    mm::Buffer source;
    mm::NdiDriver driver;
    Wall wall(4, 2);                       // an 8-pixel frame
    setUp(driver, source, wall, 4);        // but only 4 lights behind it
    driver.prepare();

    for (mm::nrOfLightsType i = 0; i < 4; i++) paint(source, i, 99, 99, 99);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ndiTestFrameCount() == 1);
    const uint8_t* f = mm::platform::ndiTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] == 99);                     // a real light
    CHECK(f[12] == 0);                     // light 4 onward: blanked, not stale
    CHECK(f[23] == 0);
}
