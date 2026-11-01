/// @module HdmiDriver
/// @also Drivers, Correction

/// The HDMI driver's frames and status, pinned without a bridge: the desktop platform records what the driver handed over (platform.h § HDMI). The bench then only has to confirm a monitor shows it.

#include "doctest.h"
#include "light/drivers/HdmiDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "wall_fixture.h"
#include "light/layers/Buffer.h"
#include <string>

namespace {

// The stand-in and virtual time are process-global, so a guard restores both however a case leaves.
struct HdmiSeamGuard {
    HdmiSeamGuard() { mm::platform::hdmiTestClear(); }
    ~HdmiSeamGuard() { mm::platform::hdmiTestClear(); mm::platform::setTestNowMs(0); }
};

// A 4x2 wall with its source buffer and driver, the correction on RGB, as every case starts.
struct Bench {
    HdmiSeamGuard seam;
    mm::Buffer source;
    mm::HdmiDriver driver;
    mm::test::Wall wall{4, 2};
    Bench() { mm::test::wire(driver, source, wall, 8); }
};

}  // namespace

TEST_CASE("HdmiDriver offers the platform's modes and starts in 1280x720 at 60 Hz") {
    Bench b;
    b.driver.prepare();
    CHECK(mm::platform::hdmiTestOpenMode() == 2);
    const mm::platform::HdmiMode* modes = nullptr;
    REQUIRE(mm::platform::hdmiModes(&modes) > 2);
    CHECK(modes[2].width == 1280);
    CHECK(modes[2].height == 720);
}

TEST_CASE("HdmiDriver hands over one pixel per light at the layer's size, keeping its aspect by default") {
    Bench b;
    uint8_t* p = b.source.data() + 5 * 3;
    p[0] = 10; p[1] = 20; p[2] = 30;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    CHECK(mm::platform::hdmiTestFrameWidth() == 4);
    CHECK(mm::platform::hdmiTestFrameHeight() == 2);
    CHECK(mm::platform::hdmiTestFrameFit());
    const uint8_t* f = mm::platform::hdmiTestFrameData();
    REQUIRE(f != nullptr);
    CHECK(f[5 * 3 + 0] == 10);
    CHECK(f[5 * 3 + 1] == 20);
    CHECK(f[5 * 3 + 2] == 30);
}

TEST_CASE("HdmiDriver stretches when scaling is set to stretch") {
    Bench b;
    b.driver.scaling = 1;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    CHECK_FALSE(mm::platform::hdmiTestFrameFit());
}

TEST_CASE("HdmiDriver holds its frame rate to the fps ceiling") {
    Bench b;
    b.driver.fps = 10;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    mm::platform::setTestNowMs(1050);   // inside the 100 ms window
    b.driver.tick();
    CHECK(mm::platform::hdmiTestFrameCount() == 1);
    mm::platform::setTestNowMs(1100);
    b.driver.tick();
    CHECK(mm::platform::hdmiTestFrameCount() == 2);
}

TEST_CASE("HdmiDriver says when no monitor is connected, and shows the picture once one is") {
    Bench b;
    b.driver.prepare();
    CHECK(std::string(b.driver.status()).find("no monitor") != std::string::npos);
    mm::platform::setTestHdmiConnected(true);
    b.driver.tick1s();
    CHECK(std::string(b.driver.status()).find("4x2 on 1280x720 60 Hz") != std::string::npos);
}

TEST_CASE("HdmiDriver keeps the output through a re-prepare in the same mode, and restarts it on a mode change") {
    Bench b;
    b.driver.prepare();
    mm::platform::setTestNowMs(1000);
    b.driver.tick();
    REQUIRE(mm::platform::hdmiTestFrameCount() == 1);
    b.driver.prepare();   // the tree sweep a network change or an upload runs
    CHECK(mm::platform::hdmiTestFrameCount() == 1);   // a closed output keeps no frames, so the output stayed open
    CHECK(mm::platform::hdmiTestOpenMode() == 2);
    b.driver.mode = 1;
    b.driver.prepare();
    CHECK(mm::platform::hdmiTestFrameCount() == 0);
    CHECK(mm::platform::hdmiTestOpenMode() == 1);
}

TEST_CASE("HdmiDriver closes the output on release, so the screen goes dark") {
    Bench b;
    b.driver.prepare();
    REQUIRE(mm::platform::hdmiTestOpenMode() >= 0);
    b.driver.release();
    CHECK(mm::platform::hdmiTestOpenMode() == -1);
    CHECK(b.driver.status() == nullptr);   // the base release clears it along with the staging
}
