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
    CHECK(std::string(driver.status()) == "a 1x1 window at scale 16 is below the encoder's 128x96: widen the window");
    driver.release();
}
