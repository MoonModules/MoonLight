/// @module OscModule
/// @also Scheduler

/// OSC feedback: its settings are validated without a socket, and over loopback every value is resent every 30 seconds, one per tick.

#include "doctest.h"
#include "core/services/OscModule.h"
#include "core/module/Scheduler.h"
#include "core/system/ControlModule.h"
#include "platform/platform.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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

// Regression: a follower that missed one multicast datagram kept the wrong value until that slot changed again, which for a palette was a whole scene.
TEST_CASE("OSC feedback sends every surface value again every 30 seconds, one per tick") {
    Scheduler scheduler;
    auto* control = new ControlModule();
    control->setName("Control");
    auto* osc = new OscModule();
    osc->setName("Osc");
    scheduler.addModule(control);
    scheduler.addModule(osc);
    scheduler.setup();

    // A client on this machine, which the feedback reaches by unicast.
    const uint16_t base = static_cast<uint16_t>(39000 + platform::millis() % 500);
    platform::UdpSocket client;
    REQUIRE(client.open());
    REQUIRE(client.bind(static_cast<uint16_t>(base + 1)));
    char body[48];
    std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(base));
    REQUIRE(scheduler.setControl("Osc", "port", body) == Scheduler::SetControlResult::Ok);
    std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(base + 1));
    REQUIRE(scheduler.setControl("Osc", "feedbackPort", body) == Scheduler::SetControlResult::Ok);
    REQUIRE(scheduler.setControl("Osc", "hosts", "{\"value\":\"127.0.0.1\"}") == Scheduler::SetControlResult::Ok);
    REQUIRE(scheduler.setControl("Osc", "feedback", "{\"value\":true}") == Scheduler::SetControlResult::Ok);
    REQUIRE(scheduler.setControl("Osc", "listen", "{\"value\":true}") == Scheduler::SetControlResult::Ok);

    uint8_t pkt[256];
    // Loopback delivery is asynchronous, so each count waits a moment for the datagrams already sent.
    const auto received = [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        int n = 0;
        while (client.recvFrom(pkt, sizeof(pkt)) > 0) n++;
        return n;
    };
    // Opening the socket and the feedback switch each seed the client; let both finish.
    for (int i = 0; i < 2 * ControlModule::kSlotCount + 4; i++) osc->tick();
    received();

    for (int s = 0; s < 29; s++) osc->tick1s();
    osc->tick();
    CHECK(received() == 0);   // nothing before the 30 seconds are up
    osc->tick1s();
    int total = 0;
    for (int i = 0; i < ControlModule::kSlotCount + 4; i++) {
        osc->tick();
        const int n = received();
        CHECK(n <= 1);        // never a burst
        total += n;
    }
    CHECK(total == ControlModule::kSlotCount);   // every value, once
    scheduler.release();
}
