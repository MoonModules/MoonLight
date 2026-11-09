/// @module Control

/// A module's control list grows as it declares, and a control past what the list can hold is left out rather than written past its end.

#include "doctest.h"
#include "core/module/Control.h"
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"

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

// Regression (sanitizers, 2026-11-09): setControl read its descriptor after the reaction, whose rebuild had moved the array to fit, a read of freed memory. A write that moves the array applies and marks the module for a save.
TEST_CASE("setControl applies a write whose rebuild moves the control array, and marks it for a save") {
    struct Five : mm::MoonModule {
        uint8_t v[5] = {};
        void defineControls() override { for (uint8_t i = 0; i < 4; i++) controls_.addControl("c", v[i]); controls_.addControl("last", v[4]); }
    };
    mm::Scheduler scheduler;
    auto* m = new Five();
    m->setName("Five");
    scheduler.addModule(m);
    m->defineControls();                  // declared once: the array doubled to 8 for 5
    REQUIRE(m->controls().capacity() == 8);
    static int saves = 0;
    saves = 0;
    scheduler.setNoteDirtyHook([] { saves++; });
    CHECK(scheduler.setControl("Five", "last", "{\"value\":7}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(m->v[4] == 7);
    CHECK(m->controls().capacity() == 5);   // the rebuild moved the array to fit
    CHECK(saves == 1);
    scheduler.release();
}
