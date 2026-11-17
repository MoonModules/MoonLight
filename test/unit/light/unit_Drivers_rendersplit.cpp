/// @module Drivers
/// @also platform

/// Pins the multicore render↔encode split (Step 2a) on the host, where platform::spawnPinnedTask is a real std::thread, so the cross-core handoff invariants run on an actual second thread and TSan/ASan can catch a race or use-after-free on the shared outputBuffer_. The ESP32 core-1 task relies on exactly these invariants; here a MockDriver stands in for the real drivers (I80/Parlio are inert on the host, lanesAvailable()==0). One rule under test: while `multicore` is on, EVERY driver's tick() runs on core 1 against the finished frame; core 0 returns to rendering immediately.

#include "doctest.h"
#include "light/drivers/Drivers.h"
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

// A driver stub that records what it reads from the source buffer each tick. `sawTear` latches if it ever reads the producer's mid-write sentinel (0xEE), the proof the frame boundary held (core 0 never overwrote outputBuffer_ while core 1 was reading it).
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

// A driver the test can PARK inside its own tick(), standing in for a real 16K-light encode that holds core 1 for tens of ms. The handshake is a condition variable, not a sleep: `entered` fires the moment the worker is inside tick(), and the worker then blocks until the test releases it. So the test drives the race window deterministically instead of hoping a 40 ms sleep is long enough, no timing luck, and no unbounded spin that could hang the suite forever.
class SlowDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    // DELIBERATELY blocking, despite MM_NONBLOCKING: the mutex and timed wait ARE the test. They hold the use-after-free window open on demand, which is the only way to observe the race this file pins. The annotation is inherited from MoonModule::tick and cannot be dropped without breaking the override, so clang-hotpath will report these lines, that report is correct and expected here.
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

// RAII: install the quiesce-render hook pointing at `d`, and ALWAYS clear both the hook and the file static on scope exit, even if a REQUIRE throws past the test body. Without this a failed assertion would leave the process-global hook installed with g_hookDrivers dangling at a destroyed rig, so the next test's addChild would call quiesceRenderSplit() on freed memory (a cascade crash).
struct HookGuard {
    explicit HookGuard(mm::Drivers* d) {
        g_hookDrivers = d;
        mm::MoonModule::setQuiesceRenderHook([] { if (g_hookDrivers) g_hookDrivers->quiesceRenderSplit(); });
    }
    ~HookGuard() { mm::MoonModule::setQuiesceRenderHook(nullptr); g_hookDrivers = nullptr; }
};

}  // namespace

// DIAGNOSTIC (bench flap): a SINGLE enabled layer + one driver + multicore. On the bench renderWait alternated on/off second-to-second. needOutput is false here (one layer, no LUT), so the split is held only by splitWanted forcing outputBuffer_. Tick many frames and assert the split stays STABLY engaged, never flaps off, with no config change between ticks.
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

    // Tick 30 frames WITHOUT touching config. The split must not disengage on its own. renderSplitActive() is a state flag set/cleared synchronously in prepare()/tick(), not by worker timing, so it can be asserted right after each tick(), no sleep needed (a sleep would only add flake, not synchronization).
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
    CHECK_FALSE(a.sawTear.load());            // never read a mid-write buffer — the boundary held
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

// REGRESSION (v3.0.0 "UI refresh freezes the LEDs"): GET /api/types (which the web UI fetches on every page load) builds a throwaway probe of each registered type to read its default control values. A ParallelLedDriver probe runs selectDefaultPeripheral → swapPeripheral in its constructor/defineControls, and swapPeripheral fired MoonModule::notifyQuiesceRender() unconditionally. That global hook resolves to the LIVE Drivers via the static ActiveInstance seat (Drivers::active()), so a DETACHED probe (never in the tree) tore down the running split, and nothing re-engaged it. On a fast board it silently dropped to single-core; on a slower one the LEDs visibly froze until multicore was toggled. The fix: swapPeripheral notifies only when it is about to free a real backend (`peripheral_` non-null). A probe's first swap selects the default peripheral from a null backend, so it never notifies, the live worker is untouched. This pins that a detached ParallelLedDriver swapping its peripheral leaves a live split untouched.
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

