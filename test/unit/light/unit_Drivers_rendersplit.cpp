/// @module Drivers
/// @also platform

/// Pins the multicore render↔encode split on the host, where platform::spawnPinnedTask is a real std::thread, so TSan and ASan can catch a race or use-after-free on the shared outputBuffer_. One rule under test: while `multicore` is on, every driver's tick() runs on the worker against the finished frame, and core 0 returns to rendering at once.

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "core/module/Scheduler.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

using namespace std::chrono_literals;

namespace {

// A driver stub that records what it reads each tick; `sawTear` latches on the producer's mid-write sentinel (0xEE), so a clear flag proves core 0 left outputBuffer_ alone while the worker read it.
class MockDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* b) override { src_ = b; }
    void tick() MM_NONBLOCKING override {
        ticks.fetch_add(1);
        if (src_ && src_->data() && src_->count() > 0 && src_->data()[0] == 0xEE)
            sawTear.store(true);
    }
    mm::Buffer* src_ = nullptr;
    std::atomic<int> ticks{0};
    std::atomic<bool> sawTear{false};
};

// A driver the test parks inside its own tick(), standing in for a long encode: a condition variable, not a sleep, so the test opens and closes the race window on demand, with no timing luck and no unbounded spin.
class SlowDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    // DELIBERATELY blocking despite MM_NONBLOCKING, inherited from MoonModule::tick: the mutex and the timed wait hold the use-after-free window open, so clang-hotpath reports these lines, correctly.
    void tick() MM_NONBLOCKING override {
        {
            std::unique_lock<std::mutex> lk(m);
            inTick = true;
            entered.notify_all();            // "core 1 is now INSIDE this object"
            // Hold here until the test says go. This is the use-after-free window, held open on demand.
            release.wait_for(lk, 5s, [this] { return released; });
        }
        touched = 0xABCD;                    // ASan traps here if core 0 freed us mid-tick
        std::scoped_lock<std::mutex> lk(m);
        inTick = false;
        exited.notify_all();
    }
    // Block until the worker is provably inside tick(). Bounded: a false return means it never got there, which the caller asserts on rather than spinning forever.
    bool waitEntered() {
        std::unique_lock<std::mutex> lk(m);
        return entered.wait_for(lk, 5s, [this] { return inTick; });
    }
    void letGo() {
        { std::scoped_lock<std::mutex> lk(m); released = true; }
        release.notify_all();
    }
    bool isInTick() { std::scoped_lock<std::mutex> lk(m); return inTick; }

    std::mutex m;
    std::condition_variable entered, release, exited;
    bool inTick = false, released = false;
    uint16_t touched = 0;
};

// Two enabled Effects over a shared Layouts/Grid → needOutput is true (≥2 layers composite), so a real outputBuffer_ exists for the driver to read across the core boundary.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects layers;
    mm::Layer layerA;
    mm::Layer layerB;
    mm::Drivers drivers;
    explicit Rig(uint16_t w) {
        grid.width = w; grid.height = 1; grid.depth = 1;
        layouts.addChild(&grid);
        layerA.setChannelsPerLight(3);
        layerB.setChannelsPerLight(3);
        layers.addChild(&layerA);
        layers.addChild(&layerB);
        layers.setLayouts(&layouts);   // propagates to the child layers
        drivers.setEffects(&layers);
        layers.applyState();           // sizes the layer buffers
    }
};

// File-static the quiesce-render hook reads (the hook is a non-capturing function pointer). Set to a rig's drivers for the layout-mutation test below; null the rest of the time.
mm::Drivers* g_hookDrivers = nullptr;

// RAII: install the quiesce-render hook pointing at `d`, and clear the hook and the file static on every exit, a thrown REQUIRE included, so the global hook outlives no rig it points at.
struct HookGuard {
    explicit HookGuard(mm::Drivers* d) {
        g_hookDrivers = d;
        mm::MoonModule::setQuiesceRenderHook([] { if (g_hookDrivers) g_hookDrivers->quiesceRenderSplit(); });
    }
    ~HookGuard() { mm::MoonModule::setQuiesceRenderHook(nullptr); g_hookDrivers = nullptr; }
};

}  // namespace

