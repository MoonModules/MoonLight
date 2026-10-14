/// @module platform

/// Pins the worker-task seam the multicore render/encode split is built on. On the host the seam is a std::thread and a condition variable, so these tests run the real handoff where CI can catch a race. The core pin is a no-op there.

#include "doctest.h"
#include "platform/platform.h"
#include "core/util/TryLock.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {
// A worker body counting wakes until stopped: `wakes` proves each notify landed once, `running` that the fn started; it exits when a wake finds `stop` set, the production shape.
struct Counter {
    mm::platform::WorkerTask task;
    std::atomic<int> wakes{0};
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
};

void counterBody(void* arg) {
    auto* c = static_cast<Counter*>(arg);
    c->running.store(true);
    while (!c->stop.load()) {
        // 1000 ms is the fallback; a notify (or the stop-wake) returns sooner.
        if (mm::platform::waitNotify(c->task, 1000)) {
            if (c->stop.load()) break;   // woken by the stop signal, not a real notify
            c->wakes.fetch_add(1);
        }
    }
}
}  // namespace

TEST_CASE("worker seam: each notify wakes the task exactly once") {
    Counter c;
    REQUIRE(mm::platform::spawnPinnedTask(c.task, "test", &counterBody, &c, 4096, 5, -1));
    // Wait for the fn to actually start before notifying.
    while (!c.running.load()) std::this_thread::sleep_for(1ms);

    for (int i = 0; i < 5; i++) {
        mm::platform::notifyTask(c.task);
        std::this_thread::sleep_for(5ms);   // let the consumer drain this notify
    }
    // Exactly 5 wakes, no lost notify, no double-count.
    CHECK(c.wakes.load() == 5);

    c.stop.store(true);
    mm::platform::stopPinnedTask(c.task);   // signals + wakes + joins; returns only after the fn exits
    CHECK(c.wakes.load() == 5);             // the stop-wake did NOT count as a real notify
}

TEST_CASE("worker seam: waitNotify times out and returns false when not notified") {
    // A worker that records whether its wait timed out. No notify is ever sent, so the first wait must return false (timeout), proving the WDT-service path (false → the fn re-checks its flags).
    struct TimeoutProbe {
        mm::platform::WorkerTask task;
        std::atomic<bool> sawTimeout{false};
        std::atomic<bool> stop{false};
    } p;
    auto body = [](void* arg) {
        auto* pr = static_cast<TimeoutProbe*>(arg);
        while (!pr->stop.load()) {
            if (!mm::platform::waitNotify(pr->task, 20)) pr->sawTimeout.store(true);
            else break;
        }
    };
    REQUIRE(mm::platform::spawnPinnedTask(p.task, "to", body, &p, 4096, 5, -1));
    std::this_thread::sleep_for(60ms);      // ≥ two timeout windows with no notify
    CHECK(p.sawTimeout.load());
    p.stop.store(true);
    mm::platform::stopPinnedTask(p.task);
}

TEST_CASE("worker seam: stopPinnedTask joins: the fn has returned before it returns") {
    // The stop must be a real join: after stopPinnedTask returns, the worker body is guaranteed done (so its buffers are safe to free, the render-split drain contract). A flag the body sets last proves it ran to completion before stop returned.
    struct Joiner {
        mm::platform::WorkerTask task;
        std::atomic<bool> exited{false};
        std::atomic<bool> stop{false};
    } j;
    auto body = [](void* arg) {
        auto* jr = static_cast<Joiner*>(arg);
        while (!jr->stop.load()) mm::platform::waitNotify(jr->task, 1000);
        jr->exited.store(true);   // last thing the fn does
    };
    REQUIRE(mm::platform::spawnPinnedTask(j.task, "join", body, &j, 4096, 5, -1));
    j.stop.store(true);
    mm::platform::stopPinnedTask(j.task);
    CHECK(j.exited.load());       // fn finished before stopPinnedTask returned
}

// --- TryLock: the cross-core sender latch ---------------------------------------------------------
// The WS sender has two producers on two cores under the split, core 0's drain and core 1's PreviewDriver, and TryLock excludes them; try-only by design, so the loser skips its slot.
// That constraint is what lets it be an atomic exchange on a flag rather than an OS mutex.

TEST_CASE("TryLock: acquiring excludes a second acquirer, release re-opens it") {
    mm::TryLock lk;
    REQUIRE(lk.tryAcquire());          // free → acquired
    CHECK_FALSE(lk.tryAcquire());      // held → a second acquirer is REFUSED, not blocked
    lk.release();
    CHECK(lk.tryAcquire());            // released → available again
    lk.release();
}

TEST_CASE("TryLock: a second THREAD is refused while held, and never blocks") {
    mm::TryLock lk;
    REQUIRE(lk.tryAcquire());          // this thread holds it

    // The peer must come back promptly with "busy" rather than waiting for us, the whole point of try-only: on the device that peer is the encode thread, which must not stall on the transport.
    std::atomic<bool> peerGotIt{true}, peerReturned{false};
    std::thread peer([&] {
        peerGotIt.store(lk.tryAcquire());
        if (peerGotIt.load()) lk.release();
        peerReturned.store(true);
    });
    peer.join();

    CHECK(peerReturned.load());        // it RETURNED (didn't block on us)
    CHECK_FALSE(peerGotIt.load());     // and it was correctly refused

    lk.release();
}

TEST_CASE("TryLock: LockGuard releases on scope exit, and no-ops when busy") {
    mm::TryLock lk;
    {
        mm::LockGuard g{lk};
        CHECK(bool(g));                          // acquired
        mm::LockGuard g2{lk};
        CHECK_FALSE(bool(g2));                   // busy → the guard is falsy, and must NOT release on exit
    }
    CHECK(lk.tryAcquire());            // the busy guard's destructor did not wrongly release it
    lk.release();
}
