/// @module AnalogService
/// @also InputMapping, Scheduler

/// An injected ADC reading, through the row's travel and filter, into a surface control.

#include "doctest.h"
#include "core/services/AnalogService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

using namespace mm;

namespace {

/// Stands in for the control surface: a fader and a switch a row can drive.
struct FakeSurface : public MoonModule {
    uint8_t fader1 = 0;
    bool    switch1 = false;
    void defineControls() override {
        controls_.addControl("fader1", fader1, 0, 255);
        controls_.addControl("switch1", switch1);
    }
};

constexpr uint8_t kPin = 4;

struct Rig {
    Scheduler scheduler;
    FakeSurface* surface = new FakeSurface();
    AnalogService* svc = new AnalogService();
    uint32_t rowId = 0;
    Rig() {
        platform::clearTestAdcValue();
        surface->setName("Control");
        svc->setName("Analog");
        scheduler.addModule(surface);
        scheduler.addModule(svc);
        scheduler.setup();
        REQUIRE(svc->addListRow(rowId));
        set("pin", kPin);
        // No `kind`: an analog row writes the scaled level, so the module refuses that field.
        set("target", "\"Control.fader1\"");
    }
    ~Rig() { scheduler.release(); platform::clearTestAdcValue(); }

    void set(const char* field, int v) {
        char body[64];
        std::snprintf(body, sizeof(body), "{\"value\":%d}", v);
        REQUIRE(svc->setListRowField(rowId, field, body));
    }
    void set(const char* field, const char* rawJson) {
        char body[96];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", rawJson);
        REQUIRE(svc->setListRowField(rowId, field, body));
    }

    /// Hold a raw count for `ticks` polls, long enough for the filter to settle on it.
    void hold(uint16_t raw, int ticks = 60) {
        platform::setTestAdcValue(kPin, raw);
        for (int i = 0; i < ticks; i++) svc->tick20ms();
    }
};

}  // namespace

TEST_CASE("an analog input drives a control across its travel") {
    // The whole path in one: a pin reading becomes a control value. The ends are what a user actually notices, because a pedal that cannot reach 0 or full is the complaint this module's min/max exists to answer.
    Rig rig;
    const uint16_t full = platform::adcMaxCount();

    rig.hold(0);
    CHECK(rig.surface->fader1 == 0);

    rig.hold(full);
    CHECK(rig.surface->fader1 == 255);

    rig.hold(static_cast<uint16_t>(full / 2));
    // Half travel is half value, within the filter's own resolution.
    CHECK(rig.surface->fader1 > 118);
    CHECK(rig.surface->fader1 < 138);
}

TEST_CASE("a pedal's usable travel is what maps, not the full sweep") {
    // A real pedal rests above 0 and tops out below full scale, so a raw mapping never reaches either end.
    Rig rig;
    rig.set("inMin", 1000);
    rig.set("inMax", 3000);

    rig.hold(1000);
    CHECK(rig.surface->fader1 == 0);        // the bottom of the TRAVEL is the bottom of the range
    rig.hold(3000);
    CHECK(rig.surface->fader1 == 255);      // and the top is the top

    // Below and above the travel clamp rather than wrapping or running negative.
    rig.hold(200);
    CHECK(rig.surface->fader1 == 0);
    rig.hold(4000);
    CHECK(rig.surface->fader1 == 255);
}

TEST_CASE("an inverted input reads the other way round") {
    // A pot wired the other way is a wiring choice, not a fault, so it is a checkbox rather than a reason to resolder.
    Rig rig;
    rig.set("invert", 1);
    rig.hold(0);
    CHECK(rig.surface->fader1 == 255);
    rig.hold(platform::adcMaxCount());
    CHECK(rig.surface->fader1 == 0);
}

TEST_CASE("a reversed min/max pair means inverted, rather than being an error") {
    // A user calibrating by moving the pedal to each end sets whichever end they reached first. Refusing that would reject a calibration that says exactly what it means.
    Rig rig;
    rig.set("inMin", 3000);
    rig.set("inMax", 1000);
    rig.hold(1000);
    CHECK(rig.surface->fader1 == 255);
    rig.hold(3000);
    CHECK(rig.surface->fader1 == 0);
}

