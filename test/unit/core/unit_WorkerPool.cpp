/// @module WorkerPool

/// The workers that render a frame's layers side by side: each item once, the batch returning only when all have run, and a pool with no workers running the batch in order.

#include "doctest.h"
#include "core/util/WorkerPool.h"
#include "platform/platform_clock.h"   // micros, delayUs
#include "platform/platform_tasks.h"   // setTestSpawnFails

#include <atomic>
#include <cstdint>
#include <thread>

using namespace mm;

namespace {
/// A batch's record: how often each item ran, and how many ran in all.
struct Tally {
    std::atomic<int> runs[64] = {};
    std::atomic<int> total{0};
    static void item(void* ctx, uint8_t i, bool) {
        auto* t = static_cast<Tally*>(ctx);
        t->runs[i].fetch_add(1);
        t->total.fetch_add(1);
    }
};
}  // namespace

TEST_CASE("a pool runs every item of a batch once, and returns only when all have run") {
    WorkerPool pool;
    const uint8_t started = pool.start(3, 8192, 5, -1);
    CHECK(started == (WorkerPool::kMaxWorkers < 3 ? WorkerPool::kMaxWorkers : 3));
    for (int batch = 0; batch < 200; batch++) {   // many batches back to back: a worker leaving one takes items of its own batch alone
        Tally t;
        pool.run(17, &Tally::item, &t);
        CHECK(t.total.load() == 17);
        for (int i = 0; i < 17; i++) CHECK(t.runs[i].load() == 1);
    }
    pool.stop();
    CHECK(pool.workers() == 0);
}

TEST_CASE("a pool whose workers cannot be created runs the batch in order on the caller") {
    platform::setTestSpawnFails(true);
    WorkerPool pool;
    CHECK(pool.start(3, 8192, 5, -1) == 0);
    platform::setTestSpawnFails(false);
    struct Order { uint8_t seen[8] = {}; uint8_t n = 0; static void item(void* c, uint8_t i, bool) { auto* o = static_cast<Order*>(c); o->seen[o->n++] = i; } } o;
    pool.run(5, &Order::item, &o);
    REQUIRE(o.n == 5);
    for (uint8_t i = 0; i < 5; i++) CHECK(o.seen[i] == i);   // in order, on this thread
}

TEST_CASE("a batch of none or one runs on the caller") {
    WorkerPool pool;
    pool.start(2, 8192, 5, -1);
    struct Where { std::thread::id thread; int runs = 0; static void item(void* c, uint8_t, bool) { auto* w = static_cast<Where*>(c); w->thread = std::this_thread::get_id(); w->runs++; } } where;
    pool.run(0, &Where::item, &where);
    CHECK(where.runs == 0);
    pool.run(1, &Where::item, &where);
    CHECK(where.runs == 1);
    CHECK(where.thread == std::this_thread::get_id());
}

namespace {
/// An item that holds its thread for two milliseconds, so the load has time to count.
void busyItem(void*, uint8_t, bool) {
    const uint32_t t0 = platform::micros();
    while (platform::micros() - t0 < 2000) {}
}
}  // namespace

TEST_CASE("a pool's load splits the items' time by who ran them, and a take starts it again") {
    WorkerPool pool;
    pool.start(3, 8192, 5, -1);
    pool.run(8, &busyItem, nullptr);
    const WorkerPool::Load l = pool.takeLoad();
    CHECK(l.callerUs + l.workersUs >= 8 * 2000);   // every item counted, on whichever thread ran it
    if (pool.workers() == 0) CHECK(l.workersUs == 0);
    const WorkerPool::Load again = pool.takeLoad();
    CHECK(again.callerUs == 0);
    CHECK(again.workersUs == 0);
    CHECK(again.joinUs == 0);
}

TEST_CASE("a pool with no workers counts the whole batch as the caller's, with no wait") {
    WorkerPool pool;
    pool.run(3, &busyItem, nullptr);
    const WorkerPool::Load l = pool.takeLoad();
    CHECK(l.callerUs >= 3 * 2000);
    CHECK(l.workersUs == 0);
    CHECK(l.joinUs == 0);
}

namespace {
/// Each item's worker flag, and the caller's own thread to tell them apart.
struct Where {
    std::atomic<int> onWorker{0}, onCaller{0};
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<int> wrong{0};   // an item whose answer disagrees with the thread it ran on
    static void item(void* ctx, uint8_t, bool worker) {
        auto* w = static_cast<Where*>(ctx);
        (worker ? w->onWorker : w->onCaller).fetch_add(1);
        if (worker == (std::this_thread::get_id() == w->caller)) w->wrong.fetch_add(1);
        platform::delayUs(200);   // long enough that the workers take items too
    }
};
}  // namespace

TEST_CASE("an item knows whether a worker runs it, and the caller never counts as one") {
    WorkerPool pool;
    pool.start(3, 8192, 5, -1);
    Where w;
    pool.run(32, &Where::item, &w);
    CHECK(w.wrong.load() == 0);
    CHECK(w.onWorker.load() + w.onCaller.load() == 32);
    if (pool.workers() > 0) CHECK(w.onWorker.load() > 0);
}
