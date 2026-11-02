/// @module platform

/// Pins what the desktop's HDMI seam offers the driver, on the stand-in every test runs against.

#include "doctest.h"
#include "platform/platform.h"

TEST_CASE("The desktop offers HDMI modes up to 4K, beyond the ESP32's 1080p") {
    const mm::platform::HdmiMode* modes = nullptr;
    const size_t n = mm::platform::hdmiModes(&modes);
    REQUIRE(n > 0);
    CHECK(modes[n - 1].width == 3840);
    CHECK(modes[n - 1].height == 2160);
}
