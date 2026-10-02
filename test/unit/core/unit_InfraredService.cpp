/// @module InfraredService
/// @also Scheduler

/// A learned remote code drives a fake surface control, the code injected as a decoded frame would arrive.

#include "doctest.h"
#include "core/services/InfraredService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"

#include <cstring>

using namespace mm;

namespace {

// Stands in for the control surface: a switch and an encoder, the controls a row names. Named "Control" so a row targeting "Control.switch1" resolves to it.
struct FakeSurface : public MoonModule {
    bool switch1 = true;
    uint8_t encoder1 = 100;
    void defineControls() override {
        controls_.addControl("switch1", switch1);
        controls_.addControl("encoder1", encoder1, 0, 255);
    }
};

// Scheduler + FakeSurface + the service, set up so Scheduler::instance() is live and controls are bound. The scheduler owns the heap-allocated modules.
struct Rig {
    Scheduler scheduler;
    FakeSurface* surface = new FakeSurface();
    InfraredService* ir = new InfraredService();
    Rig() {
        surface->setName("Control");
        ir->setName("Ir");
        scheduler.addModule(surface);
        scheduler.addModule(ir);
        scheduler.setup();   // binds controls + sets Scheduler::instance()
    }
    ~Rig() { scheduler.release(); }

    /// Add a row bound to `target`, and return its id.
    uint32_t addRow(const char* target, const char* kind = "toggle", int value = 0) {
        uint32_t id = 0;
        REQUIRE(ir->addListRow(id));
        char v[64];
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", target);
        REQUIRE(ir->setListRowField(id, "target", v));
        std::snprintf(v, sizeof(v), "{\"value\":\"%s\"}", kind);
        REQUIRE(ir->setListRowField(id, "kind", v));
        std::snprintf(v, sizeof(v), "{\"value\":%d}", value);
        REQUIRE(ir->setListRowField(id, "value", v));
        return id;
    }

    /// Arm a row for learning, then deliver a code: the on-device flow, where the next frame binds.
    void learn(uint32_t id, uint32_t code) {
        REQUIRE(ir->setListRowField(id, "learn", "{\"value\":true}"));
        ir->injectCodeForTest(code);
    }
    void fire(uint32_t code) { ir->injectCodeForTest(code); }

    /// The id of row `n`, read from the row, since earlier tests consume ids.
    uint32_t rowId(uint8_t n) const {
        char buf[256];
        JsonSink sink(buf, sizeof(buf));
        ir->writeListRow(sink, n);
        return static_cast<uint32_t>(mm::json::parseInt(buf, "id"));
    }
};

}  // namespace

TEST_CASE("a learned code toggles the control its row targets") {
    Rig rig;
    const uint32_t id = rig.addRow("Control.switch1", "toggle");
    rig.learn(id, 0x40BF);

    CHECK(rig.surface->switch1 == true);
    rig.fire(0x40BF);
    CHECK(rig.surface->switch1 == false);   // toggle reads the current value and writes its inverse
    rig.fire(0x40BF);
    CHECK(rig.surface->switch1 == true);    // and back, which a +1 delta could never do
}

TEST_CASE("a delta row nudges its target and stops at the control's own bounds") {
    Rig rig;
    const uint32_t up = rig.addRow("Control.encoder1", "delta", 16);
    const uint32_t dn = rig.addRow("Control.encoder1", "delta", -16);
    rig.learn(up, 0x1111);
    rig.learn(dn, 0x2222);

    rig.fire(0x1111);
    CHECK(rig.surface->encoder1 == 116);
    rig.fire(0x2222);
    CHECK(rig.surface->encoder1 == 100);

    // The clamp is the CONTROL's, not the row's: the row says +16 and the control says 255, so the control wins. Without this a held key would wrap a uint8 back to 0.
    for (int i = 0; i < 20; i++) rig.fire(0x1111);
    CHECK(rig.surface->encoder1 == 255);
    for (int i = 0; i < 40; i++) rig.fire(0x2222);
    CHECK(rig.surface->encoder1 == 0);
}

TEST_CASE("learning binds the next code to the armed row, and only that row") {
    Rig rig;
    const uint32_t a = rig.addRow("Control.switch1", "toggle");
    const uint32_t b = rig.addRow("Control.encoder1", "delta", 10);

    rig.learn(a, 0x1234);
    rig.learn(b, 0x5678);

    rig.fire(0x1234);
    CHECK(rig.surface->switch1 == false);            // a's code drove a
    CHECK(rig.surface->encoder1 == 100);      // and left b alone
    rig.fire(0x5678);
    CHECK(rig.surface->encoder1 == 110);
}

TEST_CASE("arming a row disarms any other, so one code cannot bind twice") {
    Rig rig;
    const uint32_t a = rig.addRow("Control.switch1", "toggle");
    const uint32_t b = rig.addRow("Control.encoder1", "delta", 10);

    // Arm a, then arm b without delivering a code: only b should be waiting. Otherwise the next frame binds to whichever row the scan reached first, which is not a user's intent.
    REQUIRE(rig.ir->setListRowField(a, "learn", "{\"value\":true}"));
    REQUIRE(rig.ir->setListRowField(b, "learn", "{\"value\":true}"));
    rig.fire(0x9999);

    rig.fire(0x9999);
    CHECK(rig.surface->encoder1 == 110);   // b learned it
    CHECK(rig.surface->switch1 == true);          // a did not
}

TEST_CASE("an unlearned code is reported and changes nothing") {
    Rig rig;
    const uint32_t id = rig.addRow("Control.switch1", "toggle");
    rig.learn(id, 0x1111);

    rig.fire(0xDEAD);
    CHECK(rig.surface->switch1 == true);                      // untouched
    CHECK(std::strstr(rig.ir->status(), "unassigned") != nullptr);
    CHECK(rig.ir->latestCode() == 0xDEAD);               // still reported, so a user can bind it
}