// THE INVARIANT: core quiesces the worker before any structural mutation of a container's children.
// MoonModule::removeChild() calls quiesce(), a no-op for a module with no worker, overridden by Drivers to wait out the in-flight encode, so a mutation cannot begin while core 1 is inside a child's tick().
// Violate it and the sequence removeChild → release → deleteTree frees the driver (and its DMA buffers) out from under the worker mid-encode: a use-after-free, LoadProhibited on ESP32.
//
// The test deletes the driver at the one instant that is unsafe: while the worker is provably inside its tick(). Under ASan a regression is a heap-use-after-free; without ASan, the ordering assert still catches it (removeChild must not return until the worker is out).
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

    // A releaser lets the parked encode finish shortly AFTER the delete is under way, the real sequence, where the encode completes on its own while core 0 is already mutating the tree. `releasedAt` records WHEN, so the assertion below can prove removeChild() actually waited for it.
    std::atomic<bool> releaseFired{false};
    std::thread releaser([&] {
        std::this_thread::sleep_for(30ms);    // long enough that a non-waiting removeChild returns first
        releaseFired.store(true);
        slow->letGo();
    });

    r.drivers.removeChild(slow);              // core's quiesce() blocks here until the worker is out

    // THE ASSERTION, checked the instant removeChild returns, not after the join, or a non-waiting removeChild would look correct once the worker later finished on its own. With quiesce(), removeChild cannot return until the worker exited, which cannot happen before letGo() fired.
    CHECK(releaseFired.load());               // it waited for the encode (fails without quiesce)
    CHECK_FALSE(slow->isInTick());            // and the worker is provably out of tick()

    releaser.join();
    delete slow;                              // now safe — the worker is provably not inside it

    r.drivers.prepare();                      // last driver gone → disengage
    CHECK_FALSE(r.drivers.renderSplitActive());
    r.drivers.release();
}

// The SIBLING-SUBTREE case: mutating a node OUTSIDE the Drivers subtree, here a LAYOUT, while the encode worker runs. The worker ticks the drivers, and a driver walks the whole tree (PreviewDriver::sendFrame → Layouts::placeLights), so freeing a layout mid-walk is a use-after-free EVEN THOUGH the mutated node's parent (Layouts) owns no worker. `this->quiesce()` alone misses it, Layouts::quiesce() is the no-op default. The fix routes MoonModule::quiesceForMutation() through the quiesce-render HOOK, which reaches the render worker wherever it lives. This is the exact crash seen replacing a layout on a running split device (LoadProhibited); the test pins it via the hook.
TEST_CASE("render-split: mutating a LAYOUT while core 1 runs is safe via the quiesce-render hook") {
    Rig r(64);
    // A driver whose tick() reads the shared source buffer, enough to keep the worker busy on core 1.
    auto* slow = new SlowDriver();
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    // Wire the quiesce-render hook the way main.cpp does (routed through a file static since the hook is a non-capturing function pointer). The RAII guard clears it on every exit path, including a thrown REQUIRE, so a failure here can't dangle the hook into a later test.
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

    // Mutate a node OUTSIDE the Drivers subtree: remove the grid from Layouts. quiesceForMutation() must reach the render worker through the hook and block until it is out, WITHOUT the hook this returns immediately and a later free of `grid` would be a use-after-free (ASan) while the worker is still walking it.
    r.layouts.removeChild(&r.grid);

    CHECK(releaseFired.load());               // it waited for the encode (fails without the hook)
    CHECK_FALSE(slow->isInTick());            // the worker is provably out

    releaser.join();

    r.drivers.release();
    delete slow;
    // The HookGuard clears the hook + g_hookDrivers on scope exit (including a thrown REQUIRE above).
}

