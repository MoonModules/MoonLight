/// @module platform

/// Pins what the host's HDMI seam offers the driver, on the stand-in every test runs against.

#include "doctest.h"
#include "platform/platform.h"
#include "platform/host/screen.h"   // screen::pick, the display a desktop host shows on

TEST_CASE("The host offers HDMI modes up to 4K, beyond the ESP32's 1080p") {
    const mm::platform::HdmiMode* modes = nullptr;
    const size_t n = mm::platform::hdmiModes(&modes);
    REQUIRE(n > 1);
    CHECK(modes[n - 2].width == 3840);
    CHECK(modes[n - 2].height == 2160);
    CHECK(modes[n - 1].width == 0);   // and last the display's own mode, which leaves the display as it is
}

TEST_CASE("A desktop host shows on a display beside its own, and on its own only when asked and there is no other") {
    using mm::platform::screen::Display;
    using mm::platform::screen::pick;
    const Display alone[] = {{7, true}};
    CHECK(pick(alone, 1, 0, /*own=*/false) == 0);   // the desktop stays free
    CHECK(pick(alone, 1, 0, /*own=*/true) == 7);    // ownScreen covers it
    const Display two[] = {{7, true}, {9, false}};
    CHECK(pick(two, 2, 0, false) == 9);
    CHECK(pick(two, 2, 0, true) == 9);              // a second display wins, asked or not
    CHECK(pick(two, 2, 7, true) == 9);              // and takes the output from the host's own when it is plugged in
    const Display three[] = {{7, true}, {9, false}, {11, false}};
    CHECK(pick(three, 3, 0, false) == 9);           // the first listed
    CHECK(pick(three, 3, 11, false) == 11);         // the one shown is kept
    CHECK(pick(three, 3, 5, false) == 9);           // one that left is replaced
    CHECK(pick(nullptr, 0, 0, true) == 0);
}
