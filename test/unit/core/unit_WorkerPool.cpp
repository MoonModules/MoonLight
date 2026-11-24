/// @module WorkerPool

/// The workers that render a frame's layers side by side: each item once, the batch returning only when all have run, and a pool with no workers running the batch in order.

#include "doctest.h"
#include "core/util/WorkerPool.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace mm;

namespace {
/// A batch's record: how often each item ran, and how many ran in all.
struct Tally {
    std::atomic<int> runs[64] = {};
    std::atomic<int> total{0};
    static void item(void* ctx, uint8_t i) {
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
    struct Order { uint8_t seen[8] = {}; uint8_t n = 0; static void item(void* c, uint8_t i) { auto* o = static_cast<Order*>(c); o->seen[o->n++] = i; } } o;
    pool.run(5, &Order::item, &o);
    REQUIRE(o.n == 5);
    for (uint8_t i = 0; i < 5; i++) CHECK(o.seen[i] == i);   // in order, on this thread
}

TEST_CASE("a batch of none or one runs on the caller") {
    WorkerPool pool;
    pool.start(2, 8192, 5, -1);
    struct Where { std::thread::id thread; int runs = 0; static void item(void* c, uint8_t) { auto* w = static_cast<Where*>(c); w->thread = std::this_thread::get_id(); w->runs++; } } where;
    pool.run(0, &Where::item, &where);
    CHECK(where.runs == 0);
    pool.run(1, &Where::item, &where);
    CHECK(where.runs == 1);
    CHECK(where.thread == std::this_thread::get_id());
}