// The FOURTH mutator: reordering children (the drag-reorder UI → moveChildTo) permutes children_ under the worker's index-based tick loop. No free, so not a use-after-free, but a slot shift mid-loop can tick a child twice or skip one (the data-race class the batch closes). moveChildTo must quiesce like the other three. Same harness: park the worker inside a driver tick, reorder Drivers' children, and prove moveChildTo waited for the worker (fails without the quiesce).
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

// ROBUSTNESS FLOOR: a wedged core-1 worker must not hang the RENDER loop. The frame boundary waits for the encode, normally bounded by one encode (the `renderWait` KPI measures it). But a worker that never signals done, starved, wedged, a lost notify, would otherwise spin core 0 forever, and a permanent wedge ranks BELOW "degraded": the device must keep running, even poorly. So the boundary times out, DISENGAGES the split, and every driver falls back to ticking inline on core 0, the same single-core path a memory-tight board already takes. Slower, still lit.
//
// This times tick()'s boundary specifically. It does NOT go through quiesce() (the structural-mutation hook), which deliberately JOINS the worker on timeout, a blocking join is right there (the caller is about to free the driver) but would be wrong here, on the render path.
TEST_CASE("render-split: a wedged worker degrades to single-core instead of hanging the render loop") {
    Rig r(64);
    auto* slow = new SlowDriver();            // parks inside tick() until we release it
    r.drivers.addChild(slow);
    r.drivers.setup();
    r.drivers.prepare();
    REQUIRE(r.drivers.renderSplitActive());

    r.drivers.tick();                         // core 1 enters slow->tick() and parks there
    REQUIRE(slow->waitEntered());             // it is provably stuck

    // Release the worker FIRST so the disengage path can't block on it, what is under test is the BOUNDARY giving up, not the teardown. The worker still hasn't signalled done for the frame the boundary is waiting on, so the wait below is a genuine wedge from the boundary's point of view.
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

// A degraded state must be able to END. The stall warning above was set once and never lifted, so a card kept reporting "encode worker stalled" long after the split was healthy again, and even beside a multicore toggle the user had since switched OFF. A status that cannot clear stops describing the device and starts describing its history. Reported from a Linux bench, whose screenshot showed exactly that pair.
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

    // Recovery. The worker must be STOPPED AND JOINED before the child list is touched: the timed-out quiesce above already cleared renderSplitActive_, so removeChild's own quiesce() returns immediately without waiting, and the still-running worker would be walking childCount() as we mutate it. TSan caught exactly that race in CI. release() joins.
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
// Wedges its first tick on the worker until let go, as a core starved by other load does; every later tick returns at once.
class StallingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer* b) override { src_ = b; }
    // DELIBERATELY blocking, despite MM_NONBLOCKING: the wait IS the stall this test needs, as SlowDriver's is.
    void tick() MM_NONBLOCKING override {
        std::unique_lock<std::mutex> lk(m_);
        if (!stalled_) { stalled_ = true; cv_.notify_all(); cv_.wait_for(lk, 5s, [this] { return released_; }); }
    }
    /// True once the worker is inside its first tick, so the test opens the stall on demand rather than by timing.
    bool waitEntered() { std::unique_lock<std::mutex> lk(m_); return cv_.wait_for(lk, 5s, [this] { return stalled_; }); }
    void letGo() { { std::scoped_lock<std::mutex> lk(m_); released_ = true; } cv_.notify_all(); }
    mm::Buffer* src_ = nullptr;
private:
    std::mutex m_;
    std::condition_variable cv_;
    bool stalled_ = false;
    bool released_ = false;
};
}  // namespace

// A worker that misses the frame boundary drops the split to inline, and the drivers still read the shared buffer, so it must keep receiving frames: on 2026-11-17 a desktop under build load froze its preview and Network Send on the last frame here.
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