TEST_CASE("a target off the surface is refused, and the row keeps driving nothing") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));
    CHECK_FALSE(rig.ir->setListRowField(id, "target", "{\"value\":\"Nope.on\"}"));
    rig.learn(id, 0x4321);
    rig.fire(0x4321);                                    // must not crash
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("a row with no target does nothing at all") {
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));
    rig.learn(id, 0x7777);
    rig.fire(0x7777);                                    // an unassigned row is a valid state
    CHECK(rig.surface->switch1 == true);
}

TEST_CASE("rows are added and deleted at runtime, which is what a fixed action table could not do") {
    Rig rig;
    CHECK(rig.ir->listRowCount() == 0);
    const uint32_t a = rig.addRow("Control.switch1", "toggle");
    const uint32_t b = rig.addRow("Control.encoder1", "delta", 5);
    CHECK(rig.ir->listRowCount() == 2);

    CHECK(rig.ir->deleteListRow(a));
    CHECK(rig.ir->listRowCount() == 1);
    CHECK_FALSE(rig.ir->deleteListRow(a));   // already gone

    // b survives its sibling's removal, and keeps working: ids are stable, not positions.
    rig.learn(b, 0x0F0F);
    rig.fire(0x0F0F);
    CHECK(rig.surface->encoder1 == 105);
}

TEST_CASE("the pin state decides what the service reports about itself") {
    Rig rig;
    rig.ir->prepare();
    // No pin: a warning, because a receiver with no GPIO can never see a code, and saying "ready" there would be a lie a user cannot see through.
    CHECK(std::strstr(rig.ir->status(), "set pin") != nullptr);
}

TEST_CASE("a saved row off the surface is still reported once the receiver is ready") {
    // The restore runs before prepare, whose "ready" would otherwise replace the warning within a tick.
    Rig rig;
    REQUIRE(rig.scheduler.setControl("Ir", "pin", "{\"value\":4}") == Scheduler::SetControlResult::Ok);
    REQUIRE(rig.ir->restoreList("{\"codes\":[{\"code\":\"0x10\",\"target\":\"Drivers.on\"}]}", "codes"));
    rig.ir->prepare();
    REQUIRE(rig.ir->status() != nullptr);
    CHECK(std::strstr(rig.ir->status(), "1 row unassigned") != nullptr);
}

TEST_CASE("a code that is not a number is refused rather than binding something else") {
    // Keeping whatever prefix parsed would bind a code the user never typed.
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));

    REQUIRE(rig.ir->setListRowField(id, "code", "{\"value\":\"0x40BF\"}"));    // hex
    REQUIRE(rig.ir->setListRowField(id, "code", "{\"value\":\"16575\"}"));     // decimal

    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"40BF!\"}"));       // trailing junk
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"\"}"));            // nothing typed
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"nonsense\"}"));    // not a number
    // Past 32 bits: not a frame this receiver can ever decode, so it is a typo rather than a code.
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"4294967296\"}"));
    // Longer than the field holds: truncating would parse a DIFFERENT valid number.
    CHECK_FALSE(rig.ir->setListRowField(id, "code", "{\"value\":\"0x00000000000000000040BF\"}"));

    // The last GOOD value survived every refusal: a rejected edit changes nothing.
    rig.fire(16575);
    CHECK(std::strstr(rig.ir->status(), "unassigned") == nullptr);
}

TEST_CASE("a set row is refused on a remote, which has no release to clear it") {
    // A code has no release, so a `set` row would latch its control with nothing to clear it.
    Rig rig;
    const uint32_t id = rig.addRow("Control.encoder1", "set", 200);
    rig.learn(id, 0x5150);

    rig.fire(0x5150);
    CHECK(rig.surface->encoder1 == 100);          // untouched: not latched at 200
    CHECK(std::strstr(rig.ir->status(), "release") != nullptr);

    // Toggle and delta are unaffected: both are complete in one event.
    const uint32_t d = rig.addRow("Control.encoder1", "delta", 5);
    rig.learn(d, 0x5151);
    rig.fire(0x5151);
    CHECK(rig.surface->encoder1 == 105);
}

TEST_CASE("one remote key binds to one row, so a re-learned key moves rather than duplicates") {
    // Dispatch fires the FIRST row holding a code and stops, so a duplicate is a row that can never run. It reads as bound in the list while the key does another row's action.
    Rig rig;
    const uint32_t a = rig.addRow("Control.switch1", "toggle");
    const uint32_t b = rig.addRow("Control.encoder1", "delta", 10);

    rig.learn(a, 0x1234);
    rig.learn(b, 0x1234);          // the SAME key, onto the second row

    rig.fire(0x1234);
    CHECK(rig.surface->encoder1 == 110);   // the newest binding won
    CHECK(rig.surface->switch1 == true);          // and the first row no longer holds the code
}

TEST_CASE("an explicit false disarms a row, so a learn can be canceled") {
    // The UI's button sends "" to arm; an API's JSON `false` must disarm rather than read as that empty string.
    Rig rig;
    uint32_t id = 0;
    REQUIRE(rig.ir->addListRow(id));

    REQUIRE(rig.ir->setListRowField(id, "learn", "{\"value\":true}"));
    REQUIRE(rig.ir->setListRowField(id, "learn", "{\"value\":false}"));
    // Disarmed: the next code is NOT captured, so the row stays unbound and reports the code as unassigned rather than silently learning it.
    rig.fire(0x2468);
    CHECK(std::strstr(rig.ir->status(), "unassigned") != nullptr);
}