// The bench flap: one layer, one driver, multicore. needOutput is false (one layer, no LUT), so only splitWanted holds the split; many ticks with no config change must keep it engaged.
TEST_CASE("render-split: a single-layer multicore config stays engaged across many ticks (no flap)") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects layers;
    mm::Layer layer;
    mm::Drivers drivers;
    grid.width = 64; grid.height = 1; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setChannelsPerLight(3);
    layers.addChild(&layer);          // ONE layer (the bench config), not two
    layers.setLayouts(&layouts);
    drivers.setEffects(&layers);
    layers.applyState();

    MockDriver d;
    drivers.addChild(&d);
    drivers.setup();
    drivers.prepare();
    REQUIRE(drivers.renderSplitActive());   // splitWanted forces the buffer even for one no-LUT layer

    // 30 ticks with config untouched: renderSplitActive() is set and cleared synchronously in prepare() and tick(), so it is asserted right after each tick, with no sleep.
    for (int i = 0; i < 30; i++) {
        drivers.tick();
        CHECK(drivers.renderSplitActive());   // any false here reproduces the bench flap
    }

    drivers.release();
}

TEST_CASE("render-split: multicore on → every driver ticks on the worker, never on a torn frame") {
    Rig r(64);
    MockDriver a, b;                          // two drivers: BOTH move to core 1, no per-driver opt-out
    r.drivers.addChild(&a);
    r.drivers.addChild(&b);
    r.drivers.setup();
    r.drivers.prepare();                      // engage predicate runs here

    REQUIRE(r.drivers.renderSplitActive());   // a driver exists + outputBuffer_ allocated → split ON

    for (int i = 0; i < 10; i++) {
        r.drivers.tick();                     // composites, notifies the worker
        std::this_thread::sleep_for(2ms);     // let the worker drain this frame
    }
    r.drivers.quiesce();               // wait the last tick out before asserting

    CHECK(a.ticks.load() >= 1);               // the worker ran BOTH drivers
    CHECK(b.ticks.load() >= 1);
    CHECK_FALSE(a.sawTear.load());            // read no mid-write buffer: the boundary held
    CHECK_FALSE(b.sawTear.load());

    r.drivers.release();                      // stops + joins the worker (no hang, no leak)
    CHECK_FALSE(r.drivers.renderSplitActive());
}

TEST_CASE("render-split: multicore off → drivers tick inline on the render core (the proven path)") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.multicore = false;              // the user's switch
    r.drivers.setup();
    r.drivers.prepare();

    CHECK_FALSE(r.drivers.renderSplitActive());   // no split, no worker task
    for (int i = 0; i < 3; i++) r.drivers.tick();
    CHECK(d.ticks.load() == 3);               // ticked inline, once per frame, on this thread
    r.drivers.release();
}

// Regression (v3.0.0): a detached probe's first swapPeripheral, from a null backend, notifies no quiesce, so GET /api/types building throwaway probes leaves the live split engaged.
TEST_CASE("render-split: a detached ParallelLedDriver's peripheral swap does not disturb the live split") {
    // RAII: a doctest REQUIRE failure throws, so a bare set-then-reset would leak the global hook into later tests; the guard resets it on every exit path.
    struct HookGuard {
        HookGuard()  { mm::MoonModule::setQuiesceRenderHook([] { if (auto* d = mm::Drivers::active()) d->quiesceRenderSplit(); }); }
        ~HookGuard() { mm::MoonModule::setQuiesceRenderHook(nullptr); }
    } hookGuard;

    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());   // the LIVE split is engaged

    // A DETACHED ParallelLedDriver (never addChild'd, no parent), exactly like the /api/types probe. Its construction + defineControls run selectDefaultPeripheral → swapPeripheral, which must NOT reach the live render worker through the global hook.
    {
        mm::ParallelLedDriver probe;
        probe.defineDriverControls();   // triggers the default-peripheral selection + swap
    }   // probe destructs — the moment /api/types tears its throwaway down

    // THE ASSERTION: the detached probe's swap must not have touched the live split.
    CHECK(r.drivers.renderSplitActive());     // fails on the v3.0.0 bug (probe fired the hook → live teardown)

    r.drivers.release();
}

TEST_CASE("render-split: no driver → the split does not engage (nothing to move)") {
    Rig r(64);
    r.drivers.setup();
    r.drivers.prepare();
    CHECK_FALSE(r.drivers.renderSplitActive());   // no output work → no task, no handoff buffer claim
    r.drivers.tick();                             // must not crash with no children
    r.drivers.release();
}

TEST_CASE("render-split: live disengage stops the worker when the last driver leaves") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());
    r.drivers.tick();
    std::this_thread::sleep_for(2ms);

    r.drivers.removeChild(&d);                // the last driver leaves
    r.drivers.prepare();                      // re-evaluate → disengage (worker stopped + joined)
    CHECK_FALSE(r.drivers.renderSplitActive());
    r.drivers.release();
}

