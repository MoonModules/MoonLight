/// @module RtspDriver

/// The RTSP driver's frame against the encoder's minimum: a window too narrow to reach it at the largest scale says so, naming the size.

#include "doctest.h"
#include "light/drivers/RtspDriver.h"
#include "light/layers/Buffer.h"
#include "wall_fixture.h"

#include <string>

using mm::Wall;
using mm::wire;

namespace {
struct EncSeamGuard {
    EncSeamGuard() { mm::platform::setTestEncoderMode(mm::platform::EncoderTestMode::Record); }
    ~EncSeamGuard() { mm::platform::setTestEncoderMode(mm::platform::EncoderTestMode::Off); }
};
}  // namespace

TEST_CASE("RtspDriver names a window too narrow for the encoder's minimum frame, rather than a bare refusal") {
    EncSeamGuard seam;
    mm::Buffer source;
    mm::RtspDriver driver;
    Wall wall(64, 64);
    wire(driver, source, wall, 64 * 64);
    driver.setWindow(0, 1);   // one light: a 1x1 picture, 16x16 at the largest scale
    driver.prepare();
    REQUIRE(driver.status() != nullptr);
    CHECK(std::string(driver.status()) == "a 1x1 window at scale 16 is below the encoder's 80x80: widen the window");
    driver.release();
}

TEST_CASE("RtspDriver streams an 80x80 wall at scale 1, the encoder's own minimum") {
    EncSeamGuard seam;
    mm::Buffer source;
    mm::RtspDriver driver;
    Wall wall(80, 80);
    wire(driver, source, wall, 80 * 80);
    driver.scale = 1;
    driver.prepare();
    CHECK(std::string(mm::platform::encoderTestArgs()).find("-s 80x80") != std::string::npos);
    driver.release();
}

TEST_CASE("RtspDriver names the memory a window's placement lacked, rather than asking for a wider window") {
    EncSeamGuard seam;
    mm::Buffer source;
    mm::RtspDriver driver;
    Wall wall(64, 64);
    wire(driver, source, wall, 64 * 64);
    driver.setWindow(0, 64 * 32);   // half the wall, a window whose table the refusal below denies its memory
    {
        mm::RefuseAbove refuse(1024);
        driver.prepare();
    }
    REQUIRE(driver.status() != nullptr);
    CHECK(std::string(driver.status()) == "no memory to place the window's lights by the layout");
    driver.release();
}
