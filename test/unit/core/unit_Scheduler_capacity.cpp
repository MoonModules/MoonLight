/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

// Pins Scheduler::addModule at its limit. A boot that wires past the top-level array must learn of it, so the add answers false and logs a line rather than dropping the module silently.

namespace {
struct Stub : public mm::MoonModule {};
} // namespace

// The 33rd add is refused and answers false, while the first 32 are accepted and stay in order.
TEST_CASE("Scheduler::addModule refuses a module past the limit and says so") {
    mm::Scheduler s;
    Stub* mods[33];
    for (int i = 0; i < 33; i++) {
        mods[i] = new Stub();
        mods[i]->setName("M");
        const bool added = s.addModule(mods[i]);
        CHECK(added == (i < 32));
    }
    for (int i = 0; i < 32; i++) CHECK(s.module(i) == mods[i]);
    CHECK(s.module(32) == nullptr);

    CHECK_FALSE(s.addModule(nullptr));
    for (int i = 0; i < 33; i++) {
        if (i < 32) s.deleteTree(mods[i]);
        else delete mods[i];
    }
}