// Core quiesces the worker before any structural change of a container's children; the driver is deleted while the worker is provably inside its tick(), and removeChild must not return until it is out.
TEST_CASE("render-split: deleting a driver WHILE core 1 is inside its tick() is safe (no use-after-free)") {
    Rig r(64);
    auto* slow = new SlowDriver();            // heap: so ASan can see a use-after-free if we regress
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // notifies core 1 → the worker enters slow->tick()

    // Park the worker INSIDE tick(), a latch, not a sleep, so the race window is opened on demand rather than by timing luck, and a worker that never starts fails here instead of spinning forever.
    REQUIRE(slow->waitEntered());
    CHECK(slow->isInTick());                  // the worker is mid-encode, right now

    // A releaser lets the parked encode finish shortly after the delete is under way, the real sequence; `releasedAt` records when, so the assertion below proves removeChild() waited.
    std::atomic<bool> releaseFired{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);    // long enough that a non-waiting removeChild returns first
        releaseFired.store(true);
        slow->letGo();
    });

    r.drivers.removeChild(slow);              // core's quiesce() blocks here until the worker is out

    // Checked the instant removeChild returns, not after the join, where a non-waiting removeChild would look correct once the worker finished on its own.
    CHECK(releaseFired.load());               // it waited for the encode (fails without quiesce)
    CHECK_FALSE(slow->isInTick());            // and the worker is provably out of tick()

    releaser.join();
    delete slow;                              // now safe — the worker is provably not inside it

    r.drivers.prepare();                      // last driver gone → disengage
    CHECK_FALSE(r.drivers.renderSplitActive());
    r.drivers.release();
}

// A node outside the Drivers subtree, a layout, removed while the worker runs: a driver walks the whole tree, so quiesceForMutation() reaches the worker through the quiesce-render hook.
TEST_CASE("render-split: mutating a LAYOUT while core 1 runs is safe via the quiesce-render hook") {
    Rig r(64);
    // A driver whose tick() reads the shared source buffer, enough to keep the worker busy on core 1.
    auto* slow = new SlowDriver();
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    // The quiesce-render hook wired as main.cpp does, through a file static since the hook is a plain function pointer; the RAII guard clears it on every exit path.
    HookGuard hook(&r.drivers);

    r.drivers.tick();                         // notify core 1 → worker enters slow->tick()
    REQUIRE(slow->waitEntered());
    CHECK(slow->isInTick());                  // worker mid-tick, holding the tree

    std::atomic<bool> releaseFired{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);
        releaseFired.store(true);
        slow->letGo();
    });

    // The grid leaves Layouts: quiesceForMutation() must block on the worker through the hook, or a later free of `grid` is a use-after-free while the worker still walks it.
    r.layouts.removeChild(&r.grid);

    CHECK(releaseFired.load());               // it waited for the encode (fails without the hook)
    CHECK_FALSE(slow->isInTick());            // the worker is provably out

    releaser.join();

    r.drivers.release();
    delete slow;
    // The HookGuard clears the hook + g_hookDrivers on scope exit (including a thrown REQUIRE above).
}

// Reordering children (moveChildTo, the drag-reorder) shifts slots under the worker's index loop, ticking a child twice or skipping one, so it quiesces as the other three mutators do.
TEST_CASE("render-split: reordering children (moveChildTo) while core 1 runs waits for the worker") {
    Rig r(64);
    auto* slow = new SlowDriver();
    auto* second = new SlowDriver();          // a second driver so there IS a reorder to do
    r.drivers.addChild(slow);
    r.drivers.addChild(second);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    HookGuard hook(&r.drivers);

    r.drivers.tick();                         // worker enters the first driver's tick()
    REQUIRE(slow->waitEntered());
    CHECK(slow->isInTick());

    std::atomic<bool> releaseFired{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);
        releaseFired.store(true);
        slow->letGo();
        second->letGo();                      // release both in case the worker moved on to the second
    });

    r.drivers.moveChildTo(second, 0);         // reorder while the worker is mid-tick

    CHECK(releaseFired.load());               // moveChildTo waited for the encode (fails without quiesce)
    CHECK_FALSE(slow->isInTick());

    releaser.join();
    r.drivers.release();
    delete slow;
    delete second;
}

