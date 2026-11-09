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

// Declaring controls doubles the array, so up to half of it can sit empty; a rebuild sizes it to what was declared, and declaring as many again fills it without moving.
TEST_CASE("a control list shrinks to its controls, and refilling as many keeps it in place") {
    mm::ControlList list;
    uint8_t v[5] = {};
    for (uint8_t& x : v) list.addControl("c", x);
    CHECK(list.count() == 5);
    CHECK(list.capacity() == 8);          // doubled from 4 to fit the fifth
    list.shrinkToFit();
    CHECK(list.capacity() == 5);
    const mm::ControlDescriptor* before = &list[0];
    list.clear();
    for (uint8_t& x : v) list.addControl("c", x);
    CHECK(list.capacity() == 5);
    CHECK(&list[0] == before);            // the same array, no allocation
}
