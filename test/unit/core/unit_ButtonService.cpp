/// @module ButtonService
/// @also Scheduler

/// The debounce and the mapping row, a press injected as the pin reading low for long enough.

#include "doctest.h"
#include "core/services/ButtonService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "platform/platform.h"
#include "core/util/JsonSink.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for the control surface: a switch, an encoder and a pad grid, the three shapes a row can name. Only the parts a row touches: rows that publish a `slot`, and an `activate` field that fires one.
struct FakeSurface : public MoonModule, public ListSource {
    bool switch1 = true;
    uint8_t encoder1 = 100;
    uint8_t fired = 0;       ///< how many times a pad was activated
    uint8_t firedSlot = 255; ///< which one, so a test can tell pad 1 from pad 3

    void defineControls() override {
        controls_.addControl("switch1", switch1);
        controls_.addControl("encoder1", encoder1, 0, 255);
        controls_.addList("presets", *this);
    }

    bool isEditableList() const override { return true; }
    bool listAsPads() const override { return true; }
    uint8_t listRowCount() const override { return 3; }
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        // Slots 0, 2 and 5: deliberately NOT contiguous, so a test that passes by using the row index instead of the slot would fail here.
        static const uint8_t kSlots[] = {0, 2, 5};
        sink.appendf("{\"id\":%u,\"slot\":%u,\"name\":\"p%u\"}",
                     static_cast<unsigned>(row + 1), static_cast<unsigned>(kSlots[row]),
                     static_cast<unsigned>(row));
    }
    bool setListRowField(uint32_t id, const char* field, const char*) override {
        if (std::strcmp(field, "activate") != 0) return false;
        static const uint8_t kSlots[] = {0, 2, 5};
        fired++;
        firedSlot = kSlots[id - 1];
        return true;
    }
};

constexpr uint8_t kPin = 4;

struct Rig {
    Scheduler scheduler;
    FakeSurface* surface = new FakeSurface();
    ButtonService* buttons = new ButtonService();
    Rig() {
        platform::clearTestGpioLevel();
        surface->setName("Control");
        buttons->setName("Button");
        scheduler.addModule(surface);
        scheduler.addModule(buttons);
        scheduler.setup();
    }
    ~Rig() { scheduler.release(); platform::clearTestGpioLevel(); }

