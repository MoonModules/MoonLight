/// @module GamepadService
/// @also Scheduler

/// Pins the mapping: the browser's state in, row actions out, the first state as a baseline, sticks past a deadband, and learn binding the next input.

#include "doctest.h"
#include "core/services/GamepadService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for the control surface: the controls the default rows target.
struct FakeSurface : public MoonModule {
    uint8_t fader1 = 77;
    uint8_t fader2 = 77;
    uint8_t fader3 = 77;
    bool switch1 = false;
    void defineControls() override {
        controls_.addControl("fader1", fader1, 0, 255);
        controls_.addControl("fader2", fader2, 0, 255);
        controls_.addControl("fader3", fader3, 0, 255);
        controls_.addControl("switch1", switch1);
    }
};

struct Rig {
    Scheduler scheduler;
    FakeSurface* surface = new FakeSurface();
    GamepadService* pad = new GamepadService();
    Rig() {
        surface->setName("Control");
        pad->setName("Gamepad");
        scheduler.addModule(surface);
        scheduler.addModule(pad);
        scheduler.setup();
    }
    ~Rig() { scheduler.release(); }

    /// What the browser does: write the state, then let the 20 ms tick run the rows.
    void write(const char* state) {
        char body[64];
        std::snprintf(body, sizeof(body), "{\"value\":\"%s\"}", state);
        scheduler.setControl("Gamepad", "pad", body);
    }
};

}  // namespace

// A first state's sticks are rest, so a pad plugged in off-center moves nothing; its button counts, since that press is what makes a browser show the pad.
TEST_CASE("GamepadService takes a first state's sticks as rest and its buttons as presses") {
    Rig rig;
    rig.write("1,10,20,30,128");
    CHECK(rig.surface->fader1 == 77);
    CHECK(rig.surface->fader3 == 77);
    CHECK(rig.surface->switch1 == true);
}

// The default rows: A onto switch 1 as a held press, the left stick's Y onto fader 1 with up as more, its X onto fader 3.
TEST_CASE("GamepadService drives the surface through its default rows") {
    Rig rig;
    rig.write("0,128,128,128,128");
    rig.write("1,128,20,128,128");   // the stick pushed up: the browser reports 20
    CHECK(rig.surface->switch1 == true);
    CHECK(rig.surface->fader1 == 235);
    rig.write("0,200,20,128,128");
    CHECK(rig.surface->switch1 == false);   // a set row clears on the release
    CHECK(rig.surface->fader3 == 200);
}

// A stick at rest wanders a count or two, and that must not take a player away from the game.
TEST_CASE("GamepadService ignores a stick inside its deadband") {
    Rig rig;
    rig.write("0,128,128,128,128");
    rig.write("0,128,130,128,128");
    CHECK(rig.surface->fader1 == 77);
}

// A learning row binds the next pressed button, and that press does not also run the row.
TEST_CASE("GamepadService learns the next input") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.pad->addListRow(id));
    REQUIRE(rig.pad->setListRowField(id, "target", "{\"value\":\"Control.switch1\"}"));
    REQUIRE(rig.pad->setListRowField(id, "learn", "{}"));
    rig.write("0,128,128,128,128");
    rig.write("2,128,128,128,128");   // b
    char buf[256];
    JsonSink sink(buf, sizeof(buf));
    rig.pad->writeListRow(sink, rig.pad->listRowCount() - 1);
    CHECK(std::strstr(buf, "\"input\":\"b\"") != nullptr);
    CHECK(rig.surface->switch1 == false);   // the learning press binds, it does not also run the row
    REQUIRE(rig.pad->setListRowField(id, "kind", "{\"value\":\"set\"}"));
    rig.surface->switch1 = true;
    rig.write("0,128,128,128,128");   // the release of that press
    CHECK(rig.surface->switch1 == true);    // runs nothing, so it does not clear the new target
}

// A state that is not five numbers changes nothing, rather than reading as a pad with every button released.
TEST_CASE("GamepadService ignores a malformed state") {
    Rig rig;
    rig.write("0,128,128,128,128");
    rig.write("1,128,20,128,128");
    rig.write("garbage");
    CHECK(rig.surface->switch1 == true);
    CHECK(rig.surface->fader1 == 235);
}
