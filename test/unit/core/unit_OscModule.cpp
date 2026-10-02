/// @module OscModule
/// @also Scheduler

/// OSC feedback settings that need no socket: a bad host list and a missing group are reported rather than sending nowhere.

#include "doctest.h"
#include "core/services/OscModule.h"
#include "core/module/Scheduler.h"

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