    /// A row on `kPin`, active-low (a switch to ground), targeting `target`.
    uint32_t addRow(const char* target, const char* kind = "toggle", int value = 0) {
        uint32_t id = 0;
        REQUIRE(buttons->addListRow(id));
        char v[64];
        std::snprintf(v, sizeof(v), "{\"value\":%d}", static_cast<int>(kPin));
        REQUIRE(buttons->setListRowField(id, "pin", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", target);
        REQUIRE(buttons->setListRowField(id, "target", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", kind);
        REQUIRE(buttons->setListRowField(id, "kind", v));
        std::snprintf(v, sizeof(v), "{\"value\":%d}", value);
        REQUIRE(buttons->setListRowField(id, "value", v));
        return id;
    }

    /// Hold a pressed or released level for `ms`, in the 20 ms poll steps.
    void hold(bool pressed, int ms) {
        platform::setTestGpioLevel(kPin, !pressed);
        for (int t = 0; t < ms; t += 20) buttons->tick20ms();
    }
};

}  // namespace

TEST_CASE("a press toggles the control its row targets, once per press") {
    Rig rig;
    rig.addRow("Control.switch1", "toggle");
    rig.hold(false, 100);                 // settle unpressed first
    CHECK(rig.surface->switch1 == true);

    rig.hold(true, 100);
    CHECK(rig.surface->switch1 == false);      // the press toggled it
    rig.hold(true, 200);
    CHECK(rig.surface->switch1 == false);      // holding does NOT toggle again
    rig.hold(false, 100);
    CHECK(rig.surface->switch1 == false);      // and neither does the release
    rig.hold(true, 100);
    CHECK(rig.surface->switch1 == true);       // the next press does
}

TEST_CASE("a bounce shorter than the debounce window is not a press") {
    Rig rig;
    rig.addRow("Control.switch1", "toggle");
    rig.hold(false, 100);

    // 20 ms of contact against a 25 ms window: a real switch does this on every press, and counting it would toggle the lights twice for one push.
    rig.hold(true, 20);
    rig.hold(false, 100);
    CHECK(rig.surface->switch1 == true);       // untouched

    // Held past the window, it counts.
    rig.hold(true, 100);
    CHECK(rig.surface->switch1 == false);
}

TEST_CASE("a momentary row writes while held and clears on release, which is what a pedal needs") {
    Rig rig;
    rig.addRow("Control.switch1", "set", 1);
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.surface->switch1 == true);       // held: written
    rig.hold(false, 100);
    CHECK(rig.surface->switch1 == false);      // released: cleared, unlike a toggle
}

TEST_CASE("a delta row nudges its target, clamped by the control") {
    Rig rig;
    rig.addRow("Control.encoder1", "delta", 25);
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.surface->encoder1 == 125);
    rig.hold(false, 100);
    rig.hold(true, 100);
    CHECK(rig.surface->encoder1 == 150);

    // Enough presses to run past 255, each one settled. The ceiling is the CONTROL's, not the row's, so a row saying +25 cannot push a uint8 past its declared max or wrap it back to 0.
    for (int i = 0; i < 10; i++) { rig.hold(false, 100); rig.hold(true, 100); }
    CHECK(rig.surface->encoder1 == 255);
}

TEST_CASE("two buttons on two pins act independently") {
    Rig rig;
    // The rig's helper wires kPin; the second row needs its own, so it is built by hand.
    rig.addRow("Control.switch1", "toggle");
    uint32_t second = 0;
    REQUIRE(rig.buttons->addListRow(second));
    REQUIRE(rig.buttons->setListRowField(second, "pin", "{\"value\":7}"));
    REQUIRE(rig.buttons->setListRowField(second, "target", "{\"value\":\"Control.encoder1\"}"));
    REQUIRE(rig.buttons->setListRowField(second, "kind", "{\"value\":\"delta\"}"));
    REQUIRE(rig.buttons->setListRowField(second, "value", "{\"value\":10}"));

    platform::setTestGpioLevel(kPin, true);   // both unpressed (active-low)
    platform::setTestGpioLevel(7, true);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();

    // Press only the second: the first must not fire. Two buttons bouncing independently is why the debounce state is per row rather than shared.
    platform::setTestGpioLevel(7, false);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.surface->encoder1 == 110);
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("a row with no pin, or no target, is a valid state and does nothing") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.buttons->addListRow(id));    // no pin, no target
    rig.hold(true, 200);                     // must not crash
    CHECK(rig.surface->switch1 == true);

    // A pin but no target: the button reads, and drives nothing.
    REQUIRE(rig.buttons->setListRowField(id, "pin", "{\"value\":4}"));
    rig.hold(false, 100);
    rig.hold(true, 200);
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("rows are added and deleted at runtime") {
    Rig rig;
    CHECK(rig.buttons->listRowCount() == 0);
    const uint32_t a = rig.addRow("Control.switch1", "toggle");
    CHECK(rig.buttons->listRowCount() == 1);
    CHECK(rig.buttons->deleteListRow(a));
    CHECK(rig.buttons->listRowCount() == 0);
    CHECK_FALSE(rig.buttons->deleteListRow(a));

    // A deleted row stops acting: its pin is no longer polled.
    rig.hold(true, 200);
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("an active-high row reads the opposite level") {
    Rig rig;
    const uint32_t id = rig.addRow("Control.switch1", "toggle");
    REQUIRE(rig.buttons->setListRowField(id, "activeLow", "{\"value\":false}"));

    // Active-high: the switch feeds 3V3, so HIGH is pressed. Settle low first.
    platform::setTestGpioLevel(kPin, false);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.surface->switch1 == true);

    platform::setTestGpioLevel(kPin, true);
    for (int t = 0; t < 100; t += 20) rig.buttons->tick20ms();
    CHECK(rig.surface->switch1 == false);
}