TEST_CASE("render-split: toggling multicore live engages and disengages the worker") {
    Rig r(64);
    MockDriver d;
    r.drivers.addChild(&d);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());   // default ON

    r.drivers.multicore = false;              // the UI switch, applied live via the prepare sweep
    r.drivers.prepare();
    CHECK_FALSE(r.drivers.renderSplitActive());   // worker stopped + joined, back to inline
    const int inlineBefore = d.ticks.load();
    r.drivers.tick();
    CHECK(d.ticks.load() == inlineBefore + 1);    // ticked inline on this thread

    r.drivers.multicore = true;               // and back on again
    r.drivers.prepare();
    CHECK(r.drivers.renderSplitActive());     // worker respawned — no reboot needed
    r.drivers.release();
}

// A wedged worker must not hang the render loop: the boundary times out, drops the split, and every driver ticks inline on core 0; tick()'s boundary, not quiesce(), which joins the worker on timeout.
TEST_CASE("render-split: a wedged worker degrades to single-core instead of hanging the render loop") {
    Rig r(64);
    auto* slow = new SlowDriver();            // parks inside tick() until we release it
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // core 1 enters slow->tick() and parks there
    REQUIRE(slow->waitEntered());             // it is provably stuck

    // The worker is released first so the drop cannot block on it; it still has not signalled done for this frame, so the wait below is a genuine wedge to the boundary.
    const auto t0 = std::chrono::steady_clock::now();
    const bool completed = r.drivers.quiesceEncodeForTest();   // the boundary wait, in isolation
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_FALSE(completed);                           // it TIMED OUT rather than spinning forever
    CHECK(elapsed < 2s);                              // and gave up promptly (timeout is 500 ms)
    CHECK_FALSE(r.drivers.renderSplitActive());       // disengaged: drivers now tick inline on core 0
    CHECK(r.drivers.status() != nullptr);             // and the user is told why

    slow->letGo();                            // let the parked worker unwind
    r.drivers.release();                      // joins it
    delete slow;
}

// A degraded state ends: the stall warning lifts once the split is healthy again, so a card describes the device rather than its history (reported from a Linux bench).
TEST_CASE("render-split: the stall warning clears once the worker is healthy again") {
    Rig r(64);
    auto* slow = new SlowDriver();
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();
    REQUIRE(slow->waitEntered());
    REQUIRE_FALSE(r.drivers.quiesceEncodeForTest());   // stall: the warning goes up
    REQUIRE(std::strlen(r.drivers.status()) > 0);

    // The worker is stopped and joined before the child list changes: the timed-out quiesce cleared renderSplitActive_, so removeChild's quiesce() returns at once (TSan caught that race in CI); release() joins.
    slow->letGo();
    r.drivers.release();
    r.drivers.removeChild(slow);
    delete slow;

    // A healthy rig, and the split engaged again: the warning must be GONE, not merely stale.
    MockDriver quick;
    r.drivers.addChild(&quick);
    r.drivers.setup();
    r.drivers.prepare();
    r.drivers.tick();
    CHECK(r.drivers.quiesceEncodeForTest());           // the worker answers
    CHECK(std::strlen(r.drivers.status()) == 0);       // and the card stops crying stall
    r.drivers.release();
}

