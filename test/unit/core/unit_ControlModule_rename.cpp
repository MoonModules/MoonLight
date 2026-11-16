/// @module ControlModule

#include "doctest.h"
#include "core/system/ControlModule.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// A surface binding names its module as "<module>.<control>", so a rename rewrites the bindings that named the old module and leaves the rest.

namespace {

char* targetOf(mm::ControlModule& c, const char* which) {
    auto& cs = c.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, which) == 0) return static_cast<char*>(cs[i].ptr);
    FAIL("no control ", which);
    return nullptr;
}

void assign(mm::ControlModule& c, const char* which, const char* target) {
    std::snprintf(targetOf(c, which), 40, "%s", target);
}

}  // namespace

TEST_CASE("a rename rewrites every surface binding that named the old module, and only those") {
    auto owned = std::make_unique<mm::ControlModule>();   // on the heap, as the module lives: its 24 bindings make a large frame
    mm::ControlModule& c = *owned;
    c.defineControls();
    assign(c, "fader1Target", "Knob.value");
    assign(c, "switch1Target", "Knob.on");
    assign(c, "encoder1Target", "Knobs.speed");   // another module whose name starts the same
    assign(c, "fader2Target", "Drivers.brightness");

    c.onModuleRenamed("Knob", "Dial");

    CHECK(std::string(targetOf(c, "fader1Target")) == "Dial.value");
    CHECK(std::string(targetOf(c, "switch1Target")) == "Dial.on");
    CHECK(std::string(targetOf(c, "encoder1Target")) == "Knobs.speed");
    CHECK(std::string(targetOf(c, "fader2Target")) == "Drivers.brightness");
    CHECK(c.dirty());
}

TEST_CASE("a binding the new name makes too long stays as it was, and the card says so") {
    auto owned = std::make_unique<mm::ControlModule>();   // on the heap, as the module lives: its 24 bindings make a large frame
    mm::ControlModule& c = *owned;
    c.defineControls();
    const std::string control(30, 'c');   // "K." plus 30 fits the 39 characters a binding holds; a 15-character name plus 30 does not
    assign(c, "fader1Target", ("K." + control).c_str());
    c.onModuleRenamed("K", "FifteenCharName");
    CHECK(std::string(targetOf(c, "fader1Target")) == "K." + control);
    REQUIRE(c.status() != nullptr);
    CHECK(std::string(c.status()).find("no longer fit") != std::string::npos);
}

TEST_CASE("a rename no binding names leaves the surface unsaved") {
    auto owned = std::make_unique<mm::ControlModule>();   // on the heap, as the module lives: its 24 bindings make a large frame
    mm::ControlModule& c = *owned;
    c.defineControls();
    c.onModuleRenamed("Nobody", "Someone");
    CHECK_FALSE(c.dirty());
}
