/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "platform/platform_clock.h"   // setTestNowMs: the one-second window follows millis, the frames follow micros

// The scheduler publishes the second's longest frame beside its average, so one slow frame a second shows as the stutter it is.

namespace {

// A module whose tick spins for `spinUs` while asked to, as a frame that runs long holds the loop.
struct Spinner : public mm::MoonModule {
    uint32_t spinUs = 0;
    void tick() MM_NONBLOCKING override {
        const uint32_t t0 = mm::platform::micros();
        while (mm::platform::micros() - t0 < spinUs) {}
    }
};

struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };

}  // namespace

TEST_CASE("Scheduler: the second's longest frame shows beside its average, and the next second starts over") {
    ClockGuard guard;
    mm::Scheduler s;
    auto* m = new Spinner();
    s.addModule(m);

    mm::platform::setTestNowMs(500);
    for (int i = 0; i < 4; i++) s.tick();
    m->spinUs = 5000;   // one long frame among short ones
    s.tick();
    m->spinUs = 0;
    for (int i = 0; i < 4; i++) s.tick();
    mm::platform::setTestNowMs(1600);   // the window closes on this tick
    s.tick();

    CHECK(s.worstFrameUs() >= 5000u);
    CHECK(s.frameUs() < s.worstFrameUs());   // the average spreads the long frame over ten
    CHECK(m->worstTimeUs() >= 5000u);        // and the module that ran long says so too

    for (int i = 0; i < 4; i++) s.tick();
    mm::platform::setTestNowMs(2700);
    s.tick();
    CHECK(s.worstFrameUs() < 5000u);
    CHECK(m->worstTimeUs() < 5000u);

    s.deleteTree(m);
}