namespace {
// Wedges its first tick on the worker until let go, as a core starved by other load does; every later tick returns at once until the test arms the next stall.
class StallingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* b) override { src_ = b; }
    void prepare() override { prepares++; }   // how often the split's retry reaches the drivers
    int prepares = 0;
    int ticks = 0;   ///< ticks run, by whichever worker, guarded by m_
    // DELIBERATELY blocking, despite MM_NONBLOCKING: the wait IS the stall this test needs, as SlowDriver's is.
    void tick() MM_NONBLOCKING override {
        std::unique_lock<std::mutex> lk(m_);
        ticks++;
        if (!armed_) return;
        armed_ = false;
        inTick_ = true;
        cv_.notify_all();
        cv_.wait_for(lk, 5s, [this] { return released_; });
        released_ = false;
        inTick_ = false;
    }
    /// True once the worker is inside a stalled tick, so the test opens the stall on demand rather than by timing.
    bool waitEntered() { std::unique_lock<std::mutex> lk(m_); return cv_.wait_for(lk, 5s, [this] { return inTick_; }); }
    void letGo() { { std::scoped_lock<std::mutex> lk(m_); released_ = true; } cv_.notify_all(); }
    /// True once a tick past `n` has run, polled as arm() is, so the tick needs no notification.
    bool waitTicksAbove(int n) {
        for (int i = 0; i < 5000; i++) {
            if (tickCount() > n) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
    int tickCount() { std::scoped_lock<std::mutex> lk(m_); return ticks; }
    /// Stall the next tick too, once the stalled one has unwound, polled so the tick needs no second notification.
    void arm() {
        for (int i = 0; i < 5000; i++) {
            {
                std::scoped_lock<std::mutex> lk(m_);
                if (!inTick_) { armed_ = true; return; }
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    mm::Buffer* src_ = nullptr;
private:
    std::mutex m_;
    std::condition_variable cv_;
    bool armed_ = true;
    bool inTick_ = false;
    bool released_ = false;
};
}  // namespace

// A worker that misses the frame boundary drops the split to inline, and the drivers still read the shared buffer, so it must keep receiving frames: on 2026-11-17 a host under build load froze its preview and Network Send on the last frame here.
TEST_CASE("render-split: after a stalled worker drops the split to inline, new frames still reach the drivers") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects layers;
    mm::Layer layer;
    mm::Drivers drivers;
    grid.width = 16; grid.height = 1; grid.depth = 1;
    layouts.addChild(&grid);
    layer.setChannelsPerLight(3);
    layers.addChild(&layer);          // one identity layer: the shared buffer exists only for the split
    layers.setLayouts(&layouts);
    drivers.setEffects(&layers);
    layers.applyState();

    StallingDriver d;
    drivers.addChild(&d);
    drivers.setup();
    drivers.prepare();
    REQUIRE(drivers.renderSplitActive());

    drivers.tick();                                    // the worker wedges inside d.tick()
    REQUIRE(d.waitEntered());
    CHECK_FALSE(drivers.quiesceEncodeForTest());       // the boundary wait times out and drops the split to inline
    d.letGo();                                         // the wedged tick ends; the worker is notified no more
    CHECK_FALSE(drivers.renderSplitActive());

    layer.buffer().data()[0] = 0x42;                   // the effect draws a new frame
    drivers.tick();
    REQUIRE(d.src_);
    REQUIRE(d.src_->data());
    CHECK(d.src_->data()[0] == 0x42);                  // the drivers see it, rather than the frame from before the stall
    drivers.release();
}

// One stall must not cost the second core for good: a split that is wanted but off asks for a prepare on a back-off of 1, 2, 4 up to 64 seconds, counted on tick1s, and 64 seconds engaged bring the wait back to 1.
TEST_CASE("render-split: a stalled split re-engages by itself, waiting longer each time it stalls again") {
    Rig r(16);
    StallingDriver d;
    r.drivers.addChild(&d);
    mm::Scheduler s;                                   // the retry asks it for a prepare at the next frame boundary
    s.addModule(&r.drivers);
    s.setup();
    s.preparePending();
    REQUIRE(r.drivers.renderSplitActive());

    const auto stall = [&] {
        r.drivers.tick();                              // the worker wedges inside d.tick()
        REQUIRE(d.waitEntered());
        REQUIRE_FALSE(r.drivers.quiesceEncodeForTest());   // the boundary gives up and drops the split
        d.letGo();
        r.drivers.stopEncodeForTest();                 // and the worker goes, as quiesce() stops it after the timeout
        REQUIRE_FALSE(r.drivers.renderSplitActive());
    };
    const auto second = [&] { r.drivers.tick1s(); s.preparePending(); };   // one second: the 1 Hz tick, then the frame boundary

    const int prepared = d.prepares;
    stall();
    second();
    CHECK(r.drivers.renderSplitActive());              // back after 1 s
    CHECK(d.prepares == prepared);                     // Drivers re-engaged alone: the driver kept its peripheral
    const int before = d.tickCount();
    r.drivers.tick();
    CHECK(d.waitTicksAbove(before));                   // a new worker, spawned by the retry, runs the drivers

    d.arm();
    stall();
    second();
    CHECK_FALSE(r.drivers.renderSplitActive());        // stalled again, so the wait doubled
    second();
    CHECK(r.drivers.renderSplitActive());              // back after 2 s

    for (int i = 0; i < 64; i++) second();             // 64 s engaged: whatever caused the stalls has passed
    REQUIRE(r.drivers.renderSplitActive());
    d.arm();
    stall();
    second();
    CHECK(r.drivers.renderSplitActive());              // so the next stall waits 1 s again

    r.drivers.release();                               // joins the worker
}
