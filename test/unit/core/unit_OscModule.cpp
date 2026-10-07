/// @module OscModule
/// @also Scheduler

/// OSC feedback: its settings are validated without a socket, and every value is sent again every 30 seconds, one per tick.

#include "doctest.h"
#include "core/services/OscModule.h"
#include "core/module/Scheduler.h"
#include "core/system/ControlModule.h"

#include <cstring>

using namespace mm;

namespace {
struct Rig {
    Scheduler scheduler;
    OscModule* osc = new OscModule();
    Rig() {
        osc->setName("Osc");
        scheduler.addModule(osc);
        scheduler.setup();
        REQUIRE(scheduler.setControl("Osc", "feedback", "{\"value\":true}") == Scheduler::SetControlResult::Ok);
    }
    ~Rig() { scheduler.release(); }
};
}  // namespace

TEST_CASE("OSC feedback to a mistyped host list says why rather than sending to nobody") {
    Rig rig;
    REQUIRE(rig.scheduler.setControl("Osc", "hosts", "{\"value\":\"192.168.1.999\"}") == Scheduler::SetControlResult::Ok);
    REQUIRE(rig.osc->status() != nullptr);
    CHECK(std::strstr(rig.osc->status(), "hosts:") != nullptr);
    REQUIRE(rig.scheduler.setControl("Osc", "hosts", "{\"value\":\"192.168.1.99\"}") == Scheduler::SetControlResult::Ok);
    CHECK(std::strstr(rig.osc->status(), "hosts:") == nullptr);
}

TEST_CASE("OSC multicast feedback without a group says so, and the default group needs nothing filled in") {
    Rig rig;
    REQUIRE(rig.scheduler.setControl("Osc", "addressing", "{\"value\":1}") == Scheduler::SetControlResult::Ok);
    CHECK(std::strstr(rig.osc->status(), "group") == nullptr);   // the default 239.255.77.78
    REQUIRE(rig.scheduler.setControl("Osc", "group", "{\"value\":\"\"}") == Scheduler::SetControlResult::Ok);
    CHECK(std::strstr(rig.osc->status(), "needs a group") != nullptr);
}

namespace {
// The OSC module with its datagrams counted instead of sent, so the test needs no socket and no clock.
struct CountingOsc : OscModule {
    int sent = 0;
    void sendValue(SurfaceControl, uint8_t, uint8_t) override { sent++; }
};
}  // namespace

// Regression: a follower that missed one multicast datagram kept the wrong value until that slot changed again, which for a palette was a whole scene.
TEST_CASE("OSC feedback asks for every surface value again every 30 seconds, and gets one per tick") {
    Scheduler scheduler;
    auto* control = new ControlModule();
    control->setName("Control");
    auto* osc = new CountingOsc();
    osc->setName("Osc");
    scheduler.addModule(control);
    scheduler.addModule(osc);
    scheduler.setup();
    REQUIRE(scheduler.setControl("Osc", "feedback", "{\"value\":true}") == Scheduler::SetControlResult::Ok);
    REQUIRE(scheduler.setControl("Osc", "listen", "{\"value\":true}") == Scheduler::SetControlResult::Ok);

    // Attached as the module attaches itself: seeded one value per tick, not in one burst.
    control->addSurface(osc, ControlModule::Seed::Paced);
    CHECK(osc->sent == 0);
    for (int i = 0; i < ControlModule::kSurfaceValues; i++) {
        const int before = osc->sent;
        control->tick20ms();
        CHECK(osc->sent - before == 1);
    }
    control->tick20ms();
    CHECK(osc->sent == ControlModule::kSurfaceValues);   // every value once, the pads included, then quiet

    for (int s = 0; s < 29; s++) osc->tick1s();
    control->tick20ms();
    CHECK(osc->sent == ControlModule::kSurfaceValues);   // nothing before the 30 seconds are up
    osc->tick1s();
    for (int i = 0; i < ControlModule::kSurfaceValues + 2; i++) control->tick20ms();
    CHECK(osc->sent == 2 * ControlModule::kSurfaceValues);   // and every value once more
    control->removeSurface(osc);
    scheduler.release();
}
