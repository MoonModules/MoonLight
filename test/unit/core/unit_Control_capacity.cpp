/// @module Control

/// A module's control list grows as it declares, and a control past what the list can hold is left out rather than written past its end.

#include "doctest.h"
#include "core/module/Control.h"

#include <cstdint>

// The doubling once wrapped a byte at 128 to a capacity of zero, so the 129th control wrote past the array.
TEST_CASE("a control list holds 255 controls and leaves out the ones after them") {
    mm::ControlList controls;
    static uint8_t values[300];
    for (int i = 0; i < 300; i++) controls.addControl("v", values[i]);
    CHECK(controls.count() == 255);
    CHECK(controls[254].ptr == &values[254]);
}