TEST_CASE("a resting input stops writing, so jitter does not flood the control") {
    // An ADC wobbles a count or two at rest; without a deadband the row writes its target fifty times a second.
    Rig rig;
    rig.hold(2000);
    const uint8_t settled = rig.surface->fader1;

    // Something else moves the control; a resting pedal must not fight it back.
    rig.surface->fader1 = 42;
    platform::setTestAdcValue(kPin, 2001);       // one count of jitter
    for (int i = 0; i < 20; i++) rig.svc->tick20ms();
    CHECK(rig.surface->fader1 == 42);            // not rewritten

    // A real move still gets through.
    rig.hold(3500);
    CHECK(rig.surface->fader1 != 42);
    CHECK(rig.surface->fader1 > settled);
}

TEST_CASE("the first reading is taken whole, so a pedal does not sweep up from zero on boot") {
    // A filter seeded with 0 would ramp every input up from the bottom at startup, fading a light up on boot.
    Rig rig;
    platform::setTestAdcValue(kPin, platform::adcMaxCount());
    rig.svc->tick20ms();                          // ONE poll
    CHECK(rig.surface->fader1 == 255);            // already there, not on its way
}

TEST_CASE("an analog row scales into whatever range its surface control holds") {
    // The 0..255 travel is rescaled to the control's own bounds, so a switch gets off and on.
    Rig rig;
    rig.set("target", "\"Control.switch1\"");
    rig.hold(0);
    CHECK(rig.surface->switch1 == false);
    rig.hold(platform::adcMaxCount());
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("an analog row refuses a field it would silently ignore") {
    // `runInputLevel` writes the scaled reading, so `kind` and `value` have nothing to say here. Accepting them would store a setting the module ignores, which reads as a bug in the mapping rather than in the row's configuration.
    Rig rig;
    CHECK_FALSE(rig.svc->setListRowField(rig.rowId, "kind", "{\"value\":\"toggle\"}"));
    CHECK_FALSE(rig.svc->setListRowField(rig.rowId, "value", "{\"value\":42}"));
    // The target, which an analog row DOES have, still takes an edit.
    CHECK(rig.svc->setListRowField(rig.rowId, "target", "{\"value\":\"Control.fader1\"}"));
}

TEST_CASE("an out-of-range pin is refused rather than narrowed into a different pin") {
    // 300 would narrow to 44, a pin nobody named; -1 stays valid as the unconfigured state.
    Rig rig;
    CHECK_FALSE(rig.svc->setListRowField(rig.rowId, "pin", "{\"value\":300}"));
    CHECK_FALSE(rig.svc->setListRowField(rig.rowId, "pin", "{\"value\":-2}"));
    CHECK(rig.svc->setListRowField(rig.rowId, "pin", "{\"value\":-1}"));
    CHECK(rig.svc->setListRowField(rig.rowId, "pin", "{\"value\":48}"));
}

TEST_CASE("an analog row pointed at a pad refuses, rather than firing it every tick") {
    // A pedal at 40% of a preset means nothing, and a silent refusal would look like a broken pot, so it is reported.
    Rig rig;
    rig.set("target", "\"Control.pad1\"");
    rig.hold(2000);
    // Nothing reached the surface control, and the module says why.
    CHECK(rig.surface->fader1 == 0);
    const char* s = rig.svc->status();
    REQUIRE(s != nullptr);
    // The PAD-specific refusal, not the generic "has no such control": runInputLevel rejects a pad target before any lookup, so this holds whether or not a pad grid exists.
    CHECK(std::strstr(s, "a pad takes a press") != nullptr);
}

TEST_CASE("an unconfigured or unassigned row does nothing, quietly") {
    // Robustness: a fresh row has no pin and no target, which is a valid state a user passes through rather than a fault to report.
    Scheduler sched;
    auto* surface = new FakeSurface();
    auto* svc = new AnalogService();
    surface->setName("Control");
    svc->setName("Analog");
    sched.addModule(surface);
    sched.addModule(svc);
    sched.setup();

    uint32_t id = 0;
    REQUIRE(svc->addListRow(id));
    for (int i = 0; i < 10; i++) svc->tick20ms();   // no pin named at all
    CHECK(surface->fader1 == 0);

    sched.release();
}

TEST_CASE("a saved analog row naming a control off the surface is left unassigned, and the status says so") {
    Rig rig;
    REQUIRE(rig.svc->restoreList("{\"inputs\":[{\"pin\":4,\"target\":\"Drivers.brightness\"}]}", "inputs"));
    REQUIRE(rig.svc->status() != nullptr);
    CHECK(std::strstr(rig.svc->status(), "1 row unassigned") != nullptr);
}