TEST_CASE("a target round-trips through the type and number the editor shows") {
    // A row stores a bank and a number; the name is how the wire and the interface show it. A bug in either direction silently retargets a row, which is invisible until the button does the wrong thing, so both directions are pinned here.
    struct Case { const char* target; uint8_t type; uint8_t nr; };
    const Case cases[] = {
        {"Control.switch1",  1, 1},
        {"Control.encoder3", 2, 3},
        {"Control.fader8",   3, 8},
        {"Control.pad64",    4, 64},
    };
    for (const Case& c : cases) {
        uint8_t type = 0, nr = 0;
        INFO(c.target);
        CHECK(parseTarget(c.target, type, nr));
        CHECK(type == c.type);
        CHECK(nr == c.nr);
        char back[32] = {};
        targetName(back, sizeof(back), type, nr);
        CHECK(std::strcmp(back, c.target) == 0);
    }

    // An empty target is the unassigned row, not an error.
    uint8_t type = 9, nr = 9;
    CHECK(parseTarget("", type, nr));
    CHECK(type == 0);

    // A control that is not on the surface, or a number past its bank, is refused: the surface decides what a switch drives.
    CHECK_FALSE(parseTarget("Drivers.on", type, nr));
    CHECK_FALSE(parseTarget("Control.switch9", type, nr));
    CHECK_FALSE(parseTarget("Control.switch1x", type, nr));
}

TEST_CASE("editing the type or the number re-composes the target, so they cannot disagree") {
    Rig rig;
    const uint32_t id = rig.addRow("Control.encoder1", "toggle");

    // Type 1 is "switch": the row keeps whatever number it had, which is 1 for a fresh row.
    REQUIRE(rig.buttons->setListRowField(id, "target", "{\"value\":1}"));
    REQUIRE(rig.buttons->setListRowField(id, "number", "{\"value\":5}"));

    char buf[256];
    JsonSink sink(buf, sizeof(buf));
    rig.buttons->writeListRow(sink, 0);
    CHECK(std::strstr(buf, "\"target\":\"Control.switch5\"") != nullptr);

    // A name off the surface is refused, through the API as through the editor, and the row keeps its target.
    CHECK_FALSE(rig.buttons->setListRowField(id, "target", "{\"value\":\"Drivers.palette\"}"));
    JsonSink sink2(buf, sizeof(buf));
    rig.buttons->writeListRow(sink2, 0);
    CHECK(std::strstr(buf, "\"target\":\"Control.switch5\"") != nullptr);
}

TEST_CASE("a saved row naming a control off the surface is left unassigned, and the status says so") {
    Rig rig;
    const char* saved = "{\"rows\":[{\"pin\":4,\"target\":\"Drivers.on\",\"kind\":\"toggle\"},"
                        "{\"pin\":5,\"target\":\"Control.switch1\",\"kind\":\"toggle\"}]}";
    REQUIRE(rig.buttons->restoreList(saved, "rows"));
    REQUIRE(rig.buttons->listRowCount() == 2);
    char buf[256];
    JsonSink sink(buf, sizeof(buf));
    rig.buttons->writeListRow(sink, 0);
    CHECK(std::strstr(buf, "\"target\":\"\"") != nullptr);           // unassigned
    JsonSink sink2(buf, sizeof(buf));
    rig.buttons->writeListRow(sink2, 1);
    CHECK(std::strstr(buf, "\"target\":\"Control.switch1\"") != nullptr);
    REQUIRE(rig.buttons->status() != nullptr);
    CHECK(std::strstr(rig.buttons->status(), "1 row unassigned") != nullptr);
}

TEST_CASE("a button fires the pad in that grid position, so a preset has a physical key") {
    // The point of the two-step model: a pad is a ROW on a grid, not a control, so a target naming one has to be resolved through the list. Without this a row reading `Control.pad3` looks for a control called pad3, finds nothing, and silently never works.
    Rig rig;
    rig.addRow("Control.pad3", "toggle");     // pad 3 = the third grid position, slot 2
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.surface->fired == 1);
    CHECK(rig.surface->firedSlot == 2);          // by SLOT, not by row index

    // Held, then released: a preset applies ONCE per press. Re-firing on release would re-apply the same look, and would make the pad flicker under a foot pedal.
    rig.hold(true, 200);
    rig.hold(false, 200);
    CHECK(rig.surface->fired == 1);
    rig.hold(true, 100);
    CHECK(rig.surface->fired == 2);
}

TEST_CASE("a button bound to an empty pad reports it rather than firing something else") {
    Rig rig;
    rig.addRow("Control.pad2", "toggle");     // slot 1 holds nothing: the grid has 0, 2 and 5
    rig.hold(false, 100);

    rig.hold(true, 100);
    CHECK(rig.surface->fired == 0);              // nothing near it fired
    CHECK(std::strstr(rig.buttons->status(), "empty") != nullptr);
}
